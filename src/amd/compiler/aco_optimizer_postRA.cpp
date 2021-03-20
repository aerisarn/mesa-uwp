/*
 * Copyright © 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Timur Kristóf <timur.kristof@gmail.com
 *
 */

#include "aco_ir.h"

#include <vector>
#include <bitset>
#include <algorithm>
#include <array>

namespace aco {
namespace {

constexpr const size_t max_reg_cnt = 512;

enum {
   not_written_in_block = -1,
   clobbered = -2,
   const_or_undef = -3,
   written_by_multiple_instrs = -4,
};

struct pr_opt_ctx
{
   Program *program;
   Block *current_block;
   int current_instr_idx;
   std::vector<uint16_t> uses;
   std::array<int, max_reg_cnt * 4u> instr_idx_by_regs;

   void reset_block(Block *block)
   {
      current_block = block;
      current_instr_idx = -1;
      std::fill(instr_idx_by_regs.begin(), instr_idx_by_regs.end(), not_written_in_block);
   }
};

void save_reg_writes(pr_opt_ctx &ctx, aco_ptr<Instruction> &instr)
{
   for (const Definition &def : instr->definitions) {
      assert(def.regClass().type() != RegType::sgpr || def.physReg().reg() <= 255);
      assert(def.regClass().type() != RegType::vgpr || def.physReg().reg() >= 256);

      unsigned dw_size = DIV_ROUND_UP(def.bytes(), 4u);
      unsigned r = def.physReg().reg();
      int idx = ctx.current_instr_idx;

      if (def.regClass().is_subdword())
         idx = clobbered;

      assert(def.size() == dw_size || def.regClass().is_subdword());
      std::fill(&ctx.instr_idx_by_regs[r], &ctx.instr_idx_by_regs[r + dw_size], idx);
   }
}

int last_writer_idx(pr_opt_ctx &ctx, PhysReg physReg, RegClass rc)
{
   /* Verify that all of the operand's registers are written by the same instruction. */
   int instr_idx = ctx.instr_idx_by_regs[physReg.reg()];
   unsigned dw_size = DIV_ROUND_UP(rc.bytes(), 4u);
   unsigned r = physReg.reg();
   bool all_same = std::all_of(
      &ctx.instr_idx_by_regs[r], &ctx.instr_idx_by_regs[r + dw_size],
      [instr_idx](int i) { return i == instr_idx; });

   return all_same ? instr_idx : written_by_multiple_instrs;
}

int last_writer_idx(pr_opt_ctx &ctx, const Operand &op)
{
   if (op.isConstant() || op.isUndefined())
      return const_or_undef;

   int instr_idx = ctx.instr_idx_by_regs[op.physReg().reg()];

#ifndef NDEBUG
   /* Debug mode:  */
   instr_idx = last_writer_idx(ctx, op.physReg(), op.regClass());
   assert(instr_idx != written_by_multiple_instrs);
#endif

   return instr_idx;
}

void try_apply_branch_vcc(pr_opt_ctx &ctx, aco_ptr<Instruction> &instr)
{
   /* We are looking for the following pattern:
    *
    * vcc = ...                      ; last_vcc_wr
    * sX, scc = s_and_bXX vcc, exec  ; op0_instr
    * (...vcc and exec must not be clobbered inbetween...)
    * s_cbranch_XX scc               ; instr
    *
    * If possible, the above is optimized into:
    *
    * vcc = ...                      ; last_vcc_wr
    * s_cbranch_XX vcc               ; instr modified to use vcc
    */

   /* Don't try to optimize this on GFX6-7 because SMEM may corrupt the vccz bit. */
   if (ctx.program->chip_class < GFX8)
      return;

   if (instr->format != Format::PSEUDO_BRANCH ||
       instr->operands.size() == 0 ||
       instr->operands[0].physReg() != scc)
      return;

   int op0_instr_idx = last_writer_idx(ctx, instr->operands[0]);
   int last_vcc_wr_idx = last_writer_idx(ctx, vcc, ctx.program->lane_mask);
   int last_exec_wr_idx = last_writer_idx(ctx, exec, ctx.program->lane_mask);

   /* We need to make sure:
    * - the operand register used by the branch, and VCC were both written in the current block
    * - VCC was NOT written after the operand register
    * - EXEC is sane and was NOT written after the operand register
    */
   if (op0_instr_idx < 0 || last_vcc_wr_idx < 0 || last_vcc_wr_idx > op0_instr_idx ||
       last_exec_wr_idx > last_vcc_wr_idx || last_exec_wr_idx < not_written_in_block)
      return;

   aco_ptr<Instruction> &op0_instr = ctx.current_block->instructions[op0_instr_idx];
   aco_ptr<Instruction> &last_vcc_wr = ctx.current_block->instructions[last_vcc_wr_idx];

   if ((op0_instr->opcode != aco_opcode::s_and_b64 /* wave64 */ &&
        op0_instr->opcode != aco_opcode::s_and_b32 /* wave32 */) ||
       op0_instr->operands[0].physReg() != vcc ||
       op0_instr->operands[1].physReg() != exec ||
       !last_vcc_wr->isVOPC())
      return;

   assert(last_vcc_wr->definitions[0].tempId() == op0_instr->operands[0].tempId());

   /* Reduce the uses of the SCC def */
   ctx.uses[instr->operands[0].tempId()]--;
   /* Use VCC instead of SCC in the branch */
   instr->operands[0] = op0_instr->operands[0];
}

void process_instruction(pr_opt_ctx &ctx, aco_ptr<Instruction> &instr)
{
   ctx.current_instr_idx++;

   try_apply_branch_vcc(ctx, instr);

   if (instr)
      save_reg_writes(ctx, instr);
}

} /* End of empty namespace */

void optimize_postRA(Program* program)
{
   pr_opt_ctx ctx;
   ctx.program = program;
   ctx.uses = dead_code_analysis(program);

   /* Forward pass
    * Goes through each instruction exactly once, and can transform
    * instructions or adjust the use counts of temps.
    */
   for (auto &block : program->blocks) {
      ctx.reset_block(&block);

      for (aco_ptr<Instruction> &instr : block.instructions)
         process_instruction(ctx, instr);
   }

   /* Cleanup pass
    * Gets rid of instructions which are manually deleted or
    * no longer have any uses.
    */
   for (auto &block : program->blocks) {
      auto new_end = std::remove_if(
         block.instructions.begin(), block.instructions.end(),
         [&ctx](const aco_ptr<Instruction> &instr) { return !instr || is_dead(ctx.uses, instr.get()); });
      block.instructions.resize(new_end - block.instructions.begin());
   }
}

} /* End of aco namespace */
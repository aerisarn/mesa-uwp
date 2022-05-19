/*
 * Copyright (C) 2022 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "va_compiler.h"
#include "valhall_enums.h"
#include "bi_builder.h"

/*
 * Insert flow control into a scheduled and register allocated shader.  This
 * pass runs after scheduling and register allocation. This pass only
 * inserts NOPs with the appropriate flow control modifiers. It should be
 * followed by a cleanup pass to merge flow control modifiers on adjacent
 * instructions, eliminating the NOPs. This decouples optimization from
 * correctness, simplifying both passes.
 *
 * This pass is responsible for calculating dependencies, according to the
 * rules:
 *
 * 1. An instruction that depends on the results of a previous asyncronous
 *    must first wait for that instruction's slot, unless all
 *    reaching code paths already depended on it.
 * 2. More generally, any dependencies must be encoded. This includes
 *    Write-After-Write and Write-After-Read hazards with LOAD/STORE to memory.
 * 3. The shader must wait on slot #6 before running BLEND, ATEST
 * 4. The shader must wait on slot #7 before running BLEND, ST_TILE
 * 6. BARRIER must wait on every active slot.
 *
 * Unlike Bifrost, it is not necessary to worry about outbound staging
 * registers, as the hardware stalls reading staging registers when issuing
 * asynchronous instructions. So we don't track reads in our model of the
 * hardware scoreboard. This makes things a bit simpler.
 *
 * We may reuse slots for multiple asynchronous instructions, though there may
 * be a performance penalty.
 */

#define BI_NUM_GENERAL_SLOTS 3
#define BI_NUM_REGISTERS 64

/*
 * Insert a NOP instruction with given flow control.
 */
static void
bi_flow(bi_context *ctx, bi_cursor cursor, enum va_flow flow)
{
   bi_builder b = bi_init_builder(ctx, cursor);

   bi_nop(&b)->flow = flow;
}

static uint64_t
bi_read_mask(bi_instr *I)
{
   uint64_t mask = 0;

   bi_foreach_src(I, s) {
      if (I->src[s].type == BI_INDEX_REGISTER) {
         unsigned reg = I->src[s].value;
         unsigned count = bi_count_read_registers(I, s);

         mask |= (BITFIELD64_MASK(count) << reg);
      }
   }

   return mask;
}

static uint64_t
bi_write_mask(bi_instr *I)
{
   uint64_t mask = 0;

   bi_foreach_dest(I, d) {
      if (bi_is_null(I->dest[d])) continue;

      assert(I->dest[d].type == BI_INDEX_REGISTER);

      unsigned reg = I->dest[d].value;
      unsigned count = bi_count_write_registers(I, d);

      mask |= (BITFIELD64_MASK(count) << reg);
   }

   return mask;
}

static bool
bi_ld_vary_writes_hidden_register(const bi_instr *I)
{
   /* Only varying loads can write the hidden register */
   if (bi_opcode_props[I->op].message != BIFROST_MESSAGE_VARYING)
      return false;

   /* They only write in some update modes */
   return (I->update == BI_UPDATE_STORE) || (I->update == BI_UPDATE_CLOBBER);
}

static bool
bi_is_memory_access(const bi_instr *I)
{
   /* On the attribute unit but functionally a general memory load */
   if (I->op == BI_OPCODE_LD_ATTR_TEX)
      return true;

   /* UBOs are read-only so there are no ordering constriants */
   if (I->seg == BI_SEG_UBO)
      return false;

   switch (bi_opcode_props[I->op].message) {
   case BIFROST_MESSAGE_LOAD:
   case BIFROST_MESSAGE_STORE:
   case BIFROST_MESSAGE_ATOMIC:
      return true;
   default:
      return false;
   }
}

/* Update the scoreboard model to assign an instruction to a given slot */

static void
bi_push_instr(struct bi_scoreboard_state *st, bi_instr *I)
{
   if (bi_opcode_props[I->op].sr_write)
      st->write[I->slot] |= bi_write_mask(I);

   if (bi_is_memory_access(I))
      st->memory |= BITFIELD_BIT(I->slot);

   if (bi_opcode_props[I->op].message == BIFROST_MESSAGE_VARYING)
      st->varying |= BITFIELD_BIT(I->slot);
}

static uint8_t MUST_CHECK
bi_pop_slot(struct bi_scoreboard_state *st, unsigned slot)
{
   st->write[slot] = 0;
   st->varying &= ~BITFIELD_BIT(slot);
   st->memory &= ~BITFIELD_BIT(slot);

   return BITFIELD_BIT(slot);
}

/* Adds a dependency on each slot writing any specified register */

static uint8_t MUST_CHECK
bi_depend_on_writers(struct bi_scoreboard_state *st, uint64_t regmask)
{
   uint8_t slots = 0;

   for (unsigned slot = 0; slot < ARRAY_SIZE(st->write); ++slot) {
      if (st->write[slot] & regmask)
         slots |= bi_pop_slot(st, slot);
   }

   return slots;
}

/* Sets the dependencies for a given clause, updating the model */

static void
bi_set_dependencies(bi_block *block, bi_instr *I, struct bi_scoreboard_state *st)
{
   /* Depend on writers to handle read-after-write and write-after-write
    * dependencies. Write-after-read dependencies are handled in the hardware
    * where necessary, so we don't worry about them.
    */
   I->flow |= bi_depend_on_writers(st, bi_read_mask(I) | bi_write_mask(I));

   /* Handle write-after-write and write-after-read dependencies for the varying
    * hidden registers. Read-after-write dependencies handled in hardware.
    */
   if (bi_ld_vary_writes_hidden_register(I)) {
      u_foreach_bit(slot, st->varying)
         I->flow |= bi_pop_slot(st, slot);
   }

   /* For now, serialize all memory access */
   if (bi_is_memory_access(I)) {
      u_foreach_bit(slot, st->memory)
         I->flow |= bi_pop_slot(st, slot);
   }
}

static bool
scoreboard_block_update(bi_context *ctx, bi_block *blk)
{
   bool progress = false;

   /* pending_in[s] = sum { p in pred[s] } ( pending_out[p] ) */
   bi_foreach_predecessor(blk, pred) {
      for (unsigned i = 0; i < BI_NUM_SLOTS; ++i) {
         blk->scoreboard_in.read[i] |= (*pred)->scoreboard_out.read[i];
         blk->scoreboard_in.write[i] |= (*pred)->scoreboard_out.write[i];
         blk->scoreboard_in.varying |= (*pred)->scoreboard_out.varying;
         blk->scoreboard_in.memory |= (*pred)->scoreboard_out.memory;
      }
   }

   struct bi_scoreboard_state state = blk->scoreboard_in;

   /* Assign locally */

   bi_foreach_instr_in_block(blk, I) {
      bi_set_dependencies(blk, I, &state);
      bi_push_instr(&state, I);
   }

   /* Insert a wait for varyings at the end of the block.
    *
    * A varying load with .store has to wait for all other varying loads
    * in the quad to complete. The bad case looks like:
    *
    *    if (dynamic) {
    *        x = ld_var()
    *    } else {
    *       x = ld_var()
    *    }
    *
    * Logically, a given thread executes only a single ld_var instruction. But
    * if the quad diverges, the second ld_var has to wait for the first ld_var.
    * For correct handling, we need to maintain a physical control flow graph
    * and do the dataflow analysis on that instead of the logical control flow
    * graph. However, this probably doesn't matter much in practice. This seems
    * like a decent compromise for now.
    *
    * TODO: Consider optimizing this case.
    */
   if (state.varying) {
      uint8_t flow = 0;

      u_foreach_bit(slot, state.varying)
         flow |= bi_pop_slot(&state, slot);

      bi_flow(ctx, bi_after_block(blk), flow);
   }

   /* To figure out progress, diff scoreboard_out */
   progress = !!memcmp(&state, &blk->scoreboard_out, sizeof(state));

   blk->scoreboard_out = state;

   return progress;
}

static void
va_assign_scoreboard(bi_context *ctx)
{
   u_worklist worklist;
   bi_worklist_init(ctx, &worklist);

   bi_foreach_block(ctx, block) {
      bi_worklist_push_tail(&worklist, block);
   }

   /* Perform forward data flow analysis to calculate dependencies */
   while (!u_worklist_is_empty(&worklist)) {
      /* Pop from the front for forward analysis */
      bi_block *blk = bi_worklist_pop_head(&worklist);

      if (scoreboard_block_update(ctx, blk)) {
         bi_foreach_successor(blk, succ)
            bi_worklist_push_tail(&worklist, succ);
      }
   }

   u_worklist_fini(&worklist);
}

/*
 * Determine if execution should terminate after a given block. Execution cannot
 * terminate within a basic block.
 */
static bool
va_should_end(bi_block *block)
{
   /* Don't return if we're succeeded by instructions */
   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      bi_block *succ = block->successors[i];

      if (succ)
         return false;
   }

   return true;
}

/*
 * Given a program with no flow control modifiers, insert NOPs signaling the
 * required flow control. Not much optimization happens here.
 */
void
va_insert_flow_control_nops(bi_context *ctx)
{
   /* First do dataflow analysis for the scoreboard. This populates I->flow with
    * a bitmap of slots to wait on.
    */
   va_assign_scoreboard(ctx);

   bi_foreach_block(ctx, block) {
      bi_foreach_instr_in_block_safe(block, I) {
         switch (I->op) {
         /* Signal barriers immediately */
         case BI_OPCODE_BARRIER:
            bi_flow(ctx, bi_after_instr(I), VA_FLOW_WAIT);
            break;

         /* Insert waits for tilebuffer and depth/stencil instructions. These
          * only happen in regular fragment shaders, as the required waits are
          * assumed to already have happened in blend shaders.
          */
         case BI_OPCODE_BLEND:
         case BI_OPCODE_LD_TILE:
         case BI_OPCODE_ST_TILE:
            if (!ctx->inputs->is_blend)
               bi_flow(ctx, bi_before_instr(I), VA_FLOW_WAIT);
            break;
         case BI_OPCODE_ATEST:
         case BI_OPCODE_ZS_EMIT:
            if (!ctx->inputs->is_blend)
               bi_flow(ctx, bi_before_instr(I), VA_FLOW_WAIT0126);
            break;

         default:
            break;
         }

         if (I->flow && I->op != BI_OPCODE_NOP) {
            /* Wait on the results of asynchronous instructions
             *
             * Bitmap of general slots lines up with the encoding of va_flow for
             * waits on general slots. The dataflow analysis should be ignoring
             * the special slots #6 and #7, which are handled separately.
             */
            assert((I->flow & ~BITFIELD_MASK(BI_NUM_GENERAL_SLOTS)) == 0);

            bi_flow(ctx, bi_before_instr(I), I->flow);
            I->flow = 0;
         }
      }

      /* End exeuction at the end of the block if needed, or reconverge if we
       * continue but we don't need to end execution.
       */
      if (va_should_end(block) || block->needs_nop) {
         /* Don't bother adding a NOP into an unreachable block */
         if (block == bi_start_block(&ctx->blocks) || bi_num_predecessors(block))
            bi_flow(ctx, bi_after_block(block), VA_FLOW_END);
      } else if (bi_reconverge_branches(block)) {
         /* TODO: Do we have ever need to reconverge from an empty block? */
         if (!list_is_empty(&block->instructions))
            bi_flow(ctx, bi_after_block(block), VA_FLOW_RECONVERGE);
      }
   }
}

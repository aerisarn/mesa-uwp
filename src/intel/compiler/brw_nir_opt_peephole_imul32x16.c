/*
 * Copyright © 2022 Intel Corporation
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
 */

#include "brw_nir.h"
#include "compiler/nir/nir_builder.h"

/**
 * Implement a peephole pass to convert integer multiplications to imul32x16.
 */

static void
replace_imul_instr(nir_builder *b, nir_alu_instr *imul, unsigned small_val,
                   nir_op new_opcode)
{
   assert(small_val == 0 || small_val == 1);

   b->cursor = nir_before_instr(&imul->instr);

   nir_alu_instr *imul_32x16 = nir_alu_instr_create(b->shader, new_opcode);
   imul_32x16->dest.saturate = imul->dest.saturate;
   imul_32x16->dest.write_mask = imul->dest.write_mask;

   nir_alu_src_copy(&imul_32x16->src[0], &imul->src[1 - small_val], imul_32x16);
   nir_alu_src_copy(&imul_32x16->src[1], &imul->src[small_val], imul_32x16);

   nir_ssa_dest_init(&imul_32x16->instr, &imul_32x16->dest.dest,
                     imul->dest.dest.ssa.num_components,
                     32, NULL);

   nir_ssa_def_rewrite_uses(&imul->dest.dest.ssa,
                            &imul_32x16->dest.dest.ssa);

   nir_builder_instr_insert(b, &imul_32x16->instr);

   nir_instr_remove(&imul->instr);
   nir_instr_free(&imul->instr);
}

static bool
brw_nir_opt_peephole_imul32x16_instr(nir_builder *b,
                                     nir_instr *instr,
                                     UNUSED void *cb_data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *imul = nir_instr_as_alu(instr);
   if (imul->op != nir_op_imul)
      return false;

   if (imul->dest.dest.ssa.bit_size != 32)
      return false;

   nir_op new_opcode = nir_num_opcodes;

   unsigned i;
   for (i = 0; i < 2; i++) {
      if (!nir_src_is_const(imul->src[i].src))
         continue;

      int64_t lo = INT64_MAX;
      int64_t hi = INT64_MIN;

      for (unsigned comp = 0; comp < imul->dest.dest.ssa.num_components; comp++) {
         int64_t v = nir_src_comp_as_int(imul->src[i].src, comp);

         if (v < lo)
            lo = v;

         if (v > hi)
            hi = v;
      }

      if (lo >= INT16_MIN && hi <= INT16_MAX) {
         new_opcode = nir_op_imul_32x16;
         break;
      } else if (lo >= 0 && hi <= UINT16_MAX) {
         new_opcode = nir_op_umul_32x16;
         break;
      }
   }

   if (new_opcode != nir_num_opcodes) {
      replace_imul_instr(b, imul, i, new_opcode);
      return true;
   }

   return false;
}

bool
brw_nir_opt_peephole_imul32x16(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader,
                                       brw_nir_opt_peephole_imul32x16_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       NULL);
}


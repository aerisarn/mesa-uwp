/*
 * Copyright © 2020 Advanced Micro Devices, Inc.
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

/* These passes enable converting uniforms to literals when it's profitable,
 * effectively inlining uniform values in the IR. The main benefit is register
 * usage decrease leading to better SMT (hyperthreading). It's accomplished
 * by targetting uniforms that determine whether a conditional branch is
 * taken.
 *
 * Only uniforms used in if conditions are analyzed.
 *
 * nir_find_inlinable_uniforms finds uniforms that can be inlined and stores
 * that information in shader_info.
 *
 * nir_inline_uniforms inlines uniform values.
 *
 * (uniforms must be lowered to load_ubo before calling this)
 */

#include "compiler/nir/nir_builder.h"

/* Maximum value in shader_info::inlinable_uniform_dw_offsets[] */
#define MAX_OFFSET (UINT16_MAX * 4)

static bool
src_only_uses_uniforms(const nir_src *src, uint32_t *uni_offsets,
                       unsigned *num_offsets)
{
   if (!src->is_ssa)
      return false;

   nir_instr *instr = src->ssa->parent_instr;

   switch (instr->type) {
   case nir_instr_type_alu: {
      /* Return true if all sources return true. */
      /* TODO: Swizzles are ignored, so vectors can prevent inlining. */
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         if (!src_only_uses_uniforms(&alu->src[i].src, uni_offsets,
                                     num_offsets))
             return false;
      }
      return true;
   }

   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      /* Return true if the intrinsic loads from UBO 0 with a constant
       * offset.
       */
      if (intr->intrinsic == nir_intrinsic_load_ubo &&
          nir_src_is_const(intr->src[0]) &&
          nir_src_as_uint(intr->src[0]) == 0 &&
          nir_src_is_const(intr->src[1]) &&
          nir_src_as_uint(intr->src[1]) <= MAX_OFFSET &&
          /* TODO: Can't handle vectors and other bit sizes for now. */
          /* UBO loads should be scalarized. */
          intr->dest.ssa.num_components == 1 &&
          intr->dest.ssa.bit_size == 32) {
         uint32_t offset = nir_src_as_uint(intr->src[1]);
         assert(offset < MAX_OFFSET);

         /* Already recorded by other one */
         for (int i = 0; i < *num_offsets; i++) {
            if (uni_offsets[i] == offset)
               return true;
         }

         /* Exceed uniform number limit */
         if (*num_offsets == MAX_INLINABLE_UNIFORMS)
            return false;

         /* Record the uniform offset. */
         uni_offsets[(*num_offsets)++] = offset;
         return true;
      }
      return false;
   }

   case nir_instr_type_load_const:
      /* Always return true for constants. */
      return true;

   default:
      return false;
   }
}

static void
add_inlinable_uniforms(const nir_src *cond, uint32_t *uni_offsets,
                       unsigned *num_offsets)
{
   unsigned new_num = *num_offsets;

   /* Only update uniform number when all uniforms in the expression
    * can be inlined. Partially inline uniforms can't lower if/loop.
    *
    * For example, uniform can be inlined for a shader is limited to 4,
    * and we have already added 3 uniforms, then want to deal with
    *
    *     if (uniform0 + uniform1 == 10)
    *
    * only uniform0 can be inlined due to we exceed the 4 limit. But
    * unless both uniform0 and uniform1 are inlined, can we eliminate
    * the if statement.
    *
    * This is even possible when we deal with loop if the induction
    * variable init and update also contains uniform like
    *
    *    for (i = uniform0; i < uniform1; i+= uniform2)
    *
    * unless uniform0, uniform1 and uniform2 can be inlined at once,
    * can the loop be unrolled.
    */
   if (src_only_uses_uniforms(cond, uni_offsets, &new_num))
      *num_offsets = new_num;
}

void
nir_find_inlinable_uniforms(nir_shader *shader)
{
   uint32_t uni_offsets[MAX_INLINABLE_UNIFORMS];
   unsigned num_offsets = 0;

   nir_foreach_function(function, shader) {
      if (function->impl) {
         foreach_list_typed(nir_cf_node, node, node, &function->impl->body) {
            switch (node->type) {
            case nir_cf_node_if: {
               const nir_src *cond = &nir_cf_node_as_if(node)->condition;
               add_inlinable_uniforms(cond, uni_offsets, &num_offsets);
               break;
            }

            case nir_cf_node_loop:
               /* TODO: handle loops if we want to unroll them at draw time */
               break;

            default:
               break;
            }
         }
      }
   }

   for (int i = 0; i < num_offsets; i++)
      shader->info.inlinable_uniform_dw_offsets[i] = uni_offsets[i] / 4;
   shader->info.num_inlinable_uniforms = num_offsets;
}

void
nir_inline_uniforms(nir_shader *shader, unsigned num_uniforms,
                    const uint32_t *uniform_values,
                    const uint16_t *uniform_dw_offsets)
{
   if (!num_uniforms)
      return;

   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_builder b;
         nir_builder_init(&b, function->impl);
         nir_foreach_block(block, function->impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               /* Only replace UBO 0 with constant offsets. */
               if (intr->intrinsic == nir_intrinsic_load_ubo &&
                   nir_src_is_const(intr->src[0]) &&
                   nir_src_as_uint(intr->src[0]) == 0 &&
                   nir_src_is_const(intr->src[1]) &&
                   /* TODO: Can't handle vectors and other bit sizes for now. */
                   /* UBO loads should be scalarized. */
                   intr->dest.ssa.num_components == 1 &&
                   intr->dest.ssa.bit_size == 32) {
                  uint64_t offset = nir_src_as_uint(intr->src[1]);

                  for (unsigned i = 0; i < num_uniforms; i++) {
                     if (offset == uniform_dw_offsets[i] * 4) {
                        b.cursor = nir_before_instr(&intr->instr);
                        nir_ssa_def *def = nir_imm_int(&b, uniform_values[i]);
                        nir_ssa_def_rewrite_uses(&intr->dest.ssa, def);
                        nir_instr_remove(&intr->instr);
                        break;
                     }
                  }
               }
            }
         }

         nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
      }
   }
}

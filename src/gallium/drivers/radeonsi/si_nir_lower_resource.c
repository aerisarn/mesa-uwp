/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * This lowering pass converts index based buffer/image/texture access to
 * explicite descriptor based, which simplify the compiler backend translation.
 *
 * For example: load_ubo(1) -> load_ubo(vec4), where the vec4 is the buffer
 * descriptor with index==1, so compiler backend don't need to do index-to-descriptor
 * finding which is the most complicated part (move to nir now).
 */

#include "nir_builder.h"

#include "ac_nir.h"
#include "si_pipe.h"
#include "si_shader_internal.h"
#include "sid.h"

struct lower_resource_state {
   struct si_shader *shader;
   struct si_shader_args *args;
};

static nir_ssa_def *load_ubo_desc_fast_path(nir_builder *b, nir_ssa_def *addr_lo,
                                            struct si_shader_selector *sel)
{
   nir_ssa_def *addr_hi =
      nir_imm_int(b, S_008F04_BASE_ADDRESS_HI(sel->screen->info.address32_hi));

   uint32_t rsrc3 =
      S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
      S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);

   if (sel->screen->info.gfx_level >= GFX11)
      rsrc3 |= S_008F0C_FORMAT(V_008F0C_GFX11_FORMAT_32_FLOAT) |
               S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW);
   else if (sel->screen->info.gfx_level >= GFX10)
      rsrc3 |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_FLOAT) |
               S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW) | S_008F0C_RESOURCE_LEVEL(1);
   else
      rsrc3 |= S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
               S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);

   return nir_vec4(b, addr_lo, addr_hi, nir_imm_int(b, sel->info.constbuf0_num_slots * 16),
                   nir_imm_int(b, rsrc3));
}

static nir_ssa_def *clamp_index(nir_builder *b, nir_ssa_def *index, unsigned max)
{
   if (util_is_power_of_two_or_zero(max))
      return nir_iand_imm(b, index, max - 1);
   else {
      nir_ssa_def *clamp = nir_imm_int(b, max - 1);
      nir_ssa_def *cond = nir_uge(b, clamp, index);
      return nir_bcsel(b, cond, index, clamp);
   }
}

static nir_ssa_def *load_ubo_desc(nir_builder *b, nir_ssa_def *index,
                                  struct lower_resource_state *s)
{
   struct si_shader_selector *sel = s->shader->selector;

   nir_ssa_def *addr = ac_nir_load_arg(b, &s->args->ac, s->args->const_and_shader_buffers);

   if (sel->info.base.num_ubos == 1 && sel->info.base.num_ssbos == 0)
      return load_ubo_desc_fast_path(b, addr, sel);

   index = clamp_index(b, index, sel->info.base.num_ubos);
   index = nir_iadd_imm(b, index, SI_NUM_SHADER_BUFFERS);

   nir_ssa_def *offset = nir_ishl_imm(b, index, 4);
   return nir_load_smem_amd(b, 4, addr, offset);
}

static nir_ssa_def *load_ssbo_desc(nir_builder *b, nir_src *index,
                                   struct lower_resource_state *s)
{
   struct si_shader_selector *sel = s->shader->selector;

   /* Fast path if the shader buffer is in user SGPRs. */
   if (nir_src_is_const(*index)) {
      unsigned slot = nir_src_as_uint(*index);
      if (slot < sel->cs_num_shaderbufs_in_user_sgprs)
         return ac_nir_load_arg(b, &s->args->ac, s->args->cs_shaderbuf[slot]);
   }

   nir_ssa_def *addr = ac_nir_load_arg(b, &s->args->ac, s->args->const_and_shader_buffers);
   nir_ssa_def *slot = clamp_index(b, index->ssa, sel->info.base.num_ssbos);
   slot = nir_isub(b, nir_imm_int(b, SI_NUM_SHADER_BUFFERS - 1), slot);

   nir_ssa_def *offset = nir_ishl_imm(b, slot, 4);
   return nir_load_smem_amd(b, 4, addr, offset);
}

static bool lower_resource_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                                     struct lower_resource_state *s)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_ubo: {
      assert(!(nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM));

      nir_ssa_def *desc = load_ubo_desc(b, intrin->src[0].ssa, s);
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[0], desc);
      break;
   }
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic_add:
   case nir_intrinsic_ssbo_atomic_imin:
   case nir_intrinsic_ssbo_atomic_umin:
   case nir_intrinsic_ssbo_atomic_fmin:
   case nir_intrinsic_ssbo_atomic_imax:
   case nir_intrinsic_ssbo_atomic_umax:
   case nir_intrinsic_ssbo_atomic_fmax:
   case nir_intrinsic_ssbo_atomic_and:
   case nir_intrinsic_ssbo_atomic_or:
   case nir_intrinsic_ssbo_atomic_xor:
   case nir_intrinsic_ssbo_atomic_exchange:
   case nir_intrinsic_ssbo_atomic_comp_swap: {
      assert(!(nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM));

      nir_ssa_def *desc = load_ssbo_desc(b, &intrin->src[0], s);
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[0], desc);
      break;
   }
   case nir_intrinsic_store_ssbo: {
      assert(!(nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM));

      nir_ssa_def *desc = load_ssbo_desc(b, &intrin->src[1], s);
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[1], desc);
      break;
   }
   case nir_intrinsic_get_ssbo_size: {
      assert(!(nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM));

      nir_ssa_def *desc = load_ssbo_desc(b, &intrin->src[0], s);
      nir_ssa_def *size = nir_channel(b, desc, 2);
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, size);
      nir_instr_remove(&intrin->instr);
      break;
   }
   default:
      return false;
   }

   return true;
}

static bool lower_resource_instr(nir_builder *b, nir_instr *instr, void *state)
{
   struct lower_resource_state *s = (struct lower_resource_state *)state;

   b->cursor = nir_before_instr(instr);

   switch (instr->type) {
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      return lower_resource_intrinsic(b, intrin, s);
   }
   default:
      return false;
   }
}

bool si_nir_lower_resource(nir_shader *nir, struct si_shader *shader,
                           struct si_shader_args *args)
{
   struct lower_resource_state state = {
      .shader = shader,
      .args = args,
   };

   return nir_shader_instructions_pass(nir, lower_resource_instr,
                                       nir_metadata_dominance | nir_metadata_block_index,
                                       &state);
}

/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_shader.c which is:
 * Copyright © 2019 Google LLC
 *
 * Also derived from anv_pipeline.c which is
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_private.h"

#include "nir.h"
#include "nir_builder.h"

struct apply_descriptors_ctx {
   const struct panvk_pipeline_layout *layout;
   bool has_img_access;
};

static void
get_resource_deref_binding(nir_deref_instr *deref,
                           uint32_t *set, uint32_t *binding,
                           uint32_t *index_imm, nir_ssa_def **index_ssa)
{
   *index_imm = 0;
   *index_ssa = NULL;

   if (deref->deref_type == nir_deref_type_array) {
      assert(deref->arr.index.is_ssa);
      if (index_imm != NULL && nir_src_is_const(deref->arr.index))
         *index_imm = nir_src_as_uint(deref->arr.index);
      else
         *index_ssa = deref->arr.index.ssa;

      deref = nir_deref_instr_parent(deref);
   }

   assert(deref->deref_type == nir_deref_type_var);
   nir_variable *var = deref->var;

   *set = var->data.descriptor_set;
   *binding = var->data.binding;
}


static bool
lower_tex(nir_builder *b, nir_tex_instr *tex,
          const struct apply_descriptors_ctx *ctx)
{
   bool progress = false;
   int sampler_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);

   b->cursor = nir_before_instr(&tex->instr);

   if (sampler_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[sampler_src_idx].src);
      nir_tex_instr_remove_src(tex, sampler_src_idx);

      uint32_t set, binding, index_imm;
      nir_ssa_def *index_ssa;
      get_resource_deref_binding(deref, &set, &binding,
                                 &index_imm, &index_ssa);

      const struct panvk_descriptor_set_binding_layout *bind_layout =
         &ctx->layout->sets[set].layout->bindings[binding];

      tex->sampler_index = ctx->layout->sets[set].sampler_offset +
                           bind_layout->sampler_idx + index_imm;

      if (index_ssa != NULL) {
         nir_tex_instr_add_src(tex, nir_tex_src_sampler_offset,
                               nir_src_for_ssa(index_ssa));
      }
      progress = true;
   }

   int tex_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (tex_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_src_idx].src);
      nir_tex_instr_remove_src(tex, tex_src_idx);

      uint32_t set, binding, index_imm;
      nir_ssa_def *index_ssa;
      get_resource_deref_binding(deref, &set, &binding,
                                 &index_imm, &index_ssa);

      const struct panvk_descriptor_set_binding_layout *bind_layout =
         &ctx->layout->sets[set].layout->bindings[binding];

      tex->texture_index = ctx->layout->sets[set].tex_offset +
                           bind_layout->tex_idx + index_imm;

      if (index_ssa != NULL) {
         nir_tex_instr_add_src(tex, nir_tex_src_texture_offset,
                               nir_src_for_ssa(index_ssa));
      }
      progress = true;
   }

   return progress;
}

static void
lower_vulkan_resource_index(nir_builder *b, nir_intrinsic_instr *intr,
                            const struct apply_descriptors_ctx *ctx)
{
   nir_ssa_def *vulkan_idx = intr->src[0].ssa;

   unsigned set = nir_intrinsic_desc_set(intr);
   unsigned binding = nir_intrinsic_binding(intr);
   struct panvk_descriptor_set_layout *set_layout = ctx->layout->sets[set].layout;
   struct panvk_descriptor_set_binding_layout *binding_layout =
      &set_layout->bindings[binding];
   unsigned base;

   switch (binding_layout->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      base = PANVK_NUM_BUILTIN_UBOS +
             ctx->layout->sets[set].ubo_offset +
             binding_layout->ubo_idx;
      break;
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      base = PANVK_NUM_BUILTIN_UBOS +
             ctx->layout->sets[set].dyn_ubo_offset +
             ctx->layout->num_ubos +
             binding_layout->dyn_ubo_idx;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      base = binding_layout->ssbo_idx + ctx->layout->sets[set].ssbo_offset;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      base = binding_layout->dyn_ssbo_idx + ctx->layout->num_ssbos +
             ctx->layout->sets[set].dyn_ssbo_offset;
      break;
   default:
      unreachable("Invalid descriptor type");
      break;
   }

   b->cursor = nir_before_instr(&intr->instr);
   nir_ssa_def *idx = nir_iadd(b, nir_imm_int(b, base), vulkan_idx);
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, idx);
   nir_instr_remove(&intr->instr);
}

static void
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin)
{
   /* Loading the descriptor happens as part of the load/store instruction so
    * this is a no-op.
    */
   b->cursor = nir_before_instr(&intrin->instr);
   nir_ssa_def *val = nir_vec2(b, intrin->src[0].ssa, nir_imm_int(b, 0));
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, val);
   nir_instr_remove(&intrin->instr);
}

static nir_ssa_def *
get_img_index(nir_builder *b, nir_deref_instr *deref,
              const struct apply_descriptors_ctx *ctx)
{
   uint32_t set, binding, index_imm;
   nir_ssa_def *index_ssa;
   get_resource_deref_binding(deref, &set, &binding, &index_imm, &index_ssa);

   const struct panvk_descriptor_set_binding_layout *bind_layout =
      &ctx->layout->sets[set].layout->bindings[binding];
   assert(bind_layout->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
          bind_layout->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
          bind_layout->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);

   unsigned img_offset = ctx->layout->sets[set].img_offset +
                         bind_layout->img_idx;

   if (index_ssa == NULL) {
      return nir_imm_int(b, img_offset + index_imm);
   } else {
      assert(index_imm == 0);
      return nir_iadd_imm(b, index_ssa, img_offset);
   }
}

static bool
lower_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                struct apply_descriptors_ctx *ctx)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      lower_vulkan_resource_index(b, intr, ctx);
      return true;
   case nir_intrinsic_load_vulkan_descriptor:
      lower_load_vulkan_descriptor(b, intr);
      return true;
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
   case nir_intrinsic_image_deref_atomic_fadd:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);

      b->cursor = nir_before_instr(&intr->instr);
      nir_rewrite_image_intrinsic(intr, get_img_index(b, deref, ctx), false);
      ctx->has_img_access = true;
      return true;
   }
   default:
      return false;
   }

}

static bool
lower_descriptors_instr(nir_builder *b,
                        nir_instr *instr,
                        void *data)
{
   struct apply_descriptors_ctx *ctx = data;

   switch (instr->type) {
   case nir_instr_type_tex:
      return lower_tex(b, nir_instr_as_tex(instr), ctx);
   case nir_instr_type_intrinsic:
      return lower_intrinsic(b, nir_instr_as_intrinsic(instr), ctx);
   default:
      return false;
   }
}

bool
panvk_per_arch(nir_lower_descriptors)(nir_shader *nir,
                                      struct panvk_device *dev,
                                      const struct panvk_pipeline_layout *layout,
                                      bool *has_img_access_out)
{
   struct apply_descriptors_ctx ctx = {
      .layout = layout,
   };

   bool progress = nir_shader_instructions_pass(nir, lower_descriptors_instr,
                                                nir_metadata_block_index |
                                                nir_metadata_dominance,
                                                (void *)&ctx);
   if (has_img_access_out)
      *has_img_access_out = ctx.has_img_access;

   return progress;
}

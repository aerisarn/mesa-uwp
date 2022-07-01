/* SPDX-License-Identifier: MIT */

#include "nir.h"
#include "nir_builder.h"

static nir_variable *
find_identical_inline_sampler(nir_shader *nir, nir_variable *sampler)
{
   nir_foreach_variable_with_modes(uniform, nir, nir_var_uniform) {
      if (!glsl_type_is_sampler(uniform->type) || !uniform->data.sampler.is_inline_sampler)
         continue;
      if (uniform->data.sampler.addressing_mode == sampler->data.sampler.addressing_mode &&
          uniform->data.sampler.normalized_coordinates == sampler->data.sampler.normalized_coordinates &&
          uniform->data.sampler.filter_mode == sampler->data.sampler.filter_mode)
         return uniform;
   }
   unreachable("Should have at least found the input sampler");
}

static bool
nir_dedup_inline_samplers_instr(nir_builder *b,
                                    nir_instr *instr,
                                    void *cb_data)
{
   nir_shader *nir = cb_data;
   if (instr->type != nir_instr_type_tex)
      return false;

   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int sampler_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (sampler_idx == -1)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(tex->src[sampler_idx].src);
   nir_variable *sampler = nir_deref_instr_get_variable(deref);
   if (!sampler)
      return false;

   assert(sampler->data.mode == nir_var_uniform);

   if (!sampler->data.sampler.is_inline_sampler)
      return false;

   nir_variable *replacement = find_identical_inline_sampler(nir, sampler);
   if (replacement == sampler)
      return false;

   b->cursor = nir_before_instr(&tex->instr);
   nir_deref_instr *replacement_deref = nir_build_deref_var(b, replacement);
   nir_instr_rewrite_src(&tex->instr, &tex->src[sampler_idx].src,
                         nir_src_for_ssa(&replacement_deref->dest.ssa));
   nir_deref_instr_remove_if_unused(deref);

   return true;
}

bool
nir_dedup_inline_samplers(nir_shader *nir)
{
   return nir_shader_instructions_pass(nir,
                                       nir_dedup_inline_samplers_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       nir);
}

bool
nir_lower_cl_images(nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   ASSERTED int last_loc = -1;
   int num_rd_images = 0, num_wr_images = 0;
   nir_foreach_image_variable(var, shader) {
      /* Assume they come in order */
      assert(var->data.location > last_loc);
      last_loc = var->data.location;

      if (var->data.access & ACCESS_NON_WRITEABLE)
         var->data.driver_location = num_rd_images++;
      else
         var->data.driver_location = num_wr_images++;
   }
   shader->info.num_textures = num_rd_images;
   BITSET_ZERO(shader->info.textures_used);
   if (num_rd_images)
      BITSET_SET_RANGE(shader->info.textures_used, 0, num_rd_images - 1);

   BITSET_ZERO(shader->info.images_used);
   if (num_wr_images)
      BITSET_SET_RANGE(shader->info.images_used, 0, num_wr_images - 1);
   shader->info.num_images = num_wr_images;

   last_loc = -1;
   int num_samplers = 0;
   nir_foreach_uniform_variable(var, shader) {
      if (var->type == glsl_bare_sampler_type()) {
         /* Assume they come in order */
         assert(var->data.location > last_loc);
         last_loc = var->data.location;
         var->data.driver_location = num_samplers++;
      } else {
         /* CL shouldn't have any sampled images */
         assert(!glsl_type_is_sampler(var->type));
      }
   }
   BITSET_ZERO(shader->info.samplers_used);
   if (num_samplers)
      BITSET_SET_RANGE(shader->info.samplers_used, 0, num_samplers - 1);

   nir_builder b;
   nir_builder_init(&b, impl);

   bool progress = false;
   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_deref: {
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type != nir_deref_type_var)
               break;

            if (!glsl_type_is_image(deref->type) &&
                !glsl_type_is_sampler(deref->type))
               break;

            b.cursor = nir_instr_remove(&deref->instr);
            nir_ssa_def *loc =
               nir_imm_intN_t(&b, deref->var->data.driver_location,
                                  deref->dest.ssa.bit_size);
            nir_ssa_def_rewrite_uses(&deref->dest.ssa, loc);
            progress = true;
            break;
         }

         case nir_instr_type_tex: {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            unsigned count = 0;
            for (unsigned i = 0; i < tex->num_srcs; i++) {
               if (tex->src[i].src_type == nir_tex_src_texture_deref ||
                   tex->src[i].src_type == nir_tex_src_sampler_deref) {
                  nir_deref_instr *deref = nir_src_as_deref(tex->src[i].src);
                  if (deref->deref_type == nir_deref_type_var) {
                     /* In this case, we know the actual variable */
                     if (tex->src[i].src_type == nir_tex_src_texture_deref)
                        tex->texture_index = deref->var->data.driver_location;
                     else
                        tex->sampler_index = deref->var->data.driver_location;
                     /* This source gets discarded */
                     nir_instr_rewrite_src(&tex->instr, &tex->src[i].src,
                                           NIR_SRC_INIT);
                     continue;
                  } else {
                     assert(tex->src[i].src.is_ssa);
                     b.cursor = nir_before_instr(&tex->instr);
                     /* Back-ends expect a 32-bit thing, not 64-bit */
                     nir_ssa_def *offset = nir_u2u32(&b, tex->src[i].src.ssa);
                     if (tex->src[i].src_type == nir_tex_src_texture_deref)
                        tex->src[count].src_type = nir_tex_src_texture_offset;
                     else
                        tex->src[count].src_type = nir_tex_src_sampler_offset;
                     nir_instr_rewrite_src(&tex->instr, &tex->src[count].src,
                                           nir_src_for_ssa(offset));
                  }
               } else {
                  /* If we've removed a source, move this one down */
                  if (count != i) {
                     assert(count < i);
                     tex->src[count].src_type = tex->src[i].src_type;
                     nir_instr_move_src(&tex->instr, &tex->src[count].src,
                                        &tex->src[i].src);
                  }
               }
               count++;
            }
            tex->num_srcs = count;
            progress = true;
            break;
         }

         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_image_deref_load:
            case nir_intrinsic_image_deref_store:
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
            case nir_intrinsic_image_deref_atomic_inc_wrap:
            case nir_intrinsic_image_deref_atomic_dec_wrap:
            case nir_intrinsic_image_deref_size:
            case nir_intrinsic_image_deref_samples: {
               assert(intrin->src[0].is_ssa);
               b.cursor = nir_before_instr(&intrin->instr);
               /* Back-ends expect a 32-bit thing, not 64-bit */
               nir_ssa_def *offset = nir_u2u32(&b, intrin->src[0].ssa);
               nir_rewrite_image_intrinsic(intrin, offset, false);
               progress = true;
               break;
            }

            default:
               break;
            }
            break;
         }

         default:
            break;
         }
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

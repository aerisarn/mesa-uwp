/*
 * Copyright © 2020 Valve Corporation
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
 */
#include "nir.h"
#include "nir_builder.h"
#include "radv_private.h"
#include "radv_shader.h"
#include "radv_shader_args.h"

typedef struct {
   enum chip_class chip_class;
   uint32_t address32_hi;

   const struct radv_shader_args *args;
   const struct radv_shader_info *info;
   const struct radv_pipeline_layout *pipeline_layout;
} apply_layout_state;

static nir_ssa_def *
get_scalar_arg(nir_builder *b, unsigned size, struct ac_arg arg)
{
   return nir_load_scalar_arg_amd(b, size, .base = arg.arg_index);
}

static nir_ssa_def *
convert_pointer_to_64_bit(nir_builder *b, apply_layout_state *state, nir_ssa_def *ptr)
{
   return nir_pack_64_2x32_split(b, ptr, nir_imm_int(b, state->address32_hi));
}

static nir_ssa_def *
load_desc_ptr(nir_builder *b, apply_layout_state *state, unsigned set)
{
   const struct radv_userdata_locations *user_sgprs_locs = &state->info->user_sgprs_locs;
   if (user_sgprs_locs->shader_data[AC_UD_INDIRECT_DESCRIPTOR_SETS].sgpr_idx != -1) {
      nir_ssa_def *addr = get_scalar_arg(b, 1, state->args->descriptor_sets[0]);
      addr = convert_pointer_to_64_bit(b, state, addr);
      return nir_load_smem_amd(b, 1, addr, nir_imm_int(b, set * 4));
   }

   assert(state->args->descriptor_sets[set].used);
   return get_scalar_arg(b, 1, state->args->descriptor_sets[set]);
}

static void
visit_vulkan_resource_index(nir_builder *b, apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   unsigned desc_set = nir_intrinsic_desc_set(intrin);
   unsigned binding = nir_intrinsic_binding(intrin);
   struct radv_descriptor_set_layout *layout = state->pipeline_layout->set[desc_set].layout;
   unsigned offset = layout->binding[binding].offset;
   unsigned stride;

   nir_ssa_def *set_ptr;
   if (layout->binding[binding].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
       layout->binding[binding].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
      unsigned idx = state->pipeline_layout->set[desc_set].dynamic_offset_start +
                     layout->binding[binding].dynamic_offset_offset;
      set_ptr = get_scalar_arg(b, 1, state->args->ac.push_constants);
      offset = state->pipeline_layout->push_constant_size + idx * 16;
      stride = 16;
   } else {
      set_ptr = load_desc_ptr(b, state, desc_set);
      stride = layout->binding[binding].size;
   }

   nir_ssa_def *binding_ptr = nir_imul_imm(b, intrin->src[0].ssa, stride);
   nir_instr_as_alu(binding_ptr->parent_instr)->no_unsigned_wrap = true;

   binding_ptr = nir_iadd_imm(b, binding_ptr, offset);
   nir_instr_as_alu(binding_ptr->parent_instr)->no_unsigned_wrap = true;

   if (layout->binding[binding].type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
      assert(stride == 16);
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_pack_64_2x32_split(b, set_ptr, binding_ptr));
   } else {
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                               nir_vec3(b, set_ptr, binding_ptr, nir_imm_int(b, stride)));
   }
   nir_instr_remove(&intrin->instr);
}

static void
visit_vulkan_resource_reindex(nir_builder *b, apply_layout_state *state,
                              nir_intrinsic_instr *intrin)
{
   VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);
   if (desc_type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
      nir_ssa_def *set_ptr = nir_unpack_64_2x32_split_x(b, intrin->src[0].ssa);
      nir_ssa_def *binding_ptr = nir_unpack_64_2x32_split_y(b, intrin->src[0].ssa);

      nir_ssa_def *index = nir_imul_imm(b, intrin->src[1].ssa, 16);
      nir_instr_as_alu(index->parent_instr)->no_unsigned_wrap = true;

      binding_ptr = nir_iadd_nuw(b, binding_ptr, index);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_pack_64_2x32_split(b, set_ptr, binding_ptr));
   } else {
      assert(desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
             desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

      nir_ssa_def *binding_ptr = nir_channel(b, intrin->src[0].ssa, 1);
      nir_ssa_def *stride = nir_channel(b, intrin->src[0].ssa, 2);

      nir_ssa_def *index = nir_imul(b, intrin->src[1].ssa, stride);
      nir_instr_as_alu(index->parent_instr)->no_unsigned_wrap = true;

      binding_ptr = nir_iadd_nuw(b, binding_ptr, index);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                               nir_vector_insert_imm(b, intrin->src[0].ssa, binding_ptr, 1));
   }
   nir_instr_remove(&intrin->instr);
}

static void
visit_load_vulkan_descriptor(nir_builder *b, apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   if (nir_intrinsic_desc_type(intrin) == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
      nir_ssa_def *addr = convert_pointer_to_64_bit(
         b, state,
         nir_iadd(b, nir_unpack_64_2x32_split_x(b, intrin->src[0].ssa),
                     nir_unpack_64_2x32_split_y(b, intrin->src[0].ssa)));
      nir_ssa_def *desc = nir_build_load_global(b, 1, 64, addr, .access = ACCESS_NON_WRITEABLE);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);
   } else {
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                               nir_vector_insert_imm(b, intrin->src[0].ssa, nir_imm_int(b, 0), 2));
   }
   nir_instr_remove(&intrin->instr);
}

static nir_ssa_def *
load_inline_buffer_descriptor(nir_builder *b, apply_layout_state *state, nir_ssa_def *rsrc)
{
   uint32_t desc_type =
      S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
      S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);
   if (state->chip_class >= GFX10) {
      desc_type |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_FLOAT) |
                   S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW) | S_008F0C_RESOURCE_LEVEL(1);
   } else {
      desc_type |= S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
                   S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
   }

   return nir_vec4(b, rsrc, nir_imm_int(b, S_008F04_BASE_ADDRESS_HI(state->address32_hi)),
                   nir_imm_int(b, 0xffffffff), nir_imm_int(b, desc_type));
}

static nir_ssa_def *
load_buffer_descriptor(nir_builder *b, apply_layout_state *state, nir_ssa_def *rsrc)
{
   nir_binding binding = nir_chase_binding(nir_src_for_ssa(rsrc));

   /* If binding.success=false, then this is a variable pointer, which we don't support with
    * VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT.
    */
   if (binding.success) {
      struct radv_descriptor_set_layout *layout =
         state->pipeline_layout->set[binding.desc_set].layout;
      if (layout->binding[binding.binding].type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
         rsrc = nir_iadd(b, nir_channel(b, rsrc, 0), nir_channel(b, rsrc, 1));
         return load_inline_buffer_descriptor(b, state, rsrc);
      }
   }

   return rsrc;
}

static void
apply_layout_to_intrin(nir_builder *b, apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *rsrc;
   switch (intrin->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      visit_vulkan_resource_index(b, state, intrin);
      break;
   case nir_intrinsic_vulkan_resource_reindex:
      visit_vulkan_resource_reindex(b, state, intrin);
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      visit_load_vulkan_descriptor(b, state, intrin);
      break;
   case nir_intrinsic_load_ubo:
      rsrc = load_buffer_descriptor(b, state, intrin->src[0].ssa);
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[0], rsrc);
      break;
   default:
      break;
   }
}

void
radv_nir_apply_pipeline_layout(nir_shader *shader, struct radv_device *device,
                               const struct radv_pipeline_layout *layout,
                               const struct radv_shader_info *info,
                               const struct radv_shader_args *args)
{
   apply_layout_state state = {
      .chip_class = device->physical_device->rad_info.chip_class,
      .address32_hi = device->physical_device->rad_info.address32_hi,
      .args = args,
      .info = info,
      .pipeline_layout = layout,
   };

   nir_builder b;

   nir_foreach_function (function, shader) {
      if (!function->impl)
         continue;

      nir_builder_init(&b, function->impl);

      /* Iterate in reverse so load_ubo lowering can look at
       * the vulkan_resource_index to tell if it's an inline
       * ubo.
       */
      nir_foreach_block_reverse (block, function->impl) {
         nir_foreach_instr_reverse_safe (instr, block) {
            if (instr->type == nir_instr_type_intrinsic)
               apply_layout_to_intrin(&b, &state, nir_instr_as_intrinsic(instr));
         }
      }

      nir_metadata_preserve(function->impl, nir_metadata_block_index | nir_metadata_dominance);
   }
}

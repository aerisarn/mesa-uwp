#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_set.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_shader.h"
#include "vk_pipeline.h"

#include "nir_builder.h"
#include "nir_deref.h"

struct lower_descriptors_ctx {
   const struct vk_pipeline_layout *layout;
   bool clamp_desc_array_bounds;
   nir_address_format desc_addr_format;
   nir_address_format ubo_addr_format;
   nir_address_format ssbo_addr_format;
};

static nir_ssa_def *
load_descriptor_set_addr(nir_builder *b, uint32_t set,
                         UNUSED const struct lower_descriptors_ctx *ctx)
{
   uint32_t set_addr_offset =
      nvk_root_descriptor_offset(sets) + set * sizeof(uint64_t);

   return nir_load_ubo(b, 1, 64, nir_imm_int(b, 0),
                       nir_imm_int(b, set_addr_offset),
                       .align_mul = 8, .align_offset = 0, .range = ~0);
}

static nir_ssa_def *
load_descriptor(nir_builder *b, unsigned num_components, unsigned bit_size,
                uint32_t set, uint32_t binding, nir_ssa_def *index,
                unsigned offset_B, const struct lower_descriptors_ctx *ctx)
{
   assert(set < NVK_MAX_SETS);

   const struct vk_pipeline_layout *layout = ctx->layout;
   const struct nvk_descriptor_set_layout *set_layout =
         vk_to_nvk_descriptor_set_layout(layout->set_layouts[set]);
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set_layout->binding[binding];

   if (ctx->clamp_desc_array_bounds)
      index = nir_umin(b, index, nir_imm_int(b, binding_layout->array_size - 1));

   switch (binding_layout->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
      /* Get the index in the root descriptor table dynamic_buffers array. */
      uint8_t dynamic_buffer_start =
         nvk_descriptor_set_layout_dynbuf_start(layout, set);

      index = nir_iadd_imm(b, index,
                           dynamic_buffer_start +
                           binding_layout->dynamic_buffer_index);

      nir_ssa_def *root_desc_offset =
         nir_iadd_imm(b, nir_imul_imm(b, index, sizeof(struct nvk_buffer_address)),
                      nvk_root_descriptor_offset(dynamic_buffers));

      return nir_load_ubo(b, num_components, bit_size,
                          nir_imm_int(b, 0), root_desc_offset,
                          .align_mul = 16, .align_offset = 0, .range = ~0);
   }

   case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
      nir_ssa_def *base_addr =
         nir_iadd_imm(b, load_descriptor_set_addr(b, set, ctx),
                          binding_layout->offset);

      assert(binding_layout->stride == 1);
      const uint32_t binding_size = binding_layout->array_size;

      /* Convert it to nir_address_format_64bit_bounded_global */
      assert(num_components == 4 && bit_size == 32);
      return nir_vec4(b, nir_unpack_64_2x32_split_x(b, base_addr),
                         nir_unpack_64_2x32_split_y(b, base_addr),
                         nir_imm_int(b, binding_size),
                         nir_imm_int(b, 0));
   }

   default: {
      assert(binding_layout->stride > 0);
      nir_ssa_def *desc_ubo_offset =
         nir_iadd_imm(b, nir_imul_imm(b, index, binding_layout->stride),
                         binding_layout->offset + offset_B);

      unsigned desc_align = (1 << (ffs(binding_layout->stride) - 1));
      desc_align = MIN2(desc_align, 16);

      nir_ssa_def *set_addr = load_descriptor_set_addr(b, set, ctx);
      return nir_load_global_constant_offset(b, num_components, bit_size,
                                             set_addr, desc_ubo_offset,
                                             .align_mul = desc_align,
                                             .align_offset = 0);
   }
   }
}

static nir_ssa_def *
load_descriptor_for_idx_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                               const struct lower_descriptors_ctx *ctx)
{
   nir_ssa_def *index = nir_imm_int(b, 0);

   while (intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
      index = nir_iadd(b, index, nir_ssa_for_src(b, intrin->src[1], 1));
      intrin = nir_src_as_intrinsic(intrin->src[0]);
   }

   assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_index);
   uint32_t set = nir_intrinsic_desc_set(intrin);
   uint32_t binding = nir_intrinsic_binding(intrin);
   index = nir_iadd(b, index, nir_ssa_for_src(b, intrin->src[0], 1));

   return load_descriptor(b, 4, 32, set, binding, index, 0, ctx);
}

static bool
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin,
                             const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_intrinsic_instr *idx_intrin = nir_src_as_intrinsic(intrin->src[0]);
   nir_ssa_def *desc = load_descriptor_for_idx_intrin(b, idx_intrin, ctx);

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);

   return true;
}

static bool
lower_num_workgroups(nir_builder *b, nir_intrinsic_instr *load,
                     const struct lower_descriptors_ctx *ctx)
{
   const uint32_t root_table_offset =
      nvk_root_descriptor_offset(cs.group_count);

   b->cursor = nir_instr_remove(&load->instr);

   nir_ssa_def *val = nir_load_ubo(b, 3, 32,
                                   nir_imm_int(b, 0), /* Root table */
                                   nir_imm_int(b, root_table_offset),
                                   .align_mul = 4,
                                   .align_offset = 0,
                                   .range = root_table_offset + 3 * 4);

   nir_ssa_def_rewrite_uses(&load->dest.ssa, val);

   return true;
}

static bool
lower_load_base_workgroup_id(nir_builder *b, nir_intrinsic_instr *load,
                             const struct lower_descriptors_ctx *ctx)
{
   const uint32_t root_table_offset =
      nvk_root_descriptor_offset(cs.base_group);

   b->cursor = nir_instr_remove(&load->instr);

   nir_ssa_def *val = nir_load_ubo(b, 3, 32,
                                   nir_imm_int(b, 0),
                                   nir_imm_int(b, root_table_offset),
                                   .align_mul = 4,
                                   .align_offset = 0,
                                   .range = root_table_offset + 3 * 4);

   nir_ssa_def_rewrite_uses(&load->dest.ssa, val);

   return true;
}

static bool
lower_load_push_constant(nir_builder *b, nir_intrinsic_instr *load,
                         const struct lower_descriptors_ctx *ctx)
{
   const uint32_t push_region_offset =
      nvk_root_descriptor_offset(push);
   const uint32_t base = nir_intrinsic_base(load);

   b->cursor = nir_before_instr(&load->instr);

   nir_ssa_def *offset = nir_iadd_imm(b, load->src[0].ssa,
                                         push_region_offset + base);

   nir_ssa_def *val =
      nir_load_ubo(b, load->dest.ssa.num_components, load->dest.ssa.bit_size,
                   nir_imm_int(b, 0), offset,
                   .align_mul = load->dest.ssa.bit_size / 8,
                   .align_offset = 0,
                   .range = push_region_offset + base +
                            nir_intrinsic_range(load));

   nir_ssa_def_rewrite_uses(&load->dest.ssa, val);

   return true;
}

static bool
lower_load_view_index(nir_builder *b, nir_intrinsic_instr *load,
                      const struct lower_descriptors_ctx *ctx)
{
   const uint32_t root_table_offset =
      nvk_root_descriptor_offset(draw.view_index);

   b->cursor = nir_instr_remove(&load->instr);

   nir_ssa_def *val = nir_load_ubo(b, 1, 32,
                                   nir_imm_int(b, 0),
                                   nir_imm_int(b, root_table_offset),
                                   .align_mul = 4,
                                   .align_offset = 0,
                                   .range = root_table_offset + 4);

   nir_ssa_def_rewrite_uses(&load->dest.ssa, val);

   return true;
}

static void
get_resource_deref_binding(nir_builder *b, nir_deref_instr *deref,
                           uint32_t *set, uint32_t *binding,
                           nir_ssa_def **index)
{
   if (deref->deref_type == nir_deref_type_array) {
      *index = deref->arr.index.ssa;
      deref = nir_deref_instr_parent(deref);
   } else {
      *index = nir_imm_int(b, 0);
   }

   assert(deref->deref_type == nir_deref_type_var);
   nir_variable *var = deref->var;

   *set = var->data.descriptor_set;
   *binding = var->data.binding;
}

static nir_ssa_def *
load_resource_deref_desc(nir_builder *b, 
                         unsigned num_components, unsigned bit_size,
                         nir_deref_instr *deref, unsigned offset_B,
                         const struct lower_descriptors_ctx *ctx)
{
   uint32_t set, binding;
   nir_ssa_def *index;
   get_resource_deref_binding(b, deref, &set, &binding, &index);
   return load_descriptor(b, num_components, bit_size,
                          set, binding, index, offset_B, ctx);
}

static bool
lower_image_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                   const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&intrin->instr);
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_ssa_def *desc = load_resource_deref_desc(b, 1, 32, deref, 0, ctx);
   nir_rewrite_image_intrinsic(intrin, desc, true);

   /* We treat 3D images as 2D arrays */
   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_3D) {
      assert(!nir_intrinsic_image_array(intrin));
      nir_intrinsic_set_image_dim(intrin, GLSL_SAMPLER_DIM_2D);
      nir_intrinsic_set_image_array(intrin, true);
   }

   /* We don't support ReadWithoutFormat yet */
   if (intrin->intrinsic == nir_intrinsic_image_deref_load)
      assert(nir_intrinsic_format(intrin) != PIPE_FORMAT_NONE);

   return true;
}

static bool
lower_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
             const struct lower_descriptors_ctx *ctx)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_vulkan_descriptor:
      return lower_load_vulkan_descriptor(b, intrin, ctx);

   case nir_intrinsic_load_workgroup_size:
      unreachable("Should have been lowered by nir_lower_cs_intrinsics()");

   case nir_intrinsic_load_num_workgroups:
      return lower_num_workgroups(b, intrin, ctx);

   case nir_intrinsic_load_base_workgroup_id:
      return lower_load_base_workgroup_id(b, intrin, ctx);

   case nir_intrinsic_load_push_constant:
      return lower_load_push_constant(b, intrin, ctx);

   case nir_intrinsic_load_view_index:
      return lower_load_view_index(b, intrin, ctx);

   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   case nir_intrinsic_image_deref_load_param_intel:
   case nir_intrinsic_image_deref_load_raw_intel:
   case nir_intrinsic_image_deref_store_raw_intel:
      return lower_image_intrin(b, intrin, ctx);

   default:
      return false;
   }
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex,
          const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&tex->instr);

   const int texture_src_idx =
      nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   const int sampler_src_idx =
      nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (texture_src_idx < 0) {
      assert(sampler_src_idx < 0);
      return false;
   }

   nir_deref_instr *texture = nir_src_as_deref(tex->src[texture_src_idx].src);
   nir_deref_instr *sampler = sampler_src_idx < 0 ? NULL :
                              nir_src_as_deref(tex->src[sampler_src_idx].src);
   assert(texture);

   const int plane_src_idx =
      nir_tex_instr_src_index(tex, nir_tex_src_plane);

   uint32_t plane = (plane_src_idx < 0) ? 0 : 
      nir_src_as_uint(tex->src[plane_src_idx].src);
   uint64_t plane_offset_B = plane * sizeof(struct nvk_image_descriptor);

   nir_ssa_def *combined_handle;
   if (texture == sampler) {
      combined_handle = load_resource_deref_desc(b, 1, 32, texture, plane_offset_B, ctx);
   } else {
      nir_ssa_def *texture_desc =
         load_resource_deref_desc(b, 1, 32, texture, plane_offset_B, ctx);
      combined_handle = nir_iand_imm(b, texture_desc,
                                     NVK_IMAGE_DESCRIPTOR_IMAGE_INDEX_MASK);

      if (sampler != NULL) {
         nir_ssa_def *sampler_desc =
            load_resource_deref_desc(b, 1, 32, sampler, plane_offset_B, ctx);
         nir_ssa_def *sampler_index =
            nir_iand_imm(b, sampler_desc,
                         NVK_IMAGE_DESCRIPTOR_SAMPLER_INDEX_MASK);
         combined_handle = nir_ior(b, combined_handle, sampler_index);
      }
   }

   /* TODO: The nv50 back-end assumes it's 64-bit because of GL */
   combined_handle = nir_u2u64(b, combined_handle);

   /* TODO: The nv50 back-end assumes it gets handles both places, even for
    * texelFetch.
    */
   nir_instr_rewrite_src_ssa(&tex->instr,
                             &tex->src[texture_src_idx].src,
                             combined_handle);
   tex->src[texture_src_idx].src_type = nir_tex_src_texture_handle;

   if (sampler_src_idx < 0) {
      nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle,
                            nir_src_for_ssa(combined_handle));
   } else {
      nir_instr_rewrite_src_ssa(&tex->instr,
                                &tex->src[sampler_src_idx].src,
                                combined_handle);
      tex->src[sampler_src_idx].src_type = nir_tex_src_sampler_handle;
   }

   return true;
}

static bool
lower_descriptors_instr(nir_builder *b, nir_instr *instr,
                        void *_data)
{
   const struct lower_descriptors_ctx *ctx = _data;

   switch (instr->type) {
   case nir_instr_type_tex:
      return lower_tex(b, nir_instr_as_tex(instr), ctx);
   case nir_instr_type_intrinsic:
      return lower_intrin(b, nir_instr_as_intrinsic(instr), ctx);
   default:
      return false;
   }
}

bool
nvk_nir_lower_descriptors(nir_shader *nir,
                          const struct vk_pipeline_robustness_state *rs,
                          const struct vk_pipeline_layout *layout)
{
   struct lower_descriptors_ctx ctx = {
      .layout = layout,
      .clamp_desc_array_bounds =
         rs->storage_buffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT ||
         rs->uniform_buffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT ||
         rs->images != VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT,
      .desc_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nvk_buffer_addr_format(rs->storage_buffers),
      .ubo_addr_format = nvk_buffer_addr_format(rs->uniform_buffers),
   };
   return nir_shader_instructions_pass(nir, lower_descriptors_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       (void *)&ctx);
}

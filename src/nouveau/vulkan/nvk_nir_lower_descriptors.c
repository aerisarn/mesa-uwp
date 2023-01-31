#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_set.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_shader.h"

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
      offsetof(struct nvk_root_descriptor_table, sets) + set * sizeof(uint64_t);

   return nir_load_ubo(b, 1, 64, nir_imm_int(b, 0),
                       nir_imm_int(b, set_addr_offset),
                       .align_mul = 8, .align_offset = 0, .range = ~0);
}

static nir_ssa_def *
load_descriptor(nir_builder *b, unsigned num_components, unsigned bit_size,
                uint32_t set, uint32_t binding, nir_ssa_def *index,
                const struct lower_descriptors_ctx *ctx)
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
                      offsetof(struct nvk_root_descriptor_table, dynamic_buffers));

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
                         binding_layout->offset);

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

static bool
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin,
                             const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *index = nir_imm_int(b, 0);

   nir_intrinsic_instr *parent = nir_src_as_intrinsic(intrin->src[0]);
   while (parent->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
      index = nir_iadd(b, index, nir_ssa_for_src(b, intrin->src[1], 1));
      parent = nir_src_as_intrinsic(intrin->src[0]);
   }

   assert(parent->intrinsic == nir_intrinsic_vulkan_resource_index);
   uint32_t set = nir_intrinsic_desc_set(parent);
   uint32_t binding = nir_intrinsic_binding(parent);
   index = nir_iadd(b, index, nir_ssa_for_src(b, parent->src[0], 1));

   nir_ssa_def *desc = load_descriptor(b, 4, 32, set, binding, index, ctx);

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);

   return true;
}

static bool
lower_load_push_constant(nir_builder *b, nir_intrinsic_instr *load,
                         const struct lower_descriptors_ctx *ctx)
{
   const uint32_t push_region_offset =
      offsetof(struct nvk_root_descriptor_table, push);
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
load_resource_deref_desc(nir_builder *b, nir_deref_instr *deref,
                         unsigned desc_offset, unsigned num_components,
                         unsigned bit_size,
                         const struct lower_descriptors_ctx *ctx)
{
   uint32_t set, binding;
   nir_ssa_def *index;
   get_resource_deref_binding(b, deref, &set, &binding, &index);
   return load_descriptor(b, num_components, bit_size,
                          set, binding, index, ctx);
}

static bool
lower_image_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                   const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&intrin->instr);
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_ssa_def *desc = load_resource_deref_desc(b, deref, 0, 1, 32, ctx);
   nir_rewrite_image_intrinsic(intrin, desc, true);

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

   case nir_intrinsic_load_push_constant:
      return lower_load_push_constant(b, intrin, ctx);

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

   nir_ssa_def *combined_handle;
   if (texture == sampler) {
      combined_handle = load_resource_deref_desc(b, texture, 0, 1, 32, ctx);
   } else {
      nir_ssa_def *texture_desc =
         load_resource_deref_desc(b, texture, 0, 1, 32, ctx);
      combined_handle = nir_iand_imm(b, texture_desc,
                                     NVK_IMAGE_DESCRIPTOR_IMAGE_INDEX_MASK);

      if (sampler != NULL) {
         nir_ssa_def *sampler_desc =
            load_resource_deref_desc(b, sampler, 0, 1, 32, ctx);
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
                          const struct vk_pipeline_layout *layout,
                          bool robust_buffer_access)
{
   struct lower_descriptors_ctx ctx = {
      .layout = layout,
      .clamp_desc_array_bounds = robust_buffer_access,
      .desc_addr_format = nir_address_format_32bit_index_offset,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = robust_buffer_access ?
                          nir_address_format_64bit_bounded_global :
                          nir_address_format_64bit_global_32bit_offset,
   };
   return nir_shader_instructions_pass(nir, lower_descriptors_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       (void *)&ctx);
}

/*
 * Copyright © 2022 Collabora Ltd
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

#include "vk_meta_private.h"

#include "vk_command_buffer.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_pipeline.h"

#include "nir_builder.h"

struct vk_meta_blit_key {
   enum vk_meta_object_key_type key_type;
   enum glsl_sampler_dim dim;
   VkFormat dst_format;
   VkImageAspectFlags aspects;
};

static enum glsl_sampler_dim
vk_image_sampler_dim(const struct vk_image *image)
{
   switch (image->image_type) {
   case VK_IMAGE_TYPE_1D: return GLSL_SAMPLER_DIM_1D;
   case VK_IMAGE_TYPE_2D: return GLSL_SAMPLER_DIM_2D;
   case VK_IMAGE_TYPE_3D: return GLSL_SAMPLER_DIM_3D;
   default: unreachable("Invalid image type");
   }
}

enum blit_desc_binding {
   BLIT_DESC_BINDING_SAMPLER,
   BLIT_DESC_BINDING_COLOR,
   BLIT_DESC_BINDING_DEPTH,
   BLIT_DESC_BINDING_STENCIL,
};

static enum blit_desc_binding
aspect_to_tex_binding(VkImageAspectFlagBits aspect)
{
   switch (aspect) {
   case VK_IMAGE_ASPECT_COLOR_BIT: return BLIT_DESC_BINDING_COLOR;
   case VK_IMAGE_ASPECT_DEPTH_BIT: return BLIT_DESC_BINDING_DEPTH;
   case VK_IMAGE_ASPECT_STENCIL_BIT: return BLIT_DESC_BINDING_STENCIL;
   default: unreachable("Unsupported aspect");
   }
}

struct vk_meta_blit_push_data {
   float x_off, y_off, x_scale, y_scale;
   float z_off, z_scale;
   int32_t arr_delta, _pad;
};

static inline void
compute_off_scale(uint32_t src_level_size,
                  uint32_t src0, uint32_t src1,
                  uint32_t dst0, uint32_t dst1,
                  uint32_t *dst0_out, uint32_t *dst1_out,
                  float *off_out, float *scale_out)
{
   assert(src0 <= src_level_size && src1 <= src_level_size);

   if (dst0 < dst1) {
      *dst0_out = dst0;
      *dst1_out = dst1;
   } else {
      *dst0_out = dst1;
      *dst1_out = dst0;

      /* Flip the source region */
      uint32_t tmp = src0;
      src0 = src1;
      src1 = tmp;
   }

   double src_region_size = (double)src1 - (double)src0;
   assert(src_region_size != 0);

   double dst_region_size = (double)*dst1_out - (double)*dst0_out;
   assert(dst_region_size > 0);

   double src_offset = src0 / (double)src_level_size;
   double dst_scale = src_region_size / (src_level_size * dst_region_size);
   double dst_offset = (double)*dst0_out * dst_scale;

   *off_out = src_offset - dst_offset;
   *scale_out = dst_scale;
}

static inline nir_ssa_def *
load_struct_var(nir_builder *b, nir_variable *var, uint32_t field)
{
   nir_deref_instr *deref =
      nir_build_deref_struct(b, nir_build_deref_var(b, var), field);
   return nir_load_deref(b, deref);
}

static nir_shader *
build_blit_shader(const struct vk_meta_blit_key *key)
{
   nir_builder build = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                      NULL, "vk-meta-blit");
   nir_builder *b = &build;

   struct glsl_struct_field push_fields[] = {
      { .type = glsl_vec4_type(), .name = "xy_xform" },
      { .type = glsl_vec4_type(), .name = "z_xform" },
   };
   const struct glsl_type *push_iface_type =
      glsl_interface_type(push_fields, ARRAY_SIZE(push_fields),
                          GLSL_INTERFACE_PACKING_STD140,
                          false /* row_major */, "push");
   nir_variable *push = nir_variable_create(b->shader, nir_var_mem_push_const,
                                            push_iface_type, "push");

   nir_ssa_def *xy_xform = load_struct_var(b, push, 0);
   nir_ssa_def *xy_off = nir_channels(b, xy_xform, 3 << 0);
   nir_ssa_def *xy_scale = nir_channels(b, xy_xform, 3 << 2);

   nir_ssa_def *out_coord_xy = nir_load_frag_coord(b);
   out_coord_xy = nir_trim_vector(b, out_coord_xy, 2);
   nir_ssa_def *src_coord_xy = nir_ffma(b, out_coord_xy, xy_scale, xy_off);

   nir_ssa_def *z_xform = load_struct_var(b, push, 1);
   nir_ssa_def *out_layer = nir_load_layer_id(b);
   nir_ssa_def *src_coord;
   if (key->dim == GLSL_SAMPLER_DIM_3D) {
      nir_ssa_def *z_off = nir_channel(b, z_xform, 0);
      nir_ssa_def *z_scale = nir_channel(b, z_xform, 1);
      nir_ssa_def *out_coord_z = nir_fadd_imm(b, nir_u2f32(b, out_layer), 0.5);
      nir_ssa_def *src_coord_z = nir_ffma(b, out_coord_z, z_scale, z_off);
      src_coord = nir_vec3(b, nir_channel(b, src_coord_xy, 0),
                              nir_channel(b, src_coord_xy, 1),
                              src_coord_z);
   } else {
      nir_ssa_def *arr_delta = nir_channel(b, z_xform, 2);
      nir_ssa_def *in_layer = nir_iadd(b, out_layer, arr_delta);
      if (key->dim == GLSL_SAMPLER_DIM_1D) {
         src_coord = nir_vec2(b, nir_channel(b, src_coord_xy, 0),
                                 nir_u2f32(b, in_layer));
      } else {
         assert(key->dim == GLSL_SAMPLER_DIM_2D);
         src_coord = nir_vec3(b, nir_channel(b, src_coord_xy, 0),
                                 nir_channel(b, src_coord_xy, 1),
                                 nir_u2f32(b, in_layer));
      }
   }

   nir_variable *sampler = nir_variable_create(b->shader, nir_var_uniform,
                                               glsl_bare_sampler_type(), NULL);
   sampler->data.descriptor_set = 0;
   sampler->data.binding = BLIT_DESC_BINDING_SAMPLER;

   u_foreach_bit(a, key->aspects) {
      VkImageAspectFlagBits aspect = (1 << a);

      enum glsl_base_type base_type;
      unsigned out_location, out_comps;
      const char *tex_name, *out_name;
      switch (aspect) {
      case VK_IMAGE_ASPECT_COLOR_BIT:
         tex_name = "color_tex";
         if (vk_format_is_int(key->dst_format))
            base_type = GLSL_TYPE_INT;
         else if (vk_format_is_uint(key->dst_format))
            base_type = GLSL_TYPE_UINT;
         else
            base_type = GLSL_TYPE_FLOAT;
         out_name = "gl_FragData[0]";
         out_location = FRAG_RESULT_DATA0;
         out_comps = 4;
         break;
      case VK_IMAGE_ASPECT_DEPTH_BIT:
         tex_name = "depth_tex";
         base_type = GLSL_TYPE_FLOAT;
         out_name = "gl_FragDepth";
         out_location = FRAG_RESULT_DEPTH;
         out_comps = 1;
         break;
      case VK_IMAGE_ASPECT_STENCIL_BIT:
         tex_name = "stencil_tex";
         base_type = GLSL_TYPE_UINT;
         out_name = "gl_FragStencilRef";
         out_location = FRAG_RESULT_STENCIL;
         out_comps = 1;
         break;
      default:
         unreachable("Unsupported aspect");
      }

      const bool is_array = key->dim != GLSL_SAMPLER_DIM_3D;
      const struct glsl_type *texture_type =
         glsl_sampler_type(key->dim, false, is_array, base_type);
      nir_variable *texture = nir_variable_create(b->shader, nir_var_uniform,
                                                  texture_type, tex_name);
      texture->data.descriptor_set = 0;
      texture->data.binding = aspect_to_tex_binding(aspect);

      nir_tex_instr *tex = nir_tex_instr_create(b->shader, 3);
      tex->op = nir_texop_txl;
      tex->sampler_dim = key->dim;
      tex->dest_type = nir_get_nir_type_for_glsl_base_type(base_type);
      tex->coord_components = src_coord->num_components;
      tex->is_array = is_array;
      tex->is_shadow = false;

      tex->src[0] = (nir_tex_src) {
         .src_type = nir_tex_src_coord,
         .src = nir_src_for_ssa(src_coord),
      };
      tex->src[1] = (nir_tex_src) {
         .src_type = nir_tex_src_texture_deref,
         .src = nir_src_for_ssa(&nir_build_deref_var(b, texture)->dest.ssa),
      };
      tex->src[2] = (nir_tex_src) {
         .src_type = nir_tex_src_sampler_deref,
         .src = nir_src_for_ssa(&nir_build_deref_var(b, sampler)->dest.ssa),
      };

      nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32);

      nir_builder_instr_insert(b, &tex->instr);

      const struct glsl_type *out_type = glsl_vector_type(base_type, out_comps);
      nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                              out_type, out_name);
      out->data.location = out_location;

      nir_store_var(b, out, &tex->dest.ssa, BITFIELD_MASK(out_comps));
   }

   return b->shader;
}

static VkResult
get_blit_descriptor_set_layout(struct vk_device *device,
                               struct vk_meta_device *meta,
                               VkDescriptorSetLayout *layout_out)
{
   const char key[] = "vk-meta-blit-descriptor-set-layout";

   VkDescriptorSetLayout from_cache =
      vk_meta_lookup_descriptor_set_layout(meta, key, sizeof(key));
   if (from_cache != VK_NULL_HANDLE) {
      *layout_out = from_cache;
      return VK_SUCCESS;
   }

   const VkDescriptorSetLayoutBinding bindings[] = {{
      .binding = BLIT_DESC_BINDING_SAMPLER,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   }, {
      .binding = BLIT_DESC_BINDING_COLOR,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   }, {
      .binding = BLIT_DESC_BINDING_DEPTH,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   }, {
      .binding = BLIT_DESC_BINDING_STENCIL,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   }};

   const VkDescriptorSetLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   return vk_meta_create_descriptor_set_layout(device, meta, &info,
                                               key, sizeof(key), layout_out);
}

static VkResult
get_blit_pipeline_layout(struct vk_device *device,
                         struct vk_meta_device *meta,
                         VkDescriptorSetLayout set_layout,
                         VkPipelineLayout *layout_out)
{
   const char key[] = "vk-meta-blit-pipeline-layout";

   VkPipelineLayout from_cache =
      vk_meta_lookup_pipeline_layout(meta, key, sizeof(key));
   if (from_cache != VK_NULL_HANDLE) {
      *layout_out = from_cache;
      return VK_SUCCESS;
   }

   const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(struct vk_meta_blit_push_data),
   };

   const VkPipelineLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };

   return vk_meta_create_pipeline_layout(device, meta, &info,
                                         key, sizeof(key), layout_out);
}

static VkResult
get_blit_pipeline(struct vk_device *device,
                  struct vk_meta_device *meta,
                  const struct vk_meta_blit_key *key,
                  VkPipelineLayout layout,
                  VkPipeline *pipeline_out)
{
   VkPipeline from_cache = vk_meta_lookup_pipeline(meta, key, sizeof(*key));
   if (from_cache != VK_NULL_HANDLE) {
      *pipeline_out = from_cache;
      return VK_SUCCESS;
   }

   const VkPipelineShaderStageNirCreateInfoMESA fs_nir_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = build_blit_shader(key),
   };
   const VkPipelineShaderStageCreateInfo fs_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &fs_nir_info,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .pName = "main",
   };

   VkPipelineDepthStencilStateCreateInfo ds_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
   };
   struct vk_meta_rendering_info render = {
      .samples = 1,
   };
   if (key->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      render.color_attachment_count = 1;
      render.color_attachment_formats[0] = key->dst_format;
   }
   if (key->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      ds_info.depthTestEnable = VK_TRUE;
      ds_info.depthWriteEnable = VK_TRUE;
      ds_info.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      render.depth_attachment_format = key->dst_format;
   }
   if (key->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      ds_info.stencilTestEnable = VK_TRUE;
      ds_info.front.compareOp = VK_COMPARE_OP_ALWAYS;
      ds_info.front.passOp = VK_STENCIL_OP_REPLACE;
      ds_info.front.compareMask = ~0u;
      ds_info.front.writeMask = ~0u;
      ds_info.back = ds_info.front;
      render.stencil_attachment_format = key->dst_format;
   }

   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 1,
      .pStages = &fs_info,
      .pDepthStencilState = &ds_info,
      .layout = layout,
   };

   VkResult result = vk_meta_create_graphics_pipeline(device, meta, &info,
                                                      &render,
                                                      key, sizeof(*key),
                                                      pipeline_out);
   ralloc_free(fs_nir_info.nir);

   return result;
}

static VkResult
get_blit_sampler(struct vk_device *device,
                 struct vk_meta_device *meta,
                 VkFilter filter,
                 VkSampler *sampler_out)
{
   struct {
      enum vk_meta_object_key_type key_type;
      VkFilter filter;
   } key;

   memset(&key, 0, sizeof(key));
   key.key_type = VK_META_OBJECT_KEY_BLIT_SAMPLER;
   key.filter = filter;

   VkSampler from_cache = vk_meta_lookup_sampler(meta, &key, sizeof(key));
   if (from_cache != VK_NULL_HANDLE) {
      *sampler_out = from_cache;
      return VK_SUCCESS;
   }

   const VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = filter,
      .minFilter = filter,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .unnormalizedCoordinates = VK_FALSE,
   };

   return vk_meta_create_sampler(device, meta, &info,
                                 &key, sizeof(key), sampler_out);
}

void
vk_meta_blit_image(struct vk_command_buffer *cmd,
                   struct vk_meta_device *meta,
                   struct vk_image *src_image,
                   VkFormat src_format,
                   VkImageLayout src_image_layout,
                   struct vk_image *dst_image,
                   VkFormat dst_format,
                   VkImageLayout dst_image_layout,
                   uint32_t region_count,
                   const VkImageBlit2 *regions,
                   VkFilter filter)
{
   struct vk_device *device = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   VkResult result;

   VkSampler sampler;
   result = get_blit_sampler(device, meta, filter, &sampler);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   VkDescriptorSetLayout set_layout;
   result = get_blit_descriptor_set_layout(device, meta, &set_layout);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   VkPipelineLayout pipeline_layout;
   result = get_blit_pipeline_layout(device, meta, set_layout,
                                     &pipeline_layout);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(cmd, result);
      return;
   }

   struct vk_meta_blit_key key;
   memset(&key, 0, sizeof(key));
   key.key_type = VK_META_OBJECT_KEY_BLIT_PIPELINE;
   key.dim = vk_image_sampler_dim(src_image);
   key.dst_format = dst_format;

   for (uint32_t r = 0; r < region_count; r++) {
      struct vk_meta_blit_push_data push = {0};
      struct vk_meta_rect dst_rect = {0};

      uint32_t src_level = regions[r].srcSubresource.mipLevel;
      VkExtent3D src_extent = vk_image_mip_level_extent(src_image, src_level);

      compute_off_scale(src_extent.width,
                        regions[r].srcOffsets[0].x,
                        regions[r].srcOffsets[1].x,
                        regions[r].dstOffsets[0].x,
                        regions[r].dstOffsets[1].x,
                        &dst_rect.x0, &dst_rect.x1,
                        &push.x_off, &push.x_scale);
      compute_off_scale(src_extent.height,
                        regions[r].srcOffsets[0].y,
                        regions[r].srcOffsets[1].y,
                        regions[r].dstOffsets[0].y,
                        regions[r].dstOffsets[1].y,
                        &dst_rect.y0, &dst_rect.y1,
                        &push.y_off, &push.y_scale);

      uint32_t dst_base_layer, dst_layer_count;
      if (src_image->image_type == VK_IMAGE_TYPE_3D) {
         uint32_t start_layer, end_layer;
         compute_off_scale(src_extent.depth,
                           regions[r].srcOffsets[0].z,
                           regions[r].srcOffsets[1].z,
                           regions[r].dstOffsets[0].z,
                           regions[r].dstOffsets[1].z,
                           &start_layer, &end_layer,
                           &push.z_off, &push.z_scale);
         dst_base_layer = start_layer;
         dst_layer_count = end_layer - start_layer;
      } else {
         dst_base_layer = regions[r].dstSubresource.baseArrayLayer;
         dst_layer_count = regions[r].dstSubresource.layerCount;
         push.arr_delta = regions[r].dstSubresource.baseArrayLayer -
                          regions[r].srcSubresource.baseArrayLayer;
      }

      key.aspects = regions[r].dstSubresource.aspectMask;

      VkImageView dst_view;
      const VkImageViewUsageCreateInfo dst_view_usage = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
         .usage = (key.aspects & VK_IMAGE_ASPECT_COLOR_BIT) ?
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT :
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      };
      const VkImageViewCreateInfo dst_view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .pNext = &dst_view_usage,
         .image = vk_image_to_handle(dst_image),
         .viewType = vk_image_sampled_view_type(dst_image),
         .format = dst_format,
         .subresourceRange = {
            .aspectMask = regions[r].dstSubresource.aspectMask,
            .baseMipLevel = regions[r].dstSubresource.mipLevel,
            .levelCount = 1,
            .baseArrayLayer = dst_base_layer,
            .layerCount = dst_layer_count,
         },
      };
      result = vk_meta_create_image_view(cmd, meta, &dst_view_info, &dst_view);
      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(cmd, result);
         return;
      }

      const VkRenderingAttachmentInfo vk_att = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = dst_view,
         .imageLayout = dst_image_layout,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      VkRenderingInfo vk_render = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {
            .offset = {
               dst_rect.x0,
               dst_rect.y0
            },
            .extent = {
               dst_rect.x1 - dst_rect.x0,
               dst_rect.y1 - dst_rect.y0
            },
         },
         .layerCount = dst_layer_count,
      };

      if (key.aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
         vk_render.colorAttachmentCount = 1;
         vk_render.pColorAttachments = &vk_att;
      }
      if (key.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
         vk_render.pDepthAttachment = &vk_att;
      if (key.aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
         vk_render.pStencilAttachment = &vk_att;

      disp->CmdBeginRendering(vk_command_buffer_to_handle(cmd), &vk_render);

      VkPipeline pipeline;
      result = get_blit_pipeline(device, meta, &key,
                                 pipeline_layout, &pipeline);
      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(cmd, result);
         return;
      }

      uint32_t desc_count = 0;
      VkDescriptorImageInfo image_infos[3];
      VkWriteDescriptorSet desc_writes[3];

      image_infos[desc_count] = (VkDescriptorImageInfo) {
         .sampler = sampler,
      };
      desc_writes[desc_count] = (VkWriteDescriptorSet) {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = BLIT_DESC_BINDING_SAMPLER,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .descriptorCount = 1,
         .pImageInfo = &image_infos[desc_count],
      };
      desc_count++;

      u_foreach_bit(a, regions[r].srcSubresource.aspectMask) {
         VkImageAspectFlagBits aspect = (1 << a);

         VkImageView src_view;
         const VkImageViewUsageCreateInfo src_view_usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
         };
         const VkImageViewCreateInfo src_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &src_view_usage,
            .image = vk_image_to_handle(src_image),
            .viewType = vk_image_sampled_view_type(src_image),
            .format = src_format,
            .subresourceRange = {
               .aspectMask = aspect,
               .baseMipLevel = regions[r].srcSubresource.mipLevel,
               .levelCount = 1,
               .baseArrayLayer = regions[r].srcSubresource.baseArrayLayer,
               .layerCount = regions[r].srcSubresource.layerCount,
            },
         };
         result = vk_meta_create_image_view(cmd, meta, &src_view_info,
                                            &src_view);
         if (unlikely(result != VK_SUCCESS)) {
            vk_command_buffer_set_error(cmd, result);
            return;
         }

         assert(desc_count < ARRAY_SIZE(image_infos));
         assert(desc_count < ARRAY_SIZE(desc_writes));
         image_infos[desc_count] = (VkDescriptorImageInfo) {
            .imageView = src_view,
         };
         desc_writes[desc_count] = (VkWriteDescriptorSet) {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = aspect_to_tex_binding(aspect),
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[desc_count],
         };
         desc_count++;
      }

      disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

      disp->CmdPushDescriptorSetKHR(vk_command_buffer_to_handle(cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout, 0,
                                    desc_count, desc_writes);

      disp->CmdPushConstants(vk_command_buffer_to_handle(cmd),
                             pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(push), &push);

      meta->cmd_draw_volume(cmd, meta, &dst_rect, dst_layer_count);

      disp->CmdEndRendering(vk_command_buffer_to_handle(cmd));
   }
}

void
vk_meta_blit_image2(struct vk_command_buffer *cmd,
                    struct vk_meta_device *meta,
                    const VkBlitImageInfo2 *blit)
{
   VK_FROM_HANDLE(vk_image, src_image, blit->srcImage);
   VK_FROM_HANDLE(vk_image, dst_image, blit->dstImage);

   vk_meta_blit_image(cmd, meta,
                      src_image, src_image->format, blit->srcImageLayout,
                      dst_image, dst_image->format, blit->dstImageLayout,
                      blit->regionCount, blit->pRegions, blit->filter);
}

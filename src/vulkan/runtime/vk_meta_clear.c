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
#include "vk_image.h"
#include "vk_pipeline.h"
#include "vk_util.h"

#include "nir_builder.h"

struct vk_meta_clear_key {
   struct vk_meta_rendering_info render;
   uint8_t color_attachments_cleared;
   bool clear_depth;
   bool clear_stencil;
};

struct vk_meta_clear_push_data {
   VkClearColorValue color_values[MESA_VK_MAX_COLOR_ATTACHMENTS];
};

static nir_shader *
build_clear_shader(const struct vk_meta_clear_key *key)
{
   nir_builder build = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                      NULL, "vk-meta-clear");
   nir_builder *b = &build;

   struct glsl_struct_field push_field = {
      .type = glsl_array_type(glsl_vec4_type(),
                              MESA_VK_MAX_COLOR_ATTACHMENTS,
                              16 /* explicit_stride */),
      .name = "color_values",
   };
   const struct glsl_type *push_iface_type =
      glsl_interface_type(&push_field, 1, GLSL_INTERFACE_PACKING_STD140,
                          false /* row_major */, "push");

   nir_variable *push = nir_variable_create(b->shader, nir_var_mem_push_const,
                                            push_iface_type, "push");
   nir_deref_instr *push_arr =
      nir_build_deref_struct(b, nir_build_deref_var(b, push), 0);

   u_foreach_bit(a, key->color_attachments_cleared) {
      nir_ssa_def *color_value =
         nir_load_deref(b, nir_build_deref_array_imm(b, push_arr, a));

      const struct glsl_type *out_type;
      if (vk_format_is_int(key->render.color_attachment_formats[a]))
         out_type = glsl_ivec4_type();
      else if (vk_format_is_int(key->render.color_attachment_formats[a]))
         out_type = glsl_uvec4_type();
      else
         out_type = glsl_vec4_type();

      char out_name[8];
      snprintf(out_name, sizeof(out_name), "color%u", a);

      nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                              out_type, out_name);
      out->data.location = FRAG_RESULT_DATA0 + a;

      nir_store_var(b, out, color_value, 0xf);
   }

   return b->shader;
}

static VkResult
get_clear_pipeline_layout(struct vk_device *device,
                          struct vk_meta_device *meta,
                          VkPipelineLayout *layout_out)
{
   const char key[] = "vk-meta-clear-pipeline-layout";

   VkPipelineLayout from_cache =
      vk_meta_lookup_pipeline_layout(meta, key, sizeof(key));
   if (from_cache != VK_NULL_HANDLE) {
      *layout_out = from_cache;
      return VK_SUCCESS;
   }

   const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(struct vk_meta_clear_push_data),
   };

   const VkPipelineLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };

   return vk_meta_create_pipeline_layout(device, meta, &info,
                                         key, sizeof(key), layout_out);
}

static VkResult
get_clear_pipeline(struct vk_device *device,
                   struct vk_meta_device *meta,
                   const struct vk_meta_clear_key *key,
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
      .nir = build_clear_shader(key),
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
   if (key->clear_depth) {
      ds_info.depthTestEnable = VK_TRUE;
      ds_info.depthWriteEnable = VK_TRUE;
      ds_info.depthCompareOp = VK_COMPARE_OP_ALWAYS;
   }
   if (key->clear_stencil) {
      ds_info.stencilTestEnable = VK_TRUE;
      ds_info.front.compareOp = VK_COMPARE_OP_ALWAYS;
      ds_info.front.passOp = VK_STENCIL_OP_REPLACE;
      ds_info.front.compareMask = ~0u;
      ds_info.front.writeMask = ~0u;
      ds_info.back = ds_info.front;
   }

   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 1,
      .pStages = &fs_info,
      .pDepthStencilState = &ds_info,
      .layout = layout,
   };

   VkResult result = vk_meta_create_graphics_pipeline(device, meta, &info,
                                                      &key->render,
                                                      key, sizeof(*key),
                                                      pipeline_out);
   ralloc_free(fs_nir_info.nir);

   return result;
}

static int
vk_meta_rect_cmp_layer(const void *_a, const void *_b)
{
   const struct vk_meta_rect *a = _a, *b = _b;
   assert(a->layer <= INT_MAX && b->layer <= INT_MAX);
   return a->layer - b->layer;
}

void
vk_meta_clear_attachments(struct vk_command_buffer *cmd,
                          struct vk_meta_device *meta,
                          const struct vk_meta_rendering_info *render,
                          uint32_t attachment_count,
                          const VkClearAttachment *attachments,
                          uint32_t clear_rect_count,
                          const VkClearRect *clear_rects)
{
   struct vk_device *device = cmd->base.device;
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   VkResult result;

   struct vk_meta_clear_key key;
   memset(&key, 0, sizeof(key));
   vk_meta_rendering_info_copy(&key.render, render);

   struct vk_meta_clear_push_data push = { };
   float depth_value = 1.0f;
   uint32_t stencil_value = 0;

   for (uint32_t i = 0; i < attachment_count; i++) {
      if (attachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         const uint32_t a = attachments[i].colorAttachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         assert(a < MESA_VK_MAX_COLOR_ATTACHMENTS);
         if (render->color_attachment_formats[a] == VK_FORMAT_UNDEFINED)
            continue;

         key.color_attachments_cleared |= BITFIELD_BIT(a);
         push.color_values[a] = attachments[i].clearValue.color;
      }
      if (attachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         key.clear_depth = true;
         depth_value = attachments[i].clearValue.depthStencil.depth;
      }
      if (attachments[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         key.clear_stencil = true;
         stencil_value = attachments[i].clearValue.depthStencil.stencil;
      }
   }

   VkPipelineLayout layout;
   result = get_clear_pipeline_layout(device, meta, &layout);
   if (unlikely(result != VK_SUCCESS)) {
      /* TODO: Report error */
      return;
   }

   VkPipeline pipeline;
   result = get_clear_pipeline(device, meta, &key, layout, &pipeline);
   if (unlikely(result != VK_SUCCESS)) {
      /* TODO: Report error */
      return;
   }

   disp->CmdBindPipeline(vk_command_buffer_to_handle(cmd),
                         VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   if (key.clear_stencil) {
      disp->CmdSetStencilReference(vk_command_buffer_to_handle(cmd),
                                   VK_STENCIL_FACE_FRONT_AND_BACK,
                                   stencil_value);
   }

   disp->CmdPushConstants(vk_command_buffer_to_handle(cmd),
                          layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(push), &push);

   if (render->view_mask == 0) {
      if (clear_rect_count == 1 && clear_rects[0].layerCount > 1) {
         struct vk_meta_rect rect = {
            .x0 = clear_rects[0].rect.offset.x,
            .x1 = clear_rects[0].rect.offset.x +
                  clear_rects[0].rect.extent.width,
            .y0 = clear_rects[0].rect.offset.y,
            .y1 = clear_rects[0].rect.offset.y +
                  clear_rects[0].rect.extent.height,
            .z = depth_value,
            .layer = clear_rects[0].baseArrayLayer,
         };

         meta->cmd_draw_volume(cmd, meta, &rect, clear_rects[0].layerCount);
      } else {
         uint32_t max_rect_count = 0;
         for (uint32_t r = 0; r < clear_rect_count; r++)
            max_rect_count += clear_rects[r].layerCount;

         STACK_ARRAY(struct vk_meta_rect, rects, max_rect_count);

         uint32_t rect_count = 0;
         for (uint32_t r = 0; r < clear_rect_count; r++) {
            struct vk_meta_rect rect = {
               .x0 = clear_rects[r].rect.offset.x,
               .x1 = clear_rects[r].rect.offset.x +
                     clear_rects[r].rect.extent.width,
               .y0 = clear_rects[r].rect.offset.y,
               .y1 = clear_rects[r].rect.offset.y +
                     clear_rects[r].rect.extent.height,
               .z = depth_value,
            };
            for (uint32_t a = 0; a < clear_rects[r].layerCount; a++) {
               rect.layer = clear_rects[r].baseArrayLayer + a;
               rects[rect_count++] = rect;
            }
         }
         assert(rect_count <= max_rect_count);

         /* If we have more than one clear rect, sort by layer in the hopes
          * the hardware more or less does all the clears for one layer before
          * moving on to the next, thus reducing cache thrashing.
          */
         qsort(rects, rect_count, sizeof(*rects), vk_meta_rect_cmp_layer);

         meta->cmd_draw_rects(cmd, meta, rect_count, rects);

         STACK_ARRAY_FINISH(rects);
      }
   } else {
      const uint32_t rect_count = clear_rect_count *
                                  util_bitcount(render->view_mask);
      STACK_ARRAY(struct vk_meta_rect, rects, rect_count);

      uint32_t rect_idx = 0;
      u_foreach_bit(v, render->view_mask) {
         for (uint32_t r = 0; r < clear_rect_count; r++) {
            assert(clear_rects[r].baseArrayLayer == 0);
            assert(clear_rects[r].layerCount == 1);
            rects[rect_idx++] = (struct vk_meta_rect) {
               .x0 = clear_rects[r].rect.offset.x,
               .x1 = clear_rects[r].rect.offset.x +
                     clear_rects[r].rect.extent.width,
               .y0 = clear_rects[r].rect.offset.y,
               .y1 = clear_rects[r].rect.offset.y +
                     clear_rects[r].rect.extent.height,
               .z = depth_value,
               .layer = v,
            };
         }
      }
      assert(rect_idx == rect_count);

      meta->cmd_draw_rects(cmd, meta, rect_count, rects);

      STACK_ARRAY_FINISH(rects);
   }
}

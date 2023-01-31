#include "nvk_buffer.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "nvk_image_view.h"
#include "nvk_mme.h"
#include "nvk_physical_device.h"
#include "nvk_pipeline.h"

#include "nil_format.h"
#include "vulkan/runtime/vk_render_pass.h"
#include "vulkan/util/vk_format.h"

#include "nouveau_context.h"

#include "nvk_cl9097.h"
#include "nvk_cla097.h"
#include "nvk_clb197.h"
#include "nvk_clc397.h"
#include "nvk_clc597.h"
#include "drf.h"

static inline uint16_t
nvk_cmd_buffer_3d_cls(struct nvk_cmd_buffer *cmd)
{
   return nvk_cmd_buffer_device(cmd)->ctx->eng3d.cls;
}

VkResult
nvk_queue_init_context_draw_state(struct nvk_queue *queue)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   uint32_t push_data[512];
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data));
   struct nv_push *p = &push;

   P_MTHD(p, NV9097, SET_OBJECT);
   P_NV9097_SET_OBJECT(p, {
      .class_id = dev->ctx->eng3d.cls,
      .engine_id = 0,
   });

   for (uint32_t mme = 0, mme_pos = 0; mme < NVK_MME_COUNT; mme++) {
      size_t size;
      uint32_t *dw = nvk_build_mme(dev, mme, &size);
      if (dw == NULL)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

      assert(size % sizeof(uint32_t) == 0);
      const uint32_t num_dw = size / sizeof(uint32_t);

      P_MTHD(p, NV9097, LOAD_MME_START_ADDRESS_RAM_POINTER);
      P_NV9097_LOAD_MME_START_ADDRESS_RAM_POINTER(p, mme);
      P_NV9097_LOAD_MME_START_ADDRESS_RAM(p, mme_pos);

      P_1INC(p, NV9097, LOAD_MME_INSTRUCTION_RAM_POINTER);
      P_NV9097_LOAD_MME_INSTRUCTION_RAM_POINTER(p, mme_pos);
      P_INLINE_ARRAY(p, dw, num_dw);

      mme_pos += num_dw;

      free(dw);
   }

   P_IMMD(p, NV9097, SET_RENDER_ENABLE_C, MODE_TRUE);

   P_IMMD(p, NV9097, SET_Z_COMPRESSION, ENABLE_TRUE);
   P_MTHD(p, NV9097, SET_COLOR_COMPRESSION(0));
   for (unsigned i = 0; i < 8; i++)
      P_NV9097_SET_COLOR_COMPRESSION(p, i, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_CT_SELECT, { .target_count = 1 });

//   P_MTHD(cmd->push, NVC0_3D, CSAA_ENABLE);
//   P_INLINE_DATA(cmd->push, 0);

   P_IMMD(p, NV9097, SET_ALIASED_LINE_WIDTH_ENABLE, V_TRUE);

   P_IMMD(p, NV9097, SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_BLEND_SEPARATE_FOR_ALPHA, ENABLE_TRUE);
   P_IMMD(p, NV9097, SET_SINGLE_CT_WRITE_CONTROL, ENABLE_TRUE);
   P_IMMD(p, NV9097, SET_SINGLE_ROP_CONTROL, ENABLE_FALSE);
   P_IMMD(p, NV9097, SET_TWO_SIDED_STENCIL_TEST, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_SHADE_MODE, V_OGL_SMOOTH);

   P_IMMD(p, NV9097, SET_API_VISIBLE_CALL_LIMIT, V__128);

   P_IMMD(p, NV9097, SET_ZCULL_STATS, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_L1_CONFIGURATION,
                     DIRECTLY_ADDRESSABLE_MEMORY_SIZE_48KB);

   P_IMMD(p, NV9097, SET_REDUCE_COLOR_THRESHOLDS_ENABLE, V_FALSE);
   P_IMMD(p, NV9097, SET_REDUCE_COLOR_THRESHOLDS_UNORM8, {
      .all_covered_all_hit_once = 0xff,
   });
   P_MTHD(p, NV9097, SET_REDUCE_COLOR_THRESHOLDS_UNORM10);
   P_NV9097_SET_REDUCE_COLOR_THRESHOLDS_UNORM10(p, {
      .all_covered_all_hit_once = 0xff,
   });
   P_NV9097_SET_REDUCE_COLOR_THRESHOLDS_UNORM16(p, {
      .all_covered_all_hit_once = 0xff,
   });
   P_NV9097_SET_REDUCE_COLOR_THRESHOLDS_FP11(p, {
      .all_covered_all_hit_once = 0x3f,
   });
   P_NV9097_SET_REDUCE_COLOR_THRESHOLDS_FP16(p, {
      .all_covered_all_hit_once = 0xff,
   });
   P_NV9097_SET_REDUCE_COLOR_THRESHOLDS_SRGB8(p, {
      .all_covered_all_hit_once = 0xff,
   });

   if (dev->ctx->eng3d.cls < VOLTA_A)
      P_IMMD(p, NV9097, SET_ALPHA_FRACTION, 0x3f);

   P_IMMD(p, NV9097, CHECK_SPH_VERSION, {
      .current = 3,
      .oldest_supported = 3,
   });
   P_IMMD(p, NV9097, CHECK_AAM_VERSION, {
      .current = 2,
      .oldest_supported = 2,
   });

   if (dev->ctx->eng3d.cls < VOLTA_A)
      P_IMMD(p, NV9097, SET_SHADER_SCHEDULING, MODE_OLDEST_THREAD_FIRST);

   P_IMMD(p, NV9097, SET_L2_CACHE_CONTROL_FOR_ROP_PREFETCH_READ_REQUESTS,
                     POLICY_EVICT_NORMAL);
   P_IMMD(p, NV9097, SET_L2_CACHE_CONTROL_FOR_ROP_NONINTERLOCKED_READ_REQUESTS,
                     POLICY_EVICT_NORMAL);
   P_IMMD(p, NV9097, SET_L2_CACHE_CONTROL_FOR_ROP_INTERLOCKED_READ_REQUESTS,
                     POLICY_EVICT_NORMAL);
   P_IMMD(p, NV9097, SET_L2_CACHE_CONTROL_FOR_ROP_NONINTERLOCKED_WRITE_REQUESTS,
                     POLICY_EVICT_NORMAL);
   P_IMMD(p, NV9097, SET_L2_CACHE_CONTROL_FOR_ROP_INTERLOCKED_WRITE_REQUESTS,
                     POLICY_EVICT_NORMAL);

   P_IMMD(p, NV9097, SET_BLEND_PER_FORMAT_ENABLE, SNORM8_UNORM16_SNORM16_TRUE);

   P_IMMD(p, NV9097, SET_ATTRIBUTE_DEFAULT, {
      .color_front_diffuse    = COLOR_FRONT_DIFFUSE_VECTOR_0001,
      .color_front_specular   = COLOR_FRONT_SPECULAR_VECTOR_0001,
      .generic_vector         = GENERIC_VECTOR_VECTOR_0001,
      .fixed_fnc_texture      = FIXED_FNC_TEXTURE_VECTOR_0001,
      .dx9_color0             = DX9_COLOR0_VECTOR_0001,
      .dx9_color1_to_color15  = DX9_COLOR1_TO_COLOR15_VECTOR_0000,
   });

   P_IMMD(p, NV9097, SET_DA_OUTPUT, VERTEX_ID_USES_ARRAY_START_TRUE);

   P_IMMD(p, NV9097, SET_RENDER_ENABLE_CONTROL,
                     CONDITIONAL_LOAD_CONSTANT_BUFFER_FALSE);

   P_IMMD(p, NV9097, SET_PS_OUTPUT_SAMPLE_MASK_USAGE, {
      .enable                       = ENABLE_TRUE,
      .qualify_by_anti_alias_enable = QUALIFY_BY_ANTI_ALIAS_ENABLE_ENABLE,
   });

   if (dev->ctx->eng3d.cls < VOLTA_A)
      P_IMMD(p, NV9097, SET_PRIM_CIRCULAR_BUFFER_THROTTLE, 0x3fffff);

   P_IMMD(p, NV9097, SET_BLEND_OPT_CONTROL, ALLOW_FLOAT_PIXEL_KILLS_TRUE);
   P_IMMD(p, NV9097, SET_BLEND_FLOAT_OPTION, ZERO_TIMES_ANYTHING_IS_ZERO_TRUE);

   if (dev->ctx->eng3d.cls < VOLTA_A)
      P_IMMD(p, NV9097, SET_MAX_TI_WARPS_PER_BATCH, 3);

   if (dev->ctx->eng3d.cls >= KEPLER_A &&
       dev->ctx->eng3d.cls < VOLTA_A) {
      P_IMMD(p, NVA097, SET_TEXTURE_INSTRUCTION_OPERAND,
                        ORDERING_KEPLER_ORDER);
   }

   P_IMMD(p, NV9097, SET_ALPHA_TEST, ENABLE_FALSE);
   P_IMMD(p, NV9097, SET_TWO_SIDED_LIGHT, ENABLE_FALSE);
   P_IMMD(p, NV9097, SET_COLOR_CLAMP, ENABLE_TRUE);
   P_IMMD(p, NV9097, SET_PS_SATURATE, {
      .output0 = OUTPUT0_FALSE,
      .output1 = OUTPUT1_FALSE,
      .output2 = OUTPUT2_FALSE,
      .output3 = OUTPUT3_FALSE,
      .output4 = OUTPUT4_FALSE,
      .output5 = OUTPUT5_FALSE,
      .output6 = OUTPUT6_FALSE,
      .output7 = OUTPUT7_FALSE,
   });

   P_IMMD(p, NV9097, SET_ATTRIBUTE_POINT_SIZE, {
      .enable  = ENABLE_FALSE,
      .slot    = 0,
   });
   P_IMMD(p, NV9097, SET_POINT_SIZE, fui(1.0));

   P_IMMD(p, NV9097, SET_POINT_SPRITE_SELECT, {
      .rmode      = RMODE_ZERO,
      .origin     = ORIGIN_TOP,
      .texture0   = TEXTURE0_PASSTHROUGH,
      .texture1   = TEXTURE1_PASSTHROUGH,
      .texture2   = TEXTURE2_PASSTHROUGH,
      .texture3   = TEXTURE3_PASSTHROUGH,
      .texture4   = TEXTURE4_PASSTHROUGH,
      .texture5   = TEXTURE5_PASSTHROUGH,
      .texture6   = TEXTURE6_PASSTHROUGH,
      .texture7   = TEXTURE7_PASSTHROUGH,
      .texture8   = TEXTURE8_PASSTHROUGH,
      .texture9   = TEXTURE9_PASSTHROUGH,
   });
   P_IMMD(p, NV9097, SET_POINT_SPRITE, ENABLE_FALSE);
   P_IMMD(p, NV9097, SET_ANTI_ALIASED_POINT, ENABLE_FALSE);

   if (dev->ctx->eng3d.cls >= MAXWELL_B)
      P_IMMD(p, NVB197, SET_FILL_VIA_TRIANGLE, MODE_DISABLED);

   P_IMMD(p, NV9097, SET_POLY_SMOOTH, ENABLE_FALSE);

   P_IMMD(p, NV9097, SET_VIEWPORT_PIXEL, CENTER_AT_HALF_INTEGERS);

   P_IMMD(p, NV9097, SET_HYBRID_ANTI_ALIAS_CONTROL, {
      .passes     = 1,
      .centroid   = CENTROID_PER_FRAGMENT,
   });

   if (dev->ctx->eng3d.cls >= MAXWELL_B) {
      P_IMMD(p, NVB197, SET_OFFSET_RENDER_TARGET_INDEX,
                        BY_VIEWPORT_INDEX_FALSE);
   }

   /* TODO: Vertex runout */

   P_IMMD(p, NV9097, SET_WINDOW_ORIGIN, {
      .mode    = MODE_UPPER_LEFT,
      .flip_y  = FLIP_Y_FALSE,
   });

   P_MTHD(p, NV9097, SET_WINDOW_OFFSET_X);
   P_NV9097_SET_WINDOW_OFFSET_X(p, 0);
   P_NV9097_SET_WINDOW_OFFSET_Y(p, 0);

   P_IMMD(p, NV9097, SET_ACTIVE_ZCULL_REGION, 0x3f);
   P_IMMD(p, NV9097, SET_WINDOW_CLIP_ENABLE, V_FALSE);
   P_IMMD(p, NV9097, SET_CLIP_ID_TEST, ENABLE_FALSE);

//   P_IMMD(p, NV9097, X_X_X_SET_CLEAR_CONTROL, {
//      .respect_stencil_mask   = RESPECT_STENCIL_MASK_FALSE,
//      .use_clear_rect         = USE_CLEAR_RECT_FALSE,
//   });

   P_IMMD(p, NV9097, SET_VIEWPORT_SCALE_OFFSET, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_VIEWPORT_CLIP_CONTROL, {
      .min_z_zero_max_z_one      = MIN_Z_ZERO_MAX_Z_ONE_TRUE,
      .pixel_min_z               = PIXEL_MIN_Z_CLAMP,
      .pixel_max_z               = PIXEL_MAX_Z_CLIP,
      .geometry_guardband        = GEOMETRY_GUARDBAND_SCALE_256,
      .line_point_cull_guardband = LINE_POINT_CULL_GUARDBAND_SCALE_256,
      .geometry_clip             = GEOMETRY_CLIP_WZERO_CLIP,
      .geometry_guardband_z      = GEOMETRY_GUARDBAND_Z_SAME_AS_XY_GUARDBAND,
   });

   for (unsigned i = 0; i < 16; i++)
      P_IMMD(p, NV9097, SET_SCISSOR_ENABLE(i), V_FALSE);

   P_IMMD(p, NV9097, SET_CT_MRT_ENABLE, V_TRUE);

   for (uint32_t i = 0; i < 6; i++) {
      P_IMMD(p, NV9097, SET_PIPELINE_SHADER(i), {
         .enable  = ENABLE_FALSE,
         .type    = i,
      });
   }

//   P_MTHD(cmd->push, NVC0_3D, MACRO_GP_SELECT);
//   P_INLINE_DATA(cmd->push, 0x40);
   P_IMMD(p, NV9097, SET_RT_LAYER, {
      .v = 0,
      .control = CONTROL_V_SELECTS_LAYER,
   });
//   P_MTHD(cmd->push, NVC0_3D, MACRO_TEP_SELECT;
//   P_INLINE_DATA(cmd->push, 0x30);

   P_IMMD(p, NV9097, SET_POINT_SPRITE_SELECT, {
      .rmode      = RMODE_ZERO,
      .origin     = ORIGIN_BOTTOM,
      .texture0   = TEXTURE0_PASSTHROUGH,
      .texture1   = TEXTURE1_PASSTHROUGH,
      .texture2   = TEXTURE2_PASSTHROUGH,
      .texture3   = TEXTURE3_PASSTHROUGH,
      .texture4   = TEXTURE4_PASSTHROUGH,
      .texture5   = TEXTURE5_PASSTHROUGH,
      .texture6   = TEXTURE6_PASSTHROUGH,
      .texture7   = TEXTURE7_PASSTHROUGH,
      .texture8   = TEXTURE8_PASSTHROUGH,
      .texture9   = TEXTURE9_PASSTHROUGH,
   });
   P_IMMD(p, NV9097, SET_POINT_CENTER_MODE, V_OGL);
   P_IMMD(p, NV9097, SET_EDGE_FLAG, V_TRUE);
   P_IMMD(p, NV9097, SET_SAMPLER_BINDING, V_INDEPENDENTLY);

   uint64_t zero_addr = dev->zero_page->offset;
   P_MTHD(p, NV9097, SET_VERTEX_STREAM_SUBSTITUTE_A);
   P_NV9097_SET_VERTEX_STREAM_SUBSTITUTE_A(p, zero_addr >> 32);
   P_NV9097_SET_VERTEX_STREAM_SUBSTITUTE_B(p, zero_addr);

   return nvk_queue_submit_simple(queue, push_data, nv_push_dw_count(&push),
                                  NULL /* extra_bo */);
}

void
nvk_cmd_buffer_begin_graphics(struct nvk_cmd_buffer *cmd,
                              const VkCommandBufferBeginInfo *pBeginInfo)
{
   if (cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY &&
       (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
      char gcbiar_data[VK_GCBIARR_DATA_SIZE(NVK_MAX_RTS)];
      const VkRenderingInfo *resume_info =
         vk_get_command_buffer_inheritance_as_rendering_resume(cmd->vk.level,
                                                               pBeginInfo,
                                                               gcbiar_data);
      if (resume_info) {
         nvk_CmdBeginRendering(nvk_cmd_buffer_to_handle(cmd), resume_info);
      } else {
         const VkCommandBufferInheritanceRenderingInfo *inheritance_info =
            vk_get_command_buffer_inheritance_rendering_info(cmd->vk.level,
                                                             pBeginInfo);
         assert(inheritance_info);

         struct nvk_rendering_state *render = &cmd->state.gfx.render;
         render->flags = inheritance_info->flags;
         render->area = (VkRect2D) { };
         render->layer_count = 0;
         render->view_mask = inheritance_info->viewMask;
         render->samples = inheritance_info->rasterizationSamples;

         render->color_att_count = inheritance_info->colorAttachmentCount;
         for (uint32_t i = 0; i < render->color_att_count; i++) {
            render->color_att[i].vk_format =
               inheritance_info->pColorAttachmentFormats[i];
         }
         render->depth_att.vk_format =
            inheritance_info->depthAttachmentFormat;
         render->stencil_att.vk_format =
            inheritance_info->stencilAttachmentFormat;
      }
   }
}

static void
nvk_attachment_init(struct nvk_attachment *att,
                    const VkRenderingAttachmentInfo *info)
{
   if (info == NULL || info->imageView == VK_NULL_HANDLE) {
      *att = (struct nvk_attachment) { .iview = NULL, };
      return;
   }

   VK_FROM_HANDLE(nvk_image_view, iview, info->imageView);
   *att = (struct nvk_attachment) {
      .vk_format = iview->vk.format,
      .iview = iview,
   };

   if (info->resolveMode != VK_RESOLVE_MODE_NONE) {
      VK_FROM_HANDLE(nvk_image_view, res_iview, info->resolveImageView);
      assert(iview->vk.format == res_iview->vk.format);

      att->resolve_mode = info->resolveMode;
      att->resolve_iview = res_iview;
   }
}

void
nvk_CmdBeginRendering(VkCommandBuffer commandBuffer,
                      const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nvk_rendering_state *render = &cmd->state.gfx.render;
   struct nv_push *p = P_SPACE(cmd->push, 23 + pRenderingInfo->colorAttachmentCount * 10);

   memset(render, 0, sizeof(*render));

   render->flags = pRenderingInfo->flags;
   render->area = pRenderingInfo->renderArea;
   render->view_mask = pRenderingInfo->viewMask;
   render->layer_count = pRenderingInfo->layerCount;
   render->samples = 0;

   const uint32_t layer_count =
      render->view_mask ? util_last_bit(render->view_mask) :
                          render->layer_count;

   P_MTHD(p, NV9097, SET_SURFACE_CLIP_HORIZONTAL);
   P_NV9097_SET_SURFACE_CLIP_HORIZONTAL(p, {
      .x       = render->area.offset.x,
      .width   = render->area.extent.width,
   });
   P_NV9097_SET_SURFACE_CLIP_VERTICAL(p, {
      .y       = render->area.offset.y,
      .height  = render->area.extent.height,
   });

   render->color_att_count = pRenderingInfo->colorAttachmentCount;
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      nvk_attachment_init(&render->color_att[i],
                          &pRenderingInfo->pColorAttachments[i]);
   }

   nvk_attachment_init(&render->depth_att,
                       pRenderingInfo->pDepthAttachment);
   nvk_attachment_init(&render->stencil_att,
                       pRenderingInfo->pStencilAttachment);

   /* If we don't have any attachments, emit a dummy color attachment */
   if (render->color_att_count == 0 &&
       render->depth_att.iview == NULL &&
       render->stencil_att.iview == NULL)
      render->color_att_count = 1;

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      if (render->color_att[i].iview) {
         const struct nvk_image_view *iview = render->color_att[i].iview;
         const struct nvk_image *image = (struct nvk_image *)iview->vk.image;
         const struct nil_image_level *level =
            &image->nil.levels[iview->vk.base_mip_level];

         assert(render->samples == 0 || render->samples == image->vk.samples);
         render->samples |= image->vk.samples;

         uint64_t addr = nvk_image_base_address(image) + level->offset_B;

         P_MTHD(p, NV9097, SET_COLOR_TARGET_A(i));
         P_NV9097_SET_COLOR_TARGET_A(p, i, addr >> 32);
         P_NV9097_SET_COLOR_TARGET_B(p, i, addr);
         assert(level->tiling.is_tiled);
         P_NV9097_SET_COLOR_TARGET_WIDTH(p, i, iview->vk.extent.width);
         P_NV9097_SET_COLOR_TARGET_HEIGHT(p, i, iview->vk.extent.height);
         const enum pipe_format p_format =
            vk_format_to_pipe_format(iview->vk.format);
         const uint8_t ct_format = nil_format_to_color_target(p_format);
         P_NV9097_SET_COLOR_TARGET_FORMAT(p, i, ct_format);
         P_NV9097_SET_COLOR_TARGET_MEMORY(p, i, {
            .block_width   = BLOCK_WIDTH_ONE_GOB,
            .block_height  = level->tiling.y_log2,
            .block_depth   = level->tiling.z_log2,
            .layout        = LAYOUT_BLOCKLINEAR,
            .third_dimension_control =
               (image->nil.dim == NIL_IMAGE_DIM_3D) ?
               THIRD_DIMENSION_CONTROL_THIRD_DIMENSION_DEFINES_DEPTH_SIZE :
               THIRD_DIMENSION_CONTROL_THIRD_DIMENSION_DEFINES_ARRAY_SIZE,
         });
         P_NV9097_SET_COLOR_TARGET_THIRD_DIMENSION(p, i,
            iview->vk.base_array_layer + layer_count);
         P_NV9097_SET_COLOR_TARGET_ARRAY_PITCH(p, i,
            image->nil.array_stride_B >> 2);
         P_NV9097_SET_COLOR_TARGET_LAYER(p, i, iview->vk.base_array_layer);
      } else {
         P_MTHD(p, NV9097, SET_COLOR_TARGET_A(i));
         P_NV9097_SET_COLOR_TARGET_A(p, i, 0);
         P_NV9097_SET_COLOR_TARGET_B(p, i, 0);
         P_NV9097_SET_COLOR_TARGET_WIDTH(p, i, 64);
         P_NV9097_SET_COLOR_TARGET_HEIGHT(p, i, 0);
         P_NV9097_SET_COLOR_TARGET_FORMAT(p, i, V_DISABLED);
         P_NV9097_SET_COLOR_TARGET_MEMORY(p, i, {
            .layout        = LAYOUT_BLOCKLINEAR,
         });
         P_NV9097_SET_COLOR_TARGET_THIRD_DIMENSION(p, i, layer_count);
         P_NV9097_SET_COLOR_TARGET_ARRAY_PITCH(p, i, 0);
         P_NV9097_SET_COLOR_TARGET_LAYER(p, i, 0);
      }
   }

   P_IMMD(p, NV9097, SET_CT_SELECT, {
      .target_count = render->color_att_count,
      .target0 = 0,
      .target1 = 1,
      .target2 = 2,
      .target3 = 3,
      .target4 = 4,
      .target5 = 5,
      .target6 = 6,
      .target7 = 7,
   });

   if (render->depth_att.iview || render->stencil_att.iview) {
      struct nvk_image_view *iview = render->depth_att.iview ?
                                     render->depth_att.iview :
                                     render->stencil_att.iview;
      const struct nvk_image *image = (struct nvk_image *)iview->vk.image;
      const struct nil_image_level *level =
         &image->nil.levels[iview->vk.base_mip_level];

      assert(render->samples == 0 || render->samples == image->vk.samples);
      render->samples |= image->vk.samples;

      uint64_t addr = nvk_image_base_address(image) + level->offset_B;

      P_MTHD(p, NV9097, SET_ZT_A);
      P_NV9097_SET_ZT_A(p, addr >> 32);
      P_NV9097_SET_ZT_B(p, addr);
      const enum pipe_format p_format =
         vk_format_to_pipe_format(iview->vk.format);
      const uint8_t zs_format = nil_format_to_depth_stencil(p_format);
      P_NV9097_SET_ZT_FORMAT(p, zs_format);
      assert(image->nil.dim != NIL_IMAGE_DIM_3D);
      assert(level->tiling.z_log2 == 0);
      P_NV9097_SET_ZT_BLOCK_SIZE(p, {
         .width = WIDTH_ONE_GOB,
         .height = level->tiling.y_log2,
         .depth = DEPTH_ONE_GOB,
      });
      P_NV9097_SET_ZT_ARRAY_PITCH(p, image->nil.array_stride_B >> 2);

      P_IMMD(p, NV9097, SET_ZT_SELECT, 1 /* target_count */);

      P_MTHD(p, NV9097, SET_ZT_SIZE_A);
      P_NV9097_SET_ZT_SIZE_A(p, iview->vk.extent.width);
      P_NV9097_SET_ZT_SIZE_B(p, iview->vk.extent.height);
      P_NV9097_SET_ZT_SIZE_C(p, {
         .third_dimension  = iview->vk.base_array_layer + layer_count,
         .control          = (image->nil.dim == NIL_IMAGE_DIM_3D) ?
                             CONTROL_ARRAY_SIZE_IS_ONE :
                             CONTROL_THIRD_DIMENSION_DEFINES_ARRAY_SIZE,
      });

      P_IMMD(p, NV9097, SET_ZT_LAYER, iview->vk.base_array_layer);

      if (nvk_cmd_buffer_3d_cls(cmd) >= MAXWELL_B) {
         P_IMMD(p, NVC597, SET_ZT_SPARSE, {
            .enable = ENABLE_FALSE,
         });
      }
   } else {
      P_IMMD(p, NV9097, SET_ZT_SELECT, 0 /* target_count */);
   }

   P_IMMD(p, NV9097, SET_ANTI_ALIAS, ffs(MAX2(1, render->samples)) - 1);

   if (render->flags & VK_RENDERING_RESUMING_BIT)
      return;

   uint32_t clear_count = 0;
   VkClearAttachment clear_att[NVK_MAX_RTS + 1];
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info =
         &pRenderingInfo->pColorAttachments[i];
      if (att_info->imageView == VK_NULL_HANDLE ||
          att_info->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      clear_att[clear_count++] = (VkClearAttachment) {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = i,
         .clearValue = att_info->clearValue,
      };
   }

   clear_att[clear_count] = (VkClearAttachment) { .aspectMask = 0, };
   if (pRenderingInfo->pDepthAttachment != NULL &&
       pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      clear_att[clear_count].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
      clear_att[clear_count].clearValue.depthStencil.depth =
         pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
   }
   if (pRenderingInfo->pStencilAttachment != NULL &&
       pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      clear_att[clear_count].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      clear_att[clear_count].clearValue.depthStencil.stencil =
         pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
   }
   if (clear_att[clear_count].aspectMask != 0)
      clear_count++;

   if (clear_count > 0) {
      const VkClearRect clear_rect = {
         .rect = render->area,
         .baseArrayLayer = 0,
         .layerCount = render->view_mask ? 1 : render->layer_count,
      };
      nvk_CmdClearAttachments(nvk_cmd_buffer_to_handle(cmd),
                              clear_count, clear_att, 1, &clear_rect);
   }

   /* TODO: Attachment clears */
}

void
nvk_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nvk_rendering_state *render = &cmd->state.gfx.render;

   if (!(render->flags & VK_RENDERING_SUSPENDING_BIT)) {
      /* TODO: Attachment resolves */
   }

   /* TODO: Tear down rendering if needed */
   memset(render, 0, sizeof(*render));
}

void
nvk_cmd_bind_graphics_pipeline(struct nvk_cmd_buffer *cmd,
                               struct nvk_graphics_pipeline *pipeline)
{
   cmd->state.gfx.pipeline = pipeline;
   vk_cmd_set_dynamic_graphics_state(&cmd->vk, &pipeline->dynamic);

   struct nv_push *p = P_SPACE(cmd->push, pipeline->push_dw_count);
   nv_push_raw(p, pipeline->push_data, pipeline->push_dw_count);
}

static void
nvk_flush_vi_state(struct nvk_cmd_buffer *cmd)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct nv_push *p = P_SPACE(cmd->push, 256);

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VI)) {
      u_foreach_bit(a, dyn->vi->attributes_valid) {
         const struct nvk_va_format *fmt =
            nvk_get_va_format(pdev, dyn->vi->attributes[a].format);

         P_IMMD(p, NV9097, SET_VERTEX_ATTRIBUTE_A(a), {
            .stream                 = dyn->vi->attributes[a].binding,
            .offset                 = dyn->vi->attributes[a].offset,
            .component_bit_widths   = fmt->bit_widths,
            .numerical_type         = fmt->type,
            .swap_r_and_b           = fmt->swap_rb,
         });
      }

      u_foreach_bit(b, dyn->vi->bindings_valid) {
         const bool instanced = dyn->vi->bindings[b].input_rate ==
                                VK_VERTEX_INPUT_RATE_INSTANCE;
         P_IMMD(p, NV9097, SET_VERTEX_STREAM_INSTANCE_A(b), instanced);
         P_IMMD(p, NV9097, SET_VERTEX_STREAM_A_FREQUENCY(b),
            dyn->vi->bindings[b].divisor);
      }
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VI_BINDING_STRIDES)) {
      for (uint32_t b = 0; b < 32; b++) {
         P_IMMD(p, NV9097, SET_VERTEX_STREAM_A_FORMAT(b), {
            .stride = dyn->vi_binding_strides[b],
            .enable = (dyn->vi->bindings_valid & BITFIELD_BIT(b)) != 0,
         });
      }
   }
}

static void
nvk_flush_ia_state(struct nvk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   /** Nothing to do for MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY */

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE)) {
      struct nv_push *p = P_SPACE(cmd->push, 2);
      P_IMMD(p, NV9097, SET_DA_PRIMITIVE_RESTART,
             dyn->ia.primitive_restart_enable);
   }
}

static void
nvk_flush_ts_state(struct nvk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS)) {
      struct nv_push *p = P_SPACE(cmd->push, 2);
      P_IMMD(p, NV9097, SET_PATCH, dyn->ts.patch_control_points);
   }
}

static void
nvk_flush_vp_state(struct nvk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct nv_push *p =
      P_SPACE(cmd->push, 14 * dyn->vp.viewport_count + 4 * NVK_MAX_VIEWPORTS);

   /* Nothing to do for MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT */

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS)) {
      for (uint32_t i = 0; i < dyn->vp.viewport_count; i++) {
         const VkViewport *vp = &dyn->vp.viewports[i];

         P_MTHD(p, NV9097, SET_VIEWPORT_SCALE_X(i));
         P_NV9097_SET_VIEWPORT_SCALE_X(p, i, fui(0.5f * vp->width));
         P_NV9097_SET_VIEWPORT_SCALE_Y(p, i, fui(0.5f * vp->height));
         P_NV9097_SET_VIEWPORT_SCALE_Z(p, i, fui(vp->maxDepth - vp->minDepth));

         P_NV9097_SET_VIEWPORT_OFFSET_X(p, i, fui(vp->x + 0.5f * vp->width));
         P_NV9097_SET_VIEWPORT_OFFSET_Y(p, i, fui(vp->y + 0.5f * vp->height));
         P_NV9097_SET_VIEWPORT_OFFSET_Z(p, i, fui(vp->minDepth));

         const uint32_t xmin = vp->x;
         const uint32_t xmax = vp->x + vp->width;
         const uint32_t ymin = MIN2(vp->y, vp->y + vp->height);
         const uint32_t ymax = MAX2(vp->y, vp->y + vp->height);
         assert(xmin <= xmax && ymin <= ymax);

         P_MTHD(p, NV9097, SET_VIEWPORT_CLIP_HORIZONTAL(i));
         P_NV9097_SET_VIEWPORT_CLIP_HORIZONTAL(p, i, {
            .x0      = xmin,
            .width   = xmax - xmin,
         });
         P_NV9097_SET_VIEWPORT_CLIP_VERTICAL(p, i, {
            .y0      = ymin,
            .height  = ymax - ymin,
         });
         P_NV9097_SET_VIEWPORT_CLIP_MIN_Z(p, i, fui(vp->minDepth));
         P_NV9097_SET_VIEWPORT_CLIP_MAX_Z(p, i, fui(vp->maxDepth));

         if (nvk_cmd_buffer_3d_cls(cmd) >= MAXWELL_B) {
            P_IMMD(p, NVB197, SET_VIEWPORT_COORDINATE_SWIZZLE(i), {
               .x = X_POS_X,
               .y = Y_POS_Y,
               .z = Z_POS_Z,
               .w = W_POS_W,
            });
         }
      }
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT)) {
      for (unsigned i = dyn->vp.scissor_count; i < NVK_MAX_VIEWPORTS; i++)
         P_IMMD(p, NV9097, SET_SCISSOR_ENABLE(i), V_FALSE);
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_SCISSORS)) {
      for (unsigned i = 0; i < dyn->vp.scissor_count; i++) {
         const VkRect2D *s = &dyn->vp.scissors[i];

         const uint32_t xmin = MIN2(16384, s->offset.x);
         const uint32_t xmax = MIN2(16384, s->offset.x + s->extent.width);
         const uint32_t ymin = MIN2(16384, s->offset.y);
         const uint32_t ymax = MIN2(16384, s->offset.y + s->extent.height);

         P_MTHD(p, NV9097, SET_SCISSOR_ENABLE(i));
         P_NV9097_SET_SCISSOR_ENABLE(p, i, V_TRUE);
         P_NV9097_SET_SCISSOR_HORIZONTAL(p, i, {
            .xmin = xmin,
            .xmax = xmax,
         });
         P_NV9097_SET_SCISSOR_VERTICAL(p, i, {
            .ymin = ymin,
            .ymax = ymax,
         });
      }
   }
}

static uint32_t
vk_to_nv9097_cull_mode(VkCullModeFlags vk_cull_mode)
{
   static const uint16_t vk_to_nv9097[] = {
      [VK_CULL_MODE_FRONT_BIT]      = NV9097_OGL_SET_CULL_FACE_V_FRONT,
      [VK_CULL_MODE_BACK_BIT]       = NV9097_OGL_SET_CULL_FACE_V_BACK,
      [VK_CULL_MODE_FRONT_AND_BACK] = NV9097_OGL_SET_CULL_FACE_V_FRONT_AND_BACK,
   };
   assert(vk_cull_mode < ARRAY_SIZE(vk_to_nv9097));
   return vk_to_nv9097[vk_cull_mode];
}

static uint32_t
vk_to_nv9097_front_face(VkFrontFace vk_face)
{
   /* Vulkan and OpenGL are backwards here because Vulkan assumes the D3D
    * convention in which framebuffer coordinates always start in the upper
    * left while OpenGL has framebuffer coordinates starting in the lower
    * left.  Therefore, we want the reverse of the hardware enum name.
    */
   ASSERTED static const uint16_t vk_to_nv9097[] = {
      [VK_FRONT_FACE_COUNTER_CLOCKWISE]   = NV9097_OGL_SET_FRONT_FACE_V_CCW,
      [VK_FRONT_FACE_CLOCKWISE]           = NV9097_OGL_SET_FRONT_FACE_V_CW,
   };
   assert(vk_face < ARRAY_SIZE(vk_to_nv9097));

   uint32_t nv9097_face = 0x900 | (1 - vk_face);
   assert(nv9097_face == vk_to_nv9097[vk_face]);
   return nv9097_face;
}

static void
nvk_flush_rs_state(struct nvk_cmd_buffer *cmd)
{
   struct nv_push *p = P_SPACE(cmd->push, 23);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE))
      P_IMMD(p, NV9097, SET_RASTER_ENABLE, !dyn->rs.rasterizer_discard_enable);

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE)) {
      P_IMMD(p, NV9097, OGL_SET_CULL, dyn->rs.cull_mode != VK_CULL_MODE_NONE);

      if (dyn->rs.cull_mode != VK_CULL_MODE_NONE) {
         uint32_t face = vk_to_nv9097_cull_mode(dyn->rs.cull_mode);
         P_IMMD(p, NV9097, OGL_SET_CULL_FACE, face);
      }
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE)) {
      P_IMMD(p, NV9097, OGL_SET_FRONT_FACE,
         vk_to_nv9097_front_face(dyn->rs.front_face));
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) {
      P_MTHD(p, NV9097, SET_POLY_OFFSET_POINT);
      P_NV9097_SET_POLY_OFFSET_POINT(p, dyn->rs.depth_bias.enable);
      P_NV9097_SET_POLY_OFFSET_LINE(p, dyn->rs.depth_bias.enable);
      P_NV9097_SET_POLY_OFFSET_FILL(p, dyn->rs.depth_bias.enable);
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS)) {
      P_IMMD(p, NV9097, SET_DEPTH_BIAS, fui(dyn->rs.depth_bias.constant));
      P_IMMD(p, NV9097, SET_SLOPE_SCALE_DEPTH_BIAS, fui(dyn->rs.depth_bias.slope));
      P_IMMD(p, NV9097, SET_DEPTH_BIAS_CLAMP, fui(dyn->rs.depth_bias.clamp));
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_WIDTH)) {
      P_MTHD(p, NV9097, SET_LINE_WIDTH_FLOAT);
      P_NV9097_SET_LINE_WIDTH_FLOAT(p, fui(dyn->rs.line.width));
      P_NV9097_SET_ALIASED_LINE_WIDTH_FLOAT(p, fui(dyn->rs.line.width));
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_STIPPLE)) {
      P_IMMD(p, NV9097, SET_LINE_STIPPLE_PARAMETERS, {
         .factor  = dyn->rs.line.stipple.factor,
         .pattern = dyn->rs.line.stipple.pattern,
      });
   }
}

static uint32_t
vk_to_nv9097_compare_op(VkCompareOp vk_op)
{
   ASSERTED static const uint16_t vk_to_nv9097[] = {
      [VK_COMPARE_OP_NEVER]            = NV9097_SET_DEPTH_FUNC_V_OGL_NEVER,
      [VK_COMPARE_OP_LESS]             = NV9097_SET_DEPTH_FUNC_V_OGL_LESS,
      [VK_COMPARE_OP_EQUAL]            = NV9097_SET_DEPTH_FUNC_V_OGL_EQUAL,
      [VK_COMPARE_OP_LESS_OR_EQUAL]    = NV9097_SET_DEPTH_FUNC_V_OGL_LEQUAL,
      [VK_COMPARE_OP_GREATER]          = NV9097_SET_DEPTH_FUNC_V_OGL_GREATER,
      [VK_COMPARE_OP_NOT_EQUAL]        = NV9097_SET_DEPTH_FUNC_V_OGL_NOTEQUAL,
      [VK_COMPARE_OP_GREATER_OR_EQUAL] = NV9097_SET_DEPTH_FUNC_V_OGL_GEQUAL,
      [VK_COMPARE_OP_ALWAYS]           = NV9097_SET_DEPTH_FUNC_V_OGL_ALWAYS,
   };
   assert(vk_op < ARRAY_SIZE(vk_to_nv9097));

   uint32_t nv9097_op = 0x200 | vk_op;
   assert(nv9097_op == vk_to_nv9097[vk_op]);
   return nv9097_op;
}

static uint32_t
vk_to_nv9097_stencil_op(VkStencilOp vk_op)
{
#define OP(vk, nv) [VK_STENCIL_OP_##vk] = NV9097_SET_STENCIL_OP_FAIL_V_##nv
   ASSERTED static const uint16_t vk_to_nv9097[] = {
      OP(KEEP,                D3D_KEEP),
      OP(ZERO,                D3D_ZERO),
      OP(REPLACE,             D3D_REPLACE),
      OP(INCREMENT_AND_CLAMP, D3D_INCRSAT),
      OP(DECREMENT_AND_CLAMP, D3D_DECRSAT),
      OP(INVERT,              D3D_INVERT),
      OP(INCREMENT_AND_WRAP,  D3D_INCR),
      OP(DECREMENT_AND_WRAP,  D3D_DECR),
   };
   assert(vk_op < ARRAY_SIZE(vk_to_nv9097));
#undef OP

   uint32_t nv9097_op = vk_op + 1;
   assert(nv9097_op == vk_to_nv9097[vk_op]);
   return nv9097_op;
}

static void
nvk_flush_ds_state(struct nvk_cmd_buffer *cmd)
{
   struct nv_push *p = P_SPACE(cmd->push, 35);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE))
      P_IMMD(p, NV9097, SET_DEPTH_TEST, dyn->ds.depth.test_enable);

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE))
      P_IMMD(p, NV9097, SET_DEPTH_WRITE, dyn->ds.depth.write_enable);

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP)) {
      const uint32_t func = vk_to_nv9097_compare_op(dyn->ds.depth.compare_op);
      P_IMMD(p, NV9097, SET_DEPTH_FUNC, func);
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE))
      P_IMMD(p, NV9097, SET_DEPTH_BOUNDS_TEST, dyn->ds.depth.bounds_test.enable);

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_BOUNDS)) {
      P_MTHD(p, NV9097, SET_DEPTH_BOUNDS_MIN);
      P_NV9097_SET_DEPTH_BOUNDS_MIN(p, fui(dyn->ds.depth.bounds_test.min));
      P_NV9097_SET_DEPTH_BOUNDS_MAX(p, fui(dyn->ds.depth.bounds_test.max));
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE))
      P_IMMD(p, NV9097, SET_STENCIL_TEST, dyn->ds.stencil.test_enable);

   const struct vk_stencil_test_face_state *front = &dyn->ds.stencil.front;
   const struct vk_stencil_test_face_state *back = &dyn->ds.stencil.back;
   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_OP)) {
      P_MTHD(p, NV9097, SET_STENCIL_OP_FAIL);
      P_NV9097_SET_STENCIL_OP_FAIL(p, vk_to_nv9097_stencil_op(front->op.fail));
      P_NV9097_SET_STENCIL_OP_ZFAIL(p, vk_to_nv9097_stencil_op(front->op.depth_fail));
      P_NV9097_SET_STENCIL_OP_ZPASS(p, vk_to_nv9097_stencil_op(front->op.pass));
      P_NV9097_SET_STENCIL_FUNC(p, vk_to_nv9097_compare_op(front->op.compare));

      P_MTHD(p, NV9097, SET_BACK_STENCIL_OP_FAIL);
      P_NV9097_SET_BACK_STENCIL_OP_FAIL(p, vk_to_nv9097_stencil_op(back->op.fail));
      P_NV9097_SET_BACK_STENCIL_OP_ZFAIL(p, vk_to_nv9097_stencil_op(back->op.depth_fail));
      P_NV9097_SET_BACK_STENCIL_OP_ZPASS(p, vk_to_nv9097_stencil_op(back->op.pass));
      P_NV9097_SET_BACK_STENCIL_FUNC(p, vk_to_nv9097_compare_op(back->op.compare));
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK)) {
      P_IMMD(p, NV9097, SET_STENCIL_FUNC_MASK, front->compare_mask);
      P_IMMD(p, NV9097, SET_BACK_STENCIL_FUNC_MASK, back->compare_mask);
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK)) {
      P_IMMD(p, NV9097, SET_STENCIL_MASK, front->write_mask);
      P_IMMD(p, NV9097, SET_BACK_STENCIL_MASK, back->write_mask);
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
      P_IMMD(p, NV9097, SET_STENCIL_FUNC_REF, front->reference);
      P_IMMD(p, NV9097, SET_BACK_STENCIL_FUNC_REF, back->reference);
   }
}

static uint32_t
vk_to_nv9097_logic_op(VkLogicOp vk_op)
{
   ASSERTED uint16_t vk_to_nv9097[] = {
      [VK_LOGIC_OP_CLEAR]           = NV9097_SET_LOGIC_OP_FUNC_V_CLEAR,
      [VK_LOGIC_OP_AND]             = NV9097_SET_LOGIC_OP_FUNC_V_AND,
      [VK_LOGIC_OP_AND_REVERSE]     = NV9097_SET_LOGIC_OP_FUNC_V_AND_REVERSE,
      [VK_LOGIC_OP_COPY]            = NV9097_SET_LOGIC_OP_FUNC_V_COPY,
      [VK_LOGIC_OP_AND_INVERTED]    = NV9097_SET_LOGIC_OP_FUNC_V_AND_INVERTED,
      [VK_LOGIC_OP_NO_OP]           = NV9097_SET_LOGIC_OP_FUNC_V_NOOP,
      [VK_LOGIC_OP_XOR]             = NV9097_SET_LOGIC_OP_FUNC_V_XOR,
      [VK_LOGIC_OP_OR]              = NV9097_SET_LOGIC_OP_FUNC_V_OR,
      [VK_LOGIC_OP_NOR]             = NV9097_SET_LOGIC_OP_FUNC_V_NOR,
      [VK_LOGIC_OP_EQUIVALENT]      = NV9097_SET_LOGIC_OP_FUNC_V_EQUIV,
      [VK_LOGIC_OP_INVERT]          = NV9097_SET_LOGIC_OP_FUNC_V_INVERT,
      [VK_LOGIC_OP_OR_REVERSE]      = NV9097_SET_LOGIC_OP_FUNC_V_OR_REVERSE,
      [VK_LOGIC_OP_COPY_INVERTED]   = NV9097_SET_LOGIC_OP_FUNC_V_COPY_INVERTED,
      [VK_LOGIC_OP_OR_INVERTED]     = NV9097_SET_LOGIC_OP_FUNC_V_OR_INVERTED,
      [VK_LOGIC_OP_NAND]            = NV9097_SET_LOGIC_OP_FUNC_V_NAND,
      [VK_LOGIC_OP_SET]             = NV9097_SET_LOGIC_OP_FUNC_V_SET,
   };
   assert(vk_op < ARRAY_SIZE(vk_to_nv9097));

   uint32_t nv9097_op = 0x1500 | vk_op;
   assert(nv9097_op == vk_to_nv9097[vk_op]);
   return nv9097_op;
}

static void
nvk_flush_cb_state(struct nvk_cmd_buffer *cmd)
{
   struct nv_push *p = P_SPACE(cmd->push, 7);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_LOGIC_OP)) {
      const uint32_t func = vk_to_nv9097_logic_op(dyn->cb.logic_op);
      P_IMMD(p, NV9097, SET_LOGIC_OP_FUNC, func);
   }

   /* MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES */

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS)) {
      P_MTHD(p, NV9097, SET_BLEND_CONST_RED);
      P_NV9097_SET_BLEND_CONST_RED(p,     fui(dyn->cb.blend_constants[0]));
      P_NV9097_SET_BLEND_CONST_GREEN(p,   fui(dyn->cb.blend_constants[1]));
      P_NV9097_SET_BLEND_CONST_BLUE(p,    fui(dyn->cb.blend_constants[2]));
      P_NV9097_SET_BLEND_CONST_ALPHA(p,   fui(dyn->cb.blend_constants[3]));
   }
}

static void
nvk_flush_dynamic_state(struct nvk_cmd_buffer *cmd)
{
   struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   if (!vk_dynamic_graphics_state_any_dirty(dyn))
      return;

   nvk_flush_vi_state(cmd);
   nvk_flush_ia_state(cmd);
   nvk_flush_ts_state(cmd);
   nvk_flush_vp_state(cmd);
   nvk_flush_rs_state(cmd);

   /* MESA_VK_DYNAMIC_FSR */
   /* MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS */

   nvk_flush_ds_state(cmd);
   nvk_flush_cb_state(cmd);

   vk_dynamic_graphics_state_clear_dirty(dyn);
}

static void
nvk_flush_descriptors(struct nvk_cmd_buffer *cmd)
{
   const struct nvk_descriptor_state *desc = &cmd->state.gfx.descriptors;
   VkResult result;

   uint32_t root_table_size = sizeof(desc->root);
   void *root_table_map;
   uint64_t root_table_addr;
   result = nvk_cmd_buffer_upload_alloc(cmd, root_table_size,
                                        &root_table_addr, &root_table_map);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   memcpy(root_table_map, &desc->root, sizeof(desc->root));

   struct nv_push *p = P_SPACE(cmd->push, 26);

   P_MTHD(p, NV9097, SET_CONSTANT_BUFFER_SELECTOR_A);
   P_NV9097_SET_CONSTANT_BUFFER_SELECTOR_A(p, root_table_size);
   P_NV9097_SET_CONSTANT_BUFFER_SELECTOR_B(p, root_table_addr >> 32);
   P_NV9097_SET_CONSTANT_BUFFER_SELECTOR_C(p, root_table_addr);

   for (uint32_t i = 0; i < 5; i++) {
      P_IMMD(p, NV9097, BIND_GROUP_CONSTANT_BUFFER(i), {
         .valid = VALID_TRUE,
         .shader_slot = 0,
      });
      P_IMMD(p, NV9097, BIND_GROUP_CONSTANT_BUFFER(i), {
         .valid = VALID_TRUE,
         .shader_slot = 1,
      });
   }

   assert(nvk_cmd_buffer_3d_cls(cmd) >= KEPLER_A);
   P_IMMD(p, NVA097, INVALIDATE_SHADER_CACHES_NO_WFI, {
      .constant = CONSTANT_TRUE,
   });
}

static void
nvk_flush_gfx_state(struct nvk_cmd_buffer *cmd)
{
   nvk_flush_dynamic_state(cmd);
   nvk_flush_descriptors(cmd);
}

static uint32_t
vk_to_nv_index_format(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT16:
      return NVC597_SET_INDEX_BUFFER_E_INDEX_SIZE_TWO_BYTES;
   case VK_INDEX_TYPE_UINT32:
      return NVC597_SET_INDEX_BUFFER_E_INDEX_SIZE_FOUR_BYTES;
   case VK_INDEX_TYPE_UINT8_EXT:
      return NVC597_SET_INDEX_BUFFER_E_INDEX_SIZE_ONE_BYTE;
   default:
      unreachable("Invalid index type");
   }
}

static uint32_t
vk_index_to_restart(VkIndexType index_type)
{
   switch (index_type) {
   case VK_INDEX_TYPE_UINT16:
      return 0xffff;
   case VK_INDEX_TYPE_UINT32:
      return 0xffffffff;
   case VK_INDEX_TYPE_UINT8_EXT:
      return 0xff;
   default:
      unreachable("unexpected index type");
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                       VkBuffer _buffer,
                       VkDeviceSize offset,
                       VkIndexType indexType)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);

   struct nv_push *p = P_SPACE(cmd->push, 10);

   uint64_t addr, range;
   if (buffer) {
      addr = nvk_buffer_address(buffer, offset);
      range = vk_buffer_range(&buffer->vk, offset, VK_WHOLE_SIZE);
   } else {
      range = addr = 0;
   }

   P_IMMD(p, NV9097, SET_DA_PRIMITIVE_RESTART_INDEX,
          vk_index_to_restart(indexType));

   P_MTHD(p, NV9097, SET_INDEX_BUFFER_A);
   P_NV9097_SET_INDEX_BUFFER_A(p, addr >> 32);
   P_NV9097_SET_INDEX_BUFFER_B(p, addr);

   if (nvk_cmd_buffer_3d_cls(cmd) >= TURING_A) {
      P_MTHD(p, NVC597, SET_INDEX_BUFFER_SIZE_A);
      P_NVC597_SET_INDEX_BUFFER_SIZE_A(p, range >> 32);
      P_NVC597_SET_INDEX_BUFFER_SIZE_B(p, range);
   } else {
      uint64_t limit = addr + range;
      P_MTHD(p, NV9097, SET_INDEX_BUFFER_C);
      P_NV9097_SET_INDEX_BUFFER_C(p, limit >> 32);
      P_NV9097_SET_INDEX_BUFFER_D(p, limit);
   }

   P_IMMD(p, NV9097, SET_INDEX_BUFFER_E, vk_to_nv_index_format(indexType));
}

void
nvk_cmd_bind_vertex_buffer(struct nvk_cmd_buffer *cmd, uint32_t vb_idx,
                           struct nvk_addr_range addr_range)
{
   struct nv_push *p = P_SPACE(cmd->push, 6);

   P_MTHD(p, NV9097, SET_VERTEX_STREAM_A_LOCATION_A(vb_idx));
   P_NV9097_SET_VERTEX_STREAM_A_LOCATION_A(p, vb_idx, addr_range.addr >> 32);
   P_NV9097_SET_VERTEX_STREAM_A_LOCATION_B(p, vb_idx, addr_range.addr);

   if (nvk_cmd_buffer_3d_cls(cmd) >= TURING_A) {
      P_MTHD(p, NVC597, SET_VERTEX_STREAM_SIZE_A(vb_idx));
      P_NVC597_SET_VERTEX_STREAM_SIZE_A(p, vb_idx, addr_range.range >> 32);
      P_NVC597_SET_VERTEX_STREAM_SIZE_B(p, vb_idx, addr_range.range);
   } else {
      uint64_t limit = addr_range.addr + addr_range.range - 1;
      P_MTHD(p, NV9097, SET_VERTEX_STREAM_LIMIT_A_A(vb_idx));
      P_NV9097_SET_VERTEX_STREAM_LIMIT_A_A(p, vb_idx, limit >> 32);
      P_NV9097_SET_VERTEX_STREAM_LIMIT_A_B(p, vb_idx, limit);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer,
                          uint32_t firstBinding,
                          uint32_t bindingCount,
                          const VkBuffer *pBuffers,
                          const VkDeviceSize *pOffsets,
                          const VkDeviceSize *pSizes,
                          const VkDeviceSize *pStrides)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pStrides) {
      vk_cmd_set_vertex_binding_strides(&cmd->vk, firstBinding,
                                        bindingCount, pStrides);
   }

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(nvk_buffer, buffer, pBuffers[i]);
      uint32_t idx = firstBinding + i;

      uint64_t size = pSizes ? pSizes[i] : VK_WHOLE_SIZE;
      struct nvk_addr_range addr_range = { };
      if (buffer) {
         addr_range.addr = nvk_buffer_address(buffer, pOffsets[i]);
         addr_range.range = vk_buffer_range(&buffer->vk, pOffsets[i], size);
      }

      /* Used for meta save/restore */
      if (idx == 0)
         cmd->state.gfx.vb0 = addr_range;

      nvk_cmd_bind_vertex_buffer(cmd, idx, addr_range);
   }
}

static uint32_t
vk_to_nv9097_primitive_topology(VkPrimitiveTopology prim)
{
   switch (prim) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return NV9097_BEGIN_OP_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return NV9097_BEGIN_OP_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return NV9097_BEGIN_OP_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
   case VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA:
#pragma GCC diagnostic pop
      return NV9097_BEGIN_OP_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return NV9097_BEGIN_OP_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return NV9097_BEGIN_OP_TRIANGLE_FAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      return NV9097_BEGIN_OP_LINELIST_ADJCY;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return NV9097_BEGIN_OP_LINESTRIP_ADJCY;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      return NV9097_BEGIN_OP_TRIANGLELIST_ADJCY;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      return NV9097_BEGIN_OP_TRIANGLESTRIP_ADJCY;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      return NV9097_BEGIN_OP_PATCH;
   default:
      unreachable("Invalid primitive topology");
   }
}

static void
nvk_build_mme_draw(struct mme_builder *b, struct mme_value begin)
{
   /* These are in VkDrawIndirectCommand order */
   struct mme_value vertex_count = mme_load(b);
   struct mme_value instance_count = mme_load(b);
   struct mme_value first_vertex = mme_load(b);
   struct mme_value first_instance = mme_load(b);

   mme_mthd(b, NV9097_SET_GLOBAL_BASE_INSTANCE_INDEX);
   mme_emit(b, first_instance);

   mme_loop(b, instance_count) {
      mme_mthd(b, NV9097_BEGIN);
      mme_emit(b, begin);

      mme_mthd(b, NV9097_SET_VERTEX_ARRAY_START);
      mme_emit(b, first_vertex);
      mme_emit(b, vertex_count);

      mme_mthd(b, NV9097_END);
      mme_emit(b, mme_zero());

      mme_set_field_enum(b, begin, NV9097_BEGIN_INSTANCE_ID, SUBSEQUENT);
   }
}

void
nvk_mme_draw(struct nvk_device *dev, struct mme_builder *b)
{
   struct mme_value begin = mme_load(b);

   nvk_build_mme_draw(b, begin);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDraw(VkCommandBuffer commandBuffer,
            uint32_t vertexCount,
            uint32_t instanceCount,
            uint32_t firstVertex,
            uint32_t firstInstance)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   nvk_flush_gfx_state(cmd);

   uint32_t begin;
   V_NV9097_BEGIN(begin, {
      .op = vk_to_nv9097_primitive_topology(dyn->ia.primitive_topology),
      .primitive_id = NV9097_BEGIN_PRIMITIVE_ID_FIRST,
      .instance_id = NV9097_BEGIN_INSTANCE_ID_FIRST,
      .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
   });

   struct nv_push *p = P_SPACE(cmd->push, 6);
   P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_DRAW));
   P_INLINE_DATA(p, begin);
   P_INLINE_DATA(p, vertexCount);
   P_INLINE_DATA(p, instanceCount);
   P_INLINE_DATA(p, firstVertex);
   P_INLINE_DATA(p, firstInstance);
}

static void
nvk_mme_build_draw_indexed(struct mme_builder *b,
                           struct mme_value begin)
{
   /* These are in VkDrawIndexedIndirectCommand order */
   struct mme_value index_count = mme_load(b);
   struct mme_value instance_count = mme_load(b);
   struct mme_value first_index = mme_load(b);
   struct mme_value vertex_offset = mme_load(b);
   struct mme_value first_instance = mme_load(b);

   mme_mthd(b, NV9097_SET_GLOBAL_BASE_VERTEX_INDEX);
   mme_emit(b, vertex_offset);

   mme_mthd(b, NV9097_SET_VERTEX_ID_BASE);
   mme_emit(b, vertex_offset);

   mme_mthd(b, NV9097_SET_GLOBAL_BASE_INSTANCE_INDEX);
   mme_emit(b, first_instance);

   mme_loop(b, instance_count) {
      mme_mthd(b, NV9097_BEGIN);
      mme_emit(b, begin);

      mme_mthd(b, NV9097_SET_INDEX_BUFFER_F);
      mme_emit(b, first_index);
      mme_emit(b, index_count);

      mme_mthd(b, NV9097_END);
      mme_emit(b, mme_zero());

      mme_set_field_enum(b, begin, NV9097_BEGIN_INSTANCE_ID, SUBSEQUENT);
   }
}

void
nvk_mme_draw_indexed(struct nvk_device *dev, struct mme_builder *b)
{
   struct mme_value begin = mme_load(b);

   nvk_mme_build_draw_indexed(b, begin);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                   uint32_t indexCount,
                   uint32_t instanceCount,
                   uint32_t firstIndex,
                   int32_t vertexOffset,
                   uint32_t firstInstance)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   nvk_flush_gfx_state(cmd);

   uint32_t begin;
   V_NV9097_BEGIN(begin, {
      .op = vk_to_nv9097_primitive_topology(dyn->ia.primitive_topology),
      .primitive_id = NV9097_BEGIN_PRIMITIVE_ID_FIRST,
      .instance_id = NV9097_BEGIN_INSTANCE_ID_FIRST,
      .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
   });

   struct nv_push *p = P_SPACE(cmd->push, 7);
   P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_DRAW_INDEXED));
   P_INLINE_DATA(p, begin);
   P_INLINE_DATA(p, indexCount);
   P_INLINE_DATA(p, instanceCount);
   P_INLINE_DATA(p, firstIndex);
   P_INLINE_DATA(p, vertexOffset);
   P_INLINE_DATA(p, firstInstance);
}

static inline void
mme_read_fifoed(struct mme_builder *b,
                struct mme_value64 addr,
                uint32_t count)
{
   mme_mthd(b, NVC597_SET_MME_MEM_ADDRESS_A);
   mme_emit(b, addr.hi);
   mme_emit(b, addr.lo);

   mme_mthd(b, NVC597_MME_DMA_READ_FIFOED);
   mme_emit(b, mme_imm(count));

   mme_tu104_alu_no_dst(b, MME_TU104_ALU_OP_EXTENDED,
                        mme_imm(0x1000), mme_imm(1), 0);
}

void
nvk_mme_draw_indirect(struct nvk_device *dev, struct mme_builder *b)
{
   struct mme_value begin = mme_load(b);
   struct mme_value draw_addr_hi = mme_load(b);
   struct mme_value draw_addr_lo = mme_load(b);
   struct mme_value64 draw_addr = mme_value64(draw_addr_lo, draw_addr_hi);
   struct mme_value draw_count = mme_load(b);
   struct mme_value stride = mme_load(b);

   struct mme_value draw = mme_mov(b, mme_zero());
   mme_while(b, ult, draw, draw_count) {
      mme_read_fifoed(b, draw_addr, 4);

      nvk_build_mme_draw(b, begin);

      mme_add_to(b, draw, draw, mme_imm(1));
      mme_add64_to(b, draw_addr, draw_addr, mme_value64(stride, mme_zero()));
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                    VkBuffer _buffer,
                    VkDeviceSize offset,
                    uint32_t drawCount,
                    uint32_t stride)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   nvk_flush_gfx_state(cmd);

   uint32_t begin;
   V_NV9097_BEGIN(begin, {
      .op = vk_to_nv9097_primitive_topology(dyn->ia.primitive_topology),
      .primitive_id = NV9097_BEGIN_PRIMITIVE_ID_FIRST,
      .instance_id = NV9097_BEGIN_INSTANCE_ID_FIRST,
      .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
   });

   struct nv_push *p = P_SPACE(cmd->push, 8);
   P_IMMD(p, NVC597, SET_MME_DATA_FIFO_CONFIG, FIFO_SIZE_SIZE_4KB);
   P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT));
   P_INLINE_DATA(p, begin);
   uint64_t draw_addr = nvk_buffer_address(buffer, offset);
   P_INLINE_DATA(p, draw_addr >> 32);
   P_INLINE_DATA(p, draw_addr);
   P_INLINE_DATA(p, drawCount);
   P_INLINE_DATA(p, stride);
}

void
nvk_mme_draw_indexed_indirect(struct nvk_device *dev, struct mme_builder *b)
{
   struct mme_value begin = mme_load(b);
   struct mme_value draw_addr_hi = mme_load(b);
   struct mme_value draw_addr_lo = mme_load(b);
   struct mme_value64 draw_addr = mme_value64(draw_addr_lo, draw_addr_hi);
   struct mme_value draw_count = mme_load(b);
   struct mme_value stride = mme_load(b);

   struct mme_value draw = mme_mov(b, mme_zero());
   mme_while(b, ult, draw, draw_count) {
      mme_read_fifoed(b, draw_addr, 5);

      nvk_mme_build_draw_indexed(b, begin);

      mme_add_to(b, draw, draw, mme_imm(1));
      mme_add64_to(b, draw_addr, draw_addr, mme_value64(stride, mme_zero()));
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                           VkBuffer _buffer,
                           VkDeviceSize offset,
                           uint32_t drawCount,
                           uint32_t stride)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   nvk_flush_gfx_state(cmd);

   uint32_t begin;
   V_NV9097_BEGIN(begin, {
      .op = vk_to_nv9097_primitive_topology(dyn->ia.primitive_topology),
      .primitive_id = NV9097_BEGIN_PRIMITIVE_ID_FIRST,
      .instance_id = NV9097_BEGIN_INSTANCE_ID_FIRST,
      .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
   });

   struct nv_push *p = P_SPACE(cmd->push, 8);
   P_IMMD(p, NVC597, SET_MME_DATA_FIFO_CONFIG, FIFO_SIZE_SIZE_4KB);
   P_1INC(p, NV9097, CALL_MME_MACRO(NVK_MME_DRAW_INDEXED_INDIRECT));
   P_INLINE_DATA(p, begin);
   uint64_t draw_addr = nvk_buffer_address(buffer, offset);
   P_INLINE_DATA(p, draw_addr >> 32);
   P_INLINE_DATA(p, draw_addr);
   P_INLINE_DATA(p, drawCount);
   P_INLINE_DATA(p, stride);
}

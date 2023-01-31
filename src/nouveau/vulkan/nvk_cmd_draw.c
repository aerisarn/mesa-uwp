#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"

#include "nvk_cl9097.h"
#include "nvk_cla097.h"
#include "nvk_clb197.h"
#include "nvk_clc397.h"
#include "nvk_clc597.h"

static inline uint16_t
nvk_cmd_buffer_3d_cls(struct nvk_cmd_buffer *cmd)
{
   return nvk_cmd_buffer_device(cmd)->ctx->eng3d.cls;
}

static void
magic_3d_init(struct nvk_cmd_buffer *cmd)
{
   struct nouveau_ws_push *p = cmd->push;

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
   if (nvk_cmd_buffer_3d_cls(cmd) < VOLTA_A)
      P_IMMD(p, NV9097, SET_ALPHA_FRACTION, 0x3f);

   P_IMMD(p, NV9097, CHECK_SPH_VERSION, {
      .current = 3,
      .oldest_supported = 3,
   });
   P_IMMD(p, NV9097, CHECK_AAM_VERSION, {
      .current = 2,
      .oldest_supported = 2,
   });

   if (nvk_cmd_buffer_3d_cls(cmd) < VOLTA_A)
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

   if (nvk_cmd_buffer_3d_cls(cmd) < VOLTA_A)
      P_IMMD(p, NV9097, SET_PRIM_CIRCULAR_BUFFER_THROTTLE, 0x3fffff);

   P_IMMD(p, NV9097, SET_BLEND_OPT_CONTROL, ALLOW_FLOAT_PIXEL_KILLS_TRUE);
   P_IMMD(p, NV9097, SET_BLEND_FLOAT_OPTION, ZERO_TIMES_ANYTHING_IS_ZERO_TRUE);

   if (nvk_cmd_buffer_3d_cls(cmd) < VOLTA_A)
      P_IMMD(p, NV9097, SET_MAX_TI_WARPS_PER_BATCH, 3);

   if (nvk_cmd_buffer_3d_cls(cmd) >= KEPLER_A &&
       nvk_cmd_buffer_3d_cls(cmd) < VOLTA_A) {
      P_IMMD(p, NVA097, SET_TEXTURE_INSTRUCTION_OPERAND,
                        ORDERING_KEPLER_ORDER);
   }
}

void
nvk_cmd_buffer_begin_graphics(struct nvk_cmd_buffer *cmd,
                              const VkCommandBufferBeginInfo *pBeginInfo)
{
   struct nouveau_ws_push *p = cmd->push;

   P_MTHD(p, NV9097, SET_OBJECT);
   P_NV9097_SET_OBJECT(p, {
      .class_id = nvk_cmd_buffer_3d_cls(cmd),
      .engine_id = 0,
   });

   P_IMMD(p, NV9097, SET_RENDER_ENABLE_C, MODE_TRUE);

   P_IMMD(p, NV9097, SET_Z_COMPRESSION, ENABLE_TRUE);
   P_MTHD(p, NV9097, SET_COLOR_COMPRESSION(0));
   for (unsigned i = 0; i < 8; i++)
      P_NV9097_SET_COLOR_COMPRESSION(p, i, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_CT_SELECT, { .target_count = 1 });

//   P_MTHD(cmd->push, NVC0_3D, CSAA_ENABLE);
//   P_INLINE_DATA(cmd->push, 0);

   P_IMMD(p, NV9097, SET_ANTI_ALIAS_ENABLE, V_FALSE);
   P_IMMD(p, NV9097, SET_ANTI_ALIAS, SAMPLES_MODE_1X1);
   P_IMMD(p, NV9097, SET_ANTI_ALIAS_ALPHA_CONTROL, {
      .alpha_to_coverage   = ALPHA_TO_COVERAGE_DISABLE,
      .alpha_to_one        = ALPHA_TO_ONE_DISABLE,
   });
   P_IMMD(p, NV9097, SET_ALIASED_LINE_WIDTH_ENABLE, V_TRUE);

   P_IMMD(p, NV9097, SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_BLEND_SEPARATE_FOR_ALPHA, ENABLE_TRUE);
   P_IMMD(p, NV9097, SET_SINGLE_ROP_CONTROL, ENABLE_FALSE);

   P_IMMD(p, NV9097, SET_SHADE_MODE, V_OGL_SMOOTH);

   P_IMMD(p, NV9097, SET_API_VISIBLE_CALL_LIMIT, V__128);

   P_IMMD(p, NV9097, SET_ZCULL_STATS, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_L1_CONFIGURATION,
                     DIRECTLY_ADDRESSABLE_MEMORY_SIZE_48KB);

   magic_3d_init(cmd);

   /* TODO: Vertex runout */
   /* TODO: temp */

   P_IMMD(p, NV9097, SET_SHADER_LOCAL_MEMORY_WINDOW, 0xff000000); /* TODO */

   /* TODO: TIC */
   /* TODO: TSC */

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

   P_IMMD(p, NV9097, X_X_X_SET_CLEAR_CONTROL, {
      .respect_stencil_mask   = RESPECT_STENCIL_MASK_FALSE,
      .use_clear_rect         = USE_CLEAR_RECT_FALSE,
   });

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

   /* TODO: Macros */

   P_IMMD(p, NV9097, SET_RASTER_ENABLE, V_TRUE);
   P_IMMD(p, NV9097, SET_CT_MRT_ENABLE, V_TRUE);
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(3), {
      .enable  = ENABLE_FALSE,
      .type    = TYPE_TESSELLATION,
   });
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(4), {
      .enable  = ENABLE_FALSE,
      .type    = TYPE_GEOMETRY,
   });
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(5), {
      .enable  = ENABLE_FALSE,
      .type    = TYPE_PIXEL,
   });
//   P_MTHD(cmd->push, NVC0_3D, MACRO_GP_SELECT);
//   P_INLINE_DATA(cmd->push, 0x40);
   P_IMMD(p, NV9097, SET_RT_LAYER, {
      .v = 0,
      .control = CONTROL_V_SELECTS_LAYER,
   });
//   P_MTHD(cmd->push, NVC0_3D, MACRO_TEP_SELECT;
//   P_INLINE_DATA(cmd->push, 0x30);
   P_IMMD(p, NV9097, SET_PATCH, 3);
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(2), {
      .enable  = ENABLE_FALSE,
      .type    = TYPE_TESSELLATION_INIT,
   });
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(0), {
      .enable  = ENABLE_FALSE,
      .type    = TYPE_VERTEX_CULL_BEFORE_FETCH,
   });

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
   P_IMMD(p, NV9097, INVALIDATE_SAMPLER_CACHE, {
      .lines = LINES_ALL
   });
}

void
nvk_cmd_bind_graphics_pipeline(struct nvk_cmd_buffer *cmd,
                               struct nvk_graphics_pipeline *pipeline)
{
   cmd->state.gfx.pipeline = pipeline;
}

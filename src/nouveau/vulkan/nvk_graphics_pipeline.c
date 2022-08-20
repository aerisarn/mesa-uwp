#include "nvk_pipeline.h"

#include "nvk_device.h"
#include "nvk_pipeline_layout.h"
#include "nvk_shader.h"
#include "vk_nir.h"
#include "vk_pipeline.h"

#include "nouveau_context.h"

#include "nvk_cl9097.h"
#include "nvk_clb197.h"
#include "nvk_clc397.h"

static void
emit_pipeline_ts_state(struct nvk_graphics_pipeline *pipeline,
                       const struct vk_tessellation_state *ts)
{
   assert(ts->domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT);
}

static void
emit_pipeline_vp_state(struct nvk_graphics_pipeline *pipeline,
                       const struct vk_viewport_state *vp)
{
   struct nouveau_ws_push_buffer *p = P_SPACE(&pipeline->push, 0);

   P_IMMD(p, NV9097, SET_VIEWPORT_Z_CLIP, vp->depth_clip_negative_one_to_one ?
                                          RANGE_NEGATIVE_W_TO_POSITIVE_W :
                                          RANGE_ZERO_TO_POSITIVE_W);
}

static uint32_t
vk_to_nv9097_polygon_mode(VkPolygonMode vk_mode)
{
   ASSERTED uint16_t vk_to_nv9097[] = {
      [VK_POLYGON_MODE_FILL]  = NV9097_SET_FRONT_POLYGON_MODE_V_FILL,
      [VK_POLYGON_MODE_LINE]  = NV9097_SET_FRONT_POLYGON_MODE_V_LINE,
      [VK_POLYGON_MODE_POINT] = NV9097_SET_FRONT_POLYGON_MODE_V_POINT,
   };
   assert(vk_mode < ARRAY_SIZE(vk_to_nv9097));

   uint32_t nv9097_mode = 0x1b00 | (2 - vk_mode);
   assert(nv9097_mode == vk_to_nv9097[vk_mode]);
   return nv9097_mode;
}

static uint32_t
vk_to_nv9097_provoking_vertex(VkProvokingVertexModeEXT vk_mode)
{
   STATIC_ASSERT(VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT ==
                 NV9097_SET_PROVOKING_VERTEX_V_FIRST);
   STATIC_ASSERT(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ==
                 NV9097_SET_PROVOKING_VERTEX_V_LAST);
   return vk_mode;
}

static void
emit_pipeline_rs_state(struct nvk_graphics_pipeline *pipeline,
                       const struct vk_rasterization_state *rs)
{
   struct nouveau_ws_push_buffer *p = P_SPACE(&pipeline->push, 0);

   /* TODO: Depth clip/clamp? */
   P_IMMD(p, NV9097, SET_VIEWPORT_CLIP_CONTROL, {
      .min_z_zero_max_z_one      = MIN_Z_ZERO_MAX_Z_ONE_TRUE,
      .pixel_min_z               = PIXEL_MIN_Z_CLAMP,
      .pixel_max_z               = PIXEL_MAX_Z_CLIP,
      .geometry_guardband        = GEOMETRY_GUARDBAND_SCALE_256,
      .line_point_cull_guardband = LINE_POINT_CULL_GUARDBAND_SCALE_256,
      .geometry_clip             = GEOMETRY_CLIP_WZERO_CLIP,
      .geometry_guardband_z      = GEOMETRY_GUARDBAND_Z_SAME_AS_XY_GUARDBAND,
   });

   const uint32_t polygon_mode = vk_to_nv9097_polygon_mode(rs->polygon_mode);
   P_MTHD(p, NV9097, SET_FRONT_POLYGON_MODE);
   P_NV9097_SET_FRONT_POLYGON_MODE(p, polygon_mode);
   P_NV9097_SET_BACK_POLYGON_MODE(p, polygon_mode);

   P_IMMD(p, NV9097, SET_PROVOKING_VERTEX,
          vk_to_nv9097_provoking_vertex(rs->provoking_vertex));

   assert(rs->rasterization_stream == 0);

   assert(rs->line.mode == VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT);

   P_IMMD(p, NV9097, SET_LINE_STIPPLE, rs->line.stipple.enable);
}

static void
emit_pipeline_ms_state(struct nvk_graphics_pipeline *pipeline,
                       const struct vk_multisample_state *ms)
{
   struct nouveau_ws_push_buffer *p = P_SPACE(&pipeline->push, 0);

   P_IMMD(p, NV9097, SET_ANTI_ALIAS, ffs(ms->rasterization_samples) - 1);
   P_IMMD(p, NV9097, SET_ANTI_ALIAS_ENABLE, ms->sample_shading_enable);
   P_IMMD(p, NV9097, SET_ANTI_ALIAS_ALPHA_CONTROL, {
      .alpha_to_coverage   = ms->alpha_to_coverage_enable,
      .alpha_to_one        = ms->alpha_to_one_enable,
   });

   /* TODO */
   P_IMMD(p, NV9097, SET_ANTI_ALIASED_LINE, ENABLE_FALSE);
}

static uint32_t
vk_to_nv9097_blend_op(VkBlendOp vk_op)
{
#define OP(vk, nv) [VK_BLEND_OP_##vk] = NV9097_SET_BLEND_COLOR_OP_V_OGL_##nv
   ASSERTED uint16_t vk_to_nv9097[] = {
      OP(ADD,              FUNC_ADD),
      OP(SUBTRACT,         FUNC_SUBTRACT),
      OP(REVERSE_SUBTRACT, FUNC_REVERSE_SUBTRACT),
      OP(MIN,              MIN),
      OP(MAX,              MAX),
   };
   assert(vk_op < ARRAY_SIZE(vk_to_nv9097));
#undef OP

   return vk_to_nv9097[vk_op];
}

static uint32_t
vk_to_nv9097_blend_factor(VkBlendFactor vk_factor)
{
#define FACTOR(vk, nv) [VK_BLEND_FACTOR_##vk] = \
   NV9097_SET_BLEND_COLOR_SOURCE_COEFF_V_##nv
   ASSERTED uint16_t vk_to_nv9097[] = {
      FACTOR(ZERO,                     OGL_ZERO),
      FACTOR(ONE,                      OGL_ONE),
      FACTOR(SRC_COLOR,                OGL_SRC_COLOR),
      FACTOR(ONE_MINUS_SRC_COLOR,      OGL_ONE_MINUS_SRC_COLOR),
      FACTOR(DST_COLOR,                OGL_DST_COLOR),
      FACTOR(ONE_MINUS_DST_COLOR,      OGL_ONE_MINUS_DST_COLOR),
      FACTOR(SRC_ALPHA,                OGL_SRC_ALPHA),
      FACTOR(ONE_MINUS_SRC_ALPHA,      OGL_ONE_MINUS_SRC_ALPHA),
      FACTOR(DST_ALPHA,                OGL_DST_ALPHA),
      FACTOR(ONE_MINUS_DST_ALPHA,      OGL_ONE_MINUS_DST_ALPHA),
      FACTOR(CONSTANT_COLOR,           OGL_CONSTANT_COLOR),
      FACTOR(ONE_MINUS_CONSTANT_COLOR, OGL_ONE_MINUS_CONSTANT_COLOR),
      FACTOR(CONSTANT_ALPHA,           OGL_CONSTANT_ALPHA),
      FACTOR(ONE_MINUS_CONSTANT_ALPHA, OGL_ONE_MINUS_CONSTANT_ALPHA),
      FACTOR(SRC_ALPHA_SATURATE,       OGL_SRC_ALPHA_SATURATE),
      FACTOR(SRC1_COLOR,               OGL_SRC1COLOR),
      FACTOR(ONE_MINUS_SRC1_COLOR,     OGL_INVSRC1COLOR),
      FACTOR(SRC1_ALPHA,               OGL_SRC1ALPHA),
      FACTOR(ONE_MINUS_SRC1_ALPHA,     OGL_INVSRC1ALPHA),
   };
   assert(vk_factor < ARRAY_SIZE(vk_to_nv9097));
#undef FACTOR

   return vk_to_nv9097[vk_factor];
}

static void
emit_pipeline_cb_state(struct nvk_graphics_pipeline *pipeline,
                       const struct vk_color_blend_state *cb)
{
   struct nouveau_ws_push_buffer *p = P_SPACE(&pipeline->push, 0);

   P_IMMD(p, NV9097, SET_BLEND_STATE_PER_TARGET, ENABLE_TRUE);

   P_IMMD(p, NV9097, SET_LOGIC_OP, cb->logic_op_enable);

   for (uint32_t a = 0; a < cb->attachment_count; a++) {
      const struct vk_color_blend_attachment_state *att = &cb->attachments[a];
      P_IMMD(p, NV9097, SET_BLEND(a), att->blend_enable);

      P_MTHD(p, NV9097, SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(a));
      P_NV9097_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(p, a, ENABLE_TRUE);
      P_NV9097_SET_BLEND_PER_TARGET_COLOR_OP(p, a,
         vk_to_nv9097_blend_op(att->color_blend_op));
      P_NV9097_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF(p, a,
         vk_to_nv9097_blend_factor(att->src_color_blend_factor));
      P_NV9097_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF(p, a,
         vk_to_nv9097_blend_factor(att->dst_color_blend_factor));
      P_NV9097_SET_BLEND_PER_TARGET_ALPHA_OP(p, a,
         vk_to_nv9097_blend_op(att->alpha_blend_op));
      P_NV9097_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF(p, a,
         vk_to_nv9097_blend_factor(att->src_alpha_blend_factor));
      P_NV9097_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF(p, a,
         vk_to_nv9097_blend_factor(att->dst_alpha_blend_factor));

      P_IMMD(p, NV9097, SET_CT_WRITE(a), {
         .r_enable = (att->write_mask & BITFIELD_BIT(0)) != 0,
         .g_enable = (att->write_mask & BITFIELD_BIT(1)) != 0,
         .b_enable = (att->write_mask & BITFIELD_BIT(2)) != 0,
         .a_enable = (att->write_mask & BITFIELD_BIT(3)) != 0,
      });
   }
}

static const uint32_t mesa_to_nv9097_shader_type[] = {
   [MESA_SHADER_VERTEX]    = NV9097_SET_PIPELINE_SHADER_TYPE_VERTEX,
   [MESA_SHADER_TESS_CTRL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION_INIT,
   [MESA_SHADER_TESS_EVAL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION,
   [MESA_SHADER_GEOMETRY]  = NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY,
   [MESA_SHADER_FRAGMENT]  = NV9097_SET_PIPELINE_SHADER_TYPE_PIXEL,
};

VkResult
nvk_graphics_pipeline_create(struct nvk_device *device,
                             struct vk_pipeline_cache *cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   VK_FROM_HANDLE(nvk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   struct nvk_physical_device *pdevice = nvk_device_physical(device);
   struct nvk_graphics_pipeline *pipeline;
   VkResult result = VK_SUCCESS;

   pipeline = vk_object_zalloc(&device->vk, pAllocator, sizeof(*pipeline),
                               VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->base.type = NVK_PIPELINE_GRAPHICS;

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *sinfo = &pCreateInfo->pStages[i];
      gl_shader_stage stage = vk_to_mesa_shader_stage(sinfo->stage);

      const nir_shader_compiler_options *nir_options =
         nvk_physical_device_nir_options(pdevice, stage);
      const struct spirv_to_nir_options *spirv_options =
         nvk_physical_device_spirv_options(pdevice);

      nir_shader *nir;
      result = vk_pipeline_shader_stage_to_nir(&device->vk, sinfo,
                                               spirv_options, nir_options,
                                               NULL, &nir);
      if (result != VK_SUCCESS)
         goto fail;

      nvk_lower_nir(device, nir, pipeline_layout);

      result = nvk_compile_nir(pdevice, nir, &pipeline->base.shaders[stage]);
      ralloc_free(nir);
      if (result != VK_SUCCESS)
         goto fail;

      nvk_shader_upload(device, &pipeline->base.shaders[stage]);
   }

   nouveau_ws_push_init_cpu(&pipeline->push, &pipeline->push_data,
                            sizeof(pipeline->push_data));
   struct nouveau_ws_push_buffer *p = P_SPACE(&pipeline->push, 0);

   struct nvk_shader *last_geom = NULL;
   for (gl_shader_stage stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      struct nvk_shader *shader = &pipeline->base.shaders[stage];
      uint32_t idx = mesa_to_nv9097_shader_type[stage];

      P_IMMD(p, NV9097, SET_PIPELINE_SHADER(idx), {
         .enable  = shader->bo != NULL,
         .type    = mesa_to_nv9097_shader_type[stage],
      });

      if (shader->bo == NULL)
         continue;

      if (stage != MESA_SHADER_FRAGMENT)
         last_geom = shader;

      uint64_t addr = nvk_shader_address(shader);
      assert(device->ctx->eng3d.cls >= VOLTA_A);
      P_MTHD(p, NVC397, SET_PIPELINE_PROGRAM_ADDRESS_A(idx));
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_A(p, idx, addr >> 32 );
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_B(p, idx, addr);

      P_IMMD(p, NV9097, SET_PIPELINE_REGISTER_COUNT(idx), shader->num_gprs);

      switch (stage) {
      case MESA_SHADER_VERTEX: {
         uint8_t clip_cull = shader->vs.clip_enable | shader->vs.cull_enable;
         P_IMMD(p, NV9097, SET_USER_CLIP_ENABLE, {
            .plane0 = (clip_cull >> 0) & 1,
            .plane1 = (clip_cull >> 1) & 1,
            .plane2 = (clip_cull >> 2) & 1,
            .plane3 = (clip_cull >> 3) & 1,
            .plane4 = (clip_cull >> 4) & 1,
            .plane5 = (clip_cull >> 5) & 1,
            .plane6 = (clip_cull >> 6) & 1,
            .plane7 = (clip_cull >> 7) & 1,
         });
         P_IMMD(p, NV9097, SET_USER_CLIP_OP, {
            .plane0 = (shader->vs.cull_enable >> 0) & 1,
            .plane1 = (shader->vs.cull_enable >> 1) & 1,
            .plane2 = (shader->vs.cull_enable >> 2) & 1,
            .plane3 = (shader->vs.cull_enable >> 3) & 1,
            .plane4 = (shader->vs.cull_enable >> 4) & 1,
            .plane5 = (shader->vs.cull_enable >> 5) & 1,
            .plane6 = (shader->vs.cull_enable >> 6) & 1,
            .plane7 = (shader->vs.cull_enable >> 7) & 1,
         });
         break;
      }

      case MESA_SHADER_FRAGMENT:
         P_IMMD(p, NV9097, SET_SUBTILING_PERF_KNOB_A, {
            .fraction_of_spm_register_file_per_subtile         = 0x10,
            .fraction_of_spm_pixel_output_buffer_per_subtile   = 0x40,
            .fraction_of_spm_triangle_ram_per_subtile          = 0x16,
            .fraction_of_max_quads_per_subtile                 = 0x20,
         });
         P_NV9097_SET_SUBTILING_PERF_KNOB_B(p, 0x20);

         P_IMMD(p, NV9097, SET_API_MANDATED_EARLY_Z, shader->fs.early_z);

         if (device->ctx->eng3d.cls >= MAXWELL_B) {
            P_IMMD(p, NVB197, SET_POST_Z_PS_IMASK,
                   shader->fs.post_depth_coverage);
         } else {
            assert(!shader->fs.post_depth_coverage);
         }

         P_MTHD(p, NV9097, SET_ZCULL_BOUNDS);
         P_INLINE_DATA(p, shader->flags[0]);
         break;

      default:
         unreachable("Unsupported shader stage");
      }
   }

   /* TODO: prog_selects_layer */
   P_IMMD(p, NV9097, SET_RT_LAYER, {
      .v       = 0,
      .control = (last_geom->hdr[13] & (1 << 9)) ?
                 CONTROL_GEOMETRY_SHADER_SELECTS_LAYER :
                 CONTROL_V_SELECTS_LAYER,
   });

   struct vk_graphics_pipeline_all_state all;
   struct vk_graphics_pipeline_state state = {};
   result = vk_graphics_pipeline_state_fill(&device->vk, &state, pCreateInfo,
                                            NULL, &all, NULL, 0, NULL);
   assert(result == VK_SUCCESS);

   if (state.ts) emit_pipeline_ts_state(pipeline, state.ts);
   if (state.vp) emit_pipeline_vp_state(pipeline, state.vp);
   if (state.rs) emit_pipeline_rs_state(pipeline, state.rs);
   if (state.ms) emit_pipeline_ms_state(pipeline, state.ms);
   if (state.cb) emit_pipeline_cb_state(pipeline, state.cb);

   pipeline->dynamic.vi = &pipeline->_dynamic_vi;
   vk_dynamic_graphics_state_fill(&pipeline->dynamic, &state);

   *pPipeline = nvk_pipeline_to_handle(&pipeline->base);

   return VK_SUCCESS;

fail:
   vk_object_free(&device->vk, pAllocator, pipeline);
   return result;
}

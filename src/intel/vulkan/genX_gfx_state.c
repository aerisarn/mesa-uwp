/*
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
#include "common/intel_guardband.h"
#include "compiler/brw_prim.h"

void
genX(cmd_buffer_enable_pma_fix)(struct anv_cmd_buffer *cmd_buffer, bool enable)
{
   if (cmd_buffer->state.pma_fix_enabled == enable)
      return;

   cmd_buffer->state.pma_fix_enabled = enable;

   /* According to the Broadwell PIPE_CONTROL documentation, software should
    * emit a PIPE_CONTROL with the CS Stall and Depth Cache Flush bits set
    * prior to the LRI.  If stencil buffer writes are enabled, then a Render
    * Cache Flush is also necessary.
    *
    * The Skylake docs say to use a depth stall rather than a command
    * streamer stall.  However, the hardware seems to violently disagree.
    * A full command streamer stall seems to be needed in both cases.
    */
   genX(batch_emit_pipe_control)
      (&cmd_buffer->batch, cmd_buffer->device->info,
       ANV_PIPE_DEPTH_CACHE_FLUSH_BIT |
       ANV_PIPE_CS_STALL_BIT |
#if GFX_VER >= 12
       ANV_PIPE_TILE_CACHE_FLUSH_BIT |
#endif
       ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT);

#if GFX_VER == 9
   uint32_t cache_mode;
   anv_pack_struct(&cache_mode, GENX(CACHE_MODE_0),
                   .STCPMAOptimizationEnable = enable,
                   .STCPMAOptimizationEnableMask = true);
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
      lri.RegisterOffset   = GENX(CACHE_MODE_0_num);
      lri.DataDWord        = cache_mode;
   }

#endif /* GFX_VER == 9 */

   /* After the LRI, a PIPE_CONTROL with both the Depth Stall and Depth Cache
    * Flush bits is often necessary.  We do it regardless because it's easier.
    * The render cache flush is also necessary if stencil writes are enabled.
    *
    * Again, the Skylake docs give a different set of flushes but the BDW
    * flushes seem to work just as well.
    */
   genX(batch_emit_pipe_control)
      (&cmd_buffer->batch, cmd_buffer->device->info,
       ANV_PIPE_DEPTH_STALL_BIT |
       ANV_PIPE_DEPTH_CACHE_FLUSH_BIT |
#if GFX_VER >= 12
       ANV_PIPE_TILE_CACHE_FLUSH_BIT |
#endif
       ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT);
}

UNUSED static bool
want_stencil_pma_fix(struct anv_cmd_buffer *cmd_buffer,
                     const struct vk_depth_stencil_state *ds)
{
   if (GFX_VER > 9)
      return false;
   assert(GFX_VER == 9);

   /* From the Skylake PRM Vol. 2c CACHE_MODE_1::STC PMA Optimization Enable:
    *
    *    Clearing this bit will force the STC cache to wait for pending
    *    retirement of pixels at the HZ-read stage and do the STC-test for
    *    Non-promoted, R-computed and Computed depth modes instead of
    *    postponing the STC-test to RCPFE.
    *
    *    STC_TEST_EN = 3DSTATE_STENCIL_BUFFER::STENCIL_BUFFER_ENABLE &&
    *                  3DSTATE_WM_DEPTH_STENCIL::StencilTestEnable
    *
    *    STC_WRITE_EN = 3DSTATE_STENCIL_BUFFER::STENCIL_BUFFER_ENABLE &&
    *                   (3DSTATE_WM_DEPTH_STENCIL::Stencil Buffer Write Enable &&
    *                    3DSTATE_DEPTH_BUFFER::STENCIL_WRITE_ENABLE)
    *
    *    COMP_STC_EN = STC_TEST_EN &&
    *                  3DSTATE_PS_EXTRA::PixelShaderComputesStencil
    *
    *    SW parses the pipeline states to generate the following logical
    *    signal indicating if PMA FIX can be enabled.
    *
    *    STC_PMA_OPT =
    *       3DSTATE_WM::ForceThreadDispatch != 1 &&
    *       !(3DSTATE_RASTER::ForceSampleCount != NUMRASTSAMPLES_0) &&
    *       3DSTATE_DEPTH_BUFFER::SURFACE_TYPE != NULL &&
    *       3DSTATE_DEPTH_BUFFER::HIZ Enable &&
    *       !(3DSTATE_WM::EDSC_Mode == 2) &&
    *       3DSTATE_PS_EXTRA::PixelShaderValid &&
    *       !(3DSTATE_WM_HZ_OP::DepthBufferClear ||
    *         3DSTATE_WM_HZ_OP::DepthBufferResolve ||
    *         3DSTATE_WM_HZ_OP::Hierarchical Depth Buffer Resolve Enable ||
    *         3DSTATE_WM_HZ_OP::StencilBufferClear) &&
    *       (COMP_STC_EN || STC_WRITE_EN) &&
    *       ((3DSTATE_PS_EXTRA::PixelShaderKillsPixels ||
    *         3DSTATE_WM::ForceKillPix == ON ||
    *         3DSTATE_PS_EXTRA::oMask Present to RenderTarget ||
    *         3DSTATE_PS_BLEND::AlphaToCoverageEnable ||
    *         3DSTATE_PS_BLEND::AlphaTestEnable ||
    *         3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable) ||
    *        (3DSTATE_PS_EXTRA::Pixel Shader Computed Depth mode != PSCDEPTH_OFF))
    */

   /* These are always true:
    *    3DSTATE_WM::ForceThreadDispatch != 1 &&
    *    !(3DSTATE_RASTER::ForceSampleCount != NUMRASTSAMPLES_0)
    */

   /* We only enable the PMA fix if we know for certain that HiZ is enabled.
    * If we don't know whether HiZ is enabled or not, we disable the PMA fix
    * and there is no harm.
    *
    * (3DSTATE_DEPTH_BUFFER::SURFACE_TYPE != NULL) &&
    * 3DSTATE_DEPTH_BUFFER::HIZ Enable
    */
   if (!cmd_buffer->state.hiz_enabled)
      return false;

   /* We can't possibly know if HiZ is enabled without the depth attachment */
   ASSERTED const struct anv_image_view *d_iview =
      cmd_buffer->state.gfx.depth_att.iview;
   assert(d_iview && d_iview->image->planes[0].aux_usage == ISL_AUX_USAGE_HIZ);

   /* 3DSTATE_PS_EXTRA::PixelShaderValid */
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT))
      return false;

   /* !(3DSTATE_WM::EDSC_Mode == 2) */
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);
   if (wm_prog_data->early_fragment_tests)
      return false;

   /* We never use anv_pipeline for HiZ ops so this is trivially true:
   *    !(3DSTATE_WM_HZ_OP::DepthBufferClear ||
    *      3DSTATE_WM_HZ_OP::DepthBufferResolve ||
    *      3DSTATE_WM_HZ_OP::Hierarchical Depth Buffer Resolve Enable ||
    *      3DSTATE_WM_HZ_OP::StencilBufferClear)
    */

   /* 3DSTATE_STENCIL_BUFFER::STENCIL_BUFFER_ENABLE &&
    * 3DSTATE_WM_DEPTH_STENCIL::StencilTestEnable
    */
   const bool stc_test_en = ds->stencil.test_enable;

   /* 3DSTATE_STENCIL_BUFFER::STENCIL_BUFFER_ENABLE &&
    * (3DSTATE_WM_DEPTH_STENCIL::Stencil Buffer Write Enable &&
    *  3DSTATE_DEPTH_BUFFER::STENCIL_WRITE_ENABLE)
    */
   const bool stc_write_en = ds->stencil.write_enable;

   /* STC_TEST_EN && 3DSTATE_PS_EXTRA::PixelShaderComputesStencil */
   const bool comp_stc_en = stc_test_en && wm_prog_data->computed_stencil;

   /* COMP_STC_EN || STC_WRITE_EN */
   if (!(comp_stc_en || stc_write_en))
      return false;

   /* (3DSTATE_PS_EXTRA::PixelShaderKillsPixels ||
    *  3DSTATE_WM::ForceKillPix == ON ||
    *  3DSTATE_PS_EXTRA::oMask Present to RenderTarget ||
    *  3DSTATE_PS_BLEND::AlphaToCoverageEnable ||
    *  3DSTATE_PS_BLEND::AlphaTestEnable ||
    *  3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable) ||
    * (3DSTATE_PS_EXTRA::Pixel Shader Computed Depth mode != PSCDEPTH_OFF)
    */
   return pipeline->kill_pixel ||
          wm_prog_data->computed_depth_mode != PSCDEPTH_OFF;
}

static void
genX(cmd_emit_te)(struct anv_cmd_buffer *cmd_buffer)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;
   const struct brw_tes_prog_data *tes_prog_data = get_tes_prog_data(pipeline);

   if (!tes_prog_data ||
       !anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
      uint32_t *dw =
         anv_batch_emitn(&cmd_buffer->batch, GENX(3DSTATE_TE_length),
                         GENX(3DSTATE_TE));
      memcpy(dw, &pipeline->partial.te, sizeof(pipeline->partial.te));
      return;
   }

   anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_TE),
                        pipeline->partial.te, te) {
      if (dyn->ts.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT) {
         te.OutputTopology = tes_prog_data->output_topology;
      } else {
         /* When the origin is upper-left, we have to flip the winding order */
         if (tes_prog_data->output_topology == OUTPUT_TRI_CCW) {
            te.OutputTopology = OUTPUT_TRI_CW;
         } else if (tes_prog_data->output_topology == OUTPUT_TRI_CW) {
            te.OutputTopology = OUTPUT_TRI_CCW;
         } else {
            te.OutputTopology = tes_prog_data->output_topology;
         }
      }
   }
}

static void
genX(emit_gs)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_GS), gs);
      return;
   }

   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_GS),
                        pipeline->partial.gs, gs) {
      switch (dyn->rs.provoking_vertex) {
      case VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT:
         gs.ReorderMode = LEADING;
         break;

      case VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT:
         gs.ReorderMode = TRAILING;
         break;

      default:
         unreachable("Invalid provoking vertex mode");
      }
   }
}

static void
genX(cmd_emit_sample_mask)(struct anv_cmd_buffer *cmd_buffer)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;

   /* From the Vulkan 1.0 spec:
   *    If pSampleMask is NULL, it is treated as if the mask has all bits
    *    enabled, i.e. no coverage is removed from fragments.
    *
    * 3DSTATE_SAMPLE_MASK.SampleMask is 16 bits.
    */
   uint32_t sample_mask = 0xffff;

   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_SAMPLE_MASK), sm) {
      sm.SampleMask = dyn->ms.sample_mask & sample_mask;
   }
}

#if GFX_VER >= 12
static uint32_t
get_cps_state_offset(struct anv_device *device, bool cps_enabled,
                     const struct vk_fragment_shading_rate_state *fsr)
{
   if (!cps_enabled)
      return device->cps_states.offset;

   uint32_t offset;
   static const uint32_t size_index[] = {
      [1] = 0,
      [2] = 1,
      [4] = 2,
   };

#if GFX_VERx10 >= 125
   offset =
      1 + /* skip disabled */
      fsr->combiner_ops[0] * 5 * 3 * 3 +
      fsr->combiner_ops[1] * 3 * 3 +
      size_index[fsr->fragment_size.width] * 3 +
      size_index[fsr->fragment_size.height];
#else
   offset =
      1 + /* skip disabled */
      size_index[fsr->fragment_size.width] * 3 +
      size_index[fsr->fragment_size.height];
#endif

   offset *= MAX_VIEWPORTS * GENX(CPS_STATE_length) * 4;

   return device->cps_states.offset + offset;
}
#endif /* GFX_VER >= 12 */

#if GFX_VER >= 11
static void
genX(emit_shading_rate)(struct anv_batch *batch,
                        const struct anv_graphics_pipeline *pipeline,
                        const struct vk_fragment_shading_rate_state *fsr)
{
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);
   const bool cps_enable = wm_prog_data &&
      brw_wm_prog_data_is_coarse(wm_prog_data, pipeline->fs_msaa_flags);

#if GFX_VER == 11
   anv_batch_emit(batch, GENX(3DSTATE_CPS), cps) {
      cps.CoarsePixelShadingMode = cps_enable ? CPS_MODE_CONSTANT : CPS_MODE_NONE;
      if (cps_enable) {
         cps.MinCPSizeX = fsr->fragment_size.width;
         cps.MinCPSizeY = fsr->fragment_size.height;
      }
   }
#elif GFX_VER >= 12
   /* TODO: we can optimize this flush in the following cases:
    *
    *    In the case where the last geometry shader emits a value that is not
    *    constant, we can avoid this stall because we can synchronize the
    *    pixel shader internally with
    *    3DSTATE_PS::EnablePSDependencyOnCPsizeChange.
    *
    *    If we know that the previous pipeline and the current one are using
    *    the same fragment shading rate.
    */
   anv_batch_emit(batch, GENX(PIPE_CONTROL), pc) {
#if GFX_VERx10 >= 125
      pc.PSSStallSyncEnable = true;
#else
      pc.PSDSyncEnable = true;
#endif
   }

   anv_batch_emit(batch, GENX(3DSTATE_CPS_POINTERS), cps) {
      struct anv_device *device = pipeline->base.base.device;

      cps.CoarsePixelShadingStateArrayPointer =
         get_cps_state_offset(device, cps_enable, fsr);
   }
#endif
}
#endif /* GFX_VER >= 11 */

const uint32_t genX(vk_to_intel_blend)[] = {
   [VK_BLEND_FACTOR_ZERO]                    = BLENDFACTOR_ZERO,
   [VK_BLEND_FACTOR_ONE]                     = BLENDFACTOR_ONE,
   [VK_BLEND_FACTOR_SRC_COLOR]               = BLENDFACTOR_SRC_COLOR,
   [VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR]     = BLENDFACTOR_INV_SRC_COLOR,
   [VK_BLEND_FACTOR_DST_COLOR]               = BLENDFACTOR_DST_COLOR,
   [VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR]     = BLENDFACTOR_INV_DST_COLOR,
   [VK_BLEND_FACTOR_SRC_ALPHA]               = BLENDFACTOR_SRC_ALPHA,
   [VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA]     = BLENDFACTOR_INV_SRC_ALPHA,
   [VK_BLEND_FACTOR_DST_ALPHA]               = BLENDFACTOR_DST_ALPHA,
   [VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA]     = BLENDFACTOR_INV_DST_ALPHA,
   [VK_BLEND_FACTOR_CONSTANT_COLOR]          = BLENDFACTOR_CONST_COLOR,
   [VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR]= BLENDFACTOR_INV_CONST_COLOR,
   [VK_BLEND_FACTOR_CONSTANT_ALPHA]          = BLENDFACTOR_CONST_ALPHA,
   [VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA]= BLENDFACTOR_INV_CONST_ALPHA,
   [VK_BLEND_FACTOR_SRC_ALPHA_SATURATE]      = BLENDFACTOR_SRC_ALPHA_SATURATE,
   [VK_BLEND_FACTOR_SRC1_COLOR]              = BLENDFACTOR_SRC1_COLOR,
   [VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR]    = BLENDFACTOR_INV_SRC1_COLOR,
   [VK_BLEND_FACTOR_SRC1_ALPHA]              = BLENDFACTOR_SRC1_ALPHA,
   [VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA]    = BLENDFACTOR_INV_SRC1_ALPHA,
};

static const uint32_t genX(vk_to_intel_blend_op)[] = {
   [VK_BLEND_OP_ADD]                         = BLENDFUNCTION_ADD,
   [VK_BLEND_OP_SUBTRACT]                    = BLENDFUNCTION_SUBTRACT,
   [VK_BLEND_OP_REVERSE_SUBTRACT]            = BLENDFUNCTION_REVERSE_SUBTRACT,
   [VK_BLEND_OP_MIN]                         = BLENDFUNCTION_MIN,
   [VK_BLEND_OP_MAX]                         = BLENDFUNCTION_MAX,
};

static void
genX(rasterization_mode)(VkPolygonMode raster_mode,
                         VkLineRasterizationModeEXT line_mode,
                         float line_width,
                         uint32_t *api_mode,
                         bool *msaa_rasterization_enable)
{
   if (raster_mode == VK_POLYGON_MODE_LINE) {
      /* Unfortunately, configuring our line rasterization hardware on gfx8
       * and later is rather painful.  Instead of giving us bits to tell the
       * hardware what line mode to use like we had on gfx7, we now have an
       * arcane combination of API Mode and MSAA enable bits which do things
       * in a table which are expected to magically put the hardware into the
       * right mode for your API.  Sadly, Vulkan isn't any of the APIs the
       * hardware people thought of so nothing works the way you want it to.
       *
       * Look at the table titled "Multisample Rasterization Modes" in Vol 7
       * of the Skylake PRM for more details.
       */
      switch (line_mode) {
      case VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT:
         *api_mode = DX101;
#if GFX_VER <= 9
         /* Prior to ICL, the algorithm the HW uses to draw wide lines
          * doesn't quite match what the CTS expects, at least for rectangular
          * lines, so we set this to false here, making it draw parallelograms
          * instead, which work well enough.
          */
         *msaa_rasterization_enable = line_width < 1.0078125;
#else
         *msaa_rasterization_enable = true;
#endif
         break;

      case VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT:
      case VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT:
         *api_mode = DX9OGL;
         *msaa_rasterization_enable = false;
         break;

      default:
         unreachable("Unsupported line rasterization mode");
      }
   } else {
      *api_mode = DX101;
      *msaa_rasterization_enable = true;
   }
}

static void
cmd_buffer_emit_clip(struct anv_cmd_buffer *cmd_buffer)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;

   if (!(cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) &&
       !BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) &&
       !BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE) &&
       !BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT) &&
       !BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE) &&
       !BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX))
      return;

   anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_CLIP),
                        pipeline->partial.clip, clip) {
      /* Take dynamic primitive topology in to account with
       *    3DSTATE_CLIP::ViewportXYClipTestEnable
       */
      const VkPolygonMode dynamic_raster_mode =
         genX(raster_polygon_mode)(pipeline,
                                   dyn->rs.polygon_mode,
                                   dyn->ia.primitive_topology);
      const bool xy_clip_test_enable =
         (dynamic_raster_mode == VK_POLYGON_MODE_FILL);

      clip.APIMode = dyn->vp.depth_clip_negative_one_to_one ?
                     APIMODE_OGL : APIMODE_D3D;
      clip.ViewportXYClipTestEnable = xy_clip_test_enable;

      ANV_SETUP_PROVOKING_VERTEX(clip, dyn->rs.provoking_vertex);

      /* TODO(mesh): Multiview. */
      if (anv_pipeline_is_primitive(pipeline)) {
         const struct brw_vue_prog_data *last =
            anv_pipeline_get_last_vue_prog_data(pipeline);
         if (last->vue_map.slots_valid & VARYING_BIT_VIEWPORT) {
            clip.MaximumVPIndex = dyn->vp.viewport_count > 0 ?
                                  dyn->vp.viewport_count - 1 : 0;
         }
      } else if (anv_pipeline_is_mesh(pipeline)) {
         const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
         if (mesh_prog_data->map.start_dw[VARYING_SLOT_VIEWPORT] >= 0) {
            clip.MaximumVPIndex = dyn->vp.viewport_count > 0 ?
                                  dyn->vp.viewport_count - 1 : 0;
         }
      }
   }
}

static void
genX(streamout_prologue)(struct anv_cmd_buffer *cmd_buffer)
{
#if GFX_VERx10 >= 120
   /* Wa_16013994831 - Disable preemption during streamout, enable back
    * again if XFB not used by the current pipeline.
    *
    * Although this workaround applies to Gfx12+, we already disable object
    * level preemption for another reason in genX_state.c so we can skip this
    * for Gfx12.
    */
   if (!intel_needs_workaround(cmd_buffer->device->info, 16013994831))
      return;

   if (cmd_buffer->state.gfx.pipeline->uses_xfb) {
      genX(cmd_buffer_set_preemption)(cmd_buffer, false);
      return;
   }

   if (!cmd_buffer->state.gfx.object_preemption)
      genX(cmd_buffer_set_preemption)(cmd_buffer, true);
#endif
}

static void
cmd_buffer_emit_streamout(struct anv_cmd_buffer *cmd_buffer)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;

   genX(streamout_prologue)(cmd_buffer);

   anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_STREAMOUT),
                        pipeline->partial.streamout_state, so) {
      so.RenderingDisable = dyn->rs.rasterizer_discard_enable;
      so.RenderStreamSelect = dyn->rs.rasterization_stream;
#if INTEL_NEEDS_WA_18022508906
      /* Wa_18022508906 :
       *
       * SKL PRMs, Volume 7: 3D-Media-GPGPU, Stream Output Logic (SOL) Stage:
       *
       * SOL_INT::Render_Enable =
       *   (3DSTATE_STREAMOUT::Force_Rending == Force_On) ||
       *   (
       *     (3DSTATE_STREAMOUT::Force_Rending != Force_Off) &&
       *     !(3DSTATE_GS::Enable && 3DSTATE_GS::Output Vertex Size == 0) &&
       *     !3DSTATE_STREAMOUT::API_Render_Disable &&
       *     (
       *       3DSTATE_DEPTH_STENCIL_STATE::Stencil_TestEnable ||
       *       3DSTATE_DEPTH_STENCIL_STATE::Depth_TestEnable ||
       *       3DSTATE_DEPTH_STENCIL_STATE::Depth_WriteEnable ||
       *       3DSTATE_PS_EXTRA::PS_Valid ||
       *       3DSTATE_WM::Legacy Depth_Buffer_Clear ||
       *       3DSTATE_WM::Legacy Depth_Buffer_Resolve_Enable ||
       *       3DSTATE_WM::Legacy Hierarchical_Depth_Buffer_Resolve_Enable
       *     )
       *   )
       *
       * If SOL_INT::Render_Enable is false, the SO stage will not forward any
       * topologies down the pipeline. Which is not what we want for occlusion
       * queries.
       *
       * Here we force rendering to get SOL_INT::Render_Enable when occlusion
       * queries are active.
       */
      if (!so.RenderingDisable && cmd_buffer->state.gfx.n_occlusion_queries > 0)
         so.ForceRendering = Force_on;
#endif

      switch (dyn->rs.provoking_vertex) {
      case VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT:
         so.ReorderMode = LEADING;
         break;

      case VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT:
         so.ReorderMode = TRAILING;
         break;

      default:
         unreachable("Invalid provoking vertex mode");
      }
   }
}

static void
cmd_buffer_emit_viewport(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_instance *instance = cmd_buffer->device->physical->instance;
   struct anv_cmd_graphics_state *gfx = &cmd_buffer->state.gfx;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   uint32_t count = dyn->vp.viewport_count;
   const VkViewport *viewports = dyn->vp.viewports;
   struct anv_state sf_clip_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 64, 64);

   const float scale = dyn->vp.depth_clip_negative_one_to_one ? 0.5f : 1.0f;

   for (uint32_t i = 0; i < count; i++) {
      const VkViewport *vp = &viewports[i];

      /* The gfx7 state struct has just the matrix and guardband fields, the
       * gfx8 struct adds the min/max viewport fields. */
      struct GENX(SF_CLIP_VIEWPORT) sfv = {
         .ViewportMatrixElementm00 = vp->width / 2,
         .ViewportMatrixElementm11 = vp->height / 2,
         .ViewportMatrixElementm22 = (vp->maxDepth - vp->minDepth) * scale,
         .ViewportMatrixElementm30 = vp->x + vp->width / 2,
         .ViewportMatrixElementm31 = vp->y + vp->height / 2,
         .ViewportMatrixElementm32 = dyn->vp.depth_clip_negative_one_to_one ?
            (vp->minDepth + vp->maxDepth) * scale : vp->minDepth,
         .XMinClipGuardband = -1.0f,
         .XMaxClipGuardband = 1.0f,
         .YMinClipGuardband = -1.0f,
         .YMaxClipGuardband = 1.0f,
         .XMinViewPort = vp->x,
         .XMaxViewPort = vp->x + vp->width - 1,
         .YMinViewPort = MIN2(vp->y, vp->y + vp->height),
         .YMaxViewPort = MAX2(vp->y, vp->y + vp->height) - 1,
      };

      /* Fix depth test misrenderings by lowering translated depth range */
      if (instance->lower_depth_range_rate != 1.0f)
         sfv.ViewportMatrixElementm32 *= instance->lower_depth_range_rate;

      const uint32_t fb_size_max = 1 << 14;
      uint32_t x_min = 0, x_max = fb_size_max;
      uint32_t y_min = 0, y_max = fb_size_max;

      /* If we have a valid renderArea, include that */
      if (gfx->render_area.extent.width > 0 &&
          gfx->render_area.extent.height > 0) {
         x_min = MAX2(x_min, gfx->render_area.offset.x);
         x_max = MIN2(x_max, gfx->render_area.offset.x +
                             gfx->render_area.extent.width);
         y_min = MAX2(y_min, gfx->render_area.offset.y);
         y_max = MIN2(y_max, gfx->render_area.offset.y +
                             gfx->render_area.extent.height);
      }

      /* The client is required to have enough scissors for whatever it sets
       * as ViewportIndex but it's possible that they've got more viewports
       * set from a previous command.  Also, from the Vulkan 1.3.207:
       *
       *    "The application must ensure (using scissor if necessary) that
       *    all rendering is contained within the render area."
       *
       * If the client doesn't set a scissor, that basically means it
       * guarantees everything is in-bounds already.  If we end up using a
       * guardband of [-1, 1] in that case, there shouldn't be much loss.
       * It's theoretically possible that they could do all their clipping
       * with clip planes but that'd be a bit odd.
       */
      if (i < dyn->vp.scissor_count) {
         const VkRect2D *scissor = &dyn->vp.scissors[i];
         x_min = MAX2(x_min, scissor->offset.x);
         x_max = MIN2(x_max, scissor->offset.x + scissor->extent.width);
         y_min = MAX2(y_min, scissor->offset.y);
         y_max = MIN2(y_max, scissor->offset.y + scissor->extent.height);
      }

      /* Only bother calculating the guardband if our known render area is
       * less than the maximum size.  Otherwise, it will calculate [-1, 1]
       * anyway but possibly with precision loss.
       */
      if (x_min > 0 || x_max < fb_size_max ||
          y_min > 0 || y_max < fb_size_max) {
         intel_calculate_guardband_size(x_min, x_max, y_min, y_max,
                                        sfv.ViewportMatrixElementm00,
                                        sfv.ViewportMatrixElementm11,
                                        sfv.ViewportMatrixElementm30,
                                        sfv.ViewportMatrixElementm31,
                                        &sfv.XMinClipGuardband,
                                        &sfv.XMaxClipGuardband,
                                        &sfv.YMinClipGuardband,
                                        &sfv.YMaxClipGuardband);
      }

      GENX(SF_CLIP_VIEWPORT_pack)(NULL, sf_clip_state.map + i * 64, &sfv);
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP), clip) {
      clip.SFClipViewportPointer = sf_clip_state.offset;
   }
}

static void
cmd_buffer_emit_depth_viewport(struct anv_cmd_buffer *cmd_buffer)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   uint32_t count = dyn->vp.viewport_count;
   const VkViewport *viewports = dyn->vp.viewports;
   struct anv_state cc_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 8, 32);

   for (uint32_t i = 0; i < count; i++) {
      const VkViewport *vp = &viewports[i];

      /* From the Vulkan spec:
       *
       *    "It is valid for minDepth to be greater than or equal to
       *    maxDepth."
       */
      float min_depth = MIN2(vp->minDepth, vp->maxDepth);
      float max_depth = MAX2(vp->minDepth, vp->maxDepth);

      struct GENX(CC_VIEWPORT) cc_viewport = {
         .MinimumDepth = dyn->rs.depth_clamp_enable ? min_depth : 0.0f,
         .MaximumDepth = dyn->rs.depth_clamp_enable ? max_depth : 1.0f,
      };

      GENX(CC_VIEWPORT_pack)(NULL, cc_state.map + i * 8, &cc_viewport);
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC), cc) {
      cc.CCViewportPointer = cc_state.offset;
   }
}

static void
cmd_buffer_emit_scissor(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_cmd_graphics_state *gfx = &cmd_buffer->state.gfx;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;
   uint32_t count = dyn->vp.scissor_count;
   const VkRect2D *scissors = dyn->vp.scissors;
   const VkViewport *viewports = dyn->vp.viewports;

   /* Wa_1409725701:
    *    "The viewport-specific state used by the SF unit (SCISSOR_RECT) is
    *    stored as an array of up to 16 elements. The location of first
    *    element of the array, as specified by Pointer to SCISSOR_RECT, should
    *    be aligned to a 64-byte boundary.
    */
   uint32_t alignment = 64;
   struct anv_state scissor_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 8, alignment);

   for (uint32_t i = 0; i < count; i++) {
      const VkRect2D *s = &scissors[i];
      const VkViewport *vp = &viewports[i];

      /* Since xmax and ymax are inclusive, we have to have xmax < xmin or
       * ymax < ymin for empty clips.  In case clip x, y, width height are all
       * 0, the clamps below produce 0 for xmin, ymin, xmax, ymax, which isn't
       * what we want. Just special case empty clips and produce a canonical
       * empty clip. */
      static const struct GENX(SCISSOR_RECT) empty_scissor = {
         .ScissorRectangleYMin = 1,
         .ScissorRectangleXMin = 1,
         .ScissorRectangleYMax = 0,
         .ScissorRectangleXMax = 0
      };

      const int max = 0xffff;

      uint32_t y_min = MAX2(s->offset.y, MIN2(vp->y, vp->y + vp->height));
      uint32_t x_min = MAX2(s->offset.x, vp->x);
      int64_t y_max = MIN2(s->offset.y + s->extent.height - 1,
                       MAX2(vp->y, vp->y + vp->height) - 1);
      int64_t x_max = MIN2(s->offset.x + s->extent.width - 1,
                       vp->x + vp->width - 1);

      y_max = CLAMP(y_max, 0, INT16_MAX >> 1);
      x_max = CLAMP(x_max, 0, INT16_MAX >> 1);

      /* Do this math using int64_t so overflow gets clamped correctly. */
      if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
         y_min = CLAMP((uint64_t) y_min, gfx->render_area.offset.y, max);
         x_min = CLAMP((uint64_t) x_min, gfx->render_area.offset.x, max);
         y_max = CLAMP((uint64_t) y_max, 0,
                       gfx->render_area.offset.y +
                       gfx->render_area.extent.height - 1);
         x_max = CLAMP((uint64_t) x_max, 0,
                       gfx->render_area.offset.x +
                       gfx->render_area.extent.width - 1);
      }

      const struct GENX(SCISSOR_RECT) scissor = {
         .ScissorRectangleYMin = y_min,
         .ScissorRectangleXMin = x_min,
         .ScissorRectangleYMax = y_max,
         .ScissorRectangleXMax = x_max
      };

      if (s->extent.width <= 0 || s->extent.height <= 0) {
         GENX(SCISSOR_RECT_pack)(NULL, scissor_state.map + i * 8,
                                 &empty_scissor);
      } else {
         GENX(SCISSOR_RECT_pack)(NULL, scissor_state.map + i * 8, &scissor);
      }
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_SCISSOR_STATE_POINTERS), ssp) {
      ssp.ScissorRectPointer = scissor_state.offset;
   }
}

void
genX(cmd_buffer_flush_dynamic_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_graphics_pipeline *pipeline = cmd_buffer->state.gfx.pipeline;
   struct vk_dynamic_graphics_state *dyn =
      &cmd_buffer->vk.dynamic_graphics_state;

   cmd_buffer_emit_clip(cmd_buffer);

   if ((cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                       ANV_CMD_DIRTY_XFB_ENABLE)) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_RASTERIZATION_STREAM) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX))
      cmd_buffer_emit_streamout(cmd_buffer);

   if ((cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                       ANV_CMD_DIRTY_RENDER_TARGETS)) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_SCISSORS) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE)) {
      cmd_buffer_emit_viewport(cmd_buffer);
      cmd_buffer_emit_depth_viewport(cmd_buffer);
      cmd_buffer_emit_scissor(cmd_buffer);
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY)) {
      uint32_t topology;
      if (anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL))
         topology = _3DPRIM_PATCHLIST(dyn->ts.patch_control_points);
      else
         topology = genX(vk_to_intel_primitive_type)[dyn->ia.primitive_topology];

      cmd_buffer->state.gfx.primitive_topology = topology;

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_VF_TOPOLOGY), vft) {
         vft.PrimitiveTopologyType = topology;
      }
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_VI)) {
      const uint32_t ve_count =
         pipeline->vs_input_elements + pipeline->svgs_count;
      const uint32_t num_dwords = 1 + 2 * MAX2(1, ve_count);
      uint32_t *p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                                    GENX(3DSTATE_VERTEX_ELEMENTS));

      if (p) {
         if (ve_count == 0) {
            memcpy(p + 1, cmd_buffer->device->empty_vs_input,
                   sizeof(cmd_buffer->device->empty_vs_input));
         } else if (ve_count == pipeline->vertex_input_elems) {
            /* MESA_VK_DYNAMIC_VI is not dynamic for this pipeline, so
             * everything is in pipeline->vertex_input_data and we can just
             * memcpy
             */
            memcpy(p + 1, pipeline->vertex_input_data, 4 * 2 * ve_count);
         } else {
            /* Use dyn->vi to emit the dynamic VERTEX_ELEMENT_STATE input. */
            genX(emit_vertex_input)(&cmd_buffer->batch, p + 1,
                                    pipeline, dyn->vi);
            /* Then append the VERTEX_ELEMENT_STATE for the draw parameters */
            memcpy(p + 1 + 2 * pipeline->vs_input_elements,
                   pipeline->vertex_input_data,
                   4 * 2 * pipeline->vertex_input_elems);
         }
      }
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_TS_DOMAIN_ORIGIN)) {
      genX(cmd_emit_te)(cmd_buffer);
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX)) {
      genX(emit_gs)(cmd_buffer);
   }

#if GFX_VER >= 11
   if (cmd_buffer->device->vk.enabled_extensions.KHR_fragment_shading_rate &&
       (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE ||
        BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_FSR)))
      genX(emit_shading_rate)(&cmd_buffer->batch, pipeline, &dyn->fsr);
#endif /* GFX_VER >= 11 */

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_WIDTH) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS)) {
      anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_SF),
                           pipeline->partial.sf, sf) {
         ANV_SETUP_PROVOKING_VERTEX(sf, dyn->rs.provoking_vertex);

         sf.LineWidth = dyn->rs.line.width;

         /**
          * From the Vulkan Spec:
          *
          *    "VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT specifies that the depth
          *     bias representation is a factor of constant r equal to 1."
          *
          * From the SKL PRMs, Volume 7: 3D-Media-GPGPU, Depth Offset:
          *
          *    "When UNORM Depth Buffer is at Output Merger (or no Depth Buffer):
          *
          *     Bias = GlobalDepthOffsetConstant * r + GlobalDepthOffsetScale * MaxDepthSlope
          *
          *     Where r is the minimum representable value > 0 in the depth
          *     buffer format, converted to float32 (note: If state bit Legacy
          *     Global Depth Bias Enable is set, the r term will be forced to
          *     1.0)"
          *
          * When VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT is set, enable
          * LegacyGlobalDepthBiasEnable.
          */
         sf.LegacyGlobalDepthBiasEnable =
            dyn->rs.depth_bias.representation == VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
      }
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_MODE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_CONSERVATIVE_MODE)) {
      /* Take dynamic primitive topology in to account with
       *    3DSTATE_RASTER::APIMode
       *    3DSTATE_RASTER::DXMultisampleRasterizationEnable
       *    3DSTATE_RASTER::AntialiasingEnable
       */
      uint32_t api_mode = 0;
      bool msaa_raster_enable = false;

      VkLineRasterizationModeEXT line_mode =
         anv_line_rasterization_mode(dyn->rs.line.mode,
                                     pipeline->rasterization_samples);

      VkPolygonMode dynamic_raster_mode =
         genX(raster_polygon_mode)(cmd_buffer->state.gfx.pipeline,
                                   dyn->rs.polygon_mode,
                                   dyn->ia.primitive_topology);

      genX(rasterization_mode)(dynamic_raster_mode,
                               line_mode, dyn->rs.line.width,
                               &api_mode, &msaa_raster_enable);

     /* From the Browadwell PRM, Volume 2, documentation for
      * 3DSTATE_RASTER, "Antialiasing Enable":
      *
      * "This field must be disabled if any of the render targets
      * have integer (UINT or SINT) surface format."
      *
      * Additionally internal documentation for Gfx12+ states:
      *
      * "This bit MUST not be set when NUM_MULTISAMPLES > 1 OR
      *  FORCED_SAMPLE_COUNT > 1."
      */
      bool aa_enable =
         anv_rasterization_aa_mode(dynamic_raster_mode, line_mode) &&
         !cmd_buffer->state.gfx.has_uint_rt &&
         !(GFX_VER >= 12 && cmd_buffer->state.gfx.samples > 1);

      bool depth_clip_enable =
         vk_rasterization_state_depth_clip_enable(&dyn->rs);

      anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_RASTER),
                           pipeline->partial.raster, raster) {
         raster.APIMode = api_mode;
         raster.DXMultisampleRasterizationEnable   = msaa_raster_enable;
         raster.AntialiasingEnable                 = aa_enable;
         raster.CullMode                           = genX(vk_to_intel_cullmode)[
                                                        dyn->rs.cull_mode];
         raster.FrontWinding                       = genX(vk_to_intel_front_face)[
                                                        dyn->rs.front_face];
         raster.GlobalDepthOffsetEnableSolid       = dyn->rs.depth_bias.enable;
         raster.GlobalDepthOffsetEnableWireframe   = dyn->rs.depth_bias.enable;
         raster.GlobalDepthOffsetEnablePoint       = dyn->rs.depth_bias.enable;
         raster.GlobalDepthOffsetConstant          = dyn->rs.depth_bias.constant;
         raster.GlobalDepthOffsetScale             = dyn->rs.depth_bias.slope;
         raster.GlobalDepthOffsetClamp             = dyn->rs.depth_bias.clamp;
         raster.FrontFaceFillMode                  = genX(vk_to_intel_fillmode)[
                                                        dyn->rs.polygon_mode];
         raster.BackFaceFillMode                   = genX(vk_to_intel_fillmode)[
                                                        dyn->rs.polygon_mode];
         raster.ViewportZFarClipTestEnable         = depth_clip_enable;
         raster.ViewportZNearClipTestEnable        = depth_clip_enable;
         raster.ConservativeRasterizationEnable    =
            dyn->rs.conservative_mode !=
            VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
      }
   }

   if ((cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)))
      genX(cmd_emit_sample_mask)(cmd_buffer);

   if ((cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                       ANV_CMD_DIRTY_RENDER_TARGETS)) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_OP) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
      VkImageAspectFlags ds_aspects = 0;
      if (cmd_buffer->state.gfx.depth_att.vk_format != VK_FORMAT_UNDEFINED)
         ds_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (cmd_buffer->state.gfx.stencil_att.vk_format != VK_FORMAT_UNDEFINED)
         ds_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;

      struct vk_depth_stencil_state opt_ds = dyn->ds;
      vk_optimize_depth_stencil_state(&opt_ds, ds_aspects, true);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_WM_DEPTH_STENCIL), ds) {
         ds.DoubleSidedStencilEnable = true;

         ds.StencilTestMask = opt_ds.stencil.front.compare_mask & 0xff;
         ds.StencilWriteMask = opt_ds.stencil.front.write_mask & 0xff;

         ds.BackfaceStencilTestMask = opt_ds.stencil.back.compare_mask & 0xff;
         ds.BackfaceStencilWriteMask = opt_ds.stencil.back.write_mask & 0xff;

         ds.StencilReferenceValue = opt_ds.stencil.front.reference & 0xff;
         ds.BackfaceStencilReferenceValue = opt_ds.stencil.back.reference & 0xff;

         ds.DepthTestEnable = opt_ds.depth.test_enable;
         ds.DepthBufferWriteEnable = opt_ds.depth.write_enable;
         ds.DepthTestFunction = genX(vk_to_intel_compare_op)[opt_ds.depth.compare_op];
         ds.StencilTestEnable = opt_ds.stencil.test_enable;
         ds.StencilBufferWriteEnable = opt_ds.stencil.write_enable;
         ds.StencilFailOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.front.op.fail];
         ds.StencilPassDepthPassOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.front.op.pass];
         ds.StencilPassDepthFailOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.front.op.depth_fail];
         ds.StencilTestFunction = genX(vk_to_intel_compare_op)[opt_ds.stencil.front.op.compare];
         ds.BackfaceStencilFailOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.back.op.fail];
         ds.BackfaceStencilPassDepthPassOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.back.op.pass];
         ds.BackfaceStencilPassDepthFailOp = genX(vk_to_intel_stencil_op)[opt_ds.stencil.back.op.depth_fail];
         ds.BackfaceStencilTestFunction = genX(vk_to_intel_compare_op)[opt_ds.stencil.back.op.compare];
      }

#if INTEL_NEEDS_WA_18019816803
      if (intel_needs_workaround(cmd_buffer->device->info, 18019816803)) {
         bool ds_write_state = opt_ds.depth.write_enable || opt_ds.stencil.write_enable;
         if (cmd_buffer->state.gfx.ds_write_state != ds_write_state) {
            genX(batch_emit_pipe_control)(&cmd_buffer->batch, cmd_buffer->device->info,
                                          ANV_PIPE_PSS_STALL_SYNC_BIT);
            cmd_buffer->state.gfx.ds_write_state = ds_write_state;
         }
      }
#endif

      const bool pma = want_stencil_pma_fix(cmd_buffer, &opt_ds);
      genX(cmd_buffer_enable_pma_fix)(cmd_buffer, pma);
   }

#if GFX_VER >= 12
   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_BOUNDS)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_DEPTH_BOUNDS), db) {
         db.DepthBoundsTestEnable = dyn->ds.depth.bounds_test.enable;
         db.DepthBoundsTestMinValue = dyn->ds.depth.bounds_test.min;
         db.DepthBoundsTestMaxValue = dyn->ds.depth.bounds_test.max;
      }
   }
#endif

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_STIPPLE)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_LINE_STIPPLE), ls) {
         ls.LineStipplePattern = dyn->rs.line.stipple.pattern;
         ls.LineStippleInverseRepeatCount =
            1.0f / MAX2(1, dyn->rs.line.stipple.factor);
         ls.LineStippleRepeatCount = dyn->rs.line.stipple.factor;
      }
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_RESTART_INDEX) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_VF), vf) {
#if GFX_VERx10 >= 125
         vf.GeometryDistributionEnable = true;
#endif
         vf.IndexedDrawCutIndexEnable  = dyn->ia.primitive_restart_enable;
         vf.CutIndex                   = cmd_buffer->state.gfx.restart_index;
      }
   }

   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_INDEX_BUFFER) {
      struct anv_buffer *buffer = cmd_buffer->state.gfx.index_buffer;
      uint32_t offset = cmd_buffer->state.gfx.index_offset;
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_INDEX_BUFFER), ib) {
         ib.IndexFormat           = cmd_buffer->state.gfx.index_type;
         ib.MOCS                  = anv_mocs(cmd_buffer->device,
                                             buffer->address.bo,
                                             ISL_SURF_USAGE_INDEX_BUFFER_BIT);
#if GFX_VER >= 12
         ib.L3BypassDisable       = true;
#endif
         ib.BufferStartingAddress = anv_address_add(buffer->address, offset);
         ib.BufferSize            = cmd_buffer->state.gfx.index_size;
      }
   }

#if GFX_VERx10 >= 125
   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_VFG), vfg) {
         /* If 3DSTATE_TE: TE Enable == 1 then RR_STRICT else RR_FREE*/
         vfg.DistributionMode =
            anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL) ? RR_STRICT :
                                                                      RR_FREE;
         vfg.DistributionGranularity = BatchLevelGranularity;
         /* Wa_14014890652 */
         if (intel_device_info_is_dg2(cmd_buffer->device->info))
            vfg.GranularityThresholdDisable = 1;
         vfg.ListCutIndexEnable = dyn->ia.primitive_restart_enable;
         /* 192 vertices for TRILIST_ADJ */
         vfg.ListNBatchSizeScale = 0;
         /* Batch size of 384 vertices */
         vfg.List3BatchSizeScale = 2;
         /* Batch size of 128 vertices */
         vfg.List2BatchSizeScale = 1;
         /* Batch size of 128 vertices */
         vfg.List1BatchSizeScale = 2;
         /* Batch size of 256 vertices for STRIP topologies */
         vfg.StripBatchSizeScale = 3;
         /* 192 control points for PATCHLIST_3 */
         vfg.PatchBatchSizeScale = 1;
         /* 192 control points for PATCHLIST_3 */
         vfg.PatchBatchSizeMultiplier = 31;
      }
   }
#endif

   if (cmd_buffer->device->vk.enabled_extensions.EXT_sample_locations &&
       (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS) ||
        BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS_ENABLE))) {
      genX(emit_sample_pattern)(&cmd_buffer->batch,
                                dyn->ms.sample_locations_enable ?
                                dyn->ms.sample_locations : NULL);
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE)) {
      /* 3DSTATE_WM in the hope we can avoid spawning fragment shaders
       * threads.
       */
      anv_batch_emit_merge(&cmd_buffer->batch, GENX(3DSTATE_WM),
                           pipeline->partial.wm, wm) {
         wm.ForceThreadDispatchEnable = anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT) &&
                                        (pipeline->force_fragment_thread_dispatch ||
                                        anv_cmd_buffer_all_color_write_masked(cmd_buffer)) ?
                                        ForceON : 0;
         wm.LineStippleEnable = dyn->rs.line.stipple.enable;
      };
   }

   if ((cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_LOGIC_OP) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_LOGIC_OP_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_MS_ALPHA_TO_ONE_ENABLE) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_WRITE_MASKS) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_ENABLES) ||
       BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS)) {
      const uint8_t color_writes = dyn->cb.color_write_enables;
      const struct anv_cmd_graphics_state *state = &cmd_buffer->state.gfx;
      const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);
      bool has_writeable_rt =
         anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT) &&
         (color_writes & ((1u << state->color_att_count) - 1)) != 0;

      uint32_t num_dwords = GENX(BLEND_STATE_length) +
         GENX(BLEND_STATE_ENTRY_length) * MAX_RTS;
      struct anv_state blend_states =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                            num_dwords * 4,
                                            64);

      uint32_t *dws = blend_states.map;

      struct GENX(BLEND_STATE) blend_state = {
         .AlphaToCoverageEnable = dyn->ms.alpha_to_coverage_enable,
         .AlphaToOneEnable = dyn->ms.alpha_to_one_enable,
      };

      /* Jump to blend entries. */
      dws += GENX(BLEND_STATE_length);

      struct GENX(BLEND_STATE_ENTRY) bs0 = { 0 };

      /* Wa_14018912822, check if we set these during RT setup. */
      bool color_blend_zero = false;
      bool alpha_blend_zero = false;

      for (uint32_t i = 0; i < MAX_RTS; i++) {
         /* Disable anything above the current number of color attachments. */
         bool write_disabled = i >= cmd_buffer->state.gfx.color_att_count ||
                               (color_writes & BITFIELD_BIT(i)) == 0;
         struct GENX(BLEND_STATE_ENTRY) entry = {
            .WriteDisableAlpha = write_disabled ||
                                 (dyn->cb.attachments[i].write_mask &
                                  VK_COLOR_COMPONENT_A_BIT) == 0,
            .WriteDisableRed   = write_disabled ||
                                 (dyn->cb.attachments[i].write_mask &
                                  VK_COLOR_COMPONENT_R_BIT) == 0,
            .WriteDisableGreen = write_disabled ||
                                 (dyn->cb.attachments[i].write_mask &
                                  VK_COLOR_COMPONENT_G_BIT) == 0,
            .WriteDisableBlue  = write_disabled ||
                                 (dyn->cb.attachments[i].write_mask &
                                  VK_COLOR_COMPONENT_B_BIT) == 0,
            /* Vulkan specification 1.2.168, VkLogicOp:
             *
             *   "Logical operations are controlled by the logicOpEnable and
             *   logicOp members of VkPipelineColorBlendStateCreateInfo. If
             *   logicOpEnable is VK_TRUE, then a logical operation selected
             *   by logicOp is applied between each color attachment and the
             *   fragment’s corresponding output value, and blending of all
             *   attachments is treated as if it were disabled."
             *
             * From the Broadwell PRM Volume 2d: Command Reference:
             * Structures: BLEND_STATE_ENTRY:
             *
             *   "Enabling LogicOp and Color Buffer Blending at the same time
             *   is UNDEFINED"
             */
            .LogicOpFunction   = genX(vk_to_intel_logic_op)[dyn->cb.logic_op],
            .LogicOpEnable     = dyn->cb.logic_op_enable,
            .ColorBufferBlendEnable =
               !dyn->cb.logic_op_enable && dyn->cb.attachments[i].blend_enable,

            .ColorClampRange = COLORCLAMP_RTFORMAT,
            .PreBlendColorClampEnable = true,
            .PostBlendColorClampEnable = true,
         };

         /* Setup blend equation. */
         entry.SourceBlendFactor =
            genX(vk_to_intel_blend)[dyn->cb.attachments[i].src_color_blend_factor];
         entry.DestinationBlendFactor =
            genX(vk_to_intel_blend)[dyn->cb.attachments[i].dst_color_blend_factor];
         entry.ColorBlendFunction =
            genX(vk_to_intel_blend_op)[dyn->cb.attachments[i].color_blend_op];
         entry.SourceAlphaBlendFactor =
            genX(vk_to_intel_blend)[dyn->cb.attachments[i].src_alpha_blend_factor];
         entry.DestinationAlphaBlendFactor =
            genX(vk_to_intel_blend)[dyn->cb.attachments[i].dst_alpha_blend_factor];
         entry.AlphaBlendFunction =
            genX(vk_to_intel_blend_op)[dyn->cb.attachments[i].alpha_blend_op];

         if (dyn->cb.attachments[i].src_color_blend_factor !=
             dyn->cb.attachments[i].src_alpha_blend_factor ||
             dyn->cb.attachments[i].dst_color_blend_factor !=
             dyn->cb.attachments[i].dst_alpha_blend_factor ||
             dyn->cb.attachments[i].color_blend_op !=
             dyn->cb.attachments[i].alpha_blend_op) {
            blend_state.IndependentAlphaBlendEnable = true;
         }

         /* The Dual Source Blending documentation says:
          *
          * "If SRC1 is included in a src/dst blend factor and
          * a DualSource RT Write message is not used, results
          * are UNDEFINED. (This reflects the same restriction in DX APIs,
          * where undefined results are produced if “o1” is not written
          * by a PS – there are no default values defined)."
          *
          * There is no way to gracefully fix this undefined situation
          * so we just disable the blending to prevent possible issues.
          */
         if (wm_prog_data && !wm_prog_data->dual_src_blend &&
             anv_is_dual_src_blend_equation(&dyn->cb.attachments[i])) {
            entry.ColorBufferBlendEnable = false;
         }

         /* Our hardware applies the blend factor prior to the blend function
          * regardless of what function is used.  Technically, this means the
          * hardware can do MORE than GL or Vulkan specify.  However, it also
          * means that, for MIN and MAX, we have to stomp the blend factor to
          * ONE to make it a no-op.
          */
         if (dyn->cb.attachments[i].color_blend_op == VK_BLEND_OP_MIN ||
             dyn->cb.attachments[i].color_blend_op == VK_BLEND_OP_MAX) {
            entry.SourceBlendFactor = BLENDFACTOR_ONE;
            entry.DestinationBlendFactor = BLENDFACTOR_ONE;
         }
         if (dyn->cb.attachments[i].alpha_blend_op == VK_BLEND_OP_MIN ||
             dyn->cb.attachments[i].alpha_blend_op == VK_BLEND_OP_MAX) {
            entry.SourceAlphaBlendFactor = BLENDFACTOR_ONE;
            entry.DestinationAlphaBlendFactor = BLENDFACTOR_ONE;
         }

         /* When MSAA is enabled, instead of using BLENDFACTOR_ZERO use CONST_COLOR,
          * CONST_ALPHA and supply zero by using blend constants.
          */
         if (intel_needs_workaround(cmd_buffer->device->info, 14018912822) &&
             pipeline->rasterization_samples > 1) {
            if (entry.DestinationBlendFactor == BLENDFACTOR_ZERO) {
               entry.DestinationBlendFactor = BLENDFACTOR_CONST_COLOR;
               color_blend_zero = true;
            }
            if (entry.DestinationAlphaBlendFactor == BLENDFACTOR_ZERO) {
               entry.DestinationAlphaBlendFactor = BLENDFACTOR_CONST_ALPHA;
               alpha_blend_zero = true;
            }
         }

         GENX(BLEND_STATE_ENTRY_pack)(NULL, dws, &entry);

         if (i == 0)
            bs0 = entry;

         dws += GENX(BLEND_STATE_ENTRY_length);
      }

      /* Blend constants modified for Wa_14018912822. */
      if (cmd_buffer->state.gfx.color_blend_zero != color_blend_zero) {
         cmd_buffer->state.gfx.color_blend_zero = color_blend_zero;
         BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS);
      }
      if (cmd_buffer->state.gfx.alpha_blend_zero != alpha_blend_zero) {
         cmd_buffer->state.gfx.alpha_blend_zero = alpha_blend_zero;
         BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS);
      }

      /* Generate blend state after entries. */
      GENX(BLEND_STATE_pack)(NULL, blend_states.map, &blend_state);

      /* 3DSTATE_PS_BLEND to be consistent with the rest of the
       * BLEND_STATE_ENTRY.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_PS_BLEND), blend) {
         blend.HasWriteableRT                = has_writeable_rt,
         blend.ColorBufferBlendEnable        = bs0.ColorBufferBlendEnable;
         blend.SourceAlphaBlendFactor        = bs0.SourceAlphaBlendFactor;
         blend.DestinationAlphaBlendFactor   = bs0.DestinationAlphaBlendFactor;
         blend.SourceBlendFactor             = bs0.SourceBlendFactor;
         blend.DestinationBlendFactor        = bs0.DestinationBlendFactor;
         blend.AlphaTestEnable               = false;
         blend.IndependentAlphaBlendEnable   = blend_state.IndependentAlphaBlendEnable;
         blend.AlphaToCoverageEnable         = dyn->ms.alpha_to_coverage_enable;
      }

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_BLEND_STATE_POINTERS), bsp) {
         bsp.BlendStatePointer      = blend_states.offset;
         bsp.BlendStatePointerValid = true;
      }
   }

   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS)) {
      struct anv_state cc_state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                            GENX(COLOR_CALC_STATE_length) * 4,
                                            64);
      struct GENX(COLOR_CALC_STATE) cc = {
         .BlendConstantColorRed   = cmd_buffer->state.gfx.color_blend_zero ?
                                    0.0f : dyn->cb.blend_constants[0],
         .BlendConstantColorGreen = cmd_buffer->state.gfx.color_blend_zero ?
                                    0.0f : dyn->cb.blend_constants[1],
         .BlendConstantColorBlue  = cmd_buffer->state.gfx.color_blend_zero ?
                                    0.0f : dyn->cb.blend_constants[2],
         .BlendConstantColorAlpha = cmd_buffer->state.gfx.alpha_blend_zero ?
                                    0.0f : dyn->cb.blend_constants[3],
      };
      GENX(COLOR_CALC_STATE_pack)(NULL, cc_state.map, &cc);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CC_STATE_POINTERS), ccp) {
         ccp.ColorCalcStatePointer = cc_state.offset;
         ccp.ColorCalcStatePointerValid = true;
      }
   }

   /* When we're done, there is no more dirty gfx state. */
   vk_dynamic_graphics_state_clear_dirty(&cmd_buffer->vk.dynamic_graphics_state);
   cmd_buffer->state.gfx.dirty = 0;
}

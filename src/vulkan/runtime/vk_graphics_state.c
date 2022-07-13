#include "vk_graphics_state.h"

#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_log.h"
#include "vk_render_pass.h"
#include "vk_standard_sample_locations.h"
#include "vk_util.h"

#include <assert.h>

enum mesa_vk_graphics_state_groups {
   MESA_VK_GRAPHICS_STATE_VERTEX_INPUT_BIT            = (1 << 0),
   MESA_VK_GRAPHICS_STATE_INPUT_ASSEMBLY_BIT          = (1 << 1),
   MESA_VK_GRAPHICS_STATE_TESSELLATION_BIT            = (1 << 2),
   MESA_VK_GRAPHICS_STATE_VIEWPORT_BIT                = (1 << 3),
   MESA_VK_GRAPHICS_STATE_DISCARD_RECTANGLES_BIT      = (1 << 4),
   MESA_VK_GRAPHICS_STATE_RASTERIZATION_BIT           = (1 << 5),
   MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT   = (1 << 6),
   MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT             = (1 << 7),
   MESA_VK_GRAPHICS_STATE_DEPTH_STENCIL_BIT           = (1 << 8),
   MESA_VK_GRAPHICS_STATE_COLOR_BLEND_BIT             = (1 << 9),
   MESA_VK_GRAPHICS_STATE_RENDER_PASS_BIT             = (1 << 10),
};

static void
clear_all_dynamic_state(BITSET_WORD *dynamic)
{
   /* Clear the whole array so there are no undefined bits at the top */
   memset(dynamic, 0, sizeof(*dynamic) *
          BITSET_WORDS(MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX));
}

static void
get_dynamic_state_groups(BITSET_WORD *dynamic,
                         enum mesa_vk_graphics_state_groups groups)
{
   clear_all_dynamic_state(dynamic);

   if (groups & MESA_VK_GRAPHICS_STATE_VERTEX_INPUT_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VI);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VI_BINDING_STRIDES);
   }

   if (groups & MESA_VK_GRAPHICS_STATE_INPUT_ASSEMBLY_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE);
   }

   if (groups & MESA_VK_GRAPHICS_STATE_TESSELLATION_BIT)
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS);

   if (groups & MESA_VK_GRAPHICS_STATE_VIEWPORT_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_VP_SCISSORS);
   }

   if (groups & MESA_VK_GRAPHICS_STATE_DISCARD_RECTANGLES_BIT)
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DR_RECTANGLES);

   if (groups & MESA_VK_GRAPHICS_STATE_RASTERIZATION_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_LINE_WIDTH);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE);
   }

   if (groups & MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT)
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_FSR);

   if (groups & MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT)
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS);

   if (groups & MESA_VK_GRAPHICS_STATE_DEPTH_STENCIL_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_BOUNDS);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_STENCIL_OP);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE);
   }

   if (groups & MESA_VK_GRAPHICS_STATE_COLOR_BLEND_BIT) {
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_CB_LOGIC_OP);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES);
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS);
   }
}

static void
validate_dynamic_state_groups(const BITSET_WORD *dynamic,
                              enum mesa_vk_graphics_state_groups groups)
{
#ifndef NDEBUG
   BITSET_DECLARE(all_dynamic, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   get_dynamic_state_groups(all_dynamic, groups);

   for (uint32_t w = 0; w < ARRAY_SIZE(all_dynamic); w++)
      assert(!(dynamic[w] & ~all_dynamic[w]));
#endif
}

void
vk_get_dynamic_graphics_states(BITSET_WORD *dynamic,
                               const VkPipelineDynamicStateCreateInfo *info)
{
   clear_all_dynamic_state(dynamic);

   /* From the Vulkan 1.3.218 spec:
    *
    *    "pDynamicState is a pointer to a VkPipelineDynamicStateCreateInfo
    *    structure defining which properties of the pipeline state object are
    *    dynamic and can be changed independently of the pipeline state. This
    *    can be NULL, which means no state in the pipeline is considered
    *    dynamic."
    */
   if (info == NULL)
      return;

#define CASE(VK, MESA) \
   case VK_DYNAMIC_STATE_##VK: \
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_##MESA); \
      break;

#define CASE2(VK, MESA1, MESA2) \
   case VK_DYNAMIC_STATE_##VK: \
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_##MESA1); \
      BITSET_SET(dynamic, MESA_VK_DYNAMIC_##MESA2); \
      break;

   for (uint32_t i = 0; i < info->dynamicStateCount; i++) {
      switch (info->pDynamicStates[i]) {
      CASE2(VERTEX_INPUT_EXT,             VI, VI_BINDING_STRIDES)
      CASE( VERTEX_INPUT_BINDING_STRIDE,  VI_BINDING_STRIDES)
      CASE( VIEWPORT,                     VP_VIEWPORTS)
      CASE( SCISSOR,                      VP_SCISSORS)
      CASE( LINE_WIDTH,                   RS_LINE_WIDTH)
      CASE( DEPTH_BIAS,                   RS_DEPTH_BIAS_FACTORS)
      CASE( BLEND_CONSTANTS,              CB_BLEND_CONSTANTS)
      CASE( DEPTH_BOUNDS,                 DS_DEPTH_BOUNDS_TEST_BOUNDS)
      CASE( STENCIL_COMPARE_MASK,         DS_STENCIL_COMPARE_MASK)
      CASE( STENCIL_WRITE_MASK,           DS_STENCIL_WRITE_MASK)
      CASE( STENCIL_REFERENCE,            DS_STENCIL_REFERENCE)
      CASE( CULL_MODE,                    RS_CULL_MODE)
      CASE( FRONT_FACE,                   RS_FRONT_FACE)
      CASE( PRIMITIVE_TOPOLOGY,           IA_PRIMITIVE_TOPOLOGY)
      CASE2(VIEWPORT_WITH_COUNT,          VP_VIEWPORT_COUNT, VP_VIEWPORTS)
      CASE2(SCISSOR_WITH_COUNT,           VP_SCISSOR_COUNT, VP_SCISSORS)
      CASE( DEPTH_TEST_ENABLE,            DS_DEPTH_TEST_ENABLE)
      CASE( DEPTH_WRITE_ENABLE,           DS_DEPTH_WRITE_ENABLE)
      CASE( DEPTH_COMPARE_OP,             DS_DEPTH_COMPARE_OP)
      CASE( DEPTH_BOUNDS_TEST_ENABLE,     DS_DEPTH_BOUNDS_TEST_ENABLE)
      CASE( STENCIL_TEST_ENABLE,          DS_STENCIL_TEST_ENABLE)
      CASE( STENCIL_OP,                   DS_STENCIL_OP)
      CASE( RASTERIZER_DISCARD_ENABLE,    RS_RASTERIZER_DISCARD_ENABLE)
      CASE( DEPTH_BIAS_ENABLE,            RS_DEPTH_BIAS_ENABLE)
      CASE( PRIMITIVE_RESTART_ENABLE,     IA_PRIMITIVE_RESTART_ENABLE)
      CASE( DISCARD_RECTANGLE_EXT,        DR_RECTANGLES)
      CASE( SAMPLE_LOCATIONS_EXT,         MS_SAMPLE_LOCATIONS)
      CASE( FRAGMENT_SHADING_RATE_KHR,    FSR)
      CASE( LINE_STIPPLE_EXT,             RS_LINE_STIPPLE)
      CASE( PATCH_CONTROL_POINTS_EXT,     TS_PATCH_CONTROL_POINTS)
      CASE( LOGIC_OP_EXT,                 CB_LOGIC_OP)
      CASE( COLOR_WRITE_ENABLE_EXT,       CB_COLOR_WRITE_ENABLES)
      default:
         unreachable("Unsupported dynamic graphics state");
      }
   }
}

#define IS_DYNAMIC(STATE) \
   BITSET_TEST(dynamic, MESA_VK_DYNAMIC_##STATE)

static void
vk_vertex_input_state_init(struct vk_vertex_input_state *vi,
                           const BITSET_WORD *dynamic,
                           const VkPipelineVertexInputStateCreateInfo *vi_info)
{
   assert(!IS_DYNAMIC(VI));

   memset(vi, 0, sizeof(*vi));

   for (uint32_t i = 0; i < vi_info->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi_info->pVertexBindingDescriptions[i];

      assert(desc->binding < MESA_VK_MAX_VERTEX_BINDINGS);
      assert(desc->stride <= MESA_VK_MAX_VERTEX_BINDING_STRIDE);
      assert(desc->inputRate <= 1);

      const uint32_t b = desc->binding;
      vi->bindings_valid |= BITFIELD_BIT(b);
      vi->bindings[b].stride = desc->stride;
      vi->bindings[b].input_rate = desc->inputRate;
      vi->bindings[b].divisor = 1;
   }

   for (uint32_t i = 0; i < vi_info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &vi_info->pVertexAttributeDescriptions[i];

      assert(desc->location < MESA_VK_MAX_VERTEX_ATTRIBUTES);
      assert(desc->binding < MESA_VK_MAX_VERTEX_BINDINGS);
      assert(vi->bindings_valid & BITFIELD_BIT(desc->binding));

      const uint32_t a = desc->location;
      vi->attributes_valid |= BITFIELD_BIT(a);
      vi->attributes[a].binding = desc->binding;
      vi->attributes[a].format = desc->format;
      vi->attributes[a].offset = desc->offset;
   }

   const VkPipelineVertexInputDivisorStateCreateInfoEXT *vi_div_state =
      vk_find_struct_const(vi_info->pNext,
                           PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);
   if (vi_div_state) {
      for (uint32_t i = 0; i < vi_div_state->vertexBindingDivisorCount; i++) {
         const VkVertexInputBindingDivisorDescriptionEXT *desc =
            &vi_div_state->pVertexBindingDivisors[i];

         assert(desc->binding < MESA_VK_MAX_VERTEX_BINDINGS);
         assert(vi->bindings_valid & BITFIELD_BIT(desc->binding));

         const uint32_t b = desc->binding;
         vi->bindings[b].divisor = desc->divisor;
      }
   }
}

static void
vk_input_assembly_state_init(struct vk_input_assembly_state *ia,
                             const BITSET_WORD *dynamic,
                             const VkPipelineInputAssemblyStateCreateInfo *ia_info)
{
   if (IS_DYNAMIC(IA_PRIMITIVE_TOPOLOGY)) {
      ia->primitive_topology = -1;
   } else {
      assert(ia_info->topology <= UINT8_MAX);
      ia->primitive_topology = ia_info->topology;
   }

   ia->primitive_restart_enable = ia_info->primitiveRestartEnable;
}

static void
vk_tessellation_state_init(struct vk_tessellation_state *ts,
                           const BITSET_WORD *dynamic,
                           const VkPipelineTessellationStateCreateInfo *ts_info)
{
   if (IS_DYNAMIC(TS_PATCH_CONTROL_POINTS)) {
      ts->patch_control_points = 0;
   } else {
      assert(ts_info->patchControlPoints <= UINT8_MAX);
      ts->patch_control_points = ts_info->patchControlPoints;
   }

   const VkPipelineTessellationDomainOriginStateCreateInfo *ts_do_info =
      vk_find_struct_const(ts_info->pNext,
                           PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO);
   if (ts_do_info != NULL) {
      assert(ts_do_info->domainOrigin <= UINT8_MAX);
      ts->domain_origin = ts_do_info->domainOrigin;
   } else {
      ts->domain_origin = VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
   }
}

static void
vk_viewport_state_init(struct vk_viewport_state *vp,
                       const BITSET_WORD *dynamic,
                       const VkPipelineViewportStateCreateInfo *vp_info)
{
   memset(vp, 0, sizeof(*vp));

   if (!IS_DYNAMIC(VP_VIEWPORT_COUNT)) {
      assert(vp_info->viewportCount <= MESA_VK_MAX_VIEWPORTS);
      vp->viewport_count = vp_info->viewportCount;
   }

   if (!IS_DYNAMIC(VP_VIEWPORTS)) {
      assert(!IS_DYNAMIC(VP_VIEWPORT_COUNT));
      typed_memcpy(vp->viewports, vp_info->pViewports,
                   vp_info->viewportCount);
   }

   if (!IS_DYNAMIC(VP_SCISSOR_COUNT)) {
      assert(vp_info->scissorCount <= MESA_VK_MAX_SCISSORS);
      vp->scissor_count = vp_info->scissorCount;
   }

   if (!IS_DYNAMIC(VP_SCISSORS)) {
      assert(!IS_DYNAMIC(VP_SCISSOR_COUNT));
      typed_memcpy(vp->scissors, vp_info->pScissors,
                   vp_info->scissorCount);
   }

   const VkPipelineViewportDepthClipControlCreateInfoEXT *vp_dcc_info =
      vk_find_struct_const(vp_info->pNext,
                           PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT);
   if (vp_dcc_info != NULL)
      vp->negative_one_to_one = vp_dcc_info->negativeOneToOne;
}

static void
vk_discard_rectangles_state_init(struct vk_discard_rectangles_state *dr,
                                 const BITSET_WORD *dynamic,
                                 const VkPipelineDiscardRectangleStateCreateInfoEXT *dr_info)
{
   memset(dr, 0, sizeof(*dr));

   if (dr_info == NULL)
      return;

   dr->mode = dr_info->discardRectangleMode;

   if (!IS_DYNAMIC(DR_RECTANGLES)) {
      assert(dr_info->discardRectangleCount <= MESA_VK_MAX_DISCARD_RECTANGLES);
      dr->rectangle_count = dr_info->discardRectangleCount;
      typed_memcpy(dr->rectangles, dr_info->pDiscardRectangles,
                   dr_info->discardRectangleCount);
   }
}

static void
vk_rasterization_state_init(struct vk_rasterization_state *rs,
                            const BITSET_WORD *dynamic,
                            const VkPipelineRasterizationStateCreateInfo *rs_info)
{
   *rs = (struct vk_rasterization_state) {
      .rasterizer_discard_enable = false,
      .conservative_mode = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT,
      .rasterization_order_amd = VK_RASTERIZATION_ORDER_STRICT_AMD,
      .provoking_vertex = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT,
      .line.mode = VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT,
   };

   if (!IS_DYNAMIC(RS_RASTERIZER_DISCARD_ENABLE))
      rs->rasterizer_discard_enable = rs_info->rasterizerDiscardEnable;

   /* From the Vulkan 1.3.218 spec:
    *
    *    "If VkPipelineRasterizationDepthClipStateCreateInfoEXT is present in
    *    the graphics pipeline state then depth clipping is disabled if
    *    VkPipelineRasterizationDepthClipStateCreateInfoEXT::depthClipEnable
    *    is VK_FALSE. Otherwise, if
    *    VkPipelineRasterizationDepthClipStateCreateInfoEXT is not present,
    *    depth clipping is disabled when
    *    VkPipelineRasterizationStateCreateInfo::depthClampEnable is VK_TRUE.
    */
   rs->depth_clamp_enable = rs_info->depthClampEnable;
   rs->depth_clip_enable = !rs_info->depthClampEnable;

   rs->polygon_mode = rs_info->polygonMode;

   rs->cull_mode = rs_info->cullMode;
   rs->front_face = rs_info->frontFace;
   rs->depth_bias.enable = rs_info->depthBiasEnable;
   if ((rs_info->depthBiasEnable || IS_DYNAMIC(RS_DEPTH_BIAS_ENABLE)) &&
       !IS_DYNAMIC(RS_DEPTH_BIAS_FACTORS)) {
      rs->depth_bias.constant = rs_info->depthBiasConstantFactor;
      rs->depth_bias.clamp = rs_info->depthBiasClamp;
      rs->depth_bias.slope = rs_info->depthBiasSlopeFactor;
   }
   rs->line.width = rs_info->lineWidth;

   vk_foreach_struct_const(ext, rs_info->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT: {
         const VkPipelineRasterizationConservativeStateCreateInfoEXT *rcs_info =
            (const VkPipelineRasterizationConservativeStateCreateInfoEXT *)ext;
         rs->conservative_mode = rcs_info->conservativeRasterizationMode;
         break;
      }

      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT: {
         const VkPipelineRasterizationDepthClipStateCreateInfoEXT *rdc_info =
            (const VkPipelineRasterizationDepthClipStateCreateInfoEXT *)ext;
         rs->depth_clip_enable = rdc_info->depthClipEnable;
         break;
      }

      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT: {
         const VkPipelineRasterizationLineStateCreateInfoEXT *rl_info =
            (const VkPipelineRasterizationLineStateCreateInfoEXT *)ext;
         rs->line.mode = rl_info->lineRasterizationMode;
         rs->line.stipple.enable = rl_info->stippledLineEnable;
         if (rs->line.stipple.enable && !IS_DYNAMIC(RS_LINE_STIPPLE)) {
            rs->line.stipple.factor = rl_info->lineStippleFactor;
            rs->line.stipple.pattern = rl_info->lineStipplePattern;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT: {
         const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *rpv_info =
            (const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *)ext;
         rs->provoking_vertex = rpv_info->provokingVertexMode;
         break;
      }

      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_RASTERIZATION_ORDER_AMD: {
         const VkPipelineRasterizationStateRasterizationOrderAMD *rro_info =
            (const VkPipelineRasterizationStateRasterizationOrderAMD *)ext;
         rs->rasterization_order_amd = rro_info->rasterizationOrder;
         break;
      }

      case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT: {
         const VkPipelineRasterizationStateStreamCreateInfoEXT *rss_info =
            (const VkPipelineRasterizationStateStreamCreateInfoEXT *)ext;
         rs->rasterization_stream = rss_info->rasterizationStream;
         break;
      }

      default:
         break;
      }
   }
}

static void
vk_fragment_shading_rate_state_init(
   struct vk_fragment_shading_rate_state *fsr,
   const BITSET_WORD *dynamic,
   const VkPipelineFragmentShadingRateStateCreateInfoKHR *fsr_info)
{
   if (fsr_info != NULL) {
      fsr->fragment_size = fsr_info->fragmentSize;
      fsr->combiner_ops[0] = fsr_info->combinerOps[0];
      fsr->combiner_ops[1] = fsr_info->combinerOps[1];
   } else {
      fsr->fragment_size = (VkExtent2D) { 1, 1 };
      fsr->combiner_ops[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
      fsr->combiner_ops[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
   }
}

static void
vk_sample_locations_state_init(struct vk_sample_locations_state *sl,
                               const VkSampleLocationsInfoEXT *sl_info)
{
   sl->per_pixel = sl_info->sampleLocationsPerPixel;
   sl->grid_size = sl_info->sampleLocationGridSize;

   /* From the Vulkan 1.3.218 spec:
    *
    *    VUID-VkSampleLocationsInfoEXT-sampleLocationsCount-01527
    *
    *    "sampleLocationsCount must equal sampleLocationsPerPixel *
    *    sampleLocationGridSize.width * sampleLocationGridSize.height"
    */
   assert(sl_info->sampleLocationsCount ==
          sl_info->sampleLocationsPerPixel *
          sl_info->sampleLocationGridSize.width *
          sl_info->sampleLocationGridSize.height);

   assert(sl_info->sampleLocationsCount <= MESA_VK_MAX_SAMPLE_LOCATIONS);
   typed_memcpy(sl->locations, sl_info->pSampleLocations,
                sl_info->sampleLocationsCount);
}

static void
vk_multisample_state_init(struct vk_multisample_state *ms,
                          const BITSET_WORD *dynamic,
                          const VkPipelineMultisampleStateCreateInfo *ms_info)
{
   ms->rasterization_samples = ms_info->rasterizationSamples;
   ms->sample_shading_enable = ms_info->sampleShadingEnable;
   ms->min_sample_shading = ms_info->minSampleShading;

   /* From the Vulkan 1.3.218 spec:
    *
    *    "If pSampleMask is NULL, it is treated as if the mask has all bits
    *    set to 1."
    */
   ms->sample_mask = ms_info->pSampleMask ? *ms_info->pSampleMask : ~0;

   ms->alpha_to_coverage_enable = ms_info->alphaToCoverageEnable;
   ms->alpha_to_one_enable = ms_info->alphaToOneEnable;

   /* These get filled in by vk_multisample_sample_locations_state_init() */
   ms->sample_locations_enable = false;
   ms->sample_locations = NULL;
}

static bool
needs_sample_locations_state(
   const BITSET_WORD *dynamic,
   const VkPipelineSampleLocationsStateCreateInfoEXT *sl_info)
{
   return !IS_DYNAMIC(MS_SAMPLE_LOCATIONS) &&
          sl_info != NULL && sl_info->sampleLocationsEnable;
}

static void
vk_multisample_sample_locations_state_init(
   struct vk_multisample_state *ms,
   struct vk_sample_locations_state *sl,
   const BITSET_WORD *dynamic,
   const VkPipelineMultisampleStateCreateInfo *ms_info,
   const VkPipelineSampleLocationsStateCreateInfoEXT *sl_info)
{
   ms->sample_locations_enable =
      sl_info != NULL && sl_info->sampleLocationsEnable;

   assert(ms->sample_locations == NULL);
   if (!IS_DYNAMIC(MS_SAMPLE_LOCATIONS)) {
      if (ms->sample_locations_enable) {
         vk_sample_locations_state_init(sl, &sl_info->sampleLocationsInfo);
         ms->sample_locations = sl;
      } else {
         /* Otherwise, pre-populate with the standard sample locations.  If
          * the driver doesn't support standard sample locations, it probably
          * doesn't support custom locations either and can completely ignore
          * this state.
          */
         ms->sample_locations =
            vk_standard_sample_locations_state(ms_info->rasterizationSamples);
      }
   }
}

static void
vk_stencil_test_face_state_init(struct vk_stencil_test_face_state *face,
                                const VkStencilOpState *info)
{
   face->op.fail = info->failOp;
   face->op.pass = info->passOp;
   face->op.depth_fail = info->depthFailOp;
   face->op.compare = info->compareOp;
   face->compare_mask = info->compareMask;
   face->write_mask = info->writeMask;
   face->reference = info->reference;
}

static void
vk_depth_stencil_state_init(struct vk_depth_stencil_state *ds,
                            const BITSET_WORD *dynamic,
                            const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   memset(ds, 0, sizeof(*ds));

   ds->depth.test_enable = ds_info->depthTestEnable;
   ds->depth.write_enable = ds_info->depthWriteEnable;
   ds->depth.compare_op = ds_info->depthCompareOp;
   ds->depth.bounds_test.enable = ds_info->depthBoundsTestEnable;
   ds->depth.bounds_test.min = ds_info->minDepthBounds;
   ds->depth.bounds_test.max = ds_info->maxDepthBounds;

   ds->stencil.test_enable = ds_info->stencilTestEnable;
   vk_stencil_test_face_state_init(&ds->stencil.front, &ds_info->front);
   vk_stencil_test_face_state_init(&ds->stencil.back, &ds_info->back);
}

static void
vk_color_blend_state_init(struct vk_color_blend_state *cb,
                          const BITSET_WORD *dynamic,
                          const VkPipelineColorBlendStateCreateInfo *cb_info)
{
   memset(cb, 0, sizeof(*cb));

   cb->logic_op_enable = cb_info->logicOpEnable;
   cb->logic_op = cb_info->logicOp;

   assert(cb_info->attachmentCount <= MESA_VK_MAX_COLOR_ATTACHMENTS);
   cb->attachment_count = cb_info->attachmentCount;
   for (uint32_t a = 0; a < cb_info->attachmentCount; a++) {
      const VkPipelineColorBlendAttachmentState *att =
         &cb_info->pAttachments[a];

      cb->attachments[a] = (struct vk_color_blend_attachment_state) {
         .blend_enable = att->blendEnable,
         .src_color_blend_factor = att->srcColorBlendFactor,
         .dst_color_blend_factor = att->dstColorBlendFactor,
         .src_alpha_blend_factor = att->srcAlphaBlendFactor,
         .dst_alpha_blend_factor = att->dstAlphaBlendFactor,
         .write_mask = att->colorWriteMask,
         .color_blend_op = att->colorBlendOp,
         .alpha_blend_op = att->alphaBlendOp,
      };
   }

   for (uint32_t i = 0; i < 4; i++)
      cb->blend_constants[i] = cb_info->blendConstants[i];

   const VkPipelineColorWriteCreateInfoEXT *cw_info =
      vk_find_struct_const(cb_info->pNext, PIPELINE_COLOR_WRITE_CREATE_INFO_EXT);
   if (cw_info != NULL) {
      assert(cb_info->attachmentCount == cw_info->attachmentCount);
      for (uint32_t a = 0; a < cw_info->attachmentCount; a++) {
         if (cw_info->pColorWriteEnables[a])
            cb->color_write_enables |= BITFIELD_BIT(a);
      }
   } else {
      cb->color_write_enables = BITFIELD_MASK(cb_info->attachmentCount);
   }
}

static bool
vk_render_pass_state_is_complete(const struct vk_render_pass_state *rp)
{
   return rp->attachment_aspects != VK_IMAGE_ASPECT_METADATA_BIT;
}

static void
vk_render_pass_state_init(struct vk_render_pass_state *rp,
                          const struct vk_render_pass_state *old_rp,
                          const VkGraphicsPipelineCreateInfo *info,
                          const struct vk_subpass_info *sp_info,
                          VkGraphicsPipelineLibraryFlagsEXT lib)
{
   /* If we already have render pass state and it has attachment info, then
    * it's complete and we don't need a new one.
    */
   if (old_rp != NULL && vk_render_pass_state_is_complete(old_rp)) {
      *rp = *old_rp;
      return;
   }

   *rp = (struct vk_render_pass_state) {
      .depth_attachment_format = VK_FORMAT_UNDEFINED,
      .stencil_attachment_format = VK_FORMAT_UNDEFINED,
   };

   if (info->renderPass != VK_NULL_HANDLE && sp_info != NULL) {
      rp->render_pass = info->renderPass;
      rp->subpass = info->subpass;
      rp->attachment_aspects = sp_info->attachment_aspects;
      rp->view_mask = sp_info->view_mask;
      return;
   }

   const VkPipelineRenderingCreateInfo *r_info =
      vk_get_pipeline_rendering_create_info(info);

   if (r_info == NULL)
      return;

   rp->view_mask = r_info->viewMask;

   /* From the Vulkan 1.3.218 spec description of pre-rasterization state:
    *
    *    "Fragment shader state is defined by:
    *    ...
    *     * VkRenderPass and subpass parameter
    *     * The viewMask parameter of VkPipelineRenderingCreateInfo (formats
    *       are ignored)"
    *
    * The description of fragment shader state contains identical text.
    *
    * If we have a render pass then we have full information.  Even if we're
    * dynamic-rendering-only, the presence of a render pass means the
    * rendering info came from a vk_render_pass and is therefore complete.
    * Otherwise, all we can grab is the view mask and we have to leave the
    * rest for later.
    */
   if (info->renderPass == VK_NULL_HANDLE &&
       !(lib & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) {
      rp->attachment_aspects = VK_IMAGE_ASPECT_METADATA_BIT;
      return;
   }

   assert(r_info->colorAttachmentCount <= MESA_VK_MAX_COLOR_ATTACHMENTS);
   rp->color_attachment_count = r_info->colorAttachmentCount;
   for (uint32_t i = 0; i < r_info->colorAttachmentCount; i++) {
      rp->color_attachment_formats[i] = r_info->pColorAttachmentFormats[i];
      if (r_info->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED)
         rp->attachment_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
   }

   rp->depth_attachment_format = r_info->depthAttachmentFormat;
   if (r_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED)
      rp->attachment_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;

   rp->stencil_attachment_format = r_info->stencilAttachmentFormat;
   if (r_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
      rp->attachment_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;

   const VkRenderingSelfDependencyInfoMESA *rsd_info =
      vk_find_struct_const(r_info->pNext, RENDERING_SELF_DEPENDENCY_INFO_MESA);
   if (rsd_info != NULL) {
      STATIC_ASSERT(sizeof(rp->color_self_dependencies) * 8 >=
                    MESA_VK_MAX_COLOR_ATTACHMENTS);
      rp->color_self_dependencies = rsd_info->colorSelfDependencies;
      rp->depth_self_dependency = rsd_info->depthSelfDependency;
      rp->stencil_self_dependency = rsd_info->stencilSelfDependency;
   }
}

#define FOREACH_STATE_GROUP(f)                           \
   f(MESA_VK_GRAPHICS_STATE_VERTEX_INPUT_BIT,            \
     vk_vertex_input_state, vi);                         \
   f(MESA_VK_GRAPHICS_STATE_INPUT_ASSEMBLY_BIT,          \
     vk_input_assembly_state, ia);                       \
   f(MESA_VK_GRAPHICS_STATE_TESSELLATION_BIT,            \
     vk_tessellation_state, ts);                         \
   f(MESA_VK_GRAPHICS_STATE_VIEWPORT_BIT,                \
     vk_viewport_state, vp);                             \
   f(MESA_VK_GRAPHICS_STATE_DISCARD_RECTANGLES_BIT,      \
     vk_discard_rectangles_state, dr);                   \
   f(MESA_VK_GRAPHICS_STATE_RASTERIZATION_BIT,           \
     vk_rasterization_state, rs);                        \
   f(MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT,   \
     vk_fragment_shading_rate_state, fsr);               \
   f(MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT,             \
     vk_multisample_state, ms);                          \
   f(MESA_VK_GRAPHICS_STATE_DEPTH_STENCIL_BIT,           \
     vk_depth_stencil_state, ds);                        \
   f(MESA_VK_GRAPHICS_STATE_COLOR_BLEND_BIT,             \
     vk_color_blend_state, cb);                          \
   f(MESA_VK_GRAPHICS_STATE_RENDER_PASS_BIT,             \
     vk_render_pass_state, rp);

static void
vk_graphics_pipeline_state_validate(const struct vk_graphics_pipeline_state *state)
{
#ifndef NDEBUG
   /* For now, we just validate dynamic state */
   enum mesa_vk_graphics_state_groups has = 0;

#define FILL_HAS(STATE, type, s) \
   if (state->s != NULL) has |= STATE

   FOREACH_STATE_GROUP(FILL_HAS)

#undef FILL_HAS

   validate_dynamic_state_groups(state->dynamic, has);
#endif
}

static bool
may_have_rasterization(const struct vk_graphics_pipeline_state *state,
                       const BITSET_WORD *dynamic,
                       const VkGraphicsPipelineCreateInfo *info)
{
   if (state->rs) {
      /* We default rasterizer_discard_enable to false when dynamic */
      return !state->rs->rasterizer_discard_enable;
   } else {
      return IS_DYNAMIC(RS_RASTERIZER_DISCARD_ENABLE) ||
             !info->pRasterizationState->rasterizerDiscardEnable;
   }
}

VkResult
vk_graphics_pipeline_state_fill(const struct vk_device *device,
                                struct vk_graphics_pipeline_state *state,
                                const VkGraphicsPipelineCreateInfo *info,
                                const struct vk_subpass_info *sp_info,
                                struct vk_graphics_pipeline_all_state *all,
                                const VkAllocationCallbacks *alloc,
                                VkSystemAllocationScope scope,
                                void **alloc_ptr_out)
{
   vk_graphics_pipeline_state_validate(state);

   BITSET_DECLARE(dynamic, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   vk_get_dynamic_graphics_states(dynamic, info->pDynamicState);

   VkShaderStageFlags stages = 0;
   for (uint32_t i = 0; i < info->stageCount; i++)
      stages |= info->pStages[i].stage;

   /* In case we return early */
   if (alloc_ptr_out != NULL)
      *alloc_ptr_out = NULL;

   /*
    * First, figure out which library-level shader/state groups we need
    */

   VkGraphicsPipelineLibraryFlagsEXT lib;
   if (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
      const VkGraphicsPipelineLibraryCreateInfoEXT *gfx_lib_info =
         vk_find_struct_const(info->pNext, GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);

      /* If we're building a pipeline library, trust the client.
       *
       * From the Vulkan 1.3.218 spec:
       *
       *    VUID-VkGraphicsPipelineLibraryCreateInfoEXT-flags-requiredbitmask
       *
       *    "flags must not be 0"
       */
      assert(gfx_lib_info->flags != 0);
      lib = gfx_lib_info->flags;
   } else {
      /* We're building a complete pipeline.  From the Vulkan 1.3.218 spec:
       *
       *    "A complete graphics pipeline always includes pre-rasterization
       *    shader state, with other subsets included depending on that state.
       *    If the pre-rasterization shader state includes a vertex shader,
       *    then vertex input state is included in a complete graphics
       *    pipeline. If the value of
       *    VkPipelineRasterizationStateCreateInfo::rasterizerDiscardEnable in
       *    the pre-rasterization shader state is VK_FALSE or the
       *    VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE dynamic state is
       *    enabled fragment shader state and fragment output interface state
       *    is included in a complete graphics pipeline."
       */
      lib = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;

      if (stages & VK_SHADER_STAGE_VERTEX_BIT)
         lib |= VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;

      if (may_have_rasterization(state, dynamic, info)) {
         lib |= VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
         lib |= VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
      }
   }

   /*
    * Next, turn those into individual states.  Among other things, this
    * de-duplicates things like FSR and multisample state which appear in
    * multiple library groups.
    */

   enum mesa_vk_graphics_state_groups needs = 0;
   if (lib & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) {
      needs |= MESA_VK_GRAPHICS_STATE_VERTEX_INPUT_BIT;
      needs |= MESA_VK_GRAPHICS_STATE_INPUT_ASSEMBLY_BIT;
   }

   /* Other stuff potentially depends on this so gather it early */
   struct vk_render_pass_state rp;
   if (lib & (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
              VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
              VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) {
      vk_render_pass_state_init(&rp, state->rp, info, sp_info, lib);

      needs |= MESA_VK_GRAPHICS_STATE_RENDER_PASS_BIT;

      /* If the old state was incomplete but the new one isn't, set state->rp
       * to NULL so it gets replaced with the new version.
       */
      if (state->rp != NULL &&
          !vk_render_pass_state_is_complete(state->rp) &&
          vk_render_pass_state_is_complete(&rp))
         state->rp = NULL;
   }

   if (lib & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) {
      /* From the Vulkan 1.3.218 spec:
       *
       *    VUID-VkGraphicsPipelineCreateInfo-stage-02096
       *
       *    "If the pipeline is being created with pre-rasterization shader
       *    state the stage member of one element of pStages must be either
       *    VK_SHADER_STAGE_VERTEX_BIT or VK_SHADER_STAGE_MESH_BIT_NV"
       */
      assert(stages & (VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_MESH_BIT_NV));

      if (stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))
         needs |= MESA_VK_GRAPHICS_STATE_TESSELLATION_BIT;

      if (may_have_rasterization(state, dynamic, info))
         needs |= MESA_VK_GRAPHICS_STATE_VIEWPORT_BIT;

      needs |= MESA_VK_GRAPHICS_STATE_DISCARD_RECTANGLES_BIT;
      needs |= MESA_VK_GRAPHICS_STATE_RASTERIZATION_BIT;
      needs |= MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT;
   }

   if (lib & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) {
      needs |= MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT;
      needs |= MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT;

      /* From the Vulkan 1.3.218 spec:
       *
       *    VUID-VkGraphicsPipelineCreateInfo-renderPass-06043
       *
       *    "If renderPass is not VK_NULL_HANDLE, the pipeline is being
       *    created with fragment shader state, and subpass uses a
       *    depth/stencil attachment, pDepthStencilState must be a valid
       *    pointer to a valid VkPipelineDepthStencilStateCreateInfo
       *    structure"
       *
       *    VUID-VkGraphicsPipelineCreateInfo-renderPass-06053
       *
       *    "If renderPass is VK_NULL_HANDLE, the pipeline is being created
       *    with fragment shader state and fragment output interface state,
       *    and either of VkPipelineRenderingCreateInfo::depthAttachmentFormat
       *    or VkPipelineRenderingCreateInfo::stencilAttachmentFormat are not
       *    VK_FORMAT_UNDEFINED, pDepthStencilState must be a valid pointer to
       *    a valid VkPipelineDepthStencilStateCreateInfo structure"
       *
       *    VUID-VkGraphicsPipelineCreateInfo-renderPass-06590
       *
       *    "If renderPass is VK_NULL_HANDLE and the pipeline is being created
       *    with fragment shader state but not fragment output interface
       *    state, pDepthStencilState must be a valid pointer to a valid
       *    VkPipelineDepthStencilStateCreateInfo structure"
       *
       * In the first case, we'll have a real set of aspects in rp.  In the
       * second case, where we have both fragment shader and fragment output
       * state, we will also have a valid set of aspects.  In the third case
       * where we only have fragment shader state and no render pass, the
       * vk_render_pass_state will be incomplete.
       */
      if ((rp.attachment_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                    VK_IMAGE_ASPECT_STENCIL_BIT)) ||
          !vk_render_pass_state_is_complete(&rp))
         needs |= MESA_VK_GRAPHICS_STATE_DEPTH_STENCIL_BIT;
   }

   if (lib & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) {
      if (rp.attachment_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                   VK_IMAGE_ASPECT_STENCIL_BIT))
         needs |= MESA_VK_GRAPHICS_STATE_DEPTH_STENCIL_BIT;

      if (rp.attachment_aspects & (VK_IMAGE_ASPECT_COLOR_BIT))
         needs |= MESA_VK_GRAPHICS_STATE_COLOR_BLEND_BIT;
   }

   /*
    * Next, Filter off any states we already have.
    */

#define FILTER_NEEDS(STATE, type, s) \
   if (state->s != NULL) needs &= ~STATE

   FOREACH_STATE_GROUP(FILTER_NEEDS)

#undef FILTER_NEEDS

   /* Filter dynamic state down to just what we're adding */
   BITSET_DECLARE(dynamic_filter, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   get_dynamic_state_groups(dynamic_filter, needs);
   BITSET_AND(dynamic, dynamic, dynamic_filter);

   /* And add it in */
   BITSET_OR(state->dynamic, state->dynamic, dynamic);

   /*
    * If vertex state or fragment shading rate state are fully dynamic, we
    * don't need to even allocate them.  Do this after we've filtered
    * dynamic state because we want to keep the MESA_VK_DYNAMIC_VI and
    * MESA_VK_DYNAMIC_FSR bits in the dynamic state but don't want the
    * actual state.
    */
   if (BITSET_TEST(dynamic, MESA_VK_DYNAMIC_VI))
      needs &= ~MESA_VK_GRAPHICS_STATE_VERTEX_INPUT_BIT;
   if (BITSET_TEST(dynamic, MESA_VK_DYNAMIC_FSR))
      needs &= ~MESA_VK_GRAPHICS_STATE_FRAGMENT_SHADING_RATE_BIT;

   /* If we don't need to set up any new states, bail early */
   if (needs == 0)
      return VK_SUCCESS;

   /*
    * Now, ensure that we have space for each of the states we're going to
    * fill.  If all != NULL, we'll pull from that.  Otherwise, we need to
    * allocate memory.
    */

   VK_MULTIALLOC(ma);

#define ENSURE_STATE_IF_NEEDED(STATE, type, s) \
   struct type *new_##s = NULL; \
   if (needs & STATE) { \
      if (all == NULL) { \
         vk_multialloc_add(&ma, &new_##s, struct type, 1); \
      } else { \
         new_##s = &all->s; \
      } \
   }

   FOREACH_STATE_GROUP(ENSURE_STATE_IF_NEEDED)

#undef ENSURE_STATE_IF_NEEDED

   /* Sample locations are a bit special.  We don't want to waste the memory
    * for 64 floats if we don't need to.  Also, we set up standard sample
    * locations if no user-provided sample locations are available.
    */
   const VkPipelineSampleLocationsStateCreateInfoEXT *sl_info = NULL;
   struct vk_sample_locations_state *new_sl = NULL;
   if (needs & MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT) {
      sl_info = vk_find_struct_const(info->pMultisampleState->pNext,
                                     PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT);
      if (needs_sample_locations_state(dynamic, sl_info)) {
         if (all == NULL) {
            vk_multialloc_add(&ma, &new_sl, struct vk_sample_locations_state, 1);
         } else {
            new_sl = &all->ms_sample_locations;
         }
      }
   }

   /*
    * Allocate memory, if needed
    */

   if (ma.size > 0) {
      assert(all == NULL);
      *alloc_ptr_out = vk_multialloc_alloc2(&ma, &device->alloc, alloc, scope);
      if (*alloc_ptr_out == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /*
    * Create aliases for various input infos so we can use or FOREACH macro
    */

#define INFO_ALIAS(_State, s) \
   const VkPipeline##_State##StateCreateInfo *s##_info = info->p##_State##State

   INFO_ALIAS(VertexInput, vi);
   INFO_ALIAS(InputAssembly, ia);
   INFO_ALIAS(Tessellation, ts);
   INFO_ALIAS(Viewport, vp);
   INFO_ALIAS(Rasterization, rs);
   INFO_ALIAS(Multisample, ms);
   INFO_ALIAS(DepthStencil, ds);
   INFO_ALIAS(ColorBlend, cb);

#undef INFO_ALIAS

   const VkPipelineDiscardRectangleStateCreateInfoEXT *dr_info =
      vk_find_struct_const(info->pNext, PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT);

   const VkPipelineFragmentShadingRateStateCreateInfoKHR *fsr_info =
      vk_find_struct_const(info->pNext, PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR);

   /*
    * Finally, fill out all the states
    */

#define INIT_STATE_IF_NEEDED(STATE, type, s) \
   if (needs & STATE) { \
      type##_init(new_##s, dynamic, s##_info); \
      state->s = new_##s; \
   }

   /* render pass state is special and we just copy it */
#define vk_render_pass_state_init(s, d, i) *s = rp

   FOREACH_STATE_GROUP(INIT_STATE_IF_NEEDED)

#undef vk_render_pass_state_init
#undef INIT_STATE_IF_NEEDED

   if (needs & MESA_VK_GRAPHICS_STATE_MULTISAMPLE_BIT) {
       vk_multisample_sample_locations_state_init(new_ms, new_sl, dynamic,
                                                  ms_info, sl_info);
   }

   return VK_SUCCESS;
}

#undef IS_DYNAMIC

void
vk_graphics_pipeline_state_merge(struct vk_graphics_pipeline_state *dst,
                                 const struct vk_graphics_pipeline_state *src)
{
   vk_graphics_pipeline_state_validate(dst);
   vk_graphics_pipeline_state_validate(src);

   BITSET_OR(dst->dynamic, dst->dynamic, src->dynamic);

   /* Render pass state needs special care because a render pass state may be
    * incomplete (view mask only).  See vk_render_pass_state_init().
    */
   if (dst->rp != NULL && src->rp != NULL &&
       !vk_render_pass_state_is_complete(dst->rp) &&
       vk_render_pass_state_is_complete(src->rp))
      dst->rp = src->rp;

#define MERGE(STATE, type, state) \
   if (dst->state == NULL && src->state != NULL) dst->state = src->state;

   FOREACH_STATE_GROUP(MERGE)

#undef MERGE
}

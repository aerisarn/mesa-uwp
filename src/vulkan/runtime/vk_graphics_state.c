#include "vk_graphics_state.h"

void
vk_get_dynamic_graphics_states(BITSET_WORD *dynamic,
                               const VkPipelineDynamicStateCreateInfo *info)
{
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

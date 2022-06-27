/*
 * Copyright © 2022 Collabora, Ltd
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

#ifndef VK_GRAPHICS_STATE_H
#define VK_GRAPHICS_STATE_H

#include "vulkan/vulkan_core.h"

#include "util/bitset.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Enumeration of all Vulkan dynamic graphics states
 *
 * Enumerants are named with both the abreviation of the state group to which
 * the state belongs as well as the name of the state itself.  These are
 * intended to pretty closely match the VkDynamicState enum but may not match
 * perfectly all the time.
 */
enum mesa_vk_dynamic_graphics_state {
   MESA_VK_DYNAMIC_VI,
   MESA_VK_DYNAMIC_VI_BINDING_STRIDES,
   MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY,
   MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE,
   MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS,
   MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT,
   MESA_VK_DYNAMIC_VP_VIEWPORTS,
   MESA_VK_DYNAMIC_VP_SCISSOR_COUNT,
   MESA_VK_DYNAMIC_VP_SCISSORS,
   MESA_VK_DYNAMIC_DR_RECTANGLES,
   MESA_VK_DYNAMIC_RS_RASTERIZER_DISCARD_ENABLE,
   MESA_VK_DYNAMIC_RS_CULL_MODE,
   MESA_VK_DYNAMIC_RS_FRONT_FACE,
   MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE,
   MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS,
   MESA_VK_DYNAMIC_RS_LINE_WIDTH,
   MESA_VK_DYNAMIC_RS_LINE_STIPPLE,
   MESA_VK_DYNAMIC_FSR,
   MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS,
   MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE,
   MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE,
   MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP,
   MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE,
   MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_BOUNDS,
   MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE,
   MESA_VK_DYNAMIC_DS_STENCIL_OP,
   MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK,
   MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK,
   MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE,
   MESA_VK_DYNAMIC_CB_LOGIC_OP,
   MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES,
   MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS,

   /* Must be left at the end */
   MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX,
};

/** Populate a bitset with dynamic states
 *
 * This function maps a VkPipelineDynamicStateCreateInfo to a bitset indexed
 * by mesa_vk_dynamic_graphics_state enumerants.
 *
 * @param[out] dynamic  Bitset to populate
 * @param[in]  info     VkPipelineDynamicStateCreateInfo or NULL
 */
void
vk_get_dynamic_graphics_states(BITSET_WORD *dynamic,
                               const VkPipelineDynamicStateCreateInfo *info);

#ifdef __cplusplus
}
#endif

#endif  /* VK_GRAPHICS_STATE_H */

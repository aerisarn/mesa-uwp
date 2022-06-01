/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PVR_HARDCODE_SHADERS_H
#define PVR_HARDCODE_SHADERS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "rogue/rogue_build_data.h"

/**
 * \file pvr_hardcode.h
 *
 * \brief Contains hard coding functions.
 * This should eventually be deleted as the compiler becomes more capable.
 */

struct pvr_compute_pipeline_shader_state;
struct pvr_device;

struct pvr_explicit_constant_usage {
   /* Hardware register number assigned to the explicit constant with the lower
    * pre_assigned offset.
    */
   uint32_t start_offset;
};

struct pvr_hard_code_compute_build_info {
   struct rogue_ubo_data ubo_data;

   uint32_t local_invocation_regs[2];
   uint32_t work_group_regs[3];
   uint32_t barrier_reg;
   uint32_t usc_temps;

   struct pvr_explicit_constant_usage explicit_conts_usage;
};

/* Returns true if the shader for the currently running program requires hard
 * coded shaders.
 */
bool pvr_hard_code_shader_required(void);

VkResult pvr_hard_code_compute_pipeline(
   struct pvr_device *const device,
   struct pvr_compute_pipeline_shader_state *const shader_state_out,
   struct pvr_hard_code_compute_build_info *const build_info_out);

#endif /* PVR_HARDCODE_SHADERS_H */

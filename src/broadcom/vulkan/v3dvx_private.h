/*
 * Copyright © 2021 Raspberry Pi
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

/* This file generates the per-v3d-version function prototypes.  It must only
 * be included from v3dv_private.h.
 */

#ifndef V3DV_PRIVATE_H
#error This file is included by means other than v3dv_private.h
#endif

/* Used at v3dv_image */

void
v3dX(pack_texture_shader_state)(struct v3dv_device *device,
                                struct v3dv_image_view *iview);

void
v3dX(pack_texture_shader_state_from_buffer_view)(struct v3dv_device *device,
                                                 struct v3dv_buffer_view *buffer_view);

/* Used at v3dv_pipeline */
void
v3dX(pipeline_pack_state)(struct v3dv_pipeline *pipeline,
                          const VkPipelineColorBlendStateCreateInfo *cb_info,
                          const VkPipelineDepthStencilStateCreateInfo *ds_info,
                          const VkPipelineRasterizationStateCreateInfo *rs_info,
                          const VkPipelineMultisampleStateCreateInfo *ms_info);
void
v3dX(pipeline_pack_compile_state)(struct v3dv_pipeline *pipeline,
                                  const VkPipelineVertexInputStateCreateInfo *vi_info);

/* Used at v3dv_queue */
void
v3dX(job_emit_noop)(struct v3dv_job *job);

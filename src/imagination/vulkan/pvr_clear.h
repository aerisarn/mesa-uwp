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

#ifndef PVR_CLEAR_H
#define PVR_CLEAR_H

#include <vulkan/vulkan_core.h>

#define PVR_CLEAR_VERTEX_COUNT 4
#define PVR_CLEAR_VERTEX_COORDINATES 3

struct pvr_bo;
struct pvr_device;
struct pvr_pds_upload;
struct pvr_pds_vertex_shader_program;

void pvr_pds_clear_vertex_shader_program_init_base(
   struct pvr_pds_vertex_shader_program *program,
   const struct pvr_bo *usc_shader_bo);

VkResult pvr_pds_clear_vertex_shader_program_create_and_upload(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_device *device,
   const struct pvr_bo *vertices_bo,
   struct pvr_pds_upload *const upload_out);

#endif /* PVR_CLEAR_H */

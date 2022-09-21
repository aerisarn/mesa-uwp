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

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "pvr_csb.h"

#define PVR_CLEAR_VERTEX_COUNT 4
#define PVR_CLEAR_VERTEX_COORDINATES 3

/* We don't always need ROGUE_VDMCTRL_INDEX_LIST3 so maybe change the code to
 * not have it in here but use an alternative definition when needed if we want
 * to really squeeze out a some bytes of memory.
 */
#define PVR_CLEAR_VDM_STATE_DWORD_COUNT                                        \
   (pvr_cmd_length(VDMCTRL_VDM_STATE0) + pvr_cmd_length(VDMCTRL_VDM_STATE2) +  \
    pvr_cmd_length(VDMCTRL_VDM_STATE3) + pvr_cmd_length(VDMCTRL_VDM_STATE4) +  \
    pvr_cmd_length(VDMCTRL_VDM_STATE5) + pvr_cmd_length(VDMCTRL_INDEX_LIST0) + \
    pvr_cmd_length(VDMCTRL_INDEX_LIST2) + pvr_cmd_length(VDMCTRL_INDEX_LIST3))

struct pvr_bo;
struct pvr_cmd_buffer;
struct pvr_device;
struct pvr_device_info;
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
VkResult pvr_pds_clear_vertex_shader_program_create_and_upload_data(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_cmd_buffer *cmd_buffer,
   struct pvr_bo *vertices_bo,
   struct pvr_pds_upload *const pds_upload_out);

void pvr_pds_clear_rta_vertex_shader_program_init_base(
   struct pvr_pds_vertex_shader_program *program,
   const struct pvr_bo *usc_shader_bo);

/* Each code and data upload function clears the other's fields in the
 * pds_upload_out. So when uploading the code, the data fields will be 0.
 */
VkResult pvr_pds_clear_rta_vertex_shader_program_create_and_upload_code(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_cmd_buffer *cmd_buffer,
   uint32_t base_array_layer,
   struct pvr_pds_upload *const pds_upload_out);

static inline VkResult
pvr_pds_clear_rta_vertex_shader_program_create_and_upload_data(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_cmd_buffer *cmd_buffer,
   struct pvr_bo *vertices_bo,
   struct pvr_pds_upload *const pds_upload_out)
{
   return pvr_pds_clear_vertex_shader_program_create_and_upload_data(
      program,
      cmd_buffer,
      vertices_bo,
      pds_upload_out);
}

void pvr_pack_clear_vdm_state(
   const struct pvr_device_info *const dev_info,
   const struct pvr_pds_upload *const program,
   uint32_t temps,
   uint32_t index_count,
   uint32_t vs_output_size_in_bytes,
   uint32_t layer_count,
   uint32_t state_buffer[const static PVR_CLEAR_VDM_STATE_DWORD_COUNT]);

#endif /* PVR_CLEAR_H */

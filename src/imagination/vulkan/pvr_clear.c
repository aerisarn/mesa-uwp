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

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "pvr_clear.h"
#include "pvr_pds.h"
#include "pvr_private.h"
#include "vk_alloc.h"
#include "vk_log.h"

void pvr_pds_clear_vertex_shader_program_init_base(
   struct pvr_pds_vertex_shader_program *program,
   const struct pvr_bo *usc_shader_bo)
{
   *program = (struct pvr_pds_vertex_shader_program){
      .num_streams = 1,
      .streams = {
         [0] = {
            /* We'll get this from this interface's client when generating the
             * data segment. This will be the address of the vertex buffer.
             */
            .address = 0,
            .stride = PVR_CLEAR_VERTEX_COORDINATES * sizeof(uint32_t),
            .num_elements = 1,
            .elements = {
               [0] = {
                  .size = PVR_CLEAR_VERTEX_COUNT * PVR_CLEAR_VERTEX_COORDINATES,
               },
            },
         },
      },
   };

   pvr_pds_setup_doutu(&program->usc_task_control,
                       usc_shader_bo->vma->dev_addr.addr,
                       0,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);
}

VkResult pvr_pds_clear_vertex_shader_program_create_and_upload(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_device *device,
   const struct pvr_bo *vertices_bo,
   struct pvr_pds_upload *const upload_out)
{
   const struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   program->streams[0].address = vertices_bo->vma->dev_addr.addr;

   pvr_pds_vertex_shader(program, NULL, PDS_GENERATE_SIZES, dev_info);

   staging_buffer_size =
      (program->code_size + program->data_size) * sizeof(*staging_buffer);

   staging_buffer = vk_alloc(&device->vk.alloc,
                             staging_buffer_size,
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_exit;
   }

   pvr_pds_vertex_shader(program,
                         staging_buffer,
                         PDS_GENERATE_DATA_SEGMENT,
                         dev_info);
   pvr_pds_vertex_shader(program,
                         &staging_buffer[program->data_size],
                         PDS_GENERATE_CODE_SEGMENT,
                         dev_info);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[0],
                               program->data_size,
                               16,
                               &staging_buffer[program->data_size],
                               program->code_size,
                               16,
                               16,
                               upload_out);
   if (result != VK_SUCCESS)
      goto err_free_staging_buffer;

   vk_free(&device->vk.alloc, staging_buffer);
   return VK_SUCCESS;

err_free_staging_buffer:
   vk_free(&device->vk.alloc, staging_buffer);

err_exit:
   *upload_out = (struct pvr_pds_upload){ 0 };
   return result;
}

VkResult pvr_pds_clear_vertex_shader_program_create_and_upload_data(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_cmd_buffer *cmd_buffer,
   struct pvr_bo *vertices_bo,
   struct pvr_pds_upload *const pds_upload_out)
{
   struct pvr_device_info *dev_info = &cmd_buffer->device->pdevice->dev_info;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   program->streams[0].address = vertices_bo->vma->dev_addr.addr;

   pvr_pds_vertex_shader(program, NULL, PDS_GENERATE_SIZES, dev_info);

   staging_buffer_size = program->data_size * sizeof(*staging_buffer);

   staging_buffer = vk_alloc(&cmd_buffer->device->vk.alloc,
                             staging_buffer_size,
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      *pds_upload_out = (struct pvr_pds_upload){ 0 };

      result = vk_error(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      cmd_buffer->state.status = result;
      return result;
   }

   pvr_pds_vertex_shader(program,
                         staging_buffer,
                         PDS_GENERATE_DATA_SEGMENT,
                         dev_info);

   result = pvr_cmd_buffer_upload_pds(cmd_buffer,
                                      staging_buffer,
                                      program->data_size,
                                      4,
                                      NULL,
                                      0,
                                      0,
                                      4,
                                      pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->device->vk.alloc, staging_buffer);

      *pds_upload_out = (struct pvr_pds_upload){ 0 };

      cmd_buffer->state.status = result;
      return result;
   }

   vk_free(&cmd_buffer->device->vk.alloc, staging_buffer);

   return VK_SUCCESS;
}

void pvr_pds_clear_rta_vertex_shader_program_init_base(
   struct pvr_pds_vertex_shader_program *program,
   const struct pvr_bo *usc_shader_bo)
{
   pvr_pds_clear_vertex_shader_program_init_base(program, usc_shader_bo);

   /* We'll set the render target index to be the instance id + base array
    * layer. Since the base array layer can change in between clear rects, we
    * don't set it here and ask for it when generating the code and data
    * section.
    */
   /* This is 3 because the instance id register will follow the xyz coordinate
    * registers in the register file.
    * TODO: Maybe we want this to be hooked up to the compiler?
    */
   program->iterate_instance_id = true;
   program->instance_id_register = 3;
}

VkResult pvr_pds_clear_rta_vertex_shader_program_create_and_upload_code(
   struct pvr_pds_vertex_shader_program *program,
   struct pvr_cmd_buffer *cmd_buffer,
   uint32_t base_array_layer,
   struct pvr_pds_upload *const pds_upload_out)
{
   struct pvr_device_info *dev_info = &cmd_buffer->device->pdevice->dev_info;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   program->instance_id_modifier = base_array_layer;

   pvr_pds_vertex_shader(program, NULL, PDS_GENERATE_SIZES, dev_info);

   staging_buffer_size = program->code_size * sizeof(*staging_buffer);

   staging_buffer = vk_alloc(&cmd_buffer->device->vk.alloc,
                             staging_buffer_size,
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      *pds_upload_out = (struct pvr_pds_upload){ 0 };

      result = vk_error(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      cmd_buffer->state.status = result;
      return result;
   }

   pvr_pds_vertex_shader(program,
                         staging_buffer,
                         PDS_GENERATE_CODE_SEGMENT,
                         dev_info);

   result = pvr_cmd_buffer_upload_pds(cmd_buffer,
                                      NULL,
                                      0,
                                      0,
                                      staging_buffer,
                                      program->code_size,
                                      4,
                                      4,
                                      pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->device->vk.alloc, staging_buffer);

      *pds_upload_out = (struct pvr_pds_upload){ 0 };

      cmd_buffer->state.status = result;
      return result;
   }

   vk_free(&cmd_buffer->device->vk.alloc, staging_buffer);

   return VK_SUCCESS;
}

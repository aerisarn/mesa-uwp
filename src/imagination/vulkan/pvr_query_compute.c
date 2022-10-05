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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "hwdef/rogue_hw_utils.h"
#include "pvr_bo.h"
#include "pvr_formats.h"
#include "pvr_pds.h"
#include "pvr_private.h"
#include "pvr_shader_factory.h"
#include "pvr_static_shaders.h"
#include "pvr_tex_state.h"
#include "vk_alloc.h"
#include "vk_command_pool.h"

static inline void pvr_init_primary_compute_pds_program(
   struct pvr_pds_compute_shader_program *program)
{
   *program = (struct pvr_pds_compute_shader_program) {
      .local_input_regs = {
         0,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
      },

      /* Workgroup id is in reg0. */
      .work_group_input_regs = {
         0,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
      },

      .global_input_regs = {
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
         PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
      },

      .barrier_coefficient = PVR_PDS_COMPUTE_INPUT_REG_UNUSED,
      .flattened_work_groups = true,
      .kick_usc = true,
   };
}

static VkResult pvr_create_compute_secondary_prog(
   struct pvr_device *device,
   const struct pvr_shader_factory_info *shader_factory_info,
   struct pvr_compute_query_shader *query_prog)
{
   const size_t size =
      pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes();
   struct pvr_pds_descriptor_program_input sec_pds_program;
   struct pvr_pds_info *info = &query_prog->info;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   info->entries =
      vk_alloc(&device->vk.alloc, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!info->entries)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   info->entries_size_in_bytes = size;

   sec_pds_program = (struct pvr_pds_descriptor_program_input){
      .buffer_count = 1,
      .buffers = {
         [0] = {
            .buffer_id = 0,
            .source_offset = 0,
            .type = PVR_BUFFER_TYPE_COMPILE_TIME,
            .size_in_dwords = shader_factory_info->const_shared_regs,
            .destination = shader_factory_info->explicit_const_start_offset,
         }
      },
   };

   pvr_pds_generate_descriptor_upload_program(&sec_pds_program, NULL, info);

   staging_buffer_size = info->code_size_in_dwords;

   staging_buffer = vk_alloc(&device->vk.alloc,
                             staging_buffer_size * sizeof(*staging_buffer),
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      vk_free(&device->vk.alloc, info->entries);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pvr_pds_generate_descriptor_upload_program(&sec_pds_program,
                                              staging_buffer,
                                              info);

   assert(info->code_size_in_dwords <= staging_buffer_size);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               staging_buffer,
                               info->code_size_in_dwords,
                               16,
                               16,
                               &query_prog->pds_sec_code);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, staging_buffer);
      vk_free(&device->vk.alloc, info->entries);
      return result;
   }

   vk_free(&device->vk.alloc, staging_buffer);

   return VK_SUCCESS;
}

static void
pvr_destroy_compute_secondary_prog(struct pvr_device *device,
                                   struct pvr_compute_query_shader *program)
{
   pvr_bo_free(device, program->pds_sec_code.pvr_bo);
   vk_free(&device->vk.alloc, program->info.entries);
}

static VkResult pvr_create_compute_query_program(
   struct pvr_device *device,
   const struct pvr_shader_factory_info *shader_factory_info,
   struct pvr_compute_query_shader *query_prog)
{
   const uint32_t cache_line_size =
      rogue_get_slc_cache_line_size(&device->pdevice->dev_info);
   struct pvr_pds_compute_shader_program pds_primary_prog;
   VkResult result;

   /* No support for query constant calc program. */
   assert(shader_factory_info->const_calc_prog_inst_bytes == 0);
   /* No support for query coefficient update program. */
   assert(shader_factory_info->coeff_update_prog_start == PVR_INVALID_INST);

   result = pvr_gpu_upload_usc(device,
                               shader_factory_info->shader_code,
                               shader_factory_info->code_size,
                               cache_line_size,
                               &query_prog->usc_bo);
   if (result != VK_SUCCESS)
      return result;

   pvr_init_primary_compute_pds_program(&pds_primary_prog);

   pvr_pds_setup_doutu(&pds_primary_prog.usc_task_control,
                       query_prog->usc_bo->vma->dev_addr.addr,
                       shader_factory_info->temps_required,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);

   result =
      pvr_pds_compute_shader_create_and_upload(device,
                                               &pds_primary_prog,
                                               &query_prog->pds_prim_code);
   if (result != VK_SUCCESS)
      goto err_free_usc_bo;

   query_prog->primary_data_size_dw = pds_primary_prog.data_size;
   query_prog->primary_num_temps = pds_primary_prog.temps_used;

   result = pvr_create_compute_secondary_prog(device,
                                              shader_factory_info,
                                              query_prog);
   if (result != VK_SUCCESS)
      goto err_free_pds_prim_code_bo;

   return VK_SUCCESS;

err_free_pds_prim_code_bo:
   pvr_bo_free(device, query_prog->pds_prim_code.pvr_bo);

err_free_usc_bo:
   pvr_bo_free(device, query_prog->usc_bo);

   return result;
}

static void
pvr_destroy_compute_query_program(struct pvr_device *device,
                                  struct pvr_compute_query_shader *program)
{
   pvr_destroy_compute_secondary_prog(device, program);
   pvr_bo_free(device, program->pds_prim_code.pvr_bo);
   pvr_bo_free(device, program->usc_bo);
}

static VkResult pvr_create_multibuffer_compute_query_program(
   struct pvr_device *device,
   const struct pvr_shader_factory_info *const *shader_factory_info,
   struct pvr_compute_query_shader *query_programs)
{
   const uint32_t core_count = device->pdevice->dev_runtime_info.core_count;
   VkResult result;
   uint32_t i;

   for (i = 0; i < core_count; i++) {
      result = pvr_create_compute_query_program(device,
                                                shader_factory_info[i],
                                                &query_programs[i]);
      if (result != VK_SUCCESS)
         goto err_destroy_compute_query_program;
   }

   return VK_SUCCESS;

err_destroy_compute_query_program:
   for (uint32_t j = 0; j < i; j++)
      pvr_destroy_compute_query_program(device, &query_programs[j]);

   return result;
}

VkResult pvr_device_create_compute_query_programs(struct pvr_device *device)
{
   const uint32_t core_count = device->pdevice->dev_runtime_info.core_count;
   VkResult result;

   result = pvr_create_compute_query_program(device,
                                             &availability_query_write_info,
                                             &device->availability_shader);
   if (result != VK_SUCCESS)
      return result;

   device->copy_results_shaders =
      vk_alloc(&device->vk.alloc,
               sizeof(*device->copy_results_shaders) * core_count,
               8,
               VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device->copy_results_shaders) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_destroy_availability_query_program;
   }

   result = pvr_create_multibuffer_compute_query_program(
      device,
      copy_query_results_collection,
      device->copy_results_shaders);
   if (result != VK_SUCCESS)
      goto err_vk_free_copy_results_shaders;

   device->reset_queries_shaders =
      vk_alloc(&device->vk.alloc,
               sizeof(*device->reset_queries_shaders) * core_count,
               8,
               VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device->reset_queries_shaders) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto err_destroy_copy_results_query_programs;
   }

   result = pvr_create_multibuffer_compute_query_program(
      device,
      reset_query_collection,
      device->reset_queries_shaders);
   if (result != VK_SUCCESS)
      goto err_vk_free_reset_queries_shaders;

   return VK_SUCCESS;

err_vk_free_reset_queries_shaders:
   vk_free(&device->vk.alloc, device->reset_queries_shaders);

err_destroy_copy_results_query_programs:
   for (uint32_t i = 0; i < core_count; i++) {
      pvr_destroy_compute_query_program(device,
                                        &device->copy_results_shaders[i]);
   }

err_vk_free_copy_results_shaders:
   vk_free(&device->vk.alloc, device->copy_results_shaders);

err_destroy_availability_query_program:
   pvr_destroy_compute_query_program(device, &device->availability_shader);

   return result;
}

void pvr_device_destroy_compute_query_programs(struct pvr_device *device)
{
   const uint32_t core_count = device->pdevice->dev_runtime_info.core_count;

   pvr_destroy_compute_query_program(device, &device->availability_shader);

   for (uint32_t i = 0; i < core_count; i++) {
      pvr_destroy_compute_query_program(device,
                                        &device->copy_results_shaders[i]);
      pvr_destroy_compute_query_program(device,
                                        &device->reset_queries_shaders[i]);
   }

   vk_free(&device->vk.alloc, device->copy_results_shaders);
   vk_free(&device->vk.alloc, device->reset_queries_shaders);
}

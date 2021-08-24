/*
 * Copyright © 2021 Google
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

#include "radv_private.h"
#include "radv_shader.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"

static VkRayTracingPipelineCreateInfoKHR
radv_create_merged_rt_create_info(const VkRayTracingPipelineCreateInfoKHR *pCreateInfo)
{
   VkRayTracingPipelineCreateInfoKHR local_create_info = *pCreateInfo;
   uint32_t total_stages = pCreateInfo->stageCount;
   uint32_t total_groups = pCreateInfo->groupCount;

   if (pCreateInfo->pLibraryInfo) {
      for (unsigned i = 0; i < pCreateInfo->pLibraryInfo->libraryCount; ++i) {
         RADV_FROM_HANDLE(radv_pipeline, library, pCreateInfo->pLibraryInfo->pLibraries[i]);
         total_stages += library->library.stage_count;
         total_groups += library->library.group_count;
      }
   }
   VkPipelineShaderStageCreateInfo *stages = NULL;
   VkRayTracingShaderGroupCreateInfoKHR *groups = NULL;
   local_create_info.stageCount = total_stages;
   local_create_info.groupCount = total_groups;
   local_create_info.pStages = stages =
      malloc(sizeof(VkPipelineShaderStageCreateInfo) * total_stages);
   local_create_info.pGroups = groups =
      malloc(sizeof(VkRayTracingShaderGroupCreateInfoKHR) * total_groups);
   if (!local_create_info.pStages || !local_create_info.pGroups)
      return local_create_info;

   total_stages = pCreateInfo->stageCount;
   total_groups = pCreateInfo->groupCount;
   for (unsigned j = 0; j < pCreateInfo->stageCount; ++j)
      stages[j] = pCreateInfo->pStages[j];
   for (unsigned j = 0; j < pCreateInfo->groupCount; ++j)
      groups[j] = pCreateInfo->pGroups[j];

   if (pCreateInfo->pLibraryInfo) {
      for (unsigned i = 0; i < pCreateInfo->pLibraryInfo->libraryCount; ++i) {
         RADV_FROM_HANDLE(radv_pipeline, library, pCreateInfo->pLibraryInfo->pLibraries[i]);
         for (unsigned j = 0; j < library->library.stage_count; ++j)
            stages[total_stages + j] = library->library.stages[j];
         for (unsigned j = 0; j < library->library.group_count; ++j) {
            VkRayTracingShaderGroupCreateInfoKHR *dst = &groups[total_groups + j];
            *dst = library->library.groups[j];
            if (dst->generalShader != VK_SHADER_UNUSED_KHR)
               dst->generalShader += total_stages;
            if (dst->closestHitShader != VK_SHADER_UNUSED_KHR)
               dst->closestHitShader += total_stages;
            if (dst->anyHitShader != VK_SHADER_UNUSED_KHR)
               dst->anyHitShader += total_stages;
            if (dst->intersectionShader != VK_SHADER_UNUSED_KHR)
               dst->intersectionShader += total_stages;
         }
         total_stages += library->library.stage_count;
         total_groups += library->library.group_count;
      }
   }
   return local_create_info;
}

static VkResult
radv_rt_pipeline_library_create(VkDevice _device, VkPipelineCache _cache,
                                const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkPipeline *pPipeline)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   struct radv_pipeline *pipeline;

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);
   pipeline->type = RADV_PIPELINE_LIBRARY;

   VkRayTracingPipelineCreateInfoKHR local_create_info =
      radv_create_merged_rt_create_info(pCreateInfo);
   if (!local_create_info.pStages || !local_create_info.pGroups)
      goto fail;

   if (local_create_info.stageCount) {
      size_t size = sizeof(VkPipelineShaderStageCreateInfo) * local_create_info.stageCount;
      pipeline->library.stage_count = local_create_info.stageCount;
      pipeline->library.stages = malloc(size);
      if (!pipeline->library.stages)
         goto fail;
      memcpy(pipeline->library.stages, local_create_info.pStages, size);
   }

   if (local_create_info.groupCount) {
      size_t size = sizeof(VkRayTracingShaderGroupCreateInfoKHR) * local_create_info.groupCount;
      pipeline->library.group_count = local_create_info.groupCount;
      pipeline->library.groups = malloc(size);
      if (!pipeline->library.groups)
         goto fail;
      memcpy(pipeline->library.groups, local_create_info.pGroups, size);
   }

   *pPipeline = radv_pipeline_to_handle(pipeline);

   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   return VK_SUCCESS;
fail:
   free(pipeline->library.groups);
   free(pipeline->library.stages);
   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static nir_shader *
create_rt_shader(struct radv_device *device, const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                 struct radv_pipeline_shader_stack_size *stack_sizes)
{
   /* TODO */
   return NULL;
}

static VkResult
radv_rt_pipeline_create(VkDevice _device, VkPipelineCache _cache,
                        const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkPipeline *pPipeline)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   VkResult result;
   struct radv_pipeline *pipeline = NULL;
   struct radv_pipeline_shader_stack_size *stack_sizes = NULL;

   if (pCreateInfo->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
      return radv_rt_pipeline_library_create(_device, _cache, pCreateInfo, pAllocator, pPipeline);

   VkRayTracingPipelineCreateInfoKHR local_create_info =
      radv_create_merged_rt_create_info(pCreateInfo);
   if (!local_create_info.pStages || !local_create_info.pGroups) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   stack_sizes = calloc(sizeof(*stack_sizes), local_create_info.groupCount);
   if (!stack_sizes) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   nir_shader *shader = create_rt_shader(device, &local_create_info, stack_sizes);
   VkComputePipelineCreateInfo compute_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = NULL,
      .flags = pCreateInfo->flags,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = vk_shader_module_handle_from_nir(shader),
            .pName = "main",
         },
      .layout = pCreateInfo->layout,
   };
   result = radv_compute_pipeline_create(_device, _cache, &compute_info, pAllocator, pPipeline);
   if (result != VK_SUCCESS)
      goto shader_fail;

   pipeline = radv_pipeline_from_handle(*pPipeline);

   pipeline->compute.rt_group_handles =
      calloc(sizeof(*pipeline->compute.rt_group_handles), local_create_info.groupCount);
   if (!pipeline->compute.rt_group_handles) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto shader_fail;
   }

   pipeline->compute.rt_stack_sizes = stack_sizes;
   stack_sizes = NULL;

   for (unsigned i = 0; i < local_create_info.groupCount; ++i) {
      const VkRayTracingShaderGroupCreateInfoKHR *group_info = &local_create_info.pGroups[i];
      switch (group_info->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
         if (group_info->generalShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[0] = i + 2;
         break;
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
         if (group_info->intersectionShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[1] = i + 2;
         FALLTHROUGH;
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
         if (group_info->closestHitShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[0] = i + 2;
         if (group_info->anyHitShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[1] = i + 2;
         break;
      case VK_SHADER_GROUP_SHADER_MAX_ENUM_KHR:
         unreachable("VK_SHADER_GROUP_SHADER_MAX_ENUM_KHR");
      }
   }

shader_fail:
   if (result != VK_SUCCESS && pipeline)
      radv_pipeline_destroy(device, pipeline, pAllocator);
   ralloc_free(shader);
fail:
   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   free(stack_sizes);
   return result;
}

VkResult
radv_CreateRayTracingPipelinesKHR(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  VkPipelineCache pipelineCache, uint32_t count,
                                  const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
                                  const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      VkResult r;
      r = radv_rt_pipeline_create(_device, pipelineCache, &pCreateInfos[i], pAllocator,
                                  &pPipelines[i]);
      if (r != VK_SUCCESS) {
         result = r;
         pPipelines[i] = VK_NULL_HANDLE;

         if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT)
            break;
      }
   }

   for (; i < count; ++i)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}
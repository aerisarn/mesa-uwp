/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_pipeline.h"

#include "venus-protocol/vn_protocol_driver_pipeline.h"
#include "venus-protocol/vn_protocol_driver_pipeline_cache.h"
#include "venus-protocol/vn_protocol_driver_pipeline_layout.h"
#include "venus-protocol/vn_protocol_driver_shader_module.h"

#include "vn_device.h"
#include "vn_physical_device.h"

/* shader module commands */

VkResult
vn_CreateShaderModule(VkDevice device,
                      const VkShaderModuleCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkShaderModule *pShaderModule)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_shader_module *mod =
      vk_zalloc(alloc, sizeof(*mod), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mod)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&mod->base, VK_OBJECT_TYPE_SHADER_MODULE, &dev->base);

   VkShaderModule mod_handle = vn_shader_module_to_handle(mod);
   vn_async_vkCreateShaderModule(dev->instance, device, pCreateInfo, NULL,
                                 &mod_handle);

   *pShaderModule = mod_handle;

   return VK_SUCCESS;
}

void
vn_DestroyShaderModule(VkDevice device,
                       VkShaderModule shaderModule,
                       const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_shader_module *mod = vn_shader_module_from_handle(shaderModule);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!mod)
      return;

   vn_async_vkDestroyShaderModule(dev->instance, device, shaderModule, NULL);

   vn_object_base_fini(&mod->base);
   vk_free(alloc, mod);
}

/* pipeline layout commands */

VkResult
vn_CreatePipelineLayout(VkDevice device,
                        const VkPipelineLayoutCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkPipelineLayout *pPipelineLayout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_pipeline_layout *layout =
      vk_zalloc(alloc, sizeof(*layout), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!layout)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&layout->base, VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                       &dev->base);

   VkPipelineLayout layout_handle = vn_pipeline_layout_to_handle(layout);
   vn_async_vkCreatePipelineLayout(dev->instance, device, pCreateInfo, NULL,
                                   &layout_handle);

   *pPipelineLayout = layout_handle;

   return VK_SUCCESS;
}

void
vn_DestroyPipelineLayout(VkDevice device,
                         VkPipelineLayout pipelineLayout,
                         const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline_layout *layout =
      vn_pipeline_layout_from_handle(pipelineLayout);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!layout)
      return;

   vn_async_vkDestroyPipelineLayout(dev->instance, device, pipelineLayout,
                                    NULL);

   vn_object_base_fini(&layout->base);
   vk_free(alloc, layout);
}

/* pipeline cache commands */

VkResult
vn_CreatePipelineCache(VkDevice device,
                       const VkPipelineCacheCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkPipelineCache *pPipelineCache)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_pipeline_cache *cache =
      vk_zalloc(alloc, sizeof(*cache), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cache)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&cache->base, VK_OBJECT_TYPE_PIPELINE_CACHE,
                       &dev->base);

   VkPipelineCacheCreateInfo local_create_info;
   if (pCreateInfo->initialDataSize) {
      const struct vk_pipeline_cache_header *header =
         pCreateInfo->pInitialData;

      local_create_info = *pCreateInfo;
      local_create_info.initialDataSize -= header->header_size;
      local_create_info.pInitialData += header->header_size;
      pCreateInfo = &local_create_info;
   }

   VkPipelineCache cache_handle = vn_pipeline_cache_to_handle(cache);
   vn_async_vkCreatePipelineCache(dev->instance, device, pCreateInfo, NULL,
                                  &cache_handle);

   *pPipelineCache = cache_handle;

   return VK_SUCCESS;
}

void
vn_DestroyPipelineCache(VkDevice device,
                        VkPipelineCache pipelineCache,
                        const VkAllocationCallbacks *pAllocator)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline_cache *cache =
      vn_pipeline_cache_from_handle(pipelineCache);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!cache)
      return;

   vn_async_vkDestroyPipelineCache(dev->instance, device, pipelineCache,
                                   NULL);

   vn_object_base_fini(&cache->base);
   vk_free(alloc, cache);
}

VkResult
vn_GetPipelineCacheData(VkDevice device,
                        VkPipelineCache pipelineCache,
                        size_t *pDataSize,
                        void *pData)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_physical_device *physical_dev = dev->physical_device;

   struct vk_pipeline_cache_header *header = pData;
   VkResult result;
   if (!pData) {
      result = vn_call_vkGetPipelineCacheData(dev->instance, device,
                                              pipelineCache, pDataSize, NULL);
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);

      *pDataSize += sizeof(*header);
      return VK_SUCCESS;
   }

   if (*pDataSize <= sizeof(*header)) {
      *pDataSize = 0;
      return VK_INCOMPLETE;
   }

   const VkPhysicalDeviceProperties *props =
      &physical_dev->properties.vulkan_1_0;
   header->header_size = sizeof(*header);
   header->header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
   header->vendor_id = props->vendorID;
   header->device_id = props->deviceID;
   memcpy(header->uuid, props->pipelineCacheUUID, VK_UUID_SIZE);

   *pDataSize -= header->header_size;
   result =
      vn_call_vkGetPipelineCacheData(dev->instance, device, pipelineCache,
                                     pDataSize, pData + header->header_size);
   if (result < VK_SUCCESS)
      return vn_error(dev->instance, result);

   *pDataSize += header->header_size;

   return result;
}

VkResult
vn_MergePipelineCaches(VkDevice device,
                       VkPipelineCache dstCache,
                       uint32_t srcCacheCount,
                       const VkPipelineCache *pSrcCaches)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);

   vn_async_vkMergePipelineCaches(dev->instance, device, dstCache,
                                  srcCacheCount, pSrcCaches);

   return VK_SUCCESS;
}

/* pipeline commands */

/** Fixes for a single VkGraphicsPipelineCreateInfo. */
struct vn_graphics_pipeline_create_info_fix {
   bool ignore_tessellation_state;
   bool ignore_viewport_state;
   bool ignore_multisample_state;

   /* Ignore the following:
    *    pDepthStencilState
    *    pColorBlendState
    */
   bool ignore_raster_dedicated_states;
};

/** Temporary storage for fixes in vkCreateGraphicsPipelines. */
struct vn_create_graphics_pipelines_fixes {
   VkGraphicsPipelineCreateInfo *create_infos;
};

static struct vn_create_graphics_pipelines_fixes *
vn_alloc_create_graphics_pipelines_fixes(const VkAllocationCallbacks *alloc,
                                         uint32_t info_count)
{
   struct vn_create_graphics_pipelines_fixes *fixes;
   VkGraphicsPipelineCreateInfo *create_infos;

   VK_MULTIALLOC(ma);
   vk_multialloc_add(&ma, &fixes, __typeof__(*fixes), 1);
   vk_multialloc_add(&ma, &create_infos, __typeof__(*create_infos),
                     info_count);

   if (!vk_multialloc_zalloc(&ma, alloc, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND))
      return NULL;

   fixes->create_infos = create_infos;

   return fixes;
}

static const VkGraphicsPipelineCreateInfo *
vn_fix_graphics_pipeline_create_info(
   struct vn_device *dev,
   uint32_t info_count,
   const VkGraphicsPipelineCreateInfo *create_infos,
   const VkAllocationCallbacks *alloc,
   struct vn_create_graphics_pipelines_fixes **out_fixes)
{
   VN_TRACE_FUNC();

   /* Defer allocation until we need a fix. */
   struct vn_create_graphics_pipelines_fixes *fixes = NULL;

   for (uint32_t i = 0; i < info_count; i++) {
      const VkGraphicsPipelineCreateInfo *info = &create_infos[i];
      struct vn_graphics_pipeline_create_info_fix fix = { 0 };
      bool any_fix = false;

      VkShaderStageFlags stages = 0;
      for (uint32_t j = 0; j < info->stageCount; j++) {
         stages |= info->pStages[j].stage;
      }

      /* VkDynamicState */
      struct {
         bool rasterizer_discard_enable;
      } has_dynamic_state = { 0 };

      if (info->pDynamicState) {
         for (uint32_t j = 0; j < info->pDynamicState->dynamicStateCount; j++) {
            switch (info->pDynamicState->pDynamicStates[j]) {
            case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
               has_dynamic_state.rasterizer_discard_enable = true;
               break;
            default:
               break;
            }
         }
      }

      /* For each pipeline state category, we define a bool.
       *
       * The Vulkan spec (1.3.223) says:
       *    The state required for a graphics pipeline is divided into vertex
       *    input state, pre-rasterization shader state, fragment shader
       *    state, and fragment output state.
       *
       * Without VK_EXT_graphics_pipeline_library, most states are
       * unconditionally included in the pipeline. Despite that, we still
       * reference the state bools in the ignore rules because (a) it makes the
       * ignore condition easier to validate against the text of the relevant
       * VUs; and (b) it makes it easier to enable
       * VK_EXT_graphics_pipeline_library because we won't need to carefully
       * revisit the text of each VU to untangle the missing pipeline state
       * bools.
       */

      /* VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
       *
       * The Vulkan spec (1.3.223) says:
       *    If the pre-rasterization shader state includes a vertex shader,
       * then vertex input state is included in a complete graphics pipeline.
       *
       * We support no extension yet that allows the vertex stage to be
       * omitted, such as VK_EXT_vertex_input_dynamic_state or
       * VK_EXT_graphics_pipeline_library.
       */
      const bool UNUSED has_vertex_input_state = true;

      /* VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT */
      const bool has_pre_raster_state = true;

      /* The spec does not assign a name to this state. We define it just to
       * deduplicate code.
       *
       * The Vulkan spec (1.3.223) says:
       *    If the value of [...]rasterizerDiscardEnable in the
       *    pre-rasterization shader state is VK_FALSE or the
       *    VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE dynamic state is enabled
       *    fragment shader state and fragment output interface state is
       *    included in a complete graphics pipeline.
       */
      const bool has_raster_state =
         has_dynamic_state.rasterizer_discard_enable ||
         (info->pRasterizationState &&
          info->pRasterizationState->rasterizerDiscardEnable == VK_FALSE);

      /* VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT */
      const bool UNUSED has_fragment_shader_state = has_raster_state;

      /* VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT */
      const bool has_fragment_output_state = has_raster_state;

      /* Ignore pTessellationState?
       *    VUID-VkGraphicsPipelineCreateInfo-pStages-00731
       */
      if (info->pTessellationState &&
          (!(stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) ||
           !(stages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))) {
         fix.ignore_tessellation_state = true;
         any_fix = true;
      }

      /* Ignore pViewportState?
       *    VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00750
       *    VUID-VkGraphicsPipelineCreateInfo-pViewportState-04892
       */
      if (info->pViewportState &&
          !(has_pre_raster_state && has_raster_state)) {
         fix.ignore_viewport_state = true;
         any_fix = true;
      }

      /* Ignore pMultisampleState?
       *    VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00751
       */
      if (info->pMultisampleState && !has_fragment_output_state) {
         fix.ignore_multisample_state = true;
         any_fix = true;
      }

      /* FIXME: Conditions for ignoring pDepthStencilState and
       * pColorBlendState miss some cases that depend on the render pass. Make
       * them agree with the VUIDs.
       */
      if (!has_raster_state &&
          (info->pDepthStencilState || info->pColorBlendState)) {
         fix.ignore_raster_dedicated_states = true;
         any_fix = true;
      }

      if (!any_fix)
         continue;

      if (!fixes) {
         fixes = vn_alloc_create_graphics_pipelines_fixes(alloc, info_count);

         if (!fixes)
            return NULL;

         memcpy(fixes->create_infos, create_infos,
                info_count * sizeof(create_infos[0]));
      }

      if (fix.ignore_tessellation_state)
         fixes->create_infos[i].pTessellationState = NULL;

      if (fix.ignore_viewport_state)
         fixes->create_infos[i].pViewportState = NULL;

      if (fix.ignore_multisample_state)
         fixes->create_infos[i].pMultisampleState = NULL;

      if (fix.ignore_raster_dedicated_states) {
         fixes->create_infos[i].pDepthStencilState = NULL;
         fixes->create_infos[i].pColorBlendState = NULL;
      }
   }

   if (!fixes)
      return create_infos;

   *out_fixes = fixes;
   return fixes->create_infos;
}

VkResult
vn_CreateGraphicsPipelines(VkDevice device,
                           VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkGraphicsPipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;
   struct vn_create_graphics_pipelines_fixes *fixes = NULL;

   pCreateInfos = vn_fix_graphics_pipeline_create_info(
      dev, createInfoCount, pCreateInfos, alloc, &fixes);
   if (!pCreateInfos)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline =
         vk_zalloc(alloc, sizeof(*pipeline), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pipeline) {
         for (uint32_t j = 0; j < i; j++)
            vk_free(alloc, vn_pipeline_from_handle(pPipelines[j]));

         vk_free(alloc, fixes);
         memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      vn_object_base_init(&pipeline->base, VK_OBJECT_TYPE_PIPELINE,
                          &dev->base);

      VkPipeline pipeline_handle = vn_pipeline_to_handle(pipeline);
      pPipelines[i] = pipeline_handle;
   }

   vn_async_vkCreateGraphicsPipelines(dev->instance, device, pipelineCache,
                                      createInfoCount, pCreateInfos, NULL,
                                      pPipelines);

   vk_free(alloc, fixes);

   return VK_SUCCESS;
}

VkResult
vn_CreateComputePipelines(VkDevice device,
                          VkPipelineCache pipelineCache,
                          uint32_t createInfoCount,
                          const VkComputePipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline =
         vk_zalloc(alloc, sizeof(*pipeline), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pipeline) {
         for (uint32_t j = 0; j < i; j++)
            vk_free(alloc, vn_pipeline_from_handle(pPipelines[j]));
         memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      vn_object_base_init(&pipeline->base, VK_OBJECT_TYPE_PIPELINE,
                          &dev->base);

      VkPipeline pipeline_handle = vn_pipeline_to_handle(pipeline);
      pPipelines[i] = pipeline_handle;
   }

   vn_async_vkCreateComputePipelines(dev->instance, device, pipelineCache,
                                     createInfoCount, pCreateInfos, NULL,
                                     pPipelines);

   return VK_SUCCESS;
}

void
vn_DestroyPipeline(VkDevice device,
                   VkPipeline _pipeline,
                   const VkAllocationCallbacks *pAllocator)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline *pipeline = vn_pipeline_from_handle(_pipeline);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!pipeline)
      return;

   vn_async_vkDestroyPipeline(dev->instance, device, _pipeline, NULL);

   vn_object_base_fini(&pipeline->base);
   vk_free(alloc, pipeline);
}

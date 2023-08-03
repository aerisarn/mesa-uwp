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

#include "vn_descriptor_set.h"
#include "vn_device.h"
#include "vn_physical_device.h"
#include "vn_render_pass.h"

/**
 * Fields in the VkGraphicsPipelineCreateInfo pNext chain that we must track
 * to determine which fields are valid and which must be erased.
 */
struct vn_graphics_pipeline_create_info_fields {
   union {
      /* Bitmask exists for testing if any field is set. */
      uint32_t mask;

      /* Group the fixes by Vulkan struct. Within each group, sort by struct
       * order.
       */
      struct {
         /** VkGraphicsPipelineCreateInfo::pVertexInputState */
         bool vertex_input_state : 1;
         /** VkGraphicsPipelineCreateInfo::pTessellationState */
         bool tessellation_state : 1;
         /** VkGraphicsPipelineCreateInfo::pViewportState */
         bool viewport_state : 1;
         /** VkGraphicsPipelineCreateInfo::pMultisampleState */
         bool multisample_state : 1;
         /** VkGraphicsPipelineCreateInfo::pDepthStencilState */
         bool depth_stencil_state : 1;
         /** VkGraphicsPipelineCreateInfo::pColorBlendState */
         bool color_blend_state : 1;
         /** VkGraphicsPipelineCreateInfo::basePipelineHandle */
         bool base_pipeline_handle : 1;

         /** VkPipelineViewportStateCreateInfo::pViewports */
         bool viewport_state_viewports : 1;
         /** VkPipelineViewportStateCreateInfo::pScissors */
         bool viewport_state_scissors : 1;
      };
   };
};

static_assert(
   sizeof(struct vn_graphics_pipeline_create_info_fields) ==
      sizeof(((struct vn_graphics_pipeline_create_info_fields){}).mask),
   "vn_graphics_pipeline_create_info_fields::mask is too small");

/**
 * Temporary storage for fixes in vkCreateGraphicsPipelines.
 *
 * Length of each array is vkCreateGraphicsPipelines::createInfoCount.
 */
struct vn_graphics_pipeline_fix_tmp {
   VkGraphicsPipelineCreateInfo *infos;
   VkPipelineViewportStateCreateInfo *viewport_state_infos;
};

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

static void
vn_pipeline_layout_destroy(struct vn_device *dev,
                           struct vn_pipeline_layout *pipeline_layout)
{
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;
   if (pipeline_layout->push_descriptor_set_layout) {
      vn_descriptor_set_layout_unref(
         dev, pipeline_layout->push_descriptor_set_layout);
   }
   vn_async_vkDestroyPipelineLayout(
      dev->instance, vn_device_to_handle(dev),
      vn_pipeline_layout_to_handle(pipeline_layout), NULL);

   vn_object_base_fini(&pipeline_layout->base);
   vk_free(alloc, pipeline_layout);
}

static inline struct vn_pipeline_layout *
vn_pipeline_layout_ref(struct vn_device *dev,
                       struct vn_pipeline_layout *pipeline_layout)
{
   vn_refcount_inc(&pipeline_layout->refcount);
   return pipeline_layout;
}

static inline void
vn_pipeline_layout_unref(struct vn_device *dev,
                         struct vn_pipeline_layout *pipeline_layout)
{
   if (vn_refcount_dec(&pipeline_layout->refcount))
      vn_pipeline_layout_destroy(dev, pipeline_layout);
}

VkResult
vn_CreatePipelineLayout(VkDevice device,
                        const VkPipelineLayoutCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkPipelineLayout *pPipelineLayout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   /* ignore pAllocator as the pipeline layout is reference-counted */
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_pipeline_layout *layout =
      vk_zalloc(alloc, sizeof(*layout), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!layout)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&layout->base, VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                       &dev->base);
   layout->refcount = VN_REFCOUNT_INIT(1);

   for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) {
      struct vn_descriptor_set_layout *descriptor_set_layout =
         vn_descriptor_set_layout_from_handle(pCreateInfo->pSetLayouts[i]);

      /* Avoid null derefs. pSetLayouts may contain VK_NULL_HANDLE.
       *
       * From the Vulkan 1.3.254 spec:
       *    VUID-VkPipelineLayoutCreateInfo-pSetLayouts-parameter
       *
       *    If setLayoutCount is not 0, pSetLayouts must be a valid pointer to
       *    an array of setLayoutCount valid or VK_NULL_HANDLE
       *    VkDescriptorSetLayout handles
       */
      if (descriptor_set_layout &&
          descriptor_set_layout->is_push_descriptor) {
         layout->push_descriptor_set_layout =
            vn_descriptor_set_layout_ref(dev, descriptor_set_layout);
         break;
      }
   }

   layout->has_push_constant_ranges = pCreateInfo->pPushConstantRanges > 0;

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

   if (!layout)
      return;

   vn_pipeline_layout_unref(dev, layout);
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

static bool
vn_create_pipeline_handles(struct vn_device *dev,
                           enum vn_pipeline_type type,
                           uint32_t pipeline_count,
                           VkPipeline *pipeline_handles,
                           const VkAllocationCallbacks *alloc)
{
   for (uint32_t i = 0; i < pipeline_count; i++) {
      struct vn_pipeline *pipeline =
         vk_zalloc(alloc, sizeof(*pipeline), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      if (!pipeline) {
         for (uint32_t j = 0; j < i; j++) {
            pipeline = vn_pipeline_from_handle(pipeline_handles[j]);
            vn_object_base_fini(&pipeline->base);
            vk_free(alloc, pipeline);
         }

         memset(pipeline_handles, 0,
                pipeline_count * sizeof(pipeline_handles[0]));
         return false;
      }

      vn_object_base_init(&pipeline->base, VK_OBJECT_TYPE_PIPELINE,
                          &dev->base);
      pipeline->type = type;
      pipeline_handles[i] = vn_pipeline_to_handle(pipeline);
   }

   return true;
}

/** For vkCreate*Pipelines.  */
static void
vn_destroy_failed_pipelines(struct vn_device *dev,
                            uint32_t create_info_count,
                            VkPipeline *pipelines,
                            const VkAllocationCallbacks *alloc)
{
   for (uint32_t i = 0; i < create_info_count; i++) {
      struct vn_pipeline *pipeline = vn_pipeline_from_handle(pipelines[i]);

      if (pipeline->base.id == 0) {
         if (pipeline->layout) {
            vn_pipeline_layout_unref(dev, pipeline->layout);
         }
         vn_object_base_fini(&pipeline->base);
         vk_free(alloc, pipeline);
         pipelines[i] = VK_NULL_HANDLE;
      }
   }
}

#define VN_PIPELINE_CREATE_SYNC_MASK                                         \
   (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |               \
    VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)

static struct vn_graphics_pipeline_fix_tmp *
vn_graphics_pipeline_fix_tmp_alloc(const VkAllocationCallbacks *alloc,
                                   uint32_t info_count)
{
   struct vn_graphics_pipeline_fix_tmp *tmp;
   VkGraphicsPipelineCreateInfo *infos;
   VkPipelineViewportStateCreateInfo *viewport_state_infos;

   VK_MULTIALLOC(ma);
   vk_multialloc_add(&ma, &tmp, __typeof__(*tmp), 1);
   vk_multialloc_add(&ma, &infos, __typeof__(*infos), info_count);
   vk_multialloc_add(&ma, &viewport_state_infos,
                     __typeof__(*viewport_state_infos), info_count);

   if (!vk_multialloc_zalloc(&ma, alloc, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND))
      return NULL;

   tmp->infos = infos;
   tmp->viewport_state_infos = viewport_state_infos;

   return tmp;
}

static const VkGraphicsPipelineCreateInfo *
vn_fix_graphics_pipeline_create_infos(
   struct vn_device *dev,
   uint32_t info_count,
   const VkGraphicsPipelineCreateInfo *infos,
   const VkAllocationCallbacks *alloc,
   struct vn_graphics_pipeline_fix_tmp **out_fix_tmp)
{
   VN_TRACE_FUNC();

   /* Defer allocation until we need a fix. */
   struct vn_graphics_pipeline_fix_tmp *fix_tmp = NULL;

   for (uint32_t i = 0; i < info_count; i++) {
      /* Erase these fields to prevent the Venus encoder from reading invalid
       * memory.
       */
      struct vn_graphics_pipeline_create_info_fields erase = { 0 };

      const VkGraphicsPipelineCreateInfo *info = &infos[i];
      const VkPipelineRenderingCreateInfo *rendering_info =
         vk_find_struct_const(info, PIPELINE_RENDERING_CREATE_INFO);

      VkShaderStageFlags stages = 0;
      for (uint32_t j = 0; j < info->stageCount; j++) {
         stages |= info->pStages[j].stage;
      }

      /* VkDynamicState */
      struct {
         bool rasterizer_discard_enable;
         bool viewport;
         bool viewport_with_count;
         bool scissor;
         bool scissor_with_count;
         bool vertex_input;
      } has_dynamic_state = { 0 };

      if (info->pDynamicState) {
         for (uint32_t j = 0; j < info->pDynamicState->dynamicStateCount;
              j++) {
            switch (info->pDynamicState->pDynamicStates[j]) {
            case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
               has_dynamic_state.rasterizer_discard_enable = true;
               break;
            case VK_DYNAMIC_STATE_VIEWPORT:
               has_dynamic_state.viewport = true;
               break;
            case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
               has_dynamic_state.viewport_with_count = true;
               break;
            case VK_DYNAMIC_STATE_SCISSOR:
               has_dynamic_state.scissor = true;
               break;
            case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
               has_dynamic_state.scissor_with_count = true;
               break;
            case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
               has_dynamic_state.vertex_input = true;
               break;
            default:
               break;
            }
         }
      }

      const struct vn_render_pass *pass =
         vn_render_pass_from_handle(info->renderPass);

      const struct vn_subpass *subpass = NULL;
      if (pass)
         subpass = &pass->subpasses[info->subpass];

      /* TODO: Ignore VkPipelineRenderingCreateInfo when not using dynamic
       * rendering. This requires either a deep rewrite of
       * VkGraphicsPipelineCreateInfo::pNext or a fix in the generated
       * protocol code.
       *
       * The Vulkan spec (1.3.223) says about VkPipelineRenderingCreateInfo:
       *    If a graphics pipeline is created with a valid VkRenderPass,
       *    parameters of this structure are ignored.
       */
      const bool has_dynamic_rendering = !pass && rendering_info;

      /* For each pipeline state category, we define a bool.
       *
       * The Vulkan spec (1.3.223) says:
       *    The state required for a graphics pipeline is divided into vertex
       *    input state, pre-rasterization shader state, fragment shader
       *    state, and fragment output state.
       *
       * Without VK_EXT_graphics_pipeline_library, most states are
       * unconditionally included in the pipeline. Despite that, we still
       * reference the state bools in the ignore rules because (a) it makes
       * the ignore condition easier to validate against the text of the
       * relevant VUs; and (b) it makes it easier to enable
       * VK_EXT_graphics_pipeline_library because we won't need to carefully
       * revisit the text of each VU to untangle the missing pipeline state
       * bools.
       */

      /* The spec does not assign a name to this state. We define it just to
       * deduplicate code.
       *
       * The Vulkan spec (1.3.223) says:
       *    If the value of [...]rasterizerDiscardEnable in the
       *    pre-rasterization shader state is VK_FALSE or the
       *    VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE dynamic state is
       *    enabled fragment shader state and fragment output interface state
       *    is included in a complete graphics pipeline.
       */
      const bool has_raster_state =
         has_dynamic_state.rasterizer_discard_enable ||
         (info->pRasterizationState &&
          info->pRasterizationState->rasterizerDiscardEnable == VK_FALSE);

      /* VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT */
      const bool has_fragment_shader_state = has_raster_state;

      /* VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT */
      const bool has_fragment_output_state = has_raster_state;

      /* Ignore pTessellationState?
       *    VUID-VkGraphicsPipelineCreateInfo-pStages-00731
       */
      if (info->pTessellationState &&
          (!(stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) ||
           !(stages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))) {
         erase.tessellation_state = true;
      }

      /* Ignore pViewportState?
       *    VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00750
       *    VUID-VkGraphicsPipelineCreateInfo-pViewportState-04892
       */
      if (info->pViewportState && has_raster_state) {
         erase.viewport_state = true;
      }

      /* Ignore pViewports?
       *    VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-04130
       *
       * If viewportCount is 0, then venus encoder will ignore pViewports and
       * we do not need to erase it.
       */
      if (!erase.viewport_state && info->pViewportState &&
          info->pViewportState->pViewports &&
          info->pViewportState->viewportCount) {
         const bool has_dynamic_viewport =
            has_dynamic_state.viewport ||
            has_dynamic_state.viewport_with_count;

         if (has_dynamic_viewport) {
            erase.viewport_state_viewports = true;
         }
      }

      /* Ignore pScissors?
       *    VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-04131
       *
       * If scissorCount is 0, then venus encoder will ignore pScissors and we
       * do not need to erase it.
       */
      if (!erase.viewport_state && info->pViewportState &&
          info->pViewportState->pScissors &&
          info->pViewportState->scissorCount) {
         const bool has_dynamic_scissor =
            has_dynamic_state.scissor || has_dynamic_state.scissor_with_count;
         if (has_dynamic_scissor) {
            erase.viewport_state_scissors = true;
         }
      }

      /* Ignore pMultisampleState?
       *    VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00751
       */
      if (info->pMultisampleState && !has_fragment_output_state) {
         erase.multisample_state = true;
      }

      /* Ignore pDepthStencilState? */
      if (info->pDepthStencilState) {
         const bool has_static_attachment =
            subpass &&
            (subpass->attachment_aspects &
             (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

         /* VUID-VkGraphicsPipelineCreateInfo-renderPass-06043 */
         bool require_state =
            has_fragment_shader_state && has_static_attachment;

         if (!require_state) {
            const bool has_dynamic_attachment =
               has_dynamic_rendering &&
               (rendering_info->depthAttachmentFormat !=
                   VK_FORMAT_UNDEFINED ||
                rendering_info->stencilAttachmentFormat !=
                   VK_FORMAT_UNDEFINED);

            /* VUID-VkGraphicsPipelineCreateInfo-renderPass-06053 */
            require_state = has_fragment_shader_state &&
                            has_fragment_output_state &&
                            has_dynamic_attachment;
         }

         erase.depth_stencil_state = !require_state;
      }

      /* Ignore pColorBlendState? */
      if (info->pColorBlendState) {
         const bool has_static_attachment =
            subpass &&
            (subpass->attachment_aspects & VK_IMAGE_ASPECT_COLOR_BIT);

         /* VUID-VkGraphicsPipelineCreateInfo-renderPass-06044 */
         bool require_state =
            has_fragment_output_state && has_static_attachment;

         if (!require_state) {
            const bool has_dynamic_attachment =
               has_dynamic_rendering && rendering_info->colorAttachmentCount;

            /* VUID-VkGraphicsPipelineCreateInfo-renderPass-06054 */
            require_state =
               has_fragment_output_state && has_dynamic_attachment;
         }

         erase.color_blend_state = !require_state;
      }

      /* Ignore pVertexInputState?
       * The Vulkan spec (1.3.264) says:
       * VK_DYNAMIC_STATE_VERTEX_INPUT_EXT specifies that the
       * pVertexInputState state will be ignored and must be set dynamically
       * with vkCmdSetVertexInputEXT before any drawing commands
       */
      if (info->pVertexInputState && has_dynamic_state.vertex_input)
         erase.vertex_input_state = true;

      /* Ignore basePipelineHandle?
       *    VUID-VkGraphicsPipelineCreateInfo-flags-00722
       *    VUID-VkGraphicsPipelineCreateInfo-flags-00724
       *    VUID-VkGraphicsPipelineCreateInfo-flags-00725
       */
      if (info->basePipelineHandle != VK_NULL_HANDLE &&
          !(info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT)) {
         erase.base_pipeline_handle = true;
      }

      if (erase.mask != 0)
         continue;

      if (!fix_tmp) {
         fix_tmp = vn_graphics_pipeline_fix_tmp_alloc(alloc, info_count);

         if (!fix_tmp)
            return NULL;

         memcpy(fix_tmp->infos, infos, info_count * sizeof(infos[0]));
      }

      if (erase.tessellation_state)
         fix_tmp->infos[i].pTessellationState = NULL;

      if (erase.viewport_state)
         fix_tmp->infos[i].pViewportState = NULL;

      if (fix_tmp->infos[i].pViewportState) {
         if (erase.viewport_state_viewports ||
             erase.viewport_state_scissors) {
            fix_tmp->viewport_state_infos[i] = *info->pViewportState;
            fix_tmp->infos[i].pViewportState =
               &fix_tmp->viewport_state_infos[i];
         }

         if (erase.viewport_state_viewports)
            fix_tmp->viewport_state_infos[i].pViewports = NULL;

         if (erase.viewport_state_scissors)
            fix_tmp->viewport_state_infos[i].pScissors = NULL;
      }

      if (erase.multisample_state)
         fix_tmp->infos[i].pMultisampleState = NULL;

      if (erase.depth_stencil_state)
         fix_tmp->infos[i].pDepthStencilState = NULL;

      if (erase.color_blend_state)
         fix_tmp->infos[i].pColorBlendState = NULL;

      if (erase.vertex_input_state)
         fix_tmp->infos[i].pVertexInputState = NULL;

      if (erase.base_pipeline_handle)
         fix_tmp->infos[i].basePipelineHandle = VK_NULL_HANDLE;
   }

   if (!fix_tmp)
      return infos;

   *out_fix_tmp = fix_tmp;
   return fix_tmp->infos;
}

/**
 * We invalidate each VkPipelineCreationFeedback. This is a legal but useless
 * implementation.
 *
 * We invalidate because the venus protocol (as of 2022-08-25) does not know
 * that the VkPipelineCreationFeedback structs in the
 * VkGraphicsPipelineCreateInfo pNext are output parameters. Before
 * VK_EXT_pipeline_creation_feedback, the pNext chain was input-only.
 */
static void
vn_invalidate_pipeline_creation_feedback(const VkBaseInStructure *chain)
{
   const VkPipelineCreationFeedbackCreateInfo *feedback_info =
      vk_find_struct_const(chain, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);

   if (!feedback_info)
      return;

   feedback_info->pPipelineCreationFeedback->flags = 0;

   for (uint32_t i = 0; i < feedback_info->pipelineStageCreationFeedbackCount;
        i++)
      feedback_info->pPipelineStageCreationFeedbacks[i].flags = 0;
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
   struct vn_graphics_pipeline_fix_tmp *fix_tmp = NULL;
   bool want_sync = false;
   VkResult result;

   memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);

   pCreateInfos = vn_fix_graphics_pipeline_create_infos(
      dev, createInfoCount, pCreateInfos, alloc, &fix_tmp);
   if (!pCreateInfos)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (!vn_create_pipeline_handles(dev, VN_PIPELINE_TYPE_GRAPHICS,
                                   createInfoCount, pPipelines, alloc)) {
      vk_free(alloc, fix_tmp);
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline = vn_pipeline_from_handle(pPipelines[i]);

      /* Grab a refcount on the pipeline layout when needed. Take care; the
       * pipeline layout may be omitted or ignored in incomplete pipelines.
       */
      struct vn_pipeline_layout *layout =
         vn_pipeline_layout_from_handle(pCreateInfos[i].layout);
      if (layout && (layout->push_descriptor_set_layout ||
                     layout->has_push_constant_ranges)) {
         pipeline->layout = vn_pipeline_layout_ref(dev, layout);
      }

      if ((pCreateInfos[i].flags & VN_PIPELINE_CREATE_SYNC_MASK))
         want_sync = true;

      vn_invalidate_pipeline_creation_feedback(
         (const VkBaseInStructure *)pCreateInfos[i].pNext);
   }

   if (want_sync) {
      result = vn_call_vkCreateGraphicsPipelines(
         dev->instance, device, pipelineCache, createInfoCount, pCreateInfos,
         NULL, pPipelines);
      if (result != VK_SUCCESS)
         vn_destroy_failed_pipelines(dev, createInfoCount, pPipelines, alloc);
   } else {
      vn_async_vkCreateGraphicsPipelines(dev->instance, device, pipelineCache,
                                         createInfoCount, pCreateInfos, NULL,
                                         pPipelines);
      result = VK_SUCCESS;
   }

   vk_free(alloc, fix_tmp);
   return vn_result(dev->instance, result);
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
   bool want_sync = false;
   VkResult result;

   memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);

   if (!vn_create_pipeline_handles(dev, VN_PIPELINE_TYPE_COMPUTE,
                                   createInfoCount, pPipelines, alloc))
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline = vn_pipeline_from_handle(pPipelines[i]);
      struct vn_pipeline_layout *layout =
         vn_pipeline_layout_from_handle(pCreateInfos[i].layout);
      if (layout->push_descriptor_set_layout ||
          layout->has_push_constant_ranges) {
         pipeline->layout = vn_pipeline_layout_ref(dev, layout);
      }
      if ((pCreateInfos[i].flags & VN_PIPELINE_CREATE_SYNC_MASK))
         want_sync = true;

      vn_invalidate_pipeline_creation_feedback(
         (const VkBaseInStructure *)pCreateInfos[i].pNext);
   }

   if (want_sync) {
      result = vn_call_vkCreateComputePipelines(
         dev->instance, device, pipelineCache, createInfoCount, pCreateInfos,
         NULL, pPipelines);
      if (result != VK_SUCCESS)
         vn_destroy_failed_pipelines(dev, createInfoCount, pPipelines, alloc);
   } else {
      vn_call_vkCreateComputePipelines(dev->instance, device, pipelineCache,
                                       createInfoCount, pCreateInfos, NULL,
                                       pPipelines);
      result = VK_SUCCESS;
   }

   return vn_result(dev->instance, result);
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

   if (pipeline->layout) {
      vn_pipeline_layout_unref(dev, pipeline->layout);
   }

   vn_async_vkDestroyPipeline(dev->instance, device, _pipeline, NULL);

   vn_object_base_fini(&pipeline->base);
   vk_free(alloc, pipeline);
}

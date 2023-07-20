#include "nvk_physical_device.h"

#include "nvk_bo_sync.h"
#include "nvk_buffer.h"
#include "nvk_entrypoints.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "nvk_instance.h"
#include "nvk_shader.h"
#include "nvk_wsi.h"
#include "git_sha1.h"
#include "util/mesa-sha1.h"

#include "vulkan/runtime/vk_device.h"
#include "vulkan/wsi/wsi_common.h"

#include "cl90c0.h"
#include "cl91c0.h"
#include "cla0c0.h"
#include "cla1c0.h"
#include "clb0c0.h"
#include "clb197.h"
#include "clb1c0.h"
#include "clc0c0.h"
#include "clc1c0.h"
#include "clc3c0.h"
#include "clc597.h"
#include "clc5c0.h"
#include "clc597.h"

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties2 *pProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
   VkSampleCountFlagBits sample_counts = VK_SAMPLE_COUNT_1_BIT |
                                         VK_SAMPLE_COUNT_2_BIT |
                                         VK_SAMPLE_COUNT_4_BIT |
                                         VK_SAMPLE_COUNT_8_BIT;
   pProperties->properties = (VkPhysicalDeviceProperties) {
      .apiVersion = VK_MAKE_VERSION(1, 0, VK_HEADER_VERSION),
      .driverVersion = vk_get_driver_version(),
      .vendorID = pdev->dev->vendor_id,
      .deviceID = pdev->dev->device_id,
      .deviceType = pdev->dev->is_integrated ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                             : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
      .limits = (VkPhysicalDeviceLimits) {
         .maxImageArrayLayers = 2048,
         .maxImageDimension1D = pdev->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .maxImageDimension2D = pdev->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .maxImageDimension3D = 0x4000,
         .maxImageDimensionCube = 0x8000,
         .maxPushConstantsSize = NVK_MAX_PUSH_SIZE,
         .maxMemoryAllocationCount = 1024,
         .bufferImageGranularity = pdev->dev->chipset >= 0x120 ? 0x400 : 0x10000,
         .maxFramebufferHeight = pdev->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .maxFramebufferWidth = pdev->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .maxFramebufferLayers = 2048,
         .maxColorAttachments = NVK_MAX_RTS,
         .maxClipDistances = 8,
         .maxCullDistances = 8,
         .maxCombinedClipAndCullDistances = 8,
         .maxFragmentCombinedOutputResources = 16,
         .maxFragmentInputComponents = 128,
         .maxFragmentOutputAttachments = NVK_MAX_RTS,
         .maxFragmentDualSrcAttachments = 1,
         .maxSamplerAllocationCount = 4096,
         .maxSamplerLodBias = 15,
         .maxSamplerAnisotropy = 16,
         .maxSampleMaskWords = 1,
         .minTexelGatherOffset = -32,
         .minTexelOffset = -8,
         .maxTexelGatherOffset = 31,
         .maxTexelOffset = 7,
         .minInterpolationOffset = -0.5,
         .maxInterpolationOffset = 0.4375,
         .mipmapPrecisionBits = 8,
         .subPixelInterpolationOffsetBits = 4,
         .subPixelPrecisionBits = 8,
         .subTexelPrecisionBits = 8,
         .viewportSubPixelBits = 8,
         .maxUniformBufferRange = 65536,
         .maxStorageBufferRange = UINT32_MAX,
         .maxTexelBufferElements = 128 * 1024 * 1024,
         .maxBoundDescriptorSets = NVK_MAX_SETS,
         .maxPerStageDescriptorSamplers = UINT32_MAX,
         .maxPerStageDescriptorUniformBuffers = UINT32_MAX,
         .maxPerStageDescriptorStorageBuffers = UINT32_MAX,
         .maxPerStageDescriptorSampledImages = UINT32_MAX,
         .maxPerStageDescriptorStorageImages = UINT32_MAX,
         .maxPerStageDescriptorInputAttachments = UINT32_MAX,
         .maxPerStageResources = UINT32_MAX,
         .maxDescriptorSetSamplers = UINT32_MAX,
         .maxDescriptorSetUniformBuffers = UINT32_MAX,
         .maxDescriptorSetUniformBuffersDynamic = NVK_MAX_DYNAMIC_BUFFERS / 2,
         .maxDescriptorSetStorageBuffers = UINT32_MAX,
         .maxDescriptorSetStorageBuffersDynamic = NVK_MAX_DYNAMIC_BUFFERS / 2,
         .maxDescriptorSetSampledImages = UINT32_MAX,
         .maxDescriptorSetStorageImages = UINT32_MAX,
         .maxDescriptorSetInputAttachments = UINT32_MAX,
         .maxComputeSharedMemorySize = 49152,
         .maxComputeWorkGroupCount = {0x7fffffff, 65535, 65535},
         .maxComputeWorkGroupInvocations = 1024,
         .maxComputeWorkGroupSize = {1024, 1024, 64},
         .maxViewports = NVK_MAX_VIEWPORTS,
         .maxViewportDimensions = { 32768, 32768 },
         .viewportBoundsRange = { -65536, 65536 },
         .pointSizeRange = { 1.0, 2047.94 },
         .pointSizeGranularity = 0.0625,
         .lineWidthRange = { 1, 64 },
         .lineWidthGranularity = 0.0625,
         .nonCoherentAtomSize = 64,
         .minMemoryMapAlignment = 64,
         .minUniformBufferOffsetAlignment =
            nvk_get_buffer_alignment(pdev, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0),
         .minTexelBufferOffsetAlignment =
            nvk_get_buffer_alignment(pdev, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                           VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                                     0),
         .minStorageBufferOffsetAlignment =
            nvk_get_buffer_alignment(pdev, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0),
         .maxVertexInputAttributeOffset = 2047,
         .maxVertexInputAttributes = 32,
         .maxVertexInputBindingStride = 2048,
         .maxVertexInputBindings = 32,
         .maxVertexOutputComponents = 128,
         .maxTessellationGenerationLevel = 64,
         .maxTessellationPatchSize = 32,
         .maxTessellationControlPerVertexInputComponents = 128,
         .maxTessellationControlPerVertexOutputComponents = 128,
         .maxTessellationControlPerPatchOutputComponents = 120,
         .maxTessellationControlTotalOutputComponents = 4216,
         .maxTessellationEvaluationInputComponents = 128,
         .maxTessellationEvaluationOutputComponents = 128,
         .maxGeometryShaderInvocations = 32,
         .maxGeometryInputComponents = 128,
         .maxGeometryOutputComponents = 128,
         .maxGeometryOutputVertices = 1024,
         .maxGeometryTotalOutputComponents = 1024,
         .maxDrawIndexedIndexValue = UINT32_MAX,
         .maxDrawIndirectCount = UINT32_MAX,
         .timestampComputeAndGraphics = true,
         .timestampPeriod = 1,
         .framebufferColorSampleCounts = sample_counts,
         .framebufferDepthSampleCounts = sample_counts,
         .framebufferNoAttachmentsSampleCounts = sample_counts,
         .framebufferStencilSampleCounts = sample_counts,
         .sampledImageColorSampleCounts = sample_counts,
         .sampledImageDepthSampleCounts = sample_counts,
         .sampledImageIntegerSampleCounts = sample_counts,
         .sampledImageStencilSampleCounts = sample_counts,
         .storageImageSampleCounts = sample_counts,
         .standardSampleLocations = true,
         .optimalBufferCopyOffsetAlignment = 1,
         .optimalBufferCopyRowPitchAlignment = 1,
      },

      /* More properties */
   };

   snprintf(pProperties->properties.deviceName,
            sizeof(pProperties->properties.deviceName),
            "%s", pdev->dev->device_name);

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
      /* Vulkan 1.1 properties */
      .pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY,
      .maxMultiviewViewCount = NVK_MAX_MULTIVIEW_VIEW_COUNT,
      .maxMultiviewInstanceIndex = UINT32_MAX,
      .maxPerSetDescriptors = UINT32_MAX,
      .maxMemoryAllocationSize = (1u << 31),
   };
   memcpy(core_1_1.deviceUUID, pdev->device_uuid, VK_UUID_SIZE);
   struct nvk_instance *instance = nvk_physical_device_instance(pdev);
   memcpy(core_1_1.driverUUID, instance->driver_uuid, VK_UUID_SIZE);

   VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
      /* Vulkan 1.2 properties */
      .supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT |
                                    VK_RESOLVE_MODE_AVERAGE_BIT |
                                    VK_RESOLVE_MODE_MIN_BIT |
                                    VK_RESOLVE_MODE_MAX_BIT,
      .supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT |
                                      VK_RESOLVE_MODE_MIN_BIT |
                                      VK_RESOLVE_MODE_MAX_BIT,
      .independentResolveNone = true,
      .independentResolve = true,
      .driverID = VK_DRIVER_ID_MESA_NVK,
      .conformanceVersion = (VkConformanceVersion) { /* TODO: conf version */
         .major = 0,
         .minor = 0,
         .subminor = 0,
         .patch = 0,
      },
      .maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX,
      .shaderUniformBufferArrayNonUniformIndexingNative = false,
      .shaderSampledImageArrayNonUniformIndexingNative = pdev->info.cls_eng3d >= TURING_A,
      .shaderStorageBufferArrayNonUniformIndexingNative = true,
      .shaderStorageImageArrayNonUniformIndexingNative = pdev->info.cls_eng3d >= TURING_A,
      .shaderInputAttachmentArrayNonUniformIndexingNative = false,
      .robustBufferAccessUpdateAfterBind = true,
      .quadDivergentImplicitLod = pdev->info.cls_eng3d >= TURING_A,
      .maxPerStageDescriptorUpdateAfterBindSamplers = UINT32_MAX,
      .maxPerStageDescriptorUpdateAfterBindUniformBuffers = UINT32_MAX,
      .maxPerStageDescriptorUpdateAfterBindStorageBuffers = UINT32_MAX,
      .maxPerStageDescriptorUpdateAfterBindSampledImages = UINT32_MAX,
      .maxPerStageDescriptorUpdateAfterBindStorageImages = UINT32_MAX,
      .maxPerStageDescriptorUpdateAfterBindInputAttachments = UINT32_MAX,
      .maxPerStageUpdateAfterBindResources = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindSamplers = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindUniformBuffers = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = NVK_MAX_DYNAMIC_BUFFERS / 2,
      .maxDescriptorSetUpdateAfterBindStorageBuffers = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = NVK_MAX_DYNAMIC_BUFFERS / 2,
      .maxDescriptorSetUpdateAfterBindSampledImages = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindStorageImages = UINT32_MAX,
      .maxDescriptorSetUpdateAfterBindInputAttachments = UINT32_MAX,
      .filterMinmaxSingleComponentFormats = true,
      .filterMinmaxImageComponentMapping = true,
   };

   snprintf(core_1_2.driverName, VK_MAX_DRIVER_NAME_SIZE, "NVK");
   snprintf(core_1_2.driverInfo, VK_MAX_DRIVER_INFO_SIZE,
            "Mesa " PACKAGE_VERSION MESA_GIT_SHA1);

   VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
      /* Vulkan 1.3 properties */
      .maxInlineUniformBlockSize = 1 << 16,
      .maxPerStageDescriptorInlineUniformBlocks = 32,
      .maxBufferSize = UINT32_MAX,
   };

   vk_foreach_struct(ext, pProperties->pNext)
   {
      if (vk_get_physical_device_core_1_1_property_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_property_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_property_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *p = (void *)ext;
         p->dynamicPrimitiveTopologyUnrestricted = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT: {
         VkPhysicalDevicePCIBusInfoPropertiesEXT *p = (void *)ext;
         assert(pdev->info.type == NV_DEVICE_TYPE_DIS);
         p->pciDomain = pdev->info.pci_domain;
         p->pciBus = pdev->info.pci_bus;
         p->pciDevice = pdev->info.pci_dev;
         p->pciFunction = pdev->info.pci_func;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT: {
         VkPhysicalDeviceProvokingVertexPropertiesEXT *p = (void *)ext;
         p->provokingVertexModePerPipeline = true;
         p->transformFeedbackPreservesTriangleFanProvokingVertex = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *p = (void *)ext;
         p->maxPushDescriptors = NVK_MAX_PUSH_DESCRIPTORS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *p = (void *)ext;
         p->maxVertexAttribDivisor = UINT32_MAX;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
         VkPhysicalDeviceRobustness2PropertiesEXT *p = (void *)ext;
         p->robustStorageBufferAccessSizeAlignment =
            NVK_SSBO_BOUNDS_CHECK_ALIGNMENT;
         p->robustUniformBufferAccessSizeAlignment = NVK_MIN_UBO_ALIGNMENT;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT: {
         VkPhysicalDeviceTransformFeedbackPropertiesEXT *p = (void *)ext;
         p->maxTransformFeedbackStreams = 4;
         p->maxTransformFeedbackBuffers = 4;
         p->maxTransformFeedbackBufferSize = UINT32_MAX;
         p->maxTransformFeedbackStreamDataSize = 2048;
         p->maxTransformFeedbackBufferDataSize = 512;
         p->maxTransformFeedbackBufferDataStride = 2048;
         p->transformFeedbackQueries = true;
         p->transformFeedbackStreamsLinesTriangles = false;
         p->transformFeedbackRasterizationStreamSelect = true;
         p->transformFeedbackDraw = true;
         break;
      }
      /* More property structs */
      default:
         break;
      }
   }
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

static void
nvk_get_device_extensions(const struct nv_device_info *info,
                          struct vk_device_extension_table *ext)
{
   *ext = (struct vk_device_extension_table) {
      .KHR_bind_memory2 = true,
      .KHR_buffer_device_address = true,
      .KHR_copy_commands2 = true,
      .KHR_create_renderpass2 = true,
      .KHR_dedicated_allocation = true,
      .KHR_depth_stencil_resolve = true,
      .KHR_descriptor_update_template = true,
      .KHR_device_group = true,
      .KHR_draw_indirect_count = info->cls_eng3d >= TURING_A,
      .KHR_driver_properties = true,
      .KHR_dynamic_rendering = true,
      .KHR_external_memory = true,
      .KHR_external_memory_fd = true,
      .KHR_format_feature_flags2 = true,
      .KHR_get_memory_requirements2 = true,
      .KHR_image_format_list = true,
      .KHR_imageless_framebuffer = true,
      .KHR_maintenance1 = true,
      .KHR_maintenance2 = true,
      .KHR_maintenance3 = true,
      .KHR_maintenance4 = true,
      .KHR_map_memory2 = true,
      .KHR_multiview = true,
      .KHR_push_descriptor = true,
      .KHR_relaxed_block_layout = true,
      .KHR_sampler_mirror_clamp_to_edge = true,
      .KHR_separate_depth_stencil_layouts = true,
      .KHR_shader_draw_parameters = true,
      .KHR_shader_non_semantic_info = true,
      .KHR_spirv_1_4 = true,
      .KHR_storage_buffer_storage_class = true,
#ifdef NVK_USE_WSI_PLATFORM
      .KHR_swapchain = true,
      .KHR_swapchain_mutable_format = true,
#endif
      .KHR_uniform_buffer_standard_layout = true,
      .KHR_variable_pointers = true,
      .EXT_4444_formats = true,
      .EXT_border_color_swizzle = true,
      .EXT_buffer_device_address = true,
      .EXT_custom_border_color = true,
      .EXT_descriptor_indexing = true,
      .EXT_extended_dynamic_state = true,
      .EXT_extended_dynamic_state2 = true,
      .EXT_extended_dynamic_state3 = true,
      .EXT_external_memory_dma_buf = true,
      .EXT_host_query_reset = true,
      .EXT_image_2d_view_of_3d = true,
      .EXT_image_robustness = true,
      .EXT_image_view_min_lod = true,
      .EXT_index_type_uint8 = true,
      .EXT_inline_uniform_block = true,
      .EXT_mutable_descriptor_type = true,
      .EXT_non_seamless_cube_map = true,
      .EXT_pci_bus_info = info->type == NV_DEVICE_TYPE_DIS,
      .EXT_private_data = true,
      .EXT_provoking_vertex = true,
      .EXT_robustness2 = true,
      .EXT_sample_locations = info->cls_eng3d >= MAXWELL_B,
      .EXT_sampler_filter_minmax = info->cls_eng3d >= MAXWELL_B,
      .EXT_separate_stencil_usage = true,
      .EXT_shader_demote_to_helper_invocation = true,
      .EXT_shader_viewport_index_layer = info->cls_eng3d >= MAXWELL_B,
      .EXT_transform_feedback = true,
      .EXT_vertex_attribute_divisor = true,
      .EXT_vertex_input_dynamic_state = true,
   };
}

static void
nvk_get_device_features(const struct nv_device_info *info,
                        struct vk_features *features)
{
   *features = (struct vk_features) {
      /* Vulkan 1.0 */
      .robustBufferAccess = true,
      .fullDrawIndexUint32 = true,
      .imageCubeArray = true,
      .independentBlend = true,
      .geometryShader = true,
      .tessellationShader = true,
      .sampleRateShading = true,
      .dualSrcBlend = true,
      .logicOp = true,
      .multiDrawIndirect = true,
      .drawIndirectFirstInstance = true,
      .depthClamp = true,
      .depthBiasClamp = true,
      .fillModeNonSolid = true,
      .depthBounds = true,
      .wideLines = true,
      .largePoints = true,
      .alphaToOne = true,
      .multiViewport = true,
      .samplerAnisotropy = true,
      .textureCompressionETC2 = false,
      .textureCompressionBC = true,
      .textureCompressionASTC_LDR = false,
      .occlusionQueryPrecise = true,
      .pipelineStatisticsQuery = true,
      .vertexPipelineStoresAndAtomics = true,
      .fragmentStoresAndAtomics = true,
      .shaderTessellationAndGeometryPointSize = true,
      .shaderImageGatherExtended = true,
      .shaderStorageImageExtendedFormats = true,
      /* TODO: shaderStorageImageMultisample */
      .shaderStorageImageReadWithoutFormat = true,
      .shaderStorageImageWriteWithoutFormat = true,
      .shaderUniformBufferArrayDynamicIndexing = true,
      .shaderSampledImageArrayDynamicIndexing = true,
      .shaderStorageBufferArrayDynamicIndexing = true,
      .shaderStorageImageArrayDynamicIndexing = true,
      .shaderClipDistance = true,
      .shaderCullDistance = true,
      /* TODO: shaderFloat64 */
      /* TODO: shaderInt64 */
      /* TODO: shaderInt16 */
      /* TODO: shaderResourceResidency */
      .shaderResourceMinLod = true,
      /* TODO: sparseResidency* */
      /* TODO: variableMultisampleRate */
      /* TODO: inheritedQueries */
      .inheritedQueries = true,

      /* Vulkan 1.1 */
      .multiview = true,
      .multiviewGeometryShader = true,
      .multiviewTessellationShader = true,
      .shaderDrawParameters = true,

      /* Vulkan 1.2 */
      .samplerMirrorClampToEdge = true,
      .shaderInputAttachmentArrayDynamicIndexing = true,
      .shaderUniformTexelBufferArrayDynamicIndexing = true,
      .shaderStorageTexelBufferArrayDynamicIndexing = true,
      .shaderUniformBufferArrayNonUniformIndexing = true,
      .shaderSampledImageArrayNonUniformIndexing = true,
      .shaderStorageBufferArrayNonUniformIndexing = true,
      .shaderStorageImageArrayNonUniformIndexing = true,
      .shaderInputAttachmentArrayNonUniformIndexing = true,
      .shaderUniformTexelBufferArrayNonUniformIndexing = true,
      .shaderStorageTexelBufferArrayNonUniformIndexing = true,
      .descriptorBindingUniformBufferUpdateAfterBind = true,
      .descriptorBindingSampledImageUpdateAfterBind = true,
      .descriptorBindingStorageImageUpdateAfterBind = true,
      .descriptorBindingStorageBufferUpdateAfterBind = true,
      .descriptorBindingUniformTexelBufferUpdateAfterBind = true,
      .descriptorBindingStorageTexelBufferUpdateAfterBind = true,
      .descriptorBindingUpdateUnusedWhilePending = true,
      .descriptorBindingPartiallyBound = true,
      .descriptorBindingVariableDescriptorCount = true,
      .runtimeDescriptorArray = true,
      .imagelessFramebuffer = true,
      .uniformBufferStandardLayout = true,
      .separateDepthStencilLayouts = true,
      .hostQueryReset = true,
      .bufferDeviceAddress = true,
      .bufferDeviceAddressCaptureReplay = false,
      .bufferDeviceAddressMultiDevice = false,
      .drawIndirectCount = info->cls_eng3d >= TURING_A,
      .samplerFilterMinmax = info->cls_eng3d >= MAXWELL_B,

      /* Vulkan 1.3 */
      .robustImageAccess = true,
      .inlineUniformBlock = true,
      .descriptorBindingInlineUniformBlockUpdateAfterBind = true,
      .privateData = true,
      .shaderDemoteToHelperInvocation = true,
      .dynamicRendering = true,
      .maintenance4 = true,

      /* VK_EXT_4444_formats */
      .formatA4R4G4B4 = true,
      .formatA4B4G4R4 = true,

      /* VK_EXT_border_color_swizzle */
      .borderColorSwizzle = true,
      .borderColorSwizzleFromImage = false,

      /* VK_EXT_buffer_device_address */
      .bufferDeviceAddressCaptureReplayEXT = false,

      /* VK_EXT_custom_border_color */
      .customBorderColors = true,
      .customBorderColorWithoutFormat = true,

      /* VK_EXT_extended_dynamic_state */
      .extendedDynamicState = true,

      /* VK_EXT_extended_dynamic_state2 */
      .extendedDynamicState2 = true,
      .extendedDynamicState2LogicOp = true,
      .extendedDynamicState2PatchControlPoints = true,

      /* VK_EXT_extended_dynamic_state3 */
      .extendedDynamicState3TessellationDomainOrigin = false,
      .extendedDynamicState3DepthClampEnable = false,
      .extendedDynamicState3PolygonMode = true,
      .extendedDynamicState3RasterizationSamples = false,
      .extendedDynamicState3SampleMask = false,
      .extendedDynamicState3AlphaToCoverageEnable = false,
      .extendedDynamicState3AlphaToOneEnable = false,
      .extendedDynamicState3LogicOpEnable = true,
      .extendedDynamicState3ColorBlendEnable = false,
      .extendedDynamicState3ColorBlendEquation = false,
      .extendedDynamicState3ColorWriteMask = false,
      .extendedDynamicState3RasterizationStream = false,
      .extendedDynamicState3ConservativeRasterizationMode = false,
      .extendedDynamicState3ExtraPrimitiveOverestimationSize = false,
      .extendedDynamicState3DepthClipEnable = false,
      .extendedDynamicState3SampleLocationsEnable = false,
      .extendedDynamicState3ColorBlendAdvanced = false,
      .extendedDynamicState3ProvokingVertexMode = true,
      .extendedDynamicState3LineRasterizationMode = false,
      .extendedDynamicState3LineStippleEnable = true,
      .extendedDynamicState3DepthClipNegativeOneToOne = true,
      .extendedDynamicState3ViewportWScalingEnable = false,
      .extendedDynamicState3ViewportSwizzle = false,
      .extendedDynamicState3CoverageToColorEnable = false,
      .extendedDynamicState3CoverageToColorLocation = false,
      .extendedDynamicState3CoverageModulationMode = false,
      .extendedDynamicState3CoverageModulationTableEnable = false,
      .extendedDynamicState3CoverageModulationTable = false,
      .extendedDynamicState3CoverageReductionMode = false,
      .extendedDynamicState3RepresentativeFragmentTestEnable = false,
      .extendedDynamicState3ShadingRateImageEnable = false,

      /* VK_EXT_image_2d_view_of_3d */
      .image2DViewOf3D = true,
      .sampler2DViewOf3D = true,

      /* VK_EXT_image_view_min_lod */
      .minLod = true,

      /* VK_EXT_index_type_uint8 */
      .indexTypeUint8 = true,

      /* VK_EXT_non_seamless_cube_map */
      .nonSeamlessCubeMap = true,

      /* VK_EXT_provoking_vertex */
      .provokingVertexLast = true,
      .transformFeedbackPreservesProvokingVertex = true,

      /* VK_EXT_robustness2 */
      .robustBufferAccess2 = true,
      .robustImageAccess2 = true,
      .nullDescriptor = true,

      /* VK_EXT_transform_feedback */
      .transformFeedback = true,
      .geometryStreams = true,

      /* VK_EXT_vertex_attribute_divisor */
      .vertexAttributeInstanceRateDivisor = true,
      .vertexAttributeInstanceRateZeroDivisor = true,

      /* VK_EXT_vertex_input_dynamic_state */
      .vertexInputDynamicState = true,

      /* VALVE_mutable_descriptor_type */
      .mutableDescriptorType = true,
   };
}

VkResult
nvk_create_drm_physical_device(struct vk_instance *_instance,
                               drmDevicePtr drm_device,
                               struct vk_physical_device **device_out)
{
   struct nvk_instance *instance = (struct nvk_instance *)_instance;
   VkResult result;

   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   switch (drm_device->bustype) {
   case DRM_BUS_PCI:
      if (drm_device->deviceinfo.pci->vendor_id != NVIDIA_VENDOR_ID)
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      break;

   case DRM_BUS_PLATFORM: {
      const char *compat_prefix = "nvidia,";
      bool found = false;
      for (int i = 0; drm_device->deviceinfo.platform->compatible[i] != NULL; i++) {
         if (strncmp(drm_device->deviceinfo.platform->compatible[0], compat_prefix, strlen(compat_prefix)) == 0) {
            found = true;
            break;
         }
      }
      if (!found)
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      break;
   }

   default:
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct nouveau_ws_device *ndev = nouveau_ws_device_new(drm_device);
   if (!ndev)
      return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

   vk_warn_non_conformant_implementation("NVK");

   struct nvk_physical_device *pdev =
      vk_zalloc(&instance->vk.alloc, sizeof(*pdev),
                8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   if (pdev == NULL) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_dev_alloc;
   }

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &nvk_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   struct vk_device_extension_table supported_extensions;
   nvk_get_device_extensions(&ndev->info, &supported_extensions);

   struct vk_features supported_features;
   nvk_get_device_features(&ndev->info, &supported_features);

   result = vk_physical_device_init(&pdev->vk, &instance->vk,
                                    &supported_extensions,
                                    &supported_features,
                                    &dispatch_table);

   if (result != VK_SUCCESS)
      goto fail_alloc;

   pdev->dev = ndev;
   pdev->info = ndev->info;

   const struct {
      uint16_t vendor_id;
      uint16_t device_id;
      uint8_t pad[12];
   } dev_uuid = {
      .vendor_id = NVIDIA_VENDOR_ID,
      .device_id = pdev->info.pci_device_id,
   };
   STATIC_ASSERT(sizeof(dev_uuid) == VK_UUID_SIZE);
   memcpy(pdev->device_uuid, &dev_uuid, VK_UUID_SIZE);

   pdev->mem_heaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   pdev->mem_types[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   pdev->mem_types[0].heapIndex = 0;

   if (ndev->vram_size) {
      pdev->mem_type_cnt = 2;
      pdev->mem_heap_cnt = 2;

      pdev->mem_heaps[0].size = ndev->vram_size;
      pdev->mem_heaps[1].size = ndev->gart_size;
      pdev->mem_heaps[1].flags = 0;
      pdev->mem_types[1].heapIndex = 1;
      pdev->mem_types[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   } else {
      pdev->mem_type_cnt = 1;
      pdev->mem_heap_cnt = 1;

      pdev->mem_heaps[0].size = ndev->gart_size;
      pdev->mem_types[0].propertyFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   }

   unsigned st_idx = 0;
   pdev->sync_types[st_idx++] = &nvk_bo_sync_type;
   pdev->sync_types[st_idx++] = NULL;
   assert(st_idx <= ARRAY_SIZE(pdev->sync_types));
   pdev->vk.supported_sync_types = pdev->sync_types;

   result = nvk_init_wsi(pdev);
   if (result != VK_SUCCESS)
      goto fail_init;

   *device_out = &pdev->vk;

   return VK_SUCCESS;

fail_init:
   vk_physical_device_finish(&pdev->vk);
fail_alloc:
   vk_free(&instance->vk.alloc, pdev);
fail_dev_alloc:
   nouveau_ws_device_destroy(ndev);
   return result;
}

void
nvk_physical_device_destroy(struct vk_physical_device *vk_pdev)
{
   struct nvk_physical_device *pdev =
      container_of(vk_pdev, struct nvk_physical_device, vk);

   nvk_finish_wsi(pdev);
   nouveau_ws_device_destroy(pdev->dev);
   vk_physical_device_finish(&pdev->vk);
   vk_free(&pdev->vk.instance->alloc, pdev);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);

   pMemoryProperties->memoryProperties.memoryHeapCount = pdev->mem_heap_cnt;
   for (int i = 0; i < pdev->mem_heap_cnt; i++) {
      pMemoryProperties->memoryProperties.memoryHeaps[i] = pdev->mem_heaps[i];
   }

   pMemoryProperties->memoryProperties.memoryTypeCount = pdev->mem_type_cnt;
   for (int i = 0; i < pdev->mem_type_cnt; i++) {
      pMemoryProperties->memoryProperties.memoryTypes[i] = pdev->mem_types[i];
   }

   vk_foreach_struct(ext, pMemoryProperties->pNext)
   {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   // VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(
      VkQueueFamilyProperties2, out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT |
                                            VK_QUEUE_COMPUTE_BIT |
                                            VK_QUEUE_TRANSFER_BIT;
      p->queueFamilyProperties.queueCount = 1;
      p->queueFamilyProperties.timestampValidBits = 64;
      p->queueFamilyProperties.minImageTransferGranularity = (VkExtent3D){1, 1, 1};
   }
}

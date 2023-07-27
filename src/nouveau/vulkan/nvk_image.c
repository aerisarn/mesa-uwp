#include "nvk_image.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_physical_device.h"

#include "nil_format.h"
#include "vulkan/util/vk_format.h"

#include "clb197.h"

VkFormatFeatureFlags2
nvk_get_image_format_features(struct nvk_physical_device *pdev,
                              VkFormat vk_format, VkImageTiling tiling)
{
   VkFormatFeatureFlags2 features = 0;

   if (tiling != VK_IMAGE_TILING_OPTIMAL)
      return 0;

   enum pipe_format p_format = vk_format_to_pipe_format(vk_format);
   if (p_format == PIPE_FORMAT_NONE)
      return 0;

   if (!nil_format_supports_texturing(&pdev->info, p_format))
      return 0;

   /* You can't tile a non-power-of-two */
   if (!util_is_power_of_two_nonzero(util_format_get_blocksize(p_format)))
      return 0;

   features |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
   features |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
   features |= VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;

   if (nil_format_supports_filtering(&pdev->info, p_format)) {
      features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if (pdev->info.cls_eng3d >= MAXWELL_B)
         features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
   }

   /* TODO: VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT */
   if (vk_format_has_depth(vk_format)) {
      features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
   }

   if (nil_format_supports_color_targets(&pdev->info, p_format)) {
      features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
      if (nil_format_supports_blending(&pdev->info, p_format))
         features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
      features |= VK_FORMAT_FEATURE_2_BLIT_DST_BIT;
   }

   if (vk_format_is_depth_or_stencil(vk_format)) {
      if (!nil_format_supports_depth_stencil(&pdev->info, p_format))
         return 0;

      features |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
   }

   if (nil_format_supports_storage(&pdev->info, p_format)) {
      features |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
   }

   if (p_format == PIPE_FORMAT_R32_UINT || p_format == PIPE_FORMAT_R32_SINT)
      features |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT;

   return features;
}

static VkFormatFeatureFlags2KHR
vk_image_usage_to_format_features(VkImageUsageFlagBits usage_flag)
{
   assert(util_bitcount(usage_flag) == 1);
   switch (usage_flag) {
   case VK_IMAGE_USAGE_TRANSFER_SRC_BIT:
      return VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT_KHR |
             VK_FORMAT_FEATURE_BLIT_SRC_BIT;
   case VK_IMAGE_USAGE_TRANSFER_DST_BIT:
      return VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT_KHR |
             VK_FORMAT_FEATURE_BLIT_DST_BIT;
   case VK_IMAGE_USAGE_SAMPLED_BIT:
      return VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
   case VK_IMAGE_USAGE_STORAGE_BIT:
      return VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
   case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:
      return VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
   case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
      return VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
   default:
      return 0;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);

   const VkPhysicalDeviceExternalImageFormatInfo *external_info = NULL;
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);

   /* Initialize to zero in case we return VK_ERROR_FORMAT_NOT_SUPPORTED */
   memset(&pImageFormatProperties->imageFormatProperties, 0,
          sizeof(pImageFormatProperties->imageFormatProperties));

   VkFormatFeatureFlags2KHR features =
      nvk_get_image_format_features(pdev, pImageFormatInfo->format,
                                          pImageFormatInfo->tiling);
   if (features == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   if (vk_format_is_compressed(pImageFormatInfo->format) &&
       pImageFormatInfo->type != VK_IMAGE_TYPE_2D)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkExtent3D maxExtent;
   uint32_t maxMipLevels;
   uint32_t maxArraySize;
   VkSampleCountFlags sampleCounts;
   switch (pImageFormatInfo->type) {
   case VK_IMAGE_TYPE_1D:
      maxExtent = (VkExtent3D) { 16384, 1, 1 },
      maxMipLevels = 15;
      maxArraySize = 2048;
      sampleCounts = VK_SAMPLE_COUNT_1_BIT;
      break;
   case VK_IMAGE_TYPE_2D:
      maxExtent = (VkExtent3D) { 16384, 16384, 1 };
      maxMipLevels = 15;
      maxArraySize = 2048;
      sampleCounts = VK_SAMPLE_COUNT_1_BIT |
                     VK_SAMPLE_COUNT_2_BIT |
                     VK_SAMPLE_COUNT_4_BIT |
                     VK_SAMPLE_COUNT_8_BIT;
      break;
   case VK_IMAGE_TYPE_3D:
      maxExtent = (VkExtent3D) { 2048, 2048, 2048 };
      maxMipLevels = 12;
      maxArraySize = 1;
      sampleCounts = VK_SAMPLE_COUNT_1_BIT;
      break;
   default:
      unreachable("Invalid image type");
   }

   /* From the Vulkan 1.2.199 spec:
    *
    *    "VK_IMAGE_CREATE_EXTENDED_USAGE_BIT specifies that the image can be
    *    created with usage flags that are not supported for the format the
    *    image is created with but are supported for at least one format a
    *    VkImageView created from the image can have."
    *
    * If VK_IMAGE_CREATE_EXTENDED_USAGE_BIT is set, views can be created with
    * different usage than the image so we can't always filter on usage.
    * There is one exception to this below for storage.
    */
   const VkImageUsageFlags image_usage = pImageFormatInfo->usage;
   VkImageUsageFlags view_usage = image_usage;
   if (pImageFormatInfo->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
      view_usage = 0;

   u_foreach_bit(b, view_usage) {
      VkFormatFeatureFlags2KHR usage_features =
         vk_image_usage_to_format_features(1 << b);
      if (usage_features && !(features & usage_features))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   const VkExternalMemoryProperties *ext_mem_props = NULL;
   if (external_info != NULL && external_info->handleType != 0) {
      bool tiling_has_explicit_layout;
      switch (pImageFormatInfo->tiling) {
      case VK_IMAGE_TILING_LINEAR:
      case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:
         tiling_has_explicit_layout = true;
         break;
      case VK_IMAGE_TILING_OPTIMAL:
         tiling_has_explicit_layout = false;
         break;
      default:
         unreachable("Unsupported VkImageTiling");
      }

      switch (external_info->handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
         /* No special restrictions */
         if (tiling_has_explicit_layout) {
            /* With an explicit memory layout, we don't care which type of
             * fd the image belongs too. Both OPAQUE_FD and DMA_BUF are
             * interchangeable here.
             */
            ext_mem_props = &nvk_dma_buf_mem_props;
         } else {
            ext_mem_props = &nvk_opaque_fd_mem_props;
         }
         break;

      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
         if (!tiling_has_explicit_layout) {
            return vk_errorf(pdev, VK_ERROR_FORMAT_NOT_SUPPORTED,
                             "VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT "
                             "requires VK_IMAGE_TILING_LINEAR or "
                             "VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT");
         }
         ext_mem_props = &nvk_dma_buf_mem_props;
         break;

      default:
         /* From the Vulkan 1.3.256 spec:
          *
          *    "If handleType is not compatible with the [parameters] in
          *    VkPhysicalDeviceImageFormatInfo2, then
          *    vkGetPhysicalDeviceImageFormatProperties2 returns
          *    VK_ERROR_FORMAT_NOT_SUPPORTED."
          */
         return vk_errorf(pdev, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "unsupported VkExternalMemoryTypeFlagBits 0x%x",
                          external_info->handleType);
      }
   }

   pImageFormatProperties->imageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = maxExtent,
      .maxMipLevels = maxMipLevels,
      .maxArrayLayers = maxArraySize,
      .sampleCounts = sampleCounts,
      .maxResourceSize = UINT32_MAX, /* TODO */
   };

   vk_foreach_struct(s, pImageFormatProperties->pNext) {
      switch (s->sType) {
      case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES: {
         VkExternalImageFormatProperties *p = (void *)s;
         /* From the Vulkan 1.3.256 spec:
          *
          *    "If handleType is 0, vkGetPhysicalDeviceImageFormatProperties2
          *    will behave as if VkPhysicalDeviceExternalImageFormatInfo was
          *    not present, and VkExternalImageFormatProperties will be
          *    ignored."
          *
          * This is true if and only if ext_mem_props == NULL
          */
         if (ext_mem_props != NULL)
            p->externalMemoryProperties = *ext_mem_props;
         break;
      }
      default:
         nvk_debug_ignored_stype(s->sType);
         break;
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo,
    uint32_t *pPropertyCount,
    VkSparseImageFormatProperties2 *pProperties)
{
   /* Sparse images are not yet supported. */
   *pPropertyCount = 0;
}

static enum nil_image_dim
vk_image_type_to_nil_dim(VkImageType type)
{
   switch (type) {
   case VK_IMAGE_TYPE_1D:  return NIL_IMAGE_DIM_1D;
   case VK_IMAGE_TYPE_2D:  return NIL_IMAGE_DIM_2D;
   case VK_IMAGE_TYPE_3D:  return NIL_IMAGE_DIM_3D;
   default:
      unreachable("Invalid image type");
   }
}

static VkResult
nvk_image_init(struct nvk_device *dev,
               struct nvk_image *image,
               const VkImageCreateInfo *pCreateInfo)
{
   vk_image_init(&dev->vk, &image->vk, pCreateInfo);

   if ((image->vk.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       image->vk.samples > 1) {
      image->vk.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
      image->vk.stencil_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   }

   if (image->vk.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
      image->vk.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   if (image->vk.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
      image->vk.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   enum nil_image_usage_flags usage = 0; /* TODO */
   if (pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR)
      usage |= NIL_IMAGE_USAGE_LINEAR_BIT;
   if (pCreateInfo->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)
      usage |= NIL_IMAGE_USAGE_2D_VIEW_BIT;
   if (pCreateInfo->flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT)
      usage |= NIL_IMAGE_USAGE_2D_VIEW_BIT;

   /* We treat 3D storage images as 2D arrays.  One day, we may wire up actual
    * 3D storage image support but baseArrayLayer gets tricky.
    */
   if (image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)
      usage |= NIL_IMAGE_USAGE_2D_VIEW_BIT;

   /* In order to be able to clear 3D depth/stencil images, we need to bind
    * them as 2D arrays.  Fortunately, 3D depth/stencil shouldn't be common.
    */
   if ((image->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                             VK_IMAGE_ASPECT_STENCIL_BIT)) &&
       pCreateInfo->imageType == VK_IMAGE_TYPE_3D)
      usage |= NIL_IMAGE_USAGE_2D_VIEW_BIT;

   image->plane_count = vk_format_get_plane_count(pCreateInfo->format);
   image->disjoint = image->plane_count > 1 &&
                     (pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT);

   const struct vk_format_ycbcr_info *ycbcr_info =
      vk_format_get_ycbcr_info(pCreateInfo->format);
   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      VkFormat format = ycbcr_info ?
         ycbcr_info->planes[plane].format : pCreateInfo->format;
      const uint8_t width_scale = ycbcr_info ?
         ycbcr_info->planes[plane].denominator_scales[0] : 1;
      const uint8_t height_scale = ycbcr_info ?
         ycbcr_info->planes[plane].denominator_scales[1] : 1;
      struct nil_image_init_info nil_info = {
         .dim = vk_image_type_to_nil_dim(pCreateInfo->imageType),
         .format = vk_format_to_pipe_format(format),
         .extent_px = {
            .w = pCreateInfo->extent.width / width_scale,
            .h = pCreateInfo->extent.height / height_scale,
            .d = pCreateInfo->extent.depth,
            .a = pCreateInfo->arrayLayers,
         },
         .levels = pCreateInfo->mipLevels,
         .samples = pCreateInfo->samples,
         .usage = usage,
      };

      ASSERTED bool ok = nil_image_init(&nvk_device_physical(dev)->info,
                                        &image->planes[plane].nil, &nil_info);
      assert(ok);
   }

   if (image->vk.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
      struct nil_image_init_info stencil_nil_info = {
         .dim = vk_image_type_to_nil_dim(pCreateInfo->imageType),
         .format = PIPE_FORMAT_R32_UINT,
         .extent_px = {
            .w = pCreateInfo->extent.width,
            .h = pCreateInfo->extent.height,
            .d = pCreateInfo->extent.depth,
            .a = pCreateInfo->arrayLayers,
         },
         .levels = pCreateInfo->mipLevels,
         .samples = pCreateInfo->samples,
         .usage = usage,
      };

      ASSERTED bool ok = nil_image_init(&nvk_device_physical(dev)->info,
                                        &image->stencil_copy_temp.nil,
                                        &stencil_nil_info);
      assert(ok);
   }

   return VK_SUCCESS;
}

static void
nvk_image_finish(struct nvk_device *dev, struct nvk_image *image,
                 const VkAllocationCallbacks *pAllocator)
{
   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      if (image->planes[plane].internal)
         nvk_free_memory(dev, image->planes[plane].internal, pAllocator);
   }

   if (image->stencil_copy_temp.internal)
      nvk_free_memory(dev, image->stencil_copy_temp.internal, pAllocator);

   vk_image_finish(&image->vk);
}

static VkResult
nvk_image_plane_alloc_internal(struct nvk_device *dev,
                               struct nvk_image_plane *plane,
                               const VkAllocationCallbacks *pAllocator)
{
   if (plane->nil.pte_kind == 0)
      return VK_SUCCESS;

   assert(dev->pdev->mem_heaps[0].flags &
          VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);

   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = plane->nil.size_B,
      .memoryTypeIndex = 0,
   };
   const struct nvk_memory_tiling_info tile_info = {
      .tile_mode = plane->nil.tile_mode,
      .pte_kind = plane->nil.pte_kind,
   };
   return nvk_allocate_memory(dev, &alloc_info, &tile_info,
                              pAllocator, &plane->internal);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateImage(VkDevice device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkImage *pImage)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_image *image;
   VkResult result;

   image = vk_zalloc2(&dev->vk.alloc, pAllocator, sizeof(*image), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = nvk_image_init(dev, image, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->vk.alloc, pAllocator, image);
      return result;
   }
   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      result = nvk_image_plane_alloc_internal(dev, &image->planes[plane],
                                              pAllocator);
      if (result != VK_SUCCESS) {
         nvk_image_finish(dev, image, pAllocator);
         vk_free2(&dev->vk.alloc, pAllocator, image);
         return result;
      }
   }

   if (image->stencil_copy_temp.nil.size_B > 0) {
      result = nvk_image_plane_alloc_internal(dev, &image->stencil_copy_temp,
                                              pAllocator);
      if (result != VK_SUCCESS) {
         nvk_image_finish(dev, image, pAllocator);
         vk_free2(&dev->vk.alloc, pAllocator, image);
         return result;
      }
   }

   *pImage = nvk_image_to_handle(image);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyImage(VkDevice device,
                 VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_image, image, _image);

   if (!image)
      return;

   nvk_image_finish(dev, image, pAllocator);
   vk_free2(&dev->vk.alloc, pAllocator, image);
}

static void
nvk_image_plane_add_req(struct nvk_image_plane *plane,
                        uint64_t *size_B, uint32_t *align_B)
{
   assert(util_is_power_of_two_or_zero64(*align_B));
   assert(util_is_power_of_two_or_zero64(plane->nil.align_B));

   *align_B = MAX2(*align_B, plane->nil.align_B);
   *size_B = ALIGN_POT(*size_B, plane->nil.align_B);
   *size_B += plane->nil.size_B;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetImageMemoryRequirements2(VkDevice device,
                                const VkImageMemoryRequirementsInfo2 *pInfo,
                                VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_image, image, pInfo->image);

   uint32_t memory_types = (1 << dev->pdev->mem_type_cnt) - 1;

   // TODO hope for the best?

   VkImageAspectFlags aspects = image->vk.aspects;

   uint64_t size_B = 0;
   uint32_t align_B = 0;
   if (image->disjoint) {
      const VkImagePlaneMemoryRequirementsInfo *plane_memory_req_info =
        vk_find_struct_const(pInfo->pNext, IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO);
      aspects = plane_memory_req_info->planeAspect;
      uint8_t plane = nvk_image_aspects_to_plane(image, aspects);
      nvk_image_plane_add_req(&image->planes[plane], &size_B, &align_B);
   } else {
      for (unsigned plane = 0; plane < image->plane_count; plane++)
         nvk_image_plane_add_req(&image->planes[plane], &size_B, &align_B);
   }

   assert(image->vk.external_handle_types == 0 || image->plane_count == 1);
   bool needs_dedicated =
      image->vk.external_handle_types != 0 &&
      image->planes[0].nil.pte_kind != 0;

   if (image->stencil_copy_temp.nil.size_B > 0)
      nvk_image_plane_add_req(&image->stencil_copy_temp, &size_B, &align_B);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_types;
   pMemoryRequirements->memoryRequirements.alignment = align_B;
   pMemoryRequirements->memoryRequirements.size = size_B;

   vk_foreach_struct_const(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->prefersDedicatedAllocation = needs_dedicated;
         dedicated->requiresDedicatedAllocation = needs_dedicated;
         break;
      }
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL 
nvk_GetDeviceImageMemoryRequirements(VkDevice device,
                                     const VkDeviceImageMemoryRequirementsKHR *pInfo,
                                     VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   ASSERTED VkResult result;
   struct nvk_image image = {0};

   result = nvk_image_init(dev, &image, pInfo->pCreateInfo);
   assert(result == VK_SUCCESS);

   VkImageMemoryRequirementsInfo2 info2 = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = nvk_image_to_handle(&image),
   };

   nvk_GetImageMemoryRequirements2(device, &info2, pMemoryRequirements);
   nvk_image_finish(dev, &image, NULL);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetImageSparseMemoryRequirements2(VkDevice device,
                                      const VkImageSparseMemoryRequirementsInfo2* pInfo,
                                      uint32_t* pSparseMemoryRequirementCount,
                                      VkSparseImageMemoryRequirements2* pSparseMemoryRequirements)
{
   /* We dont support sparse images yet, this is a stub to get KHR_get_memory_requirements2 */
   *pSparseMemoryRequirementCount = 0;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDeviceImageSparseMemoryRequirements(VkDevice device,
                                           const VkDeviceImageMemoryRequirementsKHR* pInfo,
                                           uint32_t *pSparseMemoryRequirementCount,
                                           VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   /* Sparse images are not supported so this is just a stub for now. */
   *pSparseMemoryRequirementCount = 0;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetImageSubresourceLayout(VkDevice device,
                              VkImage _image,
                              const VkImageSubresource *pSubresource,
                              VkSubresourceLayout *pLayout)
{
   VK_FROM_HANDLE(nvk_image, image, _image);

   uint8_t plane = nvk_image_aspects_to_plane(image, pSubresource->aspectMask);

   *pLayout = (VkSubresourceLayout) {
      .offset = nil_image_level_layer_offset_B(&image->planes[plane].nil,
                                               pSubresource->mipLevel,
                                               pSubresource->arrayLayer),
      .size = nil_image_level_size_B(&image->planes[plane].nil, pSubresource->mipLevel),
      .rowPitch = image->planes[plane].nil.levels[pSubresource->mipLevel].row_stride_B,
      .arrayPitch = image->planes[plane].nil.array_stride_B,
      .depthPitch = nil_image_level_depth_stride_B(&image->planes[plane].nil,
                                                   pSubresource->mipLevel),
   };
}

static void
nvk_image_plane_bind(struct nvk_image_plane *plane,
                     struct nvk_device_memory *mem,
                     uint64_t *offset_B)
{
   *offset_B = ALIGN_POT(*offset_B, plane->nil.align_B);
   if (mem->dedicated_image_plane == plane) {
      assert(*offset_B == 0);
      plane->addr = mem->bo->offset;
   } else if (plane->internal != NULL) {
      plane->addr = plane->internal->bo->offset;
   } else {
      plane->addr = mem->bo->offset + *offset_B;
   }
   *offset_B += plane->nil.size_B;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindImageMemory2(VkDevice device,
                     uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(nvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(nvk_image, image, pBindInfos[i].image);

      uint64_t offset_B = pBindInfos[i].memoryOffset;
      if (image->disjoint) {
         const VkBindImagePlaneMemoryInfo *plane_info =
            vk_find_struct_const(pBindInfos[i].pNext, BIND_IMAGE_PLANE_MEMORY_INFO);
         uint8_t plane = nvk_image_aspects_to_plane(image, plane_info->planeAspect);
         nvk_image_plane_bind(&image->planes[plane], mem, &offset_B);
      } else {
         for (unsigned plane = 0; plane < image->plane_count; plane++) {
            nvk_image_plane_bind(&image->planes[plane], mem, &offset_B);
         }
      }

      if (image->stencil_copy_temp.nil.size_B > 0)
         nvk_image_plane_bind(&image->stencil_copy_temp, mem, &offset_B);
   }

   return VK_SUCCESS;
}

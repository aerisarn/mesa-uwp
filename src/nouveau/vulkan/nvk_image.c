#include "nvk_image.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_physical_device.h"

#include "nil_format.h"
#include "vulkan/util/vk_format.h"

static bool
is_storage_image_format(enum pipe_format p_format)
{
   /* TODO: This shouldn't be a fixed list */

   switch (p_format) {
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
   case PIPE_FORMAT_R16G16B16A16_UINT:
   case PIPE_FORMAT_R16G16B16A16_SINT:
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32_FLOAT:
   case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_R8G8B8A8_SINT:
   case PIPE_FORMAT_R16G16_UINT:
   case PIPE_FORMAT_R16G16_SINT:
   case PIPE_FORMAT_R16G16_FLOAT:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
   case PIPE_FORMAT_R16_UINT:
   case PIPE_FORMAT_R16_FLOAT:
   case PIPE_FORMAT_R16_SINT:
   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8_SINT:
   case PIPE_FORMAT_R10G10B10A2_UINT:
   case PIPE_FORMAT_R10G10B10A2_UNORM:
   case PIPE_FORMAT_R11G11B10_FLOAT:
   case PIPE_FORMAT_R16G16B16A16_UNORM:
   case PIPE_FORMAT_R16G16B16A16_SNORM:
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SNORM:
   case PIPE_FORMAT_R16G16_UNORM:
   case PIPE_FORMAT_R16G16_SNORM:
   case PIPE_FORMAT_R8G8_UNORM:
   case PIPE_FORMAT_R8G8_SNORM:
   case PIPE_FORMAT_R16_UNORM:
   case PIPE_FORMAT_R16_SNORM:
   case PIPE_FORMAT_R8_UNORM:
   case PIPE_FORMAT_R8_SNORM:
      return true;
   default:
      return false;
   }
}

VkFormatFeatureFlags2
nvk_get_image_format_features(struct nvk_physical_device *pdevice,
                              VkFormat vk_format, VkImageTiling tiling)
{
   VkFormatFeatureFlags2 features = 0;

   if (tiling != VK_IMAGE_TILING_OPTIMAL)
      return 0;

   enum pipe_format p_format = vk_format_to_pipe_format(vk_format);
   if (p_format == PIPE_FORMAT_NONE)
      return 0;

   const struct nil_tic_format *tic_format = nil_tic_format_for_pipe(p_format);
   if (tic_format == NULL)
      return 0;

   features |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
   features |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;

   if (util_format_is_float(p_format) ||
       util_format_is_unorm(p_format) ||
       util_format_is_snorm(p_format))
      features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

   if (is_storage_image_format(p_format)) {
      features |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
   }

   if (p_format == PIPE_FORMAT_R32_UINT)
      features |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT;

   const struct nvk_format *nvk_format = nvk_get_format(vk_format);
   if (nvk_format && nvk_format->supports_2d_blit) {
      features |= VK_FORMAT_FEATURE_2_BLIT_SRC_BIT |
                  VK_FORMAT_FEATURE_2_BLIT_DST_BIT;
   }

   return features;
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

static VkResult nvk_image_init(struct nvk_device *device,
   struct nvk_image *image,
   const VkImageCreateInfo *pCreateInfo)
{
   vk_image_init(&device->vk, &image->vk, pCreateInfo);

   struct nil_image_init_info nil_info = {
      .dim = vk_image_type_to_nil_dim(pCreateInfo->imageType),
      .format = vk_format_to_pipe_format(pCreateInfo->format),
      .extent_px = {
         .w = pCreateInfo->extent.width,
         .h = pCreateInfo->extent.height,
         .d = pCreateInfo->extent.depth,
         .a = pCreateInfo->arrayLayers,
      },
      .levels = pCreateInfo->mipLevels,
      .samples = pCreateInfo->samples,
   };

   ASSERTED bool ok = nil_image_init(nvk_device_physical(device)->dev,
                                     &image->nil, &nil_info);
   assert(ok);

   return VK_SUCCESS;
}

static void nvk_image_finish(struct nvk_image *image)
{
   vk_image_finish(&image->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateImage(VkDevice _device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkImage *pImage)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_image *image;
   VkResult result;

   image = vk_zalloc2(
      &device->vk.alloc, pAllocator, sizeof(*image), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = nvk_image_init(device, image, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, image);
      return result;
   }

   *pImage = nvk_image_to_handle(image);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyImage(VkDevice _device,
                 VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_image, image, _image);

   if (!image)
      return;

   nvk_image_finish(image);
   vk_free2(&device->vk.alloc, pAllocator, image);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetImageMemoryRequirements2(VkDevice _device,
                                const VkImageMemoryRequirementsInfo2 *pInfo,
                                VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_image, image, pInfo->image);

   uint32_t memory_types = (1 << device->pdev->mem_type_cnt) - 1;

   // TODO hope for the best?
   pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_types;
   pMemoryRequirements->memoryRequirements.alignment = 0x1000;
   pMemoryRequirements->memoryRequirements.size = image->nil.size_B;

   vk_foreach_struct_const(ext, pInfo->pNext) {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetImageSubresourceLayout(VkDevice device,
                              VkImage _image,
                              const VkImageSubresource *pSubresource,
                              VkSubresourceLayout *pLayout)
{
   VK_FROM_HANDLE(nvk_image, image, _image);

   *pLayout = (VkSubresourceLayout) {
      .offset = nil_image_level_layer_offset_B(&image->nil,
                                               pSubresource->mipLevel,
                                               pSubresource->arrayLayer),
      .size = nil_image_level_size_B(&image->nil, pSubresource->mipLevel),
      .rowPitch = image->nil.levels[pSubresource->mipLevel].row_stride_B,
      .arrayPitch = image->nil.array_stride_B,
      .depthPitch = nil_image_level_depth_stride_B(&image->nil,
                                                   pSubresource->mipLevel),
   };
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindImageMemory2(VkDevice _device,
                     uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(nvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(nvk_image, image, pBindInfos[i].image);

      image->mem = mem;
      image->offset = pBindInfos[i].memoryOffset;
   }
   return VK_SUCCESS;
}

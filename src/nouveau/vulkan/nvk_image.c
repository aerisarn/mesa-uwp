#include "nvk_image.h"

#include "vulkan/util/vk_format.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_physical_device.h"

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

VKAPI_ATTR VkResult VKAPI_CALL nvk_CreateImage(VkDevice _device,
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

VKAPI_ATTR void VKAPI_CALL nvk_DestroyImage(VkDevice _device,
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

VKAPI_ATTR void VKAPI_CALL nvk_GetImageMemoryRequirements2(
   VkDevice _device,
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

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindImageMemory2(
   VkDevice _device,
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

#include "nvk_image.h"

#include "vulkan/util/vk_format.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_physical_device.h"

/* calculates optimal tiling for a given CreateInfo
 *
 * This ends being quite wasteful, but it's a more or less plain copy of what gallium does
 */
static struct nvk_tile
nvk_image_tile_from_create_info(const VkImageCreateInfo *pCreateInfo, uint64_t modifier)
{
   struct nvk_tile tile = {};

   switch (pCreateInfo->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      tile.is_tiled = false;
      return tile;
   case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:
      tile.is_fermi = true;
      tile.is_tiled = true;
      tile.x = 0;
      tile.y = modifier & 0xf;
      tile.z = 0;
      return tile;
   case VK_IMAGE_TILING_OPTIMAL:
      /* code is below */
      break;
   default:
      assert(!"unknown image tiling");
      break;
   }

   uint32_t height = pCreateInfo->extent.height;
   uint32_t depth = pCreateInfo->extent.depth;

   // fermi is the baseline anyway (for now)
   tile.is_fermi = true;
   tile.is_tiled = true;

   // always 0 for now
   tile.x = 0;

        if (height >= 256) tile.y = 5;
   else if (height >= 128) tile.y = 4;
   else if (height >=  64) tile.y = 3;
   else if (height >=  32) tile.y = 2;
   else if (height >=  16) tile.y = 1;
   else                    tile.y = 0;

   // not quite sure why, but gallium does the same
   if (pCreateInfo->imageType == VK_IMAGE_TYPE_3D)
      tile.y = MIN2(tile.y, 2);

        if (depth >= 32) tile.z = 5;
   else if (depth >= 16) tile.z = 4;
   else if (depth >=  8) tile.z = 3;
   else if (depth >=  4) tile.z = 2;
   else if (depth >=  2) tile.z = 1;
   else                  tile.z = 0;

   return tile;
}

static VkExtent3D
nvk_image_tile_to_blocks(struct nvk_tile tile)
{
   if (!tile.is_tiled) {
      return (VkExtent3D){1, 1, 1};
   } else {
      uint32_t height = tile.is_fermi ? 8 : 4;

      return (VkExtent3D){
         .width = 64 << tile.x,
         .height = height << tile.y,
         .depth = 1 << tile.z,
      };

   }
}

static VkResult nvk_image_init(struct nvk_device *device,
   struct nvk_image *image,
   const VkImageCreateInfo *pCreateInfo)
{
   uint64_t block_size = vk_format_get_blocksizebits(pCreateInfo->format) / 8;
   struct nvk_tile tile = nvk_image_tile_from_create_info(pCreateInfo, 0);
   VkExtent3D block = nvk_image_tile_to_blocks(tile);

   vk_image_init(&device->vk, &image->vk, pCreateInfo);

   for (unsigned i = 0; i < ARRAY_SIZE(nvk_formats); i++) {
      struct nvk_format *format = &nvk_formats[i];

      if (format->vk_format != pCreateInfo->format)
         continue;

      image->format = format;
   }
   assert(image->format);

   image->tile = tile;
   image->row_stride = align(image->vk.extent.width * block_size, block.width);
   image->layer_stride = align(image->vk.extent.height, block.height) * image->row_stride;
   image->min_size = image->vk.array_layers * image->vk.extent.depth * image->layer_stride;

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
   pMemoryRequirements->memoryRequirements.size = image->min_size;

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

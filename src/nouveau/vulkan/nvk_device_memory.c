#include "nvk_device_memory.h"

#include "nouveau_bo.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"

#include <inttypes.h>
#include <sys/mman.h>

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateMemory(
   VkDevice _device,
   const VkMemoryAllocateInfo *pAllocateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VkMemoryType *type = &device->pdev->mem_types[pAllocateInfo->memoryTypeIndex];
   struct nvk_device_memory *mem;

   mem = vk_object_alloc(&device->vk, pAllocator, sizeof(*mem), VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (!mem)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   enum nouveau_ws_bo_flags flags;
   if (type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      flags = NOUVEAU_WS_BO_LOCAL;
   else
      flags = NOUVEAU_WS_BO_GART;

   if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      flags |= NOUVEAU_WS_BO_MAP;

   mem->map = NULL;
   mem->bo = nouveau_ws_bo_new(device->pdev->dev, pAllocateInfo->allocationSize, 0, flags);
   if (!mem->bo) {
      vk_object_free(&device->vk, pAllocator, mem);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *pMem = nvk_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeMemory(
   VkDevice _device,
   VkDeviceMemory _mem,
   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   if (!mem)
      return;

   if (mem->map)
      nvk_UnmapMemory(_device, _mem);

   nouveau_ws_bo_destroy(mem->bo);

   vk_object_free(&device->vk, pAllocator, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_MapMemory(
   VkDevice _device,
   VkDeviceMemory _memory,
   VkDeviceSize offset,
   VkDeviceSize size,
   VkMemoryMapFlags flags,
   void **ppData)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (size == VK_WHOLE_SIZE)
      size = mem->bo->size - offset;

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%"PRIx64" does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED, "Memory object already mapped.");
   }

   mem->map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
   if (mem->map == NULL) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED, "Memory object couldn't be mapped.");
   }

   *ppData = mem->map + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_UnmapMemory(
   VkDevice _device,
   VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (mem == NULL)
      return;

   munmap(mem->map, mem->bo->size);
   mem->map = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FlushMappedMemoryRanges(
   VkDevice _device,
   uint32_t memoryRangeCount,
   const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_InvalidateMappedMemoryRanges(
   VkDevice _device,
   uint32_t memoryRangeCount,
   const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

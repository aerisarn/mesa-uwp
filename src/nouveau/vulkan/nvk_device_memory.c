#include "nvk_device_memory.h"

#include "nouveau_bo.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nv_push.h"

#include <inttypes.h>
#include <sys/mman.h>

#include "nvtypes.h"
#include "nvk_cl902d.h"

static VkResult
zero_vram(struct nvk_device *dev, struct nouveau_ws_bo *bo)
{
   uint32_t push_data[256];
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data));
   struct nv_push *p = &push;

   uint64_t addr = bo->offset;

   /* can't go higher for whatever reason */
   uint32_t pitch = 1 << 19;

   P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);

   P_MTHD(p, NV902D, SET_DST_FORMAT);
   P_NV902D_SET_DST_FORMAT(p, V_A8B8G8R8);
   P_NV902D_SET_DST_MEMORY_LAYOUT(p, V_PITCH);

   P_MTHD(p, NV902D, SET_DST_PITCH);
   P_NV902D_SET_DST_PITCH(p, pitch);

   P_MTHD(p, NV902D, SET_DST_OFFSET_UPPER);
   P_NV902D_SET_DST_OFFSET_UPPER(p, addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(p, addr & 0xffffffff);

   P_MTHD(p, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT(p, V_A8B8G8R8);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR(p, 0);

   uint32_t height = bo->size / pitch;
   uint32_t extra = bo->size % pitch;

   if (height > 0) {
      P_IMMD(p, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

      P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 1, pitch / 4);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 1, height);
   }

   P_IMMD(p, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

   P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 0, 0);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 0, height);
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 1, extra / 4);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 1, height);

   return nvk_queue_submit_simple(&dev->queue, nv_push_dw_count(&push),
                                  push_data, 1, &bo, false /* sync */);
}

VkResult
nvk_allocate_memory(struct nvk_device *dev,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const struct nvk_memory_tiling_info *tile_info,
                    const VkAllocationCallbacks *pAllocator,
                    struct nvk_device_memory **mem_out)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkMemoryType *type = &pdev->mem_types[pAllocateInfo->memoryTypeIndex];
   struct nvk_device_memory *mem;

   mem = vk_device_memory_create(&dev->vk, pAllocateInfo,
                                 pAllocator, sizeof(*mem));
   if (!mem)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   enum nouveau_ws_bo_flags flags;
   if (type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      flags = NOUVEAU_WS_BO_LOCAL;
   else
      flags = NOUVEAU_WS_BO_GART;

   if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      flags |= NOUVEAU_WS_BO_MAP;

   mem->map = NULL;
   if (tile_info) {
      mem->bo = nouveau_ws_bo_new_tiled(pdev->dev,
                                        pAllocateInfo->allocationSize, 0,
                                        tile_info->pte_kind,
                                        tile_info->tile_mode,
                                        flags);
   } else {
      mem->bo = nouveau_ws_bo_new(pdev->dev,
                                  pAllocateInfo->allocationSize, 0, flags);
   }
   if (!mem->bo) {
      vk_object_free(&dev->vk, pAllocator, mem);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   VkResult result;
   if (pdev->dev->debug_flags & NVK_DEBUG_ZERO_MEMORY) {
      if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         void *map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
         if (map == NULL) {
            result = vk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "Memory map failed");
            goto fail_bo;
         }
         memset(map, 0, mem->bo->size);
         nouveau_ws_bo_unmap(mem->bo, map);
      } else {
         result = zero_vram(dev, mem->bo);
         if (result != VK_SUCCESS)
            goto fail_bo;
      }
   }

   pthread_mutex_lock(&dev->mutex);
   list_addtail(&mem->link, &dev->memory_objects);
   pthread_mutex_unlock(&dev->mutex);

   *mem_out = mem;

   return VK_SUCCESS;

fail_bo:
   nouveau_ws_bo_destroy(mem->bo);
   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
   return result;
}

void
nvk_free_memory(struct nvk_device *dev,
                struct nvk_device_memory *mem,
                const VkAllocationCallbacks *pAllocator)
{
   if (mem->map)
      nouveau_ws_bo_unmap(mem->bo, mem->map);

   pthread_mutex_lock(&dev->mutex);
   list_del(&mem->link);
   pthread_mutex_unlock(&dev->mutex);

   nouveau_ws_bo_destroy(mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateMemory(VkDevice device,
                   const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_device_memory *mem;
   VkResult result;

   result = nvk_allocate_memory(dev, pAllocateInfo, NULL, pAllocator, &mem);
   if (result != VK_SUCCESS)
      return result;

   *pMem = nvk_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeMemory(VkDevice device,
               VkDeviceMemory _mem,
               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   if (!mem)
      return;

   nvk_free_memory(dev, mem, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_MapMemory2KHR(VkDevice device,
                  const VkMemoryMapInfoKHR *pMemoryMapInfo,
                  void **ppData)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size =
      vk_device_memory_range(&mem->vk, pMemoryMapInfo->offset,
                                       pMemoryMapInfo->size);

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
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%"PRIx64" does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   mem->map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
   if (mem->map == NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object couldn't be mapped.");
   }

   *ppData = mem->map + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_UnmapMemory2KHR(VkDevice device,
                    const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (mem == NULL)
      return VK_SUCCESS;

   nouveau_ws_bo_unmap(mem->bo, mem->map);
   mem->map = NULL;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FlushMappedMemoryRanges(VkDevice device,
                            uint32_t memoryRangeCount,
                            const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_InvalidateMappedMemoryRanges(VkDevice device,
                                 uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDeviceMemoryCommitment(VkDevice device,
                              VkDeviceMemory _mem,
                              VkDeviceSize* pCommittedMemoryInBytes)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   *pCommittedMemoryInBytes = mem->bo->size;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetMemoryFdKHR(VkDevice device,
                   const VkMemoryGetFdInfoKHR *pGetFdInfo,
                   int *pFD)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, memory, pGetFdInfo->memory);

   switch (pGetFdInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      if (nouveau_ws_bo_dma_buf(memory->bo, pFD))
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return VK_SUCCESS;
   default:
      assert(!"unsupported handle type");
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   }
}

VKAPI_ATTR uint64_t VKAPI_CALL
nvk_GetDeviceMemoryOpaqueCaptureAddress(
   UNUSED VkDevice device,
   const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, pInfo->memory);

   return mem->bo->offset;
}

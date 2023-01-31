#include "nvk_device_memory.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"

#include <inttypes.h>
#include <sys/mman.h>

#include "nvtypes.h"
#include "nvk_cl902d.h"

static VkResult
zero_vram(struct nvk_device *dev, struct nouveau_ws_bo *bo)
{
   struct nouveau_ws_push *push = nouveau_ws_push_new(dev->pdev->dev, 4096);
   if (push == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   nouveau_ws_push_ref(push, bo, NOUVEAU_WS_BO_WR);
   uint64_t addr = bo->offset;

   /* can't go higher for whatever reason */
   uint32_t pitch = 1 << 19;

   P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY);

   P_MTHD(push, NV902D, SET_DST_FORMAT);
   P_NV902D_SET_DST_FORMAT(push, V_A8B8G8R8);
   P_NV902D_SET_DST_MEMORY_LAYOUT(push, V_PITCH);

   P_MTHD(push, NV902D, SET_DST_PITCH);
   P_NV902D_SET_DST_PITCH(push, pitch);

   P_MTHD(push, NV902D, SET_DST_OFFSET_UPPER);
   P_NV902D_SET_DST_OFFSET_UPPER(push, addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(push, addr & 0xffffffff);

   P_MTHD(push, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT(push, V_A8B8G8R8);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR(push, 0);

   uint32_t height = bo->size / pitch;
   uint32_t extra = bo->size % pitch;

   if (height > 0) {
      P_IMMD(push, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

      P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, pitch / 4);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, height);
   }

   P_IMMD(push, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

   P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, 0);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, height);
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, extra / 4);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, height);

   nouveau_ws_push_submit(push, dev->pdev->dev, dev->ctx);
   nouveau_ws_push_destroy(push);

   return VK_SUCCESS;
}

VkResult
nvk_allocate_memory(struct nvk_device *device,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const struct nvk_memory_tiling_info *tile_info,
                    const VkAllocationCallbacks *pAllocator,
                    struct nvk_device_memory **mem_out)
{
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
   if (tile_info) {
      mem->bo = nouveau_ws_bo_new_tiled(device->pdev->dev,
                                        pAllocateInfo->allocationSize, 0,
                                        tile_info->pte_kind,
                                        tile_info->tile_mode,
                                        flags);
   } else {
      mem->bo = nouveau_ws_bo_new(device->pdev->dev,
                                  pAllocateInfo->allocationSize, 0, flags);
   }
   if (!mem->bo) {
      vk_object_free(&device->vk, pAllocator, mem);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   VkResult result;
   if (device->pdev->dev->debug_flags & NVK_DEBUG_ZERO_MEMORY) {
      if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         void *map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
         if (map == NULL) {
            result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "Memory map failed");
            goto fail_bo;
         }
         memset(map, 0, mem->bo->size);
         munmap(map, mem->bo->size);
      } else {
         VkResult result = zero_vram(device, mem->bo);
         if (result != VK_SUCCESS)
            goto fail_bo;
      }
   }

   simple_mtx_lock(&device->memory_objects_lock);
   list_addtail(&mem->link, &device->memory_objects);
   simple_mtx_unlock(&device->memory_objects_lock);

   *mem_out = mem;

   return VK_SUCCESS;

fail_bo:
   nouveau_ws_bo_destroy(mem->bo);
   vk_object_free(&device->vk, pAllocator, mem);
   return result;
}

void
nvk_free_memory(struct nvk_device *device,
                struct nvk_device_memory *mem,
                const VkAllocationCallbacks *pAllocator)
{
   if (mem->map)
      munmap(mem->map, mem->bo->size);

   simple_mtx_lock(&device->memory_objects_lock);
   list_del(&mem->link);
   simple_mtx_unlock(&device->memory_objects_lock);

   nouveau_ws_bo_destroy(mem->bo);

   vk_object_free(&device->vk, pAllocator, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateMemory(
   VkDevice _device,
   const VkMemoryAllocateInfo *pAllocateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_device_memory *mem;
   VkResult result;

   result = nvk_allocate_memory(device, pAllocateInfo, NULL, pAllocator, &mem);
   if (result != VK_SUCCESS)
      return result;

   *pMem = nvk_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeMemory(VkDevice _device,
               VkDeviceMemory _mem,
               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   if (!mem)
      return;

   nvk_free_memory(device, mem, pAllocator);
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

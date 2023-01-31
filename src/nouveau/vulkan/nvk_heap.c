#include "nvk_heap.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_queue.h"

#include "util/macros.h"

#include "nv_push.h"
#include "nvk_cl90b5.h"

VkResult
nvk_heap_init(struct nvk_device *device, struct nvk_heap *heap,
              enum nouveau_ws_bo_flags bo_flags,
              enum nouveau_ws_bo_map_flags map_flags,
              uint32_t overalloc)
{
   memset(heap, 0, sizeof(*heap));

   heap->bo_flags = bo_flags;
   if (map_flags)
      heap->bo_flags |= NOUVEAU_WS_BO_MAP;
   heap->map_flags = map_flags;
   heap->overalloc = overalloc;

   simple_mtx_init(&heap->mutex, mtx_plain);
   util_vma_heap_init(&heap->heap, 0, 0);

   heap->total_size = 0;
   heap->bo_count = 0;

   return VK_SUCCESS;
}

void
nvk_heap_finish(struct nvk_device *dev, struct nvk_heap *heap)
{
   for (uint32_t bo_idx = 0; bo_idx < heap->bo_count; bo_idx++) {
      nouveau_ws_bo_unmap(heap->bos[bo_idx].bo, heap->bos[bo_idx].map);
      nouveau_ws_bo_destroy(heap->bos[bo_idx].bo);
   }

   util_vma_heap_finish(&heap->heap);
   simple_mtx_destroy(&heap->mutex);
}

static uint64_t
encode_vma(uint32_t bo_idx, uint64_t bo_offset)
{
   assert(bo_idx < UINT16_MAX - 1);
   assert(bo_offset < (1ull << 48));
   return ((uint64_t)(bo_idx + 1) << 48) | bo_offset;
}

static uint32_t
vma_bo_idx(uint64_t offset)
{
   offset = offset >> 48;
   assert(offset > 0);
   return offset - 1;
}

static uint64_t
vma_bo_offset(uint64_t offset)
{
   return offset & BITFIELD64_MASK(48);
}

static VkResult
nvk_heap_grow_locked(struct nvk_device *dev, struct nvk_heap *heap)
{
   if (heap->bo_count >= NVK_HEAP_MAX_BO_COUNT) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Heap has already hit its maximum size");
   }

   /* First two BOs are MIN_SIZE, double after that */
   const uint64_t new_bo_size =
      NVK_HEAP_MIN_SIZE << (MAX2(heap->bo_count, 1) - 1);

   heap->bos[heap->bo_count].bo =
      nouveau_ws_bo_new_mapped(dev->pdev->dev,
                               new_bo_size + heap->overalloc, 0,
                               heap->bo_flags, heap->map_flags,
                               &heap->bos[heap->bo_count].map);
   if (heap->bos[heap->bo_count].bo == NULL) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate a heap BO: %m");
   }

   uint64_t vma = encode_vma(heap->bo_count, 0);
   util_vma_heap_free(&heap->heap, vma, new_bo_size);

   heap->total_size += new_bo_size;
   heap->bo_count++;

   return VK_SUCCESS;
}

static VkResult
nvk_heap_alloc_locked(struct nvk_device *dev, struct nvk_heap *heap,
                      uint64_t size, uint32_t alignment,
                      uint64_t *addr_out, void **map_out)
{
   while (1) {
      uint64_t vma = util_vma_heap_alloc(&heap->heap, size, alignment);
      if (vma != 0) {
         uint32_t bo_idx = vma_bo_idx(vma);
         uint64_t bo_offset = vma_bo_offset(vma);

         assert(bo_idx < heap->bo_count);
         assert(heap->bos[bo_idx].bo != NULL);
         assert(bo_offset + size + heap->overalloc <=
                heap->bos[bo_idx].bo->size);

         *addr_out = heap->bos[bo_idx].bo->offset + bo_offset;
         *map_out = (char *)heap->bos[bo_idx].map + bo_offset;

         return VK_SUCCESS;
      }

      VkResult result = nvk_heap_grow_locked(dev, heap);
      if (result != VK_SUCCESS)
         return result;
   }
}

static void
nvk_heap_free_locked(struct nvk_device *dev, struct nvk_heap *heap,
                     uint64_t addr, uint64_t size)
{
   assert(addr + size > addr);

   for (uint32_t bo_idx = 0; bo_idx < heap->bo_count; bo_idx++) {
      if (addr < heap->bos[bo_idx].bo->offset)
         continue;

      uint64_t bo_offset = addr - heap->bos[bo_idx].bo->offset;
      if (bo_offset >= heap->bos[bo_idx].bo->size)
         continue;

      assert(bo_offset + size <= heap->bos[bo_idx].bo->size);
      uint64_t vma = encode_vma(bo_idx, bo_offset);

      util_vma_heap_free(&heap->heap, vma, size);

      break;
   }
}

VkResult
nvk_heap_alloc(struct nvk_device *dev, struct nvk_heap *heap,
               uint64_t size, uint32_t alignment,
               uint64_t *addr_out, void **map_out)
{
   simple_mtx_lock(&heap->mutex);
   VkResult result = nvk_heap_alloc_locked(dev, heap, size, alignment,
                                           addr_out, map_out);
   simple_mtx_unlock(&heap->mutex);

   return result;
}

VkResult
nvk_heap_upload(struct nvk_device *dev, struct nvk_heap *heap,
                const void *data, size_t size, uint32_t alignment,
                uint64_t *addr_out)
{
   simple_mtx_lock(&heap->mutex);

   void *map;
   VkResult result = nvk_heap_alloc_locked(dev, heap, size, alignment,
                                           addr_out, &map);
   if (result == VK_SUCCESS)
      memcpy(map, data, size);
   simple_mtx_unlock(&heap->mutex);

   return result;
}

void
nvk_heap_free(struct nvk_device *dev, struct nvk_heap *heap,
              uint64_t addr, uint64_t size)
{
   simple_mtx_lock(&heap->mutex);
   nvk_heap_free_locked(dev, heap, addr, size);
   simple_mtx_unlock(&heap->mutex);
}

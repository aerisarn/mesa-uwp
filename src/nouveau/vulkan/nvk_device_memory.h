#ifndef NVK_MEMORY_H
#define NVK_MEMORY_H 1

#include "nvk_private.h"

#include "util/list.h"

struct nvk_device;

struct nvk_device_memory {
   struct vk_object_base base;

   struct list_head link;

   struct nouveau_ws_bo *bo;

   void *map;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_device_memory, base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

struct nvk_memory_tiling_info {
   uint16_t tile_mode;
   uint8_t pte_kind;
};

VkResult nvk_allocate_memory(struct nvk_device *device,
                             const VkMemoryAllocateInfo *pAllocateInfo,
                             const struct nvk_memory_tiling_info *tile_info,
                             const VkAllocationCallbacks *pAllocator,
                             struct nvk_device_memory **mem_out);

void nvk_free_memory(struct nvk_device *device,
                     struct nvk_device_memory *mem,
                     const VkAllocationCallbacks *pAllocator);

#endif

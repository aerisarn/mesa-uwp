#ifndef NVK_MEMORY_H
#define NVK_MEMORY_H 1

#include "nvk_private.h"

#include "vk_device_memory.h"

#include "util/list.h"

struct nvk_device;
struct nvk_image_plane;

struct nvk_device_memory {
   struct vk_device_memory vk;

   struct list_head link;

   struct nvk_image_plane *dedicated_image_plane;

   struct nouveau_ws_bo *bo;

   void *map;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_device_memory, vk.base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

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

extern const VkExternalMemoryProperties nvk_opaque_fd_mem_props;
extern const VkExternalMemoryProperties nvk_dma_buf_mem_props;

#endif

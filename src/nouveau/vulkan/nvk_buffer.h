#ifndef NVK_BUFFER_H
#define NVK_BUFFER_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"
#include "vulkan/runtime/vk_buffer.h"

struct nvk_device_memory;

struct nvk_buffer {
   struct vk_buffer vk;
   struct nvk_device_memory *mem;
   VkDeviceSize offset;
};

VK_DEFINE_HANDLE_CASTS(nvk_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

static void
nvk_push_buffer_ref(struct nouveau_ws_push *push,
                    const struct nvk_buffer *buffer,
                    enum nouveau_ws_bo_map_flags flags)
{
   nouveau_ws_push_ref(push, buffer->mem->bo, flags);
}

static inline uint64_t
nvk_buffer_address(struct nvk_buffer *buffer, uint64_t offset)
{
   return buffer->mem->bo->offset + buffer->offset + offset;
}

#endif

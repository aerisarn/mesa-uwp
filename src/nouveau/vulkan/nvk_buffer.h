#ifndef NVK_BUFFER
#define NVK_BUFFER 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_buffer.h"

struct nvk_buffer {
   struct vk_buffer vk;
};

VK_DEFINE_HANDLE_CASTS(nvk_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

#endif

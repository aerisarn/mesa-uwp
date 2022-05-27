#ifndef NVK_MEMORY_H
#define NVK_MEMORY_H 1

#include "nvk_private.h"

struct nvk_device_memory {
   struct vk_object_base base;

   struct nouveau_ws_bo *bo;

   void *map;
};

VK_DEFINE_HANDLE_CASTS(nvk_device_memory, base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

#endif

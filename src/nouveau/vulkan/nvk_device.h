#ifndef NVK_DEVICE_H
#define NVK_DEVICE_H 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_device.h"
#include "vulkan/runtime/vk_queue.h"

struct nvk_physical_device;
struct nvk_device {
   struct vk_device vk;
   struct nvk_physical_device *pdev;

   struct vk_queue queue;
};

VK_DEFINE_HANDLE_CASTS(nvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)
#endif

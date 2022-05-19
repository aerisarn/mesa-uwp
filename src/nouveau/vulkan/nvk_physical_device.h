#ifndef NVK_PHYSICAL_DEVICE
#define NVK_PHYSICAL_DEVICE 1

#include "nvk_private.h"

#include "nouveau_device.h"

#include "vulkan/runtime/vk_physical_device.h"

struct nvk_instance;

struct nvk_physical_device {
   struct vk_physical_device vk;
   struct nvk_instance *instance;
   struct nouveau_ws_device *dev;

   /* Link in nvk_instance::physical_devices */
   struct list_head link;
};

VK_DEFINE_HANDLE_CASTS(nvk_physical_device,
   vk.base,
   VkPhysicalDevice,
   VK_OBJECT_TYPE_PHYSICAL_DEVICE)

void nvk_physical_device_destroy(struct nvk_physical_device *);

#endif

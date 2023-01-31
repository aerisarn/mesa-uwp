#ifndef NVK_IMAGE
#define NVK_IMAGE 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_image.h"

struct nvk_image {
   struct vk_image vk;
};

VK_DEFINE_HANDLE_CASTS(nvk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

#endif

#ifndef NVK_IMAGE_VIEW_H
#define NVK_IMAGE_VIEW_H 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_image.h"

struct nvk_image_view {
   struct vk_image_view vk;

   /** Index in the image descriptor table */
   uint32_t desc_idx;
};

VK_DEFINE_HANDLE_CASTS(nvk_image_view, vk.base, VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW)

#endif

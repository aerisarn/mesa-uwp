#ifndef NVK_IMAGE_VIEW_H
#define NVK_IMAGE_VIEW_H 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_image.h"

struct nvk_device;

struct nvk_image_view {
   struct vk_image_view vk;

   /** Index in the image descriptor table for the sampled image descriptor */
   uint32_t sampled_desc_index;

   /** Index in the image descriptor table for the storage image descriptor */
   uint32_t storage_desc_index;
};

VK_DEFINE_HANDLE_CASTS(nvk_image_view, vk.base, VkImageView,
                       VK_OBJECT_TYPE_IMAGE_VIEW)

VkResult nvk_image_view_init(struct nvk_device *device,
                             struct nvk_image_view *view,
                             bool driver_internal,
                             const VkImageViewCreateInfo *pCreateInfo);

void nvk_image_view_finish(struct nvk_device *device,
                           struct nvk_image_view *view);

#endif

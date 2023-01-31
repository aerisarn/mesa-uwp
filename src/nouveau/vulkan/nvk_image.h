#ifndef NVK_IMAGE_H
#define NVK_IMAGE_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "nil_image.h"
#include "nouveau_bo.h"
#include "nouveau_push.h"
#include "vulkan/runtime/vk_image.h"

struct nvk_physical_device;

VkFormatFeatureFlags2
nvk_get_image_format_features(struct nvk_physical_device *pdevice,
                              VkFormat format, VkImageTiling tiling);

struct nvk_image {
   struct vk_image vk;
   struct nvk_device_memory *mem;
   VkDeviceSize offset;

   struct nil_image nil;
};

VK_DEFINE_HANDLE_CASTS(nvk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

static void
nvk_push_image_ref(struct nouveau_ws_push *push,
                   const struct nvk_image *image,
                   enum nouveau_ws_bo_map_flags flags)
{
   nouveau_ws_push_ref(push, image->mem->bo, flags);
}

static inline uint64_t
nvk_image_base_address(const struct nvk_image *image)
{
   return image->mem->bo->offset + image->offset;
}

#endif

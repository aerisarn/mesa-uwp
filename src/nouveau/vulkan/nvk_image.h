#ifndef NVK_IMAGE_H
#define NVK_IMAGE_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "nil_image.h"
#include "nouveau_bo.h"
#include "vulkan/runtime/vk_image.h"

struct nvk_physical_device;

VkFormatFeatureFlags2
nvk_get_image_format_features(struct nvk_physical_device *pdevice,
                              VkFormat format, VkImageTiling tiling);


struct nvk_image_plane {
   struct nil_image nil;
   struct nvk_device_memory *mem;
   VkDeviceSize offset;

   /* Used for internal dedicated allocations */
   struct nvk_device_memory *internal;
};
struct nvk_image {
   struct vk_image vk;

   /** True if the planes are bound separately
    *
    * This is set based on VK_IMAGE_CREATE_DISJOINT_BIT
    */
   bool disjoint;

   uint8_t plane_count;
   struct nvk_image_plane planes[3];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

static inline uint64_t
nvk_image_base_address(const struct nvk_image *image, uint8_t plane)
{
   return image->planes[plane].mem->bo->offset + image->planes[plane].offset;
}

static inline uint8_t
nvk_image_aspects_to_plane(ASSERTED const struct nvk_image *image,
                           VkImageAspectFlags aspectMask)
{
   /* Verify that the aspects are actually in the image */
   assert(!(aspectMask & ~image->vk.aspects));

   /* Must only be one aspect unless it's depth/stencil */
   assert(aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT |
                         VK_IMAGE_ASPECT_STENCIL_BIT) ||
          util_bitcount(aspectMask) == 1);

   switch(aspectMask) {
   case VK_IMAGE_ASPECT_PLANE_1_BIT: return 1;
   case VK_IMAGE_ASPECT_PLANE_2_BIT: return 2;
   default: return 0;
   }
}

#endif

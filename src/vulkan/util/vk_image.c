/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_image.h"

#include <vulkan/vulkan_android.h>

#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_util.h"

static VkExtent3D
sanitize_image_extent(const VkImageType imageType,
                      const VkExtent3D imageExtent)
{
   switch (imageType) {
   case VK_IMAGE_TYPE_1D:
      return (VkExtent3D) { imageExtent.width, 1, 1 };
   case VK_IMAGE_TYPE_2D:
      return (VkExtent3D) { imageExtent.width, imageExtent.height, 1 };
   case VK_IMAGE_TYPE_3D:
      return imageExtent;
   default:
      unreachable("invalid image type");
   }
}

void
vk_image_init(struct vk_device *device,
              struct vk_image *image,
              const VkImageCreateInfo *pCreateInfo)
{
   vk_object_base_init(device, &image->base, VK_OBJECT_TYPE_IMAGE);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
   assert(pCreateInfo->mipLevels > 0);
   assert(pCreateInfo->arrayLayers > 0);
   assert(pCreateInfo->samples > 0);
   assert(pCreateInfo->extent.width > 0);
   assert(pCreateInfo->extent.height > 0);
   assert(pCreateInfo->extent.depth > 0);

   if (pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      assert(pCreateInfo->imageType == VK_IMAGE_TYPE_2D);
   if (pCreateInfo->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)
      assert(pCreateInfo->imageType == VK_IMAGE_TYPE_3D);

   image->create_flags = pCreateInfo->flags;
   image->image_type = pCreateInfo->imageType;
   vk_image_set_format(image, pCreateInfo->format);
   image->extent = sanitize_image_extent(pCreateInfo->imageType,
                                         pCreateInfo->extent);
   image->mip_levels = pCreateInfo->mipLevels;
   image->array_layers = pCreateInfo->arrayLayers;
   image->samples = pCreateInfo->samples;
   image->tiling = pCreateInfo->tiling;
   image->usage = pCreateInfo->usage;

   if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      const VkImageStencilUsageCreateInfoEXT *stencil_usage_info =
         vk_find_struct_const(pCreateInfo->pNext,
                              IMAGE_STENCIL_USAGE_CREATE_INFO_EXT);
      image->stencil_usage =
         stencil_usage_info ? stencil_usage_info->stencilUsage :
                              pCreateInfo->usage;
   } else {
      image->stencil_usage = 0;
   }

   const VkExternalMemoryImageCreateInfo *ext_mem_info =
      vk_find_struct_const(pCreateInfo->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   if (ext_mem_info)
      image->external_handle_types = ext_mem_info->handleTypes;
   else
      image->external_handle_types = 0;

#ifdef ANDROID
   const VkExternalFormatANDROID *ext_format =
      vk_find_struct_const(pCreateInfo->pNext, EXTERNAL_FORMAT_ANDROID);
   if (ext_format && ext_format->externalFormat != 0) {
      assert(image->format == VK_FORMAT_UNDEFINED);
      assert(image->external_handle_types &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
      image->android_external_format = ext_format->externalFormat;
   } else {
      image->android_external_format = 0;
   }
#endif
}

void *
vk_image_create(struct vk_device *device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *alloc,
                size_t size)
{
   struct vk_image *image =
      vk_zalloc2(&device->alloc, alloc, size, 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL)
      return NULL;

   vk_image_init(device, image, pCreateInfo);

   return image;
}

void
vk_image_finish(struct vk_image *image)
{
   vk_object_base_finish(&image->base);
}

void
vk_image_destroy(struct vk_device *device,
                 const VkAllocationCallbacks *alloc,
                 struct vk_image *image)
{
   vk_object_free(device, alloc, image);
}

void
vk_image_set_format(struct vk_image *image, VkFormat format)
{
   image->format = format;
   image->aspects = vk_format_aspects(format);
}

VkImageUsageFlags
vk_image_usage(const struct vk_image *image,
               VkImageAspectFlags aspect_mask)
{
   assert(!(aspect_mask & ~image->aspects));

   /* From the Vulkan 1.2.131 spec:
    *
    *    "If the image was has a depth-stencil format and was created with
    *    a VkImageStencilUsageCreateInfo structure included in the pNext
    *    chain of VkImageCreateInfo, the usage is calculated based on the
    *    subresource.aspectMask provided:
    *
    *     - If aspectMask includes only VK_IMAGE_ASPECT_STENCIL_BIT, the
    *       implicit usage is equal to
    *       VkImageStencilUsageCreateInfo::stencilUsage.
    *
    *     - If aspectMask includes only VK_IMAGE_ASPECT_DEPTH_BIT, the
    *       implicit usage is equal to VkImageCreateInfo::usage.
    *
    *     - If both aspects are included in aspectMask, the implicit usage
    *       is equal to the intersection of VkImageCreateInfo::usage and
    *       VkImageStencilUsageCreateInfo::stencilUsage.
    */
   if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
      return image->stencil_usage;
   } else if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT |
                              VK_IMAGE_ASPECT_STENCIL_BIT)) {
      return image->usage & image->stencil_usage;
   } else {
      /* This also handles the color case */
      return image->usage;
   }
}

VkImageAspectFlags
vk_image_expand_aspect_mask(const struct vk_image *image,
                            VkImageAspectFlags aspect_mask)
{
   /* If the underlying image has color plane aspects and
    * VK_IMAGE_ASPECT_COLOR_BIT has been requested, then return the aspects of
    * the underlying image. */
   if (!(image->aspects & (VK_IMAGE_ASPECT_PLANE_0_BIT |
                           VK_IMAGE_ASPECT_PLANE_1_BIT |
                           VK_IMAGE_ASPECT_PLANE_2_BIT)) &&
       aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT)
      return image->aspects;

   return aspect_mask;
}

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
#ifndef VK_IMAGE_H
#define VK_IMAGE_H

#include "vk_object.h"

#include "util/u_math.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_image {
   struct vk_object_base base;

   VkImageCreateFlags create_flags;
   VkImageType image_type;
   VkFormat format;
   VkExtent3D extent;
   uint32_t mip_levels;
   uint32_t array_layers;
   VkSampleCountFlagBits samples;
   VkImageTiling tiling;
   VkImageUsageFlags usage;

   /* Derived from format */
   VkImageAspectFlags aspects;

   /* VK_EXT_separate_stencil_usage */
   VkImageUsageFlags stencil_usage;

   /* VK_KHR_external_memory */
   VkExternalMemoryHandleTypeFlags external_handle_types;

#ifdef ANDROID
   /* VK_ANDROID_external_memory_android_hardware_buffer */
   uint64_t android_external_format;
#endif
};

void vk_image_init(struct vk_device *device,
                   struct vk_image *image,
                   const VkImageCreateInfo *pCreateInfo);
void vk_image_finish(struct vk_image *image);

void *vk_image_create(struct vk_device *device,
                      const VkImageCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *alloc,
                      size_t size);
void vk_image_destroy(struct vk_device *device,
                      const VkAllocationCallbacks *alloc,
                      struct vk_image *image);

void vk_image_set_format(struct vk_image *image, VkFormat format);

VkImageUsageFlags vk_image_usage(const struct vk_image *image,
                                 VkImageAspectFlags aspect_mask);

static inline VkExtent3D
vk_image_mip_level_extent(const struct vk_image *image,
                          uint32_t mip_level)
{
   const VkExtent3D extent = {
      u_minify(image->extent.width,  mip_level),
      u_minify(image->extent.height, mip_level),
      u_minify(image->extent.depth,  mip_level),
   };
   return extent;
}

#ifdef __cplusplus
}
#endif

#endif /* VK_IMAGE_H */

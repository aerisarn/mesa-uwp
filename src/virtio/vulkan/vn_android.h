/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_ANDROID_H
#define VN_ANDROID_H

#include "vn_common.h"

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

/* venus implements VK_ANDROID_native_buffer up to spec version 7 */
#define VN_ANDROID_NATIVE_BUFFER_SPEC_VERSION 7

struct vn_android_wsi {
   /* command pools, one per queue family */
   VkCommandPool *cmd_pools;
   /* use one lock to simplify */
   mtx_t cmd_pools_lock;
   /* for forcing VK_SHARING_MODE_CONCURRENT */
   uint32_t *queue_family_indices;
};

#ifdef ANDROID

VkResult
vn_android_wsi_init(struct vn_device *dev,
                    const VkAllocationCallbacks *alloc);

void
vn_android_wsi_fini(struct vn_device *dev,
                    const VkAllocationCallbacks *alloc);

static inline const VkNativeBufferANDROID *
vn_android_find_native_buffer(const VkImageCreateInfo *create_info)
{
   return vk_find_struct_const(create_info->pNext, NATIVE_BUFFER_ANDROID);
}

VkResult
vn_android_image_from_anb(struct vn_device *dev,
                          const VkImageCreateInfo *image_info,
                          const VkNativeBufferANDROID *anb_info,
                          const VkAllocationCallbacks *alloc,
                          struct vn_image **out_img);

bool
vn_android_get_drm_format_modifier_info(
   const VkPhysicalDeviceImageFormatInfo2 *format_info,
   VkPhysicalDeviceImageDrmFormatModifierInfoEXT *out_info);

uint64_t
vn_android_get_ahb_usage(const VkImageUsageFlags usage,
                         const VkImageCreateFlags flags);

#else

static inline VkResult
vn_android_wsi_init(UNUSED struct vn_device *dev,
                    UNUSED const VkAllocationCallbacks *alloc)
{
   return VK_SUCCESS;
}

static inline void
vn_android_wsi_fini(UNUSED struct vn_device *dev,
                    UNUSED const VkAllocationCallbacks *alloc)
{
   return;
}

static inline const VkNativeBufferANDROID *
vn_android_find_native_buffer(UNUSED const VkImageCreateInfo *create_info)
{
   return NULL;
}

static inline VkResult
vn_android_image_from_anb(UNUSED struct vn_device *dev,
                          UNUSED const VkImageCreateInfo *image_info,
                          UNUSED const VkNativeBufferANDROID *anb_info,
                          UNUSED const VkAllocationCallbacks *alloc,
                          UNUSED struct vn_image **out_img)
{
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static inline bool
vn_android_get_drm_format_modifier_info(
   UNUSED const VkPhysicalDeviceImageFormatInfo2 *format_info,
   UNUSED VkPhysicalDeviceImageDrmFormatModifierInfoEXT *out_info)
{
   return false;
}

static inline uint64_t
vn_android_get_ahb_usage(UNUSED const VkImageUsageFlags usage,
                         UNUSED const VkImageCreateFlags flags)
{
   return 0;
}

#endif /* ANDROID */

#endif /* VN_ANDROID_H */

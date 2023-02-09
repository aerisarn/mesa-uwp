/*
 * Copyright © 2023 Intel Corporation
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

#include <sys/mman.h>

#include "anv_private.h"

#include "drm-uapi/xe_drm.h"

static uint32_t
xe_gem_create(struct anv_device *device,
              const struct intel_memory_class_instance **regions,
              uint16_t regions_count, uint64_t size,
              enum anv_bo_alloc_flags alloc_flags)
{
   struct drm_xe_gem_create gem_create = {
     .vm_id = device->vm_id,
     .size = size,
   };
   for (uint16_t i = 0; i < regions_count; i++)
      gem_create.flags |= BITFIELD_BIT(regions[i]->instance);

   if (intel_ioctl(device->fd, DRM_IOCTL_XE_GEM_CREATE, &gem_create))
      return 0;

   return gem_create.handle;
}

static void
xe_gem_close(struct anv_device *device, uint32_t handle)
{
   struct drm_gem_close close = {
      .handle = handle,
   };
   intel_ioctl(device->fd, DRM_IOCTL_GEM_CLOSE, &close);
}

static void *
xe_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,
            uint64_t size, VkMemoryPropertyFlags property_flags)
{
   struct drm_xe_gem_mmap_offset args = {
      .handle = bo->gem_handle,
   };
   if (intel_ioctl(device->fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &args))
      return MAP_FAILED;

   return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
               device->fd, args.offset);
}

const struct anv_kmd_backend *
anv_xe_kmd_backend_get(void)
{
   static const struct anv_kmd_backend xe_backend = {
      .gem_create = xe_gem_create,
      .gem_close = xe_gem_close,
      .gem_mmap = xe_gem_mmap,
   };
   return &xe_backend;
}

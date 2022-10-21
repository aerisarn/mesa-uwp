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
#include "iris/iris_kmd_backend.h"

#include <sys/mman.h>

#include "common/intel_gem.h"
#include "dev/intel_debug.h"

#include "drm-uapi/i915_drm.h"

#include "iris/iris_bufmgr.h"
#include "iris_batch.h"

#define FILE_DEBUG_FLAG DEBUG_BUFMGR

static uint32_t
i915_gem_create(struct iris_bufmgr *bufmgr,
                const struct intel_memory_class_instance **regions,
                uint16_t regions_count, uint64_t size,
                enum iris_heap heap_flags, unsigned alloc_flags)
{
   if (unlikely(!iris_bufmgr_get_device_info(bufmgr)->mem.use_class_instance)) {
      struct drm_i915_gem_create create_legacy = { .size = size };

      assert(regions_count == 1 &&
             regions[0]->klass == I915_MEMORY_CLASS_SYSTEM);

      /* All new BOs we get from the kernel are zeroed, so we don't need to
       * worry about that here.
       */
      if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_I915_GEM_CREATE,
                      &create_legacy))
         return 0;

      return create_legacy.handle;
   }

   struct drm_i915_gem_memory_class_instance i915_regions[2];
   assert(regions_count <= ARRAY_SIZE(i915_regions));
   for (uint16_t i = 0; i < regions_count; i++) {
      i915_regions[i].memory_class = regions[i]->klass;
      i915_regions[i].memory_instance = regions[i]->instance;
   }

   struct drm_i915_gem_create_ext create = {
      .size = size,
   };
   struct drm_i915_gem_create_ext_memory_regions ext_regions = {
      .base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
      .num_regions = regions_count,
      .regions = (uintptr_t)i915_regions,
   };
   intel_gem_add_ext(&create.extensions,
                     I915_GEM_CREATE_EXT_MEMORY_REGIONS,
                     &ext_regions.base);

   if (iris_bufmgr_vram_size(bufmgr) > 0 &&
       !intel_vram_all_mappable(iris_bufmgr_get_device_info(bufmgr)) &&
       heap_flags == IRIS_HEAP_DEVICE_LOCAL_PREFERRED)
      create.flags |= I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS;

   /* Protected param */
   struct drm_i915_gem_create_ext_protected_content protected_param = {
      .flags = 0,
   };
   if (alloc_flags & BO_ALLOC_PROTECTED) {
      intel_gem_add_ext(&create.extensions,
                        I915_GEM_CREATE_EXT_PROTECTED_CONTENT,
                        &protected_param.base);
   }

   if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_I915_GEM_CREATE_EXT,
                   &create))
      return 0;

   return create.handle;
}

static bool
i915_bo_madvise(struct iris_bo *bo, enum iris_madvice state)
{
   uint32_t i915_state = state == IRIS_MADVICE_WILL_NEED ?
                                  I915_MADV_WILLNEED : I915_MADV_DONTNEED;
   struct drm_i915_gem_madvise madv = {
      .handle = bo->gem_handle,
      .madv = i915_state,
      .retained = 1,
   };

   intel_ioctl(iris_bufmgr_get_fd(bo->bufmgr), DRM_IOCTL_I915_GEM_MADVISE, &madv);

   return madv.retained;
}

static int
i915_bo_set_caching(struct iris_bo *bo, bool cached)
{
   struct drm_i915_gem_caching arg = {
      .handle = bo->gem_handle,
      .caching = cached ? I915_CACHING_CACHED : I915_CACHING_NONE,
   };
   return intel_ioctl(iris_bufmgr_get_fd(bo->bufmgr),
                      DRM_IOCTL_I915_GEM_SET_CACHING, &arg);
}

static void *
i915_gem_mmap_offset(struct iris_bufmgr *bufmgr, struct iris_bo *bo)
{
   struct drm_i915_gem_mmap_offset mmap_arg = {
      .handle = bo->gem_handle,
   };

   if (iris_bufmgr_get_device_info(bufmgr)->has_local_mem) {
      /* On discrete memory platforms, we cannot control the mmap caching mode
       * at mmap time.  Instead, it's fixed when the object is created (this
       * is a limitation of TTM).
       *
       * On DG1, our only currently enabled discrete platform, there is no
       * control over what mode we get.  For SMEM, we always get WB because
       * it's fast (probably what we want) and when the device views SMEM
       * across PCIe, it's always snooped.  The only caching mode allowed by
       * DG1 hardware for LMEM is WC.
       */
      if (bo->real.heap != IRIS_HEAP_SYSTEM_MEMORY)
         assert(bo->real.mmap_mode == IRIS_MMAP_WC);
      else
         assert(bo->real.mmap_mode == IRIS_MMAP_WB);

      mmap_arg.flags = I915_MMAP_OFFSET_FIXED;
   } else {
      /* Only integrated platforms get to select a mmap caching mode here */
      static const uint32_t mmap_offset_for_mode[] = {
         [IRIS_MMAP_UC]    = I915_MMAP_OFFSET_UC,
         [IRIS_MMAP_WC]    = I915_MMAP_OFFSET_WC,
         [IRIS_MMAP_WB]    = I915_MMAP_OFFSET_WB,
      };
      assert(bo->real.mmap_mode != IRIS_MMAP_NONE);
      assert(bo->real.mmap_mode < ARRAY_SIZE(mmap_offset_for_mode));
      mmap_arg.flags = mmap_offset_for_mode[bo->real.mmap_mode];
   }

   /* Get the fake offset back */
   if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_I915_GEM_MMAP_OFFSET,
                   &mmap_arg)) {
      DBG("%s:%d: Error preparing buffer %d (%s): %s .\n",
          __FILE__, __LINE__, bo->gem_handle, bo->name, strerror(errno));
      return NULL;
   }

   /* And map it */
   void *map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    iris_bufmgr_get_fd(bufmgr), mmap_arg.offset);
   if (map == MAP_FAILED) {
      DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
          __FILE__, __LINE__, bo->gem_handle, bo->name, strerror(errno));
      return NULL;
   }

   return map;
}

static void *
i915_gem_mmap_legacy(struct iris_bufmgr *bufmgr, struct iris_bo *bo)
{
   assert(iris_bufmgr_vram_size(bufmgr) == 0);
   assert(bo->real.mmap_mode == IRIS_MMAP_WB ||
          bo->real.mmap_mode == IRIS_MMAP_WC);

   struct drm_i915_gem_mmap mmap_arg = {
      .handle = bo->gem_handle,
      .size = bo->size,
      .flags = bo->real.mmap_mode == IRIS_MMAP_WC ? I915_MMAP_WC : 0,
   };

   if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_I915_GEM_MMAP,
                   &mmap_arg)) {
      DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
          __FILE__, __LINE__, bo->gem_handle, bo->name, strerror(errno));
      return NULL;
   }

   return (void *)(uintptr_t) mmap_arg.addr_ptr;
}

static void *
i915_gem_mmap(struct iris_bufmgr *bufmgr, struct iris_bo *bo)
{
   assert(iris_bo_is_real(bo));

   if (likely(iris_bufmgr_get_device_info(bufmgr)->has_mmap_offset))
      return i915_gem_mmap_offset(bufmgr, bo);
   else
      return i915_gem_mmap_legacy(bufmgr, bo);
}

static enum pipe_reset_status
i915_batch_check_for_reset(struct iris_batch *batch)
{
   struct iris_screen *screen = batch->screen;
   enum pipe_reset_status status = PIPE_NO_RESET;
   struct drm_i915_reset_stats stats = { .ctx_id = batch->ctx_id };

   if (intel_ioctl(screen->fd, DRM_IOCTL_I915_GET_RESET_STATS, &stats))
      DBG("DRM_IOCTL_I915_GET_RESET_STATS failed: %s\n", strerror(errno));

   if (stats.batch_active != 0) {
      /* A reset was observed while a batch from this hardware context was
       * executing.  Assume that this context was at fault.
       */
      status = PIPE_GUILTY_CONTEXT_RESET;
   } else if (stats.batch_pending != 0) {
      /* A reset was observed while a batch from this context was in progress,
       * but the batch was not executing.  In this case, assume that the
       * context was not at fault.
       */
      status = PIPE_INNOCENT_CONTEXT_RESET;
   }

   return status;
}

const struct iris_kmd_backend *i915_get_backend(void)
{
   static const struct iris_kmd_backend i915_backend = {
      .gem_create = i915_gem_create,
      .bo_madvise = i915_bo_madvise,
      .bo_set_caching = i915_bo_set_caching,
      .gem_mmap = i915_gem_mmap,
      .batch_check_for_reset = i915_batch_check_for_reset,
   };
   return &i915_backend;
}

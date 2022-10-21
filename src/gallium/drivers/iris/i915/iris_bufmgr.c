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
#include "i915/iris_bufmgr.h"

#include "common/intel_gem.h"
#include "iris/iris_bufmgr.h"

#include "drm-uapi/i915_drm.h"

bool
iris_i915_bo_madvise(struct iris_bo *bo, enum iris_madvice state)
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

int iris_i915_bo_set_caching(struct iris_bo *bo, bool cached)
{
   struct drm_i915_gem_caching arg = {
      .handle = bo->gem_handle,
      .caching = cached ? I915_CACHING_CACHED : I915_CACHING_NONE,
   };
   return intel_ioctl(iris_bufmgr_get_fd(bo->bufmgr),
                      DRM_IOCTL_I915_GEM_SET_CACHING, &arg);
}

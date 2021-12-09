/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VN_RENDERER_UTIL_H
#define VN_RENDERER_UTIL_H

#include "vn_renderer.h"

static inline VkResult
vn_renderer_submit_simple(struct vn_renderer *renderer,
                          const void *cs_data,
                          size_t cs_size)
{
   const struct vn_renderer_submit submit = {
      .batches =
         &(const struct vn_renderer_submit_batch){
            .cs_data = cs_data,
            .cs_size = cs_size,
         },
      .batch_count = 1,
   };
   return vn_renderer_submit(renderer, &submit);
}

VkResult
vn_renderer_submit_simple_sync(struct vn_renderer *renderer,
                               const void *cs_data,
                               size_t cs_size);

#endif /* VN_RENDERER_UTIL_H */

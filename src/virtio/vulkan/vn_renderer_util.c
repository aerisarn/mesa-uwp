/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vn_renderer_util.h"

VkResult
vn_renderer_submit_simple_sync(struct vn_renderer *renderer,
                               const void *cs_data,
                               size_t cs_size)
{
   struct vn_renderer_sync *sync;
   VkResult result =
      vn_renderer_sync_create(renderer, 0, VN_RENDERER_SYNC_BINARY, &sync);
   if (result != VK_SUCCESS)
      return result;

   const struct vn_renderer_submit submit = {
      .batches =
         &(const struct vn_renderer_submit_batch){
            .cs_data = cs_data,
            .cs_size = cs_size,
            .sync_queue_cpu = true,
            .syncs = &sync,
            .sync_values = &(const uint64_t){ 1 },
            .sync_count = 1,
         },
      .batch_count = 1,
   };
   const struct vn_renderer_wait wait = {
      .timeout = UINT64_MAX,
      .syncs = &sync,
      .sync_values = &(const uint64_t){ 1 },
      .sync_count = 1,
   };

   result = vn_renderer_submit(renderer, &submit);
   if (result == VK_SUCCESS)
      result = vn_renderer_wait(renderer, &wait);

   vn_renderer_sync_destroy(renderer, sync);

   return result;
}

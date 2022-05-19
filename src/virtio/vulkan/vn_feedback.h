/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VN_FEEDBACK_H
#define VN_FEEDBACK_H

#include "vn_common.h"

struct vn_feedback_pool {
   struct vn_device *device;
   const VkAllocationCallbacks *alloc;

   /* size in bytes of the feedback buffer */
   uint32_t size;
   /* size in bytes used of the active feedback buffer */
   uint32_t used;

   /* first entry is the active feedback buffer */
   struct list_head feedback_buffers;
};

VkResult
vn_feedback_pool_init(struct vn_device *dev,
                      struct vn_feedback_pool *pool,
                      uint32_t size,
                      const VkAllocationCallbacks *alloc);

void
vn_feedback_pool_fini(struct vn_feedback_pool *pool);

#endif /* VN_FEEDBACK_H */

/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_device.h"
#include "tu_knl.h"


VkResult
tu_bo_init_new_explicit_iova(struct tu_device *dev,
                             struct tu_bo **out_bo,
                             uint64_t size,
                             uint64_t client_iova,
                             enum tu_bo_alloc_flags flags, const char *name)
{
   return dev->instance->knl->bo_init(dev, out_bo, size, client_iova, flags, name);
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo **bo,
                  uint64_t size,
                  int fd)
{
   return dev->instance->knl->bo_init_dmabuf(dev, bo, size, fd);
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   return dev->instance->knl->bo_export_dmabuf(dev, bo);
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   dev->instance->knl->bo_finish(dev, bo);
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo)
{
   return dev->instance->knl->bo_map(dev, bo);
}

void tu_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   dev->instance->knl->bo_allow_dump(dev, bo);
}

int
tu_device_get_gpu_timestamp(struct tu_device *dev,
                            uint64_t *ts)
{
   return dev->instance->knl->device_get_gpu_timestamp(dev, ts);
}

int
tu_device_get_suspend_count(struct tu_device *dev,
                            uint64_t *suspend_count)
{
   return dev->instance->knl->device_get_suspend_count(dev, suspend_count);
}

VkResult
tu_device_wait_u_trace(struct tu_device *dev, struct tu_u_trace_syncobj *syncobj)
{
   return dev->instance->knl->device_wait_u_trace(dev, syncobj);
}

VkResult
tu_device_check_status(struct vk_device *vk_device)
{
   struct tu_device *dev = container_of(vk_device, struct tu_device, vk);
   return dev->instance->knl->device_check_status(dev);
}

int
tu_drm_submitqueue_new(const struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   return dev->instance->knl->submitqueue_new(dev, priority, queue_id);
}

void
tu_drm_submitqueue_close(const struct tu_device *dev, uint32_t queue_id)
{
   dev->instance->knl->submitqueue_close(dev, queue_id);
}

VkResult
tu_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct tu_queue *queue = container_of(vk_queue, struct tu_queue, vk);
   return queue->device->instance->knl->queue_submit(queue, submit);
}

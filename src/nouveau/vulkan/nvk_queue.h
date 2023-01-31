#ifndef NVK_QUEUE_H
#define NVK_QUEUE_H 1

#include "nvk_private.h"

#include "vk_queue.h"

struct novueau_ws_bo;
struct novueau_ws_push;
struct nvk_device;

struct nvk_queue_state {
   struct {
      struct nouveau_ws_bo *bo;
      uint32_t alloc_count;
   } images;

   struct {
      struct nouveau_ws_bo *bo;
      uint32_t alloc_count;
   } samplers;

   struct {
      struct nouveau_ws_bo *bo;
      uint32_t bytes_per_warp;
      uint32_t bytes_per_mp;
   } slm;

   struct {
      struct nouveau_ws_bo *bo;
      uint32_t dw_count;
   } push;
};

VkResult nvk_queue_state_update(struct nvk_device *dev,
                                struct nvk_queue_state *qs);

struct nvk_queue {
   struct vk_queue vk;

   struct nvk_queue_state state;

   struct nouveau_ws_bo *empty_push;
   uint32_t empty_push_dw_count;
};

static inline struct nvk_device *
nvk_queue_device(struct nvk_queue *queue)
{
   return (struct nvk_device *)queue->vk.base.device;
}

VkResult nvk_queue_init(struct nvk_device *dev, struct nvk_queue *queue,
                        const VkDeviceQueueCreateInfo *pCreateInfo,
                        uint32_t index_in_family);

void nvk_queue_finish(struct nvk_device *dev, struct nvk_queue *queue);

VkResult nvk_queue_submit_simple(struct nvk_queue *queue,
                                 const uint32_t *dw, uint32_t dw_count,
                                 struct nouveau_ws_bo *extra_bo);

VkResult nvk_queue_submit_simple_drm_nouveau(struct nvk_queue *queue,
                                             struct nouveau_ws_bo *push_bo,
                                             uint32_t push_dw_count,
                                             struct nouveau_ws_bo *extra_bo);

VkResult nvk_queue_submit_drm_nouveau(struct vk_queue *vkqueue,
                                      struct vk_queue_submit *submit);

#endif

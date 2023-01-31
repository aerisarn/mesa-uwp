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

   struct nouveau_ws_push *push;
};

struct nvk_queue {
   struct vk_queue vk;

   struct nvk_queue_state state;

   struct nouveau_ws_push *empty_push;
};

VkResult nvk_queue_init(struct nvk_device *dev, struct nvk_queue *queue,
                        const VkDeviceQueueCreateInfo *pCreateInfo,
                        uint32_t index_in_family);

void nvk_queue_finish(struct nvk_device *dev, struct nvk_queue *queue);

#endif

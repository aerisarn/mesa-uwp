#include "nvk_queue.h"

#include "nvk_bo_sync.h"
#include "nvk_cmd_buffer.h"
#include "nvk_cmd_pool.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"

#include <nouveau_drm.h>
#include <xf86drm.h>

struct push_builder {
   struct nvk_device *dev;
   struct drm_nouveau_gem_pushbuf_bo req_bo[NOUVEAU_GEM_MAX_BUFFERS];
   struct drm_nouveau_gem_pushbuf_push req_push[NOUVEAU_GEM_MAX_PUSH];
   struct drm_nouveau_gem_pushbuf req;
};

static void
push_builder_init(struct nvk_device *dev, struct push_builder *pb)
{
   pb->dev = dev;
   pb->req = (struct drm_nouveau_gem_pushbuf) {
      .channel = dev->ctx->channel,
      .nr_buffers = 0,
      .buffers = (uintptr_t)&pb->req_bo,
      .nr_push = 0,
      .push = (uintptr_t)&pb->req_push,
   };
}

static uint32_t
push_add_bo(struct push_builder *pb,
            struct nouveau_ws_bo *bo,
            enum nouveau_ws_bo_map_flags flags)
{
   const uint32_t domain = (bo->flags & NOUVEAU_WS_BO_GART) ?
                           NOUVEAU_GEM_DOMAIN_GART :
                           pb->dev->pdev->dev->local_mem_domain;

   for (uint32_t i = 0; i < pb->req.nr_buffers; i++) {
      if (pb->req_bo[i].handle == bo->handle) {
         assert(pb->req_bo[i].valid_domains == domain);
         if (flags & NOUVEAU_WS_BO_RD)
            pb->req_bo[i].read_domains |= domain;
         if (flags & NOUVEAU_WS_BO_WR)
            pb->req_bo[i].write_domains |= domain;
         return i;
      }
   }

   assert(pb->req.nr_buffers < NOUVEAU_GEM_MAX_BUFFERS);
   const uint32_t i = pb->req.nr_buffers++;

   pb->req_bo[i] = (struct drm_nouveau_gem_pushbuf_bo) {
      .handle = bo->handle,
      .valid_domains = domain,
   };
   if (flags & NOUVEAU_WS_BO_RD)
      pb->req_bo[i].read_domains |= domain;
   if (flags & NOUVEAU_WS_BO_WR)
      pb->req_bo[i].write_domains |= domain;

   return i;
}

static void
push_add_push(struct push_builder *pb, struct nouveau_ws_bo *bo,
              uint32_t dw_offset, uint32_t dw_count)
{
   if (dw_count == 0)
      return;

   uint32_t bo_index = push_add_bo(pb, bo, NOUVEAU_WS_BO_RD);

   pb->req_push[pb->req.nr_push++] = (struct drm_nouveau_gem_pushbuf_push) {
      .bo_index = bo_index,
      .offset = dw_offset * 4,
      .length = dw_count * 4,
   };
}

static VkResult
push_submit(struct push_builder *pb, struct nvk_queue *queue)
{
   int ret = drmCommandWriteRead(pb->dev->pdev->dev->fd,
                                 DRM_NOUVEAU_GEM_PUSHBUF,
                                 &pb->req, sizeof(pb->req));
   if (ret != 0) {
      return vk_queue_set_lost(&queue->vk,
                               "DRM_NOUVEAU_GEM_PUSHBUF failed: %m");
   }

   return VK_SUCCESS;
}

VkResult
nvk_queue_submit_simple_drm_nouveau(struct nvk_queue *queue,
                                    struct nouveau_ws_bo *push_bo,
                                    uint32_t push_dw_count,
                                    struct nouveau_ws_bo *extra_bo)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   struct push_builder pb;
   push_builder_init(dev, &pb);

   push_add_push(&pb, push_bo, 0, push_dw_count);

   if (extra_bo)
      push_add_bo(&pb, extra_bo, NOUVEAU_WS_BO_RDWR);

   return push_submit(&pb, queue);
}

static void
push_add_queue_state(struct push_builder *pb, struct nvk_queue_state *qs)
{
   if (qs->images.bo)
      push_add_bo(pb, qs->images.bo, NOUVEAU_WS_BO_RD);
   if (qs->samplers.bo)
      push_add_bo(pb, qs->samplers.bo, NOUVEAU_WS_BO_RD);
   if (qs->slm.bo)
      push_add_bo(pb, qs->slm.bo, NOUVEAU_WS_BO_RDWR);
   if (qs->push.bo)
      push_add_push(pb, qs->push.bo, 0, qs->push.dw_count);
}

VkResult
nvk_queue_submit_drm_nouveau(struct vk_queue *vk_queue,
                             struct vk_queue_submit *submit)
{
   struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);
   struct nvk_device *dev = nvk_queue_device(queue);
   struct push_builder pb;
   VkResult result;

   result = nvk_queue_state_update(dev, &queue->state);
   if (result != VK_SUCCESS)
      return result;

   push_builder_init(dev, &pb);

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct nvk_bo_sync *bo_sync =
         container_of(submit->signals[i].sync, struct nvk_bo_sync, sync);

      push_add_bo(&pb, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
   }

   if (submit->command_buffer_count == 0) {
      push_add_push(&pb, queue->empty_push, 0, queue->empty_push_dw_count);
   } else {
      push_add_queue_state(&pb, &queue->state);

      simple_mtx_lock(&dev->memory_objects_lock);
      list_for_each_entry(struct nvk_device_memory, mem,
                          &dev->memory_objects, link) {
         push_add_bo(&pb, mem->bo, NOUVEAU_WS_BO_RDWR);
      }
      simple_mtx_unlock(&dev->memory_objects_lock);

      for (unsigned i = 0; i < submit->command_buffer_count; i++) {
         struct nvk_cmd_buffer *cmd =
            container_of(submit->command_buffers[i], struct nvk_cmd_buffer, vk);

         list_for_each_entry_safe(struct nvk_cmd_bo, bo, &cmd->bos, link)
            push_add_bo(&pb, bo->bo, NOUVEAU_WS_BO_RD);

         util_dynarray_foreach(&cmd->pushes, struct nvk_cmd_push, push)
            push_add_push(&pb, push->bo->bo, push->start_dw, push->dw_count);

         util_dynarray_foreach(&cmd->bo_refs, struct nvk_cmd_bo_ref, ref)
            push_add_bo(&pb, ref->bo, NOUVEAU_WS_BO_RDWR);
      }
   }

   pthread_mutex_lock(&dev->mutex);

   result = push_submit(&pb, queue);
   if (result == VK_SUCCESS) {
      for (uint32_t i = 0; i < submit->signal_count; i++) {
         struct nvk_bo_sync *bo_sync =
            container_of(submit->signals[i].sync, struct nvk_bo_sync, sync);
         assert(bo_sync->state == NVK_BO_SYNC_STATE_RESET);
         bo_sync->state = NVK_BO_SYNC_STATE_SUBMITTED;
      }
   }

   pthread_cond_broadcast(&dev->queue_submit);
   pthread_mutex_unlock(&dev->mutex);

   return result;
}

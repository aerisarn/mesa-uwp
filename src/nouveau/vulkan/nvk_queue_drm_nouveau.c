#include "nvk_queue.h"

#include "nvk_bo_sync.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"
#include "nouveau_push.h"

static void
nvk_queue_state_ref(struct nouveau_ws_push *push,
                    struct nvk_queue_state *qs)
{
   if (qs->images.bo)
      nouveau_ws_push_ref(push, qs->images.bo, NOUVEAU_WS_BO_RD);
   if (qs->samplers.bo)
      nouveau_ws_push_ref(push, qs->samplers.bo, NOUVEAU_WS_BO_RD);
   if (qs->slm.bo)
      nouveau_ws_push_ref(push, qs->slm.bo, NOUVEAU_WS_BO_RDWR);
}

VkResult
nvk_queue_submit_drm_nouveau(struct vk_queue *vk_queue,
                             struct vk_queue_submit *submit)
{
   struct nvk_device *device =
      container_of(vk_queue->base.device, struct nvk_device, vk);
   struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);
   VkResult result;

   result = nvk_queue_state_update(device, &queue->state);
   if (result != VK_SUCCESS)
      return result;

   pthread_mutex_lock(&device->mutex);

   if (queue->state.push) {
      nouveau_ws_push_submit(queue->state.push, device->pdev->dev, device->ctx);
   }

   if (submit->command_buffer_count == 0) {
      unsigned real_refs = nouveau_ws_push_num_refs(queue->empty_push);
      for (uint32_t i = 0; i < submit->signal_count; i++) {
         struct nvk_bo_sync *bo_sync = container_of(submit->signals[i].sync, struct nvk_bo_sync, sync);
         nouveau_ws_push_ref(queue->empty_push, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
      }
      nouveau_ws_push_submit(queue->empty_push, device->pdev->dev, device->ctx);
      nouveau_ws_push_reset_refs(queue->empty_push, real_refs);
   }
   for (unsigned i = 0; i < submit->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd = (struct nvk_cmd_buffer *)submit->command_buffers[i];

      unsigned real_refs = nouveau_ws_push_num_refs(cmd->push);
      for (uint32_t i = 0; i < submit->signal_count; i++) {
         struct nvk_bo_sync *bo_sync = container_of(submit->signals[i].sync, struct nvk_bo_sync, sync);
         nouveau_ws_push_ref(cmd->push, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
      }

      nvk_queue_state_ref(cmd->push, &queue->state);
      nouveau_ws_push_ref(cmd->push, device->zero_page, NOUVEAU_WS_BO_RD);

      simple_mtx_lock(&device->memory_objects_lock);
      list_for_each_entry(struct nvk_device_memory, mem,
                          &device->memory_objects, link) {
         nouveau_ws_push_ref(cmd->push, mem->bo, NOUVEAU_WS_BO_RDWR);
      }
      simple_mtx_unlock(&device->memory_objects_lock);

      nouveau_ws_push_submit(cmd->push, device->pdev->dev, device->ctx);
      nouveau_ws_push_reset_refs(cmd->push, real_refs);
   }

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct nvk_bo_sync *bo_sync = container_of(submit->signals[i].sync, struct nvk_bo_sync, sync);
      assert(bo_sync->state == NVK_BO_SYNC_STATE_RESET);
      bo_sync->state = NVK_BO_SYNC_STATE_SUBMITTED;
   }

   pthread_cond_broadcast(&device->queue_submit);
   pthread_mutex_unlock(&device->mutex);

   return VK_SUCCESS;
}

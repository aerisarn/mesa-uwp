#include "nvk_device.h"

#include "nvk_bo_sync.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device_memory.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"
#include "nouveau_push.h"

#include "vulkan/wsi/wsi_common.h"

#include "nvk_cl9097.h"
#include "nvk_cl90b5.h"
#include "nvk_cla0c0.h"
#include "cla1c0.h"
#include "nvk_clc3c0.h"

static void
nvk_slm_area_init(struct nvk_slm_area *area)
{
   memset(area, 0, sizeof(*area));
   simple_mtx_init(&area->mutex, mtx_plain);
}

static void
nvk_slm_area_finish(struct nvk_slm_area *area)
{
   simple_mtx_destroy(&area->mutex);
   if (area->bo)
      nouveau_ws_bo_destroy(area->bo);
}

static struct nouveau_ws_bo *
nvk_slm_area_get_bo_ref(struct nvk_slm_area *area,
                        uint32_t *bytes_per_warp_out,
                        uint32_t *bytes_per_mp_out)
{
   simple_mtx_lock(&area->mutex);
   struct nouveau_ws_bo *bo = area->bo;
   if (bo)
      nouveau_ws_bo_ref(bo);
   *bytes_per_warp_out = area->bytes_per_warp;
   *bytes_per_mp_out = area->bytes_per_mp;
   simple_mtx_unlock(&area->mutex);

   return bo;
}

static VkResult
nvk_slm_area_ensure(struct nvk_device *dev,
                    struct nvk_slm_area *area,
                    uint32_t bytes_per_thread)
{
   assert(bytes_per_thread < (1 << 24));

   /* TODO: Volta+doesn't use CRC */
   const uint32_t crs_size = 0;

   uint64_t bytes_per_warp = bytes_per_thread * 32 + crs_size;

   /* The hardware seems to require this alignment for
    * NV9097_SET_SHADER_LOCAL_MEMORY_E_DEFAULT_SIZE_PER_WARP
    */
   bytes_per_warp = ALIGN(bytes_per_warp, 0x200);

   uint64_t bytes_per_mp = bytes_per_warp * 64; /* max warps */

   /* The hardware seems to require this alignment for
    * NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A_SIZE_LOWER.
    *
    * Fortunately, this is just the alignment for bytes_per_warp multiplied
    * by the number of warps, 64.  It might matter for real on a GPU with 48
    * warps but we don't support any of those yet.
    */
   assert(bytes_per_mp == ALIGN(bytes_per_mp, 0x8000));

   /* nvk_slm_area::bytes_per_mp only ever increases so we can check this
    * outside the lock and exit early in the common case.  We only need to
    * take the lock if we're actually going to resize.
    *
    * Also, we only care about bytes_per_mp and not bytes_per_warp because
    * they are integer multiples of each other.
    */
   if (likely(bytes_per_mp <= area->bytes_per_mp))
      return VK_SUCCESS;

   uint64_t size = bytes_per_mp * dev->pdev->dev->mp_count;

   struct nouveau_ws_bo *bo =
      nouveau_ws_bo_new(dev->pdev->dev, size, 0, NOUVEAU_WS_BO_LOCAL);
   if (bo == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct nouveau_ws_bo *unref_bo;
   simple_mtx_lock(&area->mutex);
   if (bytes_per_mp <= area->bytes_per_mp) {
      /* We lost the race, throw away our BO */
      assert(area->bytes_per_warp == bytes_per_warp);
      unref_bo = bo;
   } else {
      unref_bo = area->bo;
      area->bo = bo;
      area->bytes_per_warp = bytes_per_warp;
      area->bytes_per_mp = bytes_per_mp;
   }
   simple_mtx_unlock(&area->mutex);

   if (unref_bo)
      nouveau_ws_bo_destroy(unref_bo);

   return VK_SUCCESS;
}

static void
nvk_queue_state_init(struct nvk_queue_state *qs)
{
   memset(qs, 0, sizeof(*qs));
}

static void
nvk_queue_state_finish(struct nvk_device *dev,
                       struct nvk_queue_state *qs)
{
   if (qs->images.bo)
      nouveau_ws_bo_destroy(qs->images.bo);
   if (qs->samplers.bo)
      nouveau_ws_bo_destroy(qs->samplers.bo);
   if (qs->slm.bo)
      nouveau_ws_bo_destroy(qs->slm.bo);
   if (qs->push)
      nouveau_ws_push_destroy(qs->push);
}

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

static VkResult
nvk_queue_state_update(struct nvk_device *dev,
                       struct nvk_queue_state *qs)
{
   struct nouveau_ws_bo *bo;
   uint32_t alloc_count, bytes_per_warp, bytes_per_mp;
   bool dirty = false;

   bo = nvk_descriptor_table_get_bo_ref(&dev->images, &alloc_count);
   if (qs->images.bo != bo || qs->images.alloc_count != alloc_count) {
      if (qs->images.bo)
         nouveau_ws_bo_destroy(qs->images.bo);
      qs->images.bo = bo;
      qs->images.alloc_count = alloc_count;
      dirty = true;
   } else {
      /* No change */
      if (bo)
         nouveau_ws_bo_destroy(bo);
   }

   bo = nvk_descriptor_table_get_bo_ref(&dev->samplers, &alloc_count);
   if (qs->samplers.bo != bo || qs->samplers.alloc_count != alloc_count) {
      if (qs->samplers.bo)
         nouveau_ws_bo_destroy(qs->samplers.bo);
      qs->samplers.bo = bo;
      qs->samplers.alloc_count = alloc_count;
      dirty = true;
   } else {
      /* No change */
      if (bo)
         nouveau_ws_bo_destroy(bo);
   }

   bo = nvk_slm_area_get_bo_ref(&dev->slm, &bytes_per_warp, &bytes_per_mp);
   if (qs->slm.bo != bo || qs->slm.bytes_per_warp != bytes_per_warp ||
       qs->slm.bytes_per_mp != bytes_per_mp) {
      if (qs->slm.bo)
         nouveau_ws_bo_destroy(qs->slm.bo);
      qs->slm.bo = bo;
      qs->slm.bytes_per_warp = bytes_per_warp;
      qs->slm.bytes_per_mp = bytes_per_mp;
      dirty = true;
   } else {
      /* No change */
      if (bo)
         nouveau_ws_bo_destroy(bo);
   }

   /* TODO: We're currently depending on kernel reference counting to protect
    * us here.  If we ever stop reference counting in the kernel, we will
    * either need to delay destruction or hold on to our extra BO references
    * and insert a GPU stall here if anything has changed before dropping our
    * old references.
    */

   if (!dirty)
      return VK_SUCCESS;

   struct nouveau_ws_push *pb = nouveau_ws_push_new(dev->pdev->dev, 256);
   struct nouveau_ws_push_buffer *p = P_SPACE(pb, 256);
   if (p == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   if (qs->images.bo) {
      nouveau_ws_push_ref(pb, qs->images.bo, NOUVEAU_WS_BO_RD);

      /* Compute */
      P_MTHD(p, NVA0C0, SET_TEX_HEADER_POOL_A);
      P_NVA0C0_SET_TEX_HEADER_POOL_A(p, qs->images.bo->offset >> 32);
      P_NVA0C0_SET_TEX_HEADER_POOL_B(p, qs->images.bo->offset);
      P_NVA0C0_SET_TEX_HEADER_POOL_C(p, qs->images.alloc_count - 1);
      P_IMMD(p, NVA0C0, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });

      /* 3D */
      P_MTHD(p, NV9097, SET_TEX_HEADER_POOL_A);
      P_NV9097_SET_TEX_HEADER_POOL_A(p, qs->images.bo->offset >> 32);
      P_NV9097_SET_TEX_HEADER_POOL_B(p, qs->images.bo->offset);
      P_NV9097_SET_TEX_HEADER_POOL_C(p, qs->images.alloc_count - 1);
      P_IMMD(p, NV9097, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   if (qs->samplers.bo) {
      nouveau_ws_push_ref(pb, qs->samplers.bo, NOUVEAU_WS_BO_RD);

      /* Compute */
      P_MTHD(p, NVA0C0, SET_TEX_SAMPLER_POOL_A);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_A(p, qs->samplers.bo->offset >> 32);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_B(p, qs->samplers.bo->offset);
      P_NVA0C0_SET_TEX_SAMPLER_POOL_C(p, qs->samplers.alloc_count - 1);
      P_IMMD(p, NVA0C0, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });

      /* 3D */
      P_MTHD(p, NV9097, SET_TEX_SAMPLER_POOL_A);
      P_NV9097_SET_TEX_SAMPLER_POOL_A(p, qs->samplers.bo->offset >> 32);
      P_NV9097_SET_TEX_SAMPLER_POOL_B(p, qs->samplers.bo->offset);
      P_NV9097_SET_TEX_SAMPLER_POOL_C(p, qs->samplers.alloc_count - 1);
      P_IMMD(p, NV9097, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
         .lines = LINES_ALL
      });
   }

   if (qs->slm.bo) {
      nouveau_ws_push_ref(pb, qs->slm.bo, NOUVEAU_WS_BO_RDWR);
      const uint64_t slm_addr = qs->slm.bo->offset;
      const uint64_t slm_size = qs->slm.bo->size;
      const uint64_t slm_per_warp = qs->slm.bytes_per_warp;
      const uint64_t slm_per_mp = qs->slm.bytes_per_mp;
      assert(!(slm_per_mp & 0x7fff));

      /* Compute */
      P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_A);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);

      P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A(p, slm_per_mp >> 32);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_B(p, slm_per_mp);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_C(p, 0xff);

      if (dev->ctx->compute.cls < VOLTA_COMPUTE_A) {
         P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_THROTTLED_A);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A(p, slm_per_mp >> 32);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_B(p, slm_per_mp);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C(p, 0xff);
      }

      /* 3D */
      P_MTHD(p, NV9097, SET_SHADER_LOCAL_MEMORY_A);
      P_NV9097_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
      P_NV9097_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);
      P_NV9097_SET_SHADER_LOCAL_MEMORY_C(p, slm_size >> 32);
      P_NV9097_SET_SHADER_LOCAL_MEMORY_D(p, slm_size);
      P_NV9097_SET_SHADER_LOCAL_MEMORY_E(p, slm_per_warp);
   }

   /* We set memory windows unconditionally.  Otherwise, the memory window
    * might be in a random place and cause us to fault off into nowhere.
    */
   if (dev->ctx->compute.cls >= VOLTA_COMPUTE_A) {
      uint64_t temp = 0xfeULL << 24;
      P_MTHD(p, NVC3C0, SET_SHADER_SHARED_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_A(p, temp >> 32);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_B(p, temp & 0xffffffff);

      temp = 0xffULL << 24;
      P_MTHD(p, NVC3C0, SET_SHADER_LOCAL_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A(p, temp >> 32);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B(p, temp & 0xffffffff);
   } else {
      P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_WINDOW(p, 0xff << 24);

      P_MTHD(p, NVA0C0, SET_SHADER_SHARED_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_SHARED_MEMORY_WINDOW(p, 0xfe << 24);

      // TODO CODE_ADDRESS_HIGH
   }

   /* From nvc0_screen.c:
    *
    *    "Reduce likelihood of collision with real buffers by placing the
    *    hole at the top of the 4G area. This will have to be dealt with
    *    for real eventually by blocking off that area from the VM."
    *
    * Really?!?  TODO: Fix this for realz.  Annoyingly, we only have a
    * 32-bit pointer for this in 3D rather than a full 48 like we have for
    * compute.
    */
   P_IMMD(p, NV9097, SET_SHADER_LOCAL_MEMORY_WINDOW, 0xff << 24);

   if (qs->push)
      nouveau_ws_push_destroy(qs->push);
   qs->push = pb;

   return VK_SUCCESS;
}

static VkResult
nvk_queue_init(struct nvk_device *dev, struct nvk_queue *queue,
               const VkDeviceQueueCreateInfo *pCreateInfo,
               uint32_t index_in_family)
{
   VkResult result;

   result = vk_queue_init(&queue->vk, &dev->vk, pCreateInfo, index_in_family);
   if (result != VK_SUCCESS)
      return result;

   nvk_queue_state_init(&queue->state);

   return VK_SUCCESS;
}

static void
nvk_queue_finish(struct nvk_device *dev, struct nvk_queue *queue)
{
   nvk_queue_state_finish(dev, &queue->state);
   if (queue->empty_push)
      nouveau_ws_push_destroy(queue->empty_push);
   vk_queue_finish(&queue->vk);
}

static VkResult
nvk_queue_submit(struct vk_queue *vkqueue, struct vk_queue_submit *submission)
{
   struct nvk_device *device = container_of(vkqueue->base.device, struct nvk_device, vk);
   struct nvk_queue *queue = container_of(vkqueue, struct nvk_queue, vk);
   VkResult result;

   if (!queue->empty_push) {
      queue->empty_push = nouveau_ws_push_new(device->pdev->dev, 4096);

      struct nouveau_ws_push_buffer *p = P_SPACE(queue->empty_push, 2);

      P_MTHD(p, NV90B5, NOP);
      P_NV90B5_NOP(p, 0);
   }

   result = nvk_queue_state_update(device, &queue->state);
   if (result != VK_SUCCESS)
      return result;

   pthread_mutex_lock(&device->mutex);

   if (queue->state.push) {
      nouveau_ws_push_submit(queue->state.push, device->pdev->dev, device->ctx);
   }

   if (submission->command_buffer_count == 0) {
      unsigned real_refs = nouveau_ws_push_num_refs(queue->empty_push);
      for (uint32_t i = 0; i < submission->signal_count; i++) {
         struct nvk_bo_sync *bo_sync = container_of(submission->signals[i].sync, struct nvk_bo_sync, sync);
         nouveau_ws_push_ref(queue->empty_push, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
      }
      nouveau_ws_push_submit(queue->empty_push, device->pdev->dev, device->ctx);
      nouveau_ws_push_reset_refs(queue->empty_push, real_refs);
   }
   for (unsigned i = 0; i < submission->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd = (struct nvk_cmd_buffer *)submission->command_buffers[i];

      unsigned real_refs = nouveau_ws_push_num_refs(cmd->push);
      for (uint32_t i = 0; i < submission->signal_count; i++) {
         struct nvk_bo_sync *bo_sync = container_of(submission->signals[i].sync, struct nvk_bo_sync, sync);
         nouveau_ws_push_ref(cmd->push, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
      }

      nvk_queue_state_ref(cmd->push, &queue->state);

      simple_mtx_lock(&device->memory_objects_lock);
      list_for_each_entry(struct nvk_device_memory, mem,
                          &device->memory_objects, link) {
         nouveau_ws_push_ref(cmd->push, mem->bo, NOUVEAU_WS_BO_RDWR);
      }
      simple_mtx_unlock(&device->memory_objects_lock);

      nouveau_ws_push_submit(cmd->push, device->pdev->dev, device->ctx);
      nouveau_ws_push_reset_refs(cmd->push, real_refs);
   }

   for (uint32_t i = 0; i < submission->signal_count; i++) {
      struct nvk_bo_sync *bo_sync = container_of(submission->signals[i].sync, struct nvk_bo_sync, sync);
      assert(bo_sync->state == NVK_BO_SYNC_STATE_RESET);
      bo_sync->state = NVK_BO_SYNC_STATE_SUBMITTED;
   }

   pthread_cond_broadcast(&device->queue_submit);
   pthread_mutex_unlock(&device->mutex);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDevice(VkPhysicalDevice physicalDevice,
   const VkDeviceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDevice *pDevice)
{
   VK_FROM_HANDLE(nvk_physical_device, physical_device, physicalDevice);
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   struct nvk_device *device;

   device = vk_zalloc2(&physical_device->instance->vk.alloc,
      pAllocator,
      sizeof(*device),
      8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &nvk_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

   result =
      vk_device_init(&device->vk, &physical_device->vk, &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   device->vk.command_buffer_ops = &nvk_cmd_buffer_ops;

   int ret = nouveau_ws_context_create(physical_device->dev, &device->ctx);
   if (ret) {
      if (ret == -ENOSPC)
         result = vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
      else
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_init;
   }

   list_inithead(&device->memory_objects);
   simple_mtx_init(&device->memory_objects_lock, mtx_plain);

   result = nvk_descriptor_table_init(device, &device->images,
                                      8 * 4 /* tic entry size */,
                                      1024, 1024 * 1024);
   if (result != VK_SUCCESS)
      goto fail_memory_objects;

   /* Reserve the descriptor at offset 0 to be the null descriptor */
   uint32_t null_image[8] = { 0, };
   ASSERTED uint32_t null_image_index;
   result = nvk_descriptor_table_add(device, &device->images,
                                     null_image, sizeof(null_image),
                                     &null_image_index);
   assert(result == VK_SUCCESS);
   assert(null_image_index == 0);

   result = nvk_descriptor_table_init(device, &device->samplers,
                                      8 * 4 /* tsc entry size */,
                                      4096, 4096);
   if (result != VK_SUCCESS)
      goto fail_images;

   nvk_slm_area_init(&device->slm);

   result = nvk_queue_init(device, &device->queue,
                           &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_slm;

   if (pthread_mutex_init(&device->mutex, NULL) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_queue;
   }

   pthread_condattr_t condattr;
   if (pthread_condattr_init(&condattr) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_cond_init(&device->queue_submit, &condattr) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   pthread_condattr_destroy(&condattr);

   device->queue.vk.driver_submit = nvk_queue_submit;

   device->pdev = physical_device;

   result = nvk_device_init_meta(device);
   if (result != VK_SUCCESS)
      goto fail_queue_submit;

   *pDevice = nvk_device_to_handle(device);

   return VK_SUCCESS;

fail_queue_submit:
   pthread_cond_destroy(&device->queue_submit);
fail_mutex:
   pthread_mutex_destroy(&device->mutex);
fail_queue:
   nvk_queue_finish(device, &device->queue);
fail_slm:
   nvk_slm_area_finish(&device->slm);
   nvk_descriptor_table_finish(device, &device->samplers);
fail_images:
   nvk_descriptor_table_finish(device, &device->images);
fail_memory_objects:
   simple_mtx_destroy(&device->memory_objects_lock);
   nouveau_ws_context_destroy(device->ctx);
fail_init:
   vk_device_finish(&device->vk);
fail_alloc:
   vk_free(&device->vk.alloc, device);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);

   if (!device)
      return;

   nvk_device_finish_meta(device);

   pthread_cond_destroy(&device->queue_submit);
   pthread_mutex_destroy(&device->mutex);
   nvk_queue_finish(device, &device->queue);
   vk_device_finish(&device->vk);
   nvk_slm_area_finish(&device->slm);
   nvk_descriptor_table_finish(device, &device->samplers);
   nvk_descriptor_table_finish(device, &device->images);
   assert(list_is_empty(&device->memory_objects));
   simple_mtx_destroy(&device->memory_objects_lock);
   nouveau_ws_context_destroy(device->ctx);
   vk_free(&device->vk.alloc, device);
}

VkResult
nvk_device_ensure_slm(struct nvk_device *dev,
                      uint32_t bytes_per_thread)
{
   return nvk_slm_area_ensure(dev, &dev->slm, bytes_per_thread);
}

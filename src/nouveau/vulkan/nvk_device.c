#include "nvk_device.h"

#include "nvk_cmd_buffer.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"

#include "vulkan/wsi/wsi_common.h"

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

struct nouveau_ws_bo *
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

   if (pthread_mutex_init(&device->mutex, NULL) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_slm;
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

   device->pdev = physical_device;

   void *zero_map;
   device->zero_page = nouveau_ws_bo_new_mapped(device->pdev->dev, 0x1000, 0,
                                                NOUVEAU_WS_BO_LOCAL,
                                                NOUVEAU_WS_BO_WR, &zero_map);
   if (device->zero_page == NULL)
      goto fail_queue_submit;

   memset(zero_map, 0, 0x1000);
   nouveau_ws_bo_unmap(device->zero_page, zero_map);

   result = nvk_queue_init(device, &device->queue,
                           &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_zero_page;

   result = nvk_device_init_context_draw_state(device);
   if (result != VK_SUCCESS)
      goto fail_queue;

   result = nvk_device_init_meta(device);
   if (result != VK_SUCCESS)
      goto fail_queue;

   *pDevice = nvk_device_to_handle(device);

   return VK_SUCCESS;

fail_queue:
   nvk_queue_finish(device, &device->queue);
fail_queue_submit:
   pthread_cond_destroy(&device->queue_submit);
fail_zero_page:
   nouveau_ws_bo_destroy(device->zero_page);
fail_mutex:
   pthread_mutex_destroy(&device->mutex);
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
   nouveau_ws_bo_destroy(device->zero_page);
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

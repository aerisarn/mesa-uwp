#include "nvk_device.h"

#include "nvk_cmd_buffer.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"
#include "nouveau_push.h"

#include "vulkan/wsi/wsi_common.h"

static VkResult
nvk_queue_submit(struct vk_queue *queue, struct vk_queue_submit *submission)
{
   struct nvk_device *device = container_of(queue->base.device, struct nvk_device, vk);

   for (unsigned i = 0; i < submission->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd = (struct nvk_cmd_buffer *)submission->command_buffers[i];
      nouveau_ws_push_submit(cmd->push, device->pdev->dev, device->ctx);
   }

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

   int ret = nouveau_ws_context_create(physical_device->dev, &device->ctx);
   if (ret) {
      if (ret == -ENOSPC)
         result = vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
      else
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_init;
   }

   result = vk_queue_init(&device->queue, &device->vk, &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_ctx;

   device->queue.driver_submit = nvk_queue_submit;

   device->pdev = physical_device;

   *pDevice = nvk_device_to_handle(device);

   return VK_SUCCESS;

fail_ctx:
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

   vk_queue_finish(&device->queue);
   vk_device_finish(&device->vk);
   nouveau_ws_context_destroy(device->ctx);
   vk_free(&device->vk.alloc, device);
}

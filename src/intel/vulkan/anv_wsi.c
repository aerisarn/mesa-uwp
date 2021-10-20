/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_private.h"
#include "anv_measure.h"
#include "wsi_common.h"
#include "vk_fence.h"
#include "vk_queue.h"
#include "vk_semaphore.h"
#include "vk_util.h"

static PFN_vkVoidFunction
anv_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

static void
anv_wsi_signal_semaphore_for_memory(VkDevice _device,
                                    VkSemaphore _semaphore,
                                    VkDeviceMemory _memory)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   VK_FROM_HANDLE(vk_semaphore, semaphore, _semaphore);
   ANV_FROM_HANDLE(anv_device_memory, memory, _memory);
   ASSERTED VkResult result;

   /* Put a BO semaphore with the image BO in the temporary.  For BO binary
    * semaphores, we always set EXEC_OBJECT_WRITE so this creates a WaR
    * hazard with the display engine's read to ensure that no one writes to
    * the image before the read is complete.
    */
   vk_semaphore_reset_temporary(&device->vk, semaphore);

   result = anv_sync_create_for_bo(device, memory->bo, &semaphore->temporary);
   assert(result == VK_SUCCESS);
}

static void
anv_wsi_signal_fence_for_memory(VkDevice _device,
                                VkFence _fence,
                                VkDeviceMemory _memory)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   VK_FROM_HANDLE(vk_fence, fence, _fence);
   ANV_FROM_HANDLE(anv_device_memory, memory, _memory);
   ASSERTED VkResult result;

   /* Put a BO fence with the image BO in the temporary.  For BO fences, we
    * always just wait until the BO isn't busy and reads from the BO should
    * count as busy.
    */
   vk_fence_reset_temporary(&device->vk, fence);

   result = anv_sync_create_for_bo(device, memory->bo, &fence->temporary);
   assert(result == VK_SUCCESS);
}

VkResult
anv_init_wsi(struct anv_physical_device *physical_device)
{
   VkResult result;

   result = wsi_device_init(&physical_device->wsi_device,
                            anv_physical_device_to_handle(physical_device),
                            anv_wsi_proc_addr,
                            &physical_device->instance->vk.alloc,
                            physical_device->master_fd,
                            &physical_device->instance->dri_options,
                            false);
   if (result != VK_SUCCESS)
      return result;

   physical_device->wsi_device.supports_modifiers = true;
   physical_device->wsi_device.signal_semaphore_for_memory =
      anv_wsi_signal_semaphore_for_memory;
   physical_device->wsi_device.signal_fence_for_memory =
      anv_wsi_signal_fence_for_memory;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   wsi_device_setup_syncobj_fd(&physical_device->wsi_device,
                               physical_device->local_fd);

   return VK_SUCCESS;
}

void
anv_finish_wsi(struct anv_physical_device *physical_device)
{
   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device,
                     &physical_device->instance->vk.alloc);
}

VkResult anv_QueuePresentKHR(
    VkQueue                                  _queue,
    const VkPresentInfoKHR*                  pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   struct anv_device *device = queue->device;
   VkResult result;

   if (device->debug_frame_desc) {
      device->debug_frame_desc->frame_id++;
      if (!device->info.has_llc) {
         intel_clflush_range(device->debug_frame_desc,
                           sizeof(*device->debug_frame_desc));
      }
   }

   result = vk_queue_wait_before_present(&queue->vk, pPresentInfo);
   if (result != VK_SUCCESS)
      return result;

   result = wsi_common_queue_present(&device->physical->wsi_device,
                                     anv_device_to_handle(queue->device),
                                     _queue, 0,
                                     pPresentInfo);

   for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
      VK_FROM_HANDLE(vk_semaphore, semaphore, pPresentInfo->pWaitSemaphores[i]);
      /* From the Vulkan 1.0.53 spec:
       *
       *    "If the import is temporary, the implementation must restore the
       *    semaphore to its prior permanent state after submitting the next
       *    semaphore wait operation."
       */
      vk_semaphore_reset_temporary(&queue->device->vk, semaphore);
   }

   return result;
}

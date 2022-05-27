
#include "nvk_wsi.h"
#include "nvk_instance.h"
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

VkResult
nvk_init_wsi(struct nvk_physical_device *physical_device)
{
   struct wsi_device_options wsi_options = {
      .sw_device = false
   };
   VkResult result =
      wsi_device_init(&physical_device->wsi_device, nvk_physical_device_to_handle(physical_device),
                      nvk_wsi_proc_addr, &physical_device->instance->vk.alloc,
                      -1, NULL, &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   physical_device->vk.wsi_device = &physical_device->wsi_device;
   return result;
}

void
nvk_finish_wsi(struct nvk_physical_device *physical_device)
{
   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &physical_device->instance->vk.alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_QueuePresentKHR(VkQueue _queue, const VkPresentInfoKHR *pPresentInfo)
{
   return VK_NOT_READY;
}

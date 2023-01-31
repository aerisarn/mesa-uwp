#include "nvk_buffer_view.h"

#include "nvk_device.h"

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pBufferView)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_buffer_view *view;

   view = vk_buffer_view_create(&device->vk, pCreateInfo,
                                 pAllocator, sizeof(*view));
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pBufferView = nvk_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyBufferView(VkDevice _device,
                      VkBufferView bufferView,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_buffer_view, view, bufferView);

   if (!view)
      return;

   vk_buffer_view_destroy(&device->vk, pAllocator, &view->vk);
}

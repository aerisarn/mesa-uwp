#include "nvk_image_view.h"

#include "nvk_device.h"
#include "nvk_image.h"

static VkResult nvk_image_view_init(struct nvk_device *device,
   struct nvk_image_view *view,
   const VkImageViewCreateInfo *pCreateInfo)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL nvk_CreateImageView(VkDevice _device,
   const VkImageViewCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkImageView *pView)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_image_view *view;

   view = vk_image_view_create(&device->vk, false, pCreateInfo, pAllocator, sizeof(*view));
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pView = nvk_image_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice _device,
   VkImageView imageView,
   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_image_view, view, imageView);

   if (!view)
      return;

   vk_image_view_destroy(&device->vk, pAllocator, &view->vk);
}

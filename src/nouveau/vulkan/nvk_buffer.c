#include "nvk_buffer.h"

#include "nvk_device.h"

VKAPI_ATTR VkResult VKAPI_CALL nvk_CreateBuffer(VkDevice _device,
   const VkBufferCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_buffer *buffer;

   buffer = vk_buffer_create(&device->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (!buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pBuffer = nvk_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL nvk_DestroyBuffer(VkDevice _device,
   VkBuffer _buffer,
   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

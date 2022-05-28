#include "nvk_buffer.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"

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

VKAPI_ATTR void VKAPI_CALL nvk_GetBufferMemoryRequirements2(
    VkDevice _device,
    const VkBufferMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_buffer, buffer, pInfo->buffer);

   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements) {
      .size = buffer->vk.size,
      .alignment = 64, /* TODO */
      .memoryTypeBits = BITFIELD_MASK(device->pdev->mem_type_cnt),
   };

   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                     const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(nvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(nvk_buffer, buffer, pBindInfos[i].buffer);

      buffer->mem = mem;
      buffer->offset = pBindInfos[i].memoryOffset;
   }
   return VK_SUCCESS;
}

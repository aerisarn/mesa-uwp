#include "nvk_buffer.h"

#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateBuffer(VkDevice _device,
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

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyBuffer(VkDevice _device,
                  VkBuffer _buffer,
                  const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDeviceBufferMemoryRequirements(VkDevice _device,
                                      const VkDeviceBufferMemoryRequirements *pInfo,
                                      VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(nvk_device, device, _device);

   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements) {
      .size = pInfo->pCreateInfo->size,
      .alignment = 64, /* TODO */
      .memoryTypeBits = BITFIELD_MASK(device->pdev->mem_type_cnt),
   };

   vk_foreach_struct_const(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->prefersDedicatedAllocation = false;
         dedicated->requiresDedicatedAllocation = false;
         break;
      }
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* The Vulkan 1.3.256 spec says:
    *
    *    VUID-VkPhysicalDeviceExternalBufferInfo-handleType-parameter
    *
    *    "handleType must be a valid VkExternalMemoryHandleTypeFlagBits value"
    *
    * This differs from VkPhysicalDeviceExternalImageFormatInfo, which
    * surprisingly permits handleType == 0.
    */
   assert(pExternalBufferInfo->handleType != 0);

   /* All of the current flags are for sparse which we don't support yet.
    * Even when we do support it, doing sparse on external memory sounds
    * sketchy.  Also, just disallowing flags is the safe option.
    */
   if (pExternalBufferInfo->flags)
      goto unsupported;

   switch (pExternalBufferInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      pExternalBufferProperties->externalMemoryProperties = nvk_dma_buf_mem_props;
      return;
   default:
      goto unsupported;
   }

unsupported:
   /* From the Vulkan 1.3.256 spec:
    *
    *    compatibleHandleTypes must include at least handleType.
    */
   pExternalBufferProperties->externalMemoryProperties =
      (VkExternalMemoryProperties) {
         .compatibleHandleTypes = pExternalBufferInfo->handleType,
      };
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindBufferMemory2(VkDevice _device,
                      uint32_t bindInfoCount,
                      const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(nvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(nvk_buffer, buffer, pBindInfos[i].buffer);

      buffer->mem = mem;
      buffer->addr = mem->bo->offset + pBindInfos[i].memoryOffset;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
nvk_GetBufferDeviceAddress(UNUSED VkDevice device,
                           const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, pInfo->buffer);

   return nvk_buffer_address(buffer, 0);
}

VKAPI_ATTR uint64_t VKAPI_CALL
nvk_GetBufferOpaqueCaptureAddress(UNUSED VkDevice device,
                                  const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, pInfo->buffer);

   return nvk_buffer_address(buffer, 0);
}

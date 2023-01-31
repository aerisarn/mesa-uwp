#include "nvk_sampler.h"

#include "nvk_device.h"

VKAPI_ATTR VkResult VKAPI_CALL nvk_CreateSampler(VkDevice _device,
   const VkSamplerCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkSampler *pSampler)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_sampler *sampler;

   sampler = vk_object_zalloc(&device->vk, pAllocator, sizeof(*sampler), VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pSampler = nvk_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL nvk_DestroySampler(VkDevice _device,
   VkSampler _sampler,
   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_sampler, sampler, _sampler);

   if (!sampler)
      return;

   vk_object_free(&device->vk, pAllocator, sampler);
}

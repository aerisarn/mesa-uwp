#include "nvk_pipeline_layout.h"

#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"

#include "util/mesa-sha1.h"

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreatePipelineLayout(VkDevice _device,
                         const VkPipelineLayoutCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineLayout *pPipelineLayout)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_pipeline_layout *layout;

   layout = vk_object_alloc(&device->vk, pAllocator, sizeof(*layout),
                            VK_OBJECT_TYPE_PIPELINE_LAYOUT);
   if (layout == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->setLayoutCount;

   for (uint32_t s = 0; s < pCreateInfo->setLayoutCount; s++) {
      VK_FROM_HANDLE(nvk_descriptor_set_layout, set_layout,
                     pCreateInfo->pSetLayouts[s]);
      layout->set[s].layout = nvk_descriptor_set_layout_ref(set_layout);
   }

   struct mesa_sha1 sha1_ctx;
   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, &layout->num_sets, sizeof(layout->num_sets));
   for (uint32_t s = 0; s < pCreateInfo->setLayoutCount; s++) {
      _mesa_sha1_update(&sha1_ctx, layout->set[s].layout->sha1,
                        sizeof(layout->set[s].layout->sha1));
   }
   _mesa_sha1_final(&sha1_ctx, layout->sha1);

   *pPipelineLayout = nvk_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyPipelineLayout(VkDevice _device, VkPipelineLayout pipelineLayout,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_pipeline_layout, layout, pipelineLayout);

   if (!layout)
      return;

   for (uint32_t s = 0; s < layout->num_sets; s++)
      nvk_descriptor_set_layout_unref(device, layout->set[s].layout);

   vk_object_free(&device->vk, pAllocator, layout);
}

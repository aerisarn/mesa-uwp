#include "nvk_buffer_view.h"

#include "nil_image.h"
#include "nil_format.h"
#include "nvk_buffer.h"
#include "nvk_device.h"
#include "nvk_format.h"
#include "nvk_physical_device.h"
#include "vulkan/util/vk_format.h"

VkFormatFeatureFlags2
nvk_get_buffer_format_features(struct nvk_physical_device *pdevice,
                               VkFormat vk_format)
{
   VkFormatFeatureFlags2 features = 0;

   enum pipe_format p_format = vk_format_to_pipe_format(vk_format);
   if (p_format == PIPE_FORMAT_NONE)
      return 0;

   if (!util_format_is_compressed(p_format) &&
       !util_format_is_depth_or_stencil(p_format) &&
       nil_tic_format_for_pipe(p_format) != NULL)
      features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

   if (nvk_is_storage_image_format(vk_format))
      features |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT;

   return features;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pBufferView)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_buffer, buffer, pCreateInfo->buffer);
   struct nvk_buffer_view *view;

   view = vk_buffer_view_create(&device->vk, pCreateInfo,
                                 pAllocator, sizeof(*view));
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t *desc_map = nvk_descriptor_table_alloc(device, &device->images,
                                                   &view->desc_index);
   if (desc_map == NULL) {
      vk_buffer_view_destroy(&device->vk, pAllocator, &view->vk);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate image descriptor");
   }

   nil_buffer_fill_tic(nvk_device_physical(device)->dev,
                       nvk_buffer_address(buffer, view->vk.offset),
                       vk_format_to_pipe_format(view->vk.format),
                       view->vk.elements,
                       desc_map);

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

   nvk_descriptor_table_free(device, &device->images, view->desc_index);

   vk_buffer_view_destroy(&device->vk, pAllocator, &view->vk);
}

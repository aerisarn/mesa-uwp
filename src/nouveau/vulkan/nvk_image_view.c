#include "nvk_image_view.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "vulkan/util/vk_format.h"

static enum nil_view_type
vk_image_view_type_to_nil_view_type(VkImageViewType view_type)
{
   switch (view_type) {
   case VK_IMAGE_VIEW_TYPE_1D:         return NIL_VIEW_TYPE_1D;
   case VK_IMAGE_VIEW_TYPE_2D:         return NIL_VIEW_TYPE_2D;
   case VK_IMAGE_VIEW_TYPE_3D:         return NIL_VIEW_TYPE_3D;
   case VK_IMAGE_VIEW_TYPE_CUBE:       return NIL_VIEW_TYPE_CUBE;
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:   return NIL_VIEW_TYPE_1D_ARRAY;
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:   return NIL_VIEW_TYPE_2D_ARRAY;
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return NIL_VIEW_TYPE_CUBE_ARRAY;
   default:
      unreachable("Invalid image view type");
   }
}

static enum pipe_swizzle
vk_swizzle_to_pipe(VkComponentSwizzle swizzle)
{
   switch (swizzle) {
   case VK_COMPONENT_SWIZZLE_R:     return PIPE_SWIZZLE_X;
   case VK_COMPONENT_SWIZZLE_G:     return PIPE_SWIZZLE_Y;
   case VK_COMPONENT_SWIZZLE_B:     return PIPE_SWIZZLE_Z;
   case VK_COMPONENT_SWIZZLE_A:     return PIPE_SWIZZLE_W;
   case VK_COMPONENT_SWIZZLE_ONE:   return PIPE_SWIZZLE_1;
   case VK_COMPONENT_SWIZZLE_ZERO:  return PIPE_SWIZZLE_0;
   default:
      unreachable("Invalid component swizzle");
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_image, image, pCreateInfo->image);
   struct nvk_image_view *view;

   view = vk_image_view_create(&device->vk, false, pCreateInfo,
                               pAllocator, sizeof(*view));
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t *desc_map = nvk_descriptor_table_alloc(device, &device->images,
                                                   &view->desc_index);
   if (desc_map == NULL) {
      vk_image_view_destroy(&device->vk, pAllocator, &view->vk);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate image descriptor");
   }

   struct nil_view nil_view = {
      .type = vk_image_view_type_to_nil_view_type(view->vk.view_type),
      .format = vk_format_to_pipe_format(view->vk.format),
      .base_level = view->vk.base_mip_level,
      .num_levels = view->vk.level_count,
      .base_array_layer = view->vk.base_array_layer,
      .array_len = view->vk.layer_count,
      .swizzle = {
         vk_swizzle_to_pipe(view->vk.swizzle.r),
         vk_swizzle_to_pipe(view->vk.swizzle.g),
         vk_swizzle_to_pipe(view->vk.swizzle.b),
         vk_swizzle_to_pipe(view->vk.swizzle.a),
      },
   };

   nil_image_fill_tic(nvk_device_physical(device)->dev,
                      &image->nil, &nil_view,
                      nvk_image_base_address(image),
                      desc_map);

   *pView = nvk_image_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyImageView(VkDevice _device,
                     VkImageView imageView,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_image_view, view, imageView);

   if (!view)
      return;

   nvk_descriptor_table_free(device, &device->images, view->desc_index);

   vk_image_view_destroy(&device->vk, pAllocator, &view->vk);
}

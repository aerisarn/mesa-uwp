#include "nvk_image_view.h"

#include "nvk_device.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "vulkan/util/vk_format.h"

#include "gallium/drivers/nouveau/nv50/g80_defs.xml.h"
#include "gallium/drivers/nouveau/nv50/g80_texture.xml.h"
#include "gallium/drivers/nouveau/nvc0/gm107_texture.xml.h"

static inline uint32_t
tic_swizzle(const struct nvk_tic_format *fmt,
            VkComponentSwizzle swz, bool is_int)
{
   switch (swz) {
   case VK_COMPONENT_SWIZZLE_R: return fmt->src_x;
   case VK_COMPONENT_SWIZZLE_G: return fmt->src_y;
   case VK_COMPONENT_SWIZZLE_B: return fmt->src_z;
   case VK_COMPONENT_SWIZZLE_A: return fmt->src_w;
   case VK_COMPONENT_SWIZZLE_ONE:
      return is_int ? G80_TIC_SOURCE_ONE_INT : G80_TIC_SOURCE_ONE_FLOAT;
   case VK_COMPONENT_SWIZZLE_ZERO:
      return G80_TIC_SOURCE_ZERO;
   default:
      unreachable("Invalid component swizzle");
   }
}

static uint32_t
gm107_tic2_0_format(VkFormat format, VkComponentMapping swizzle)
{
   const enum pipe_format p_format = vk_format_to_pipe_format(format);
   const struct nvk_tic_format *fmt = &pipe_to_nvk_tic_format[p_format];
   const bool is_int = util_format_is_pure_integer(p_format);

   const uint32_t swiz_x = tic_swizzle(fmt, swizzle.r, is_int);
   const uint32_t swiz_y = tic_swizzle(fmt, swizzle.g, is_int);
   const uint32_t swiz_z = tic_swizzle(fmt, swizzle.b, is_int);
   const uint32_t swiz_w = tic_swizzle(fmt, swizzle.a, is_int);

   uint32_t tic;
   tic  = fmt->comp_sizes << GM107_TIC2_0_COMPONENTS_SIZES__SHIFT;
   tic |= fmt->type_r << GM107_TIC2_0_R_DATA_TYPE__SHIFT;
   tic |= fmt->type_g << GM107_TIC2_0_G_DATA_TYPE__SHIFT;
   tic |= fmt->type_b << GM107_TIC2_0_B_DATA_TYPE__SHIFT;
   tic |= fmt->type_a << GM107_TIC2_0_A_DATA_TYPE__SHIFT;
   tic |= swiz_x << GM107_TIC2_0_X_SOURCE__SHIFT;
   tic |= swiz_y << GM107_TIC2_0_Y_SOURCE__SHIFT;
   tic |= swiz_z << GM107_TIC2_0_Z_SOURCE__SHIFT;
   tic |= swiz_w << GM107_TIC2_0_W_SOURCE__SHIFT;

   return tic;
}

static uint32_t
gm107_tic2_4_view_type(VkImageViewType vk_type)
{
   switch (vk_type) {
   case VK_IMAGE_VIEW_TYPE_1D:
      return GM107_TIC2_4_TEXTURE_TYPE_ONE_D;
   case VK_IMAGE_VIEW_TYPE_2D:
      return GM107_TIC2_4_TEXTURE_TYPE_TWO_D;
   case VK_IMAGE_VIEW_TYPE_3D:
      return GM107_TIC2_4_TEXTURE_TYPE_THREE_D;
   case VK_IMAGE_VIEW_TYPE_CUBE:
      return GM107_TIC2_4_TEXTURE_TYPE_CUBEMAP;
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_ONE_D_ARRAY;
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_TWO_D_ARRAY;
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      return GM107_TIC2_4_TEXTURE_TYPE_CUBE_ARRAY;
   default:
      unreachable("Invalid image view type");
   }
}

static uint32_t
gm107_tic7_4_multi_sample_count(VkSampleCountFlagBits samples)
{
   switch (samples) {
   case VK_SAMPLE_COUNT_1_BIT:
      return GM107_TIC2_7_MULTI_SAMPLE_COUNT_1X1;
   case VK_SAMPLE_COUNT_2_BIT:
      return GM107_TIC2_7_MULTI_SAMPLE_COUNT_2X1;
   case VK_SAMPLE_COUNT_4_BIT:
      return GM107_TIC2_7_MULTI_SAMPLE_COUNT_2X2;
   case VK_SAMPLE_COUNT_8_BIT:
      return GM107_TIC2_7_MULTI_SAMPLE_COUNT_4X2;
   case VK_SAMPLE_COUNT_16_BIT:
      return GM107_TIC2_7_MULTI_SAMPLE_COUNT_4X4;
   default:
      unreachable("Unsupported sample count");
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

   uint32_t tic[8] = { 0, };

   tic[0] = gm107_tic2_0_format(view->vk.view_format, view->vk.swizzle);

   tic[3] |= GM107_TIC2_3_LOD_ANISO_QUALITY_2;
   tic[4] |= GM107_TIC2_4_SECTOR_PROMOTION_PROMOTE_TO_2_V;
   tic[4] |= GM107_TIC2_4_BORDER_SIZE_SAMPLER_COLOR;

   if (vk_format_is_srgb(view->vk.view_format))
      tic[4] |= GM107_TIC2_4_SRGB_CONVERSION;

   /* TODO: Unnormalized? */
   tic[5] |= GM107_TIC2_5_NORMALIZED_COORDS;

   /* TODO: What about GOBS_PER_BLOCK_WIDTH? */
   tic[2] |= GM107_TIC2_2_HEADER_VERSION_BLOCKLINEAR;
   tic[3] |= image->level[0].tile.y << GM107_TIC2_3_GOBS_PER_BLOCK_HEIGHT__SHIFT;
   tic[3] |= image->level[0].tile.z << GM107_TIC2_3_GOBS_PER_BLOCK_DEPTH__SHIFT;

   uint64_t address = nvk_image_base_address(image, 0);
   tic[1]  = address;
   tic[2] |= address >> 32;

   tic[4] |= gm107_tic2_4_view_type(view->vk.view_type);

   /* TODO: NV50_TEXVIEW_FILTER_MSAA8 */
   tic[3] |= GM107_TIC2_3_LOD_ANISO_QUALITY_HIGH |
             GM107_TIC2_3_LOD_ISO_QUALITY_HIGH;

   uint32_t depth;
   if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_3D) {
      depth = view->vk.extent.depth;
   } else if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
              view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
      depth = view->vk.layer_count / 6;
   } else {
      depth = view->vk.layer_count;
   }

   tic[4] |= view->vk.extent.width - 1;
   tic[5] |= view->vk.extent.height - 1;
   tic[5] |= (depth - 1) << 16;

   const uint32_t last_level = view->vk.base_mip_level +
                               view->vk.level_count - 1;
   tic[3] |= last_level << GM107_TIC2_3_MAX_MIP_LEVEL__SHIFT;

   tic[6] |= GM107_TIC2_6_ANISO_FINE_SPREAD_FUNC_TWO;
   tic[6] |= GM107_TIC2_6_ANISO_COARSE_SPREAD_FUNC_ONE;

   tic[7] |= (last_level << 4) | view->vk.base_mip_level;
   tic[7] |= gm107_tic7_4_multi_sample_count(image->vk.samples);

   assert(sizeof(tic) == device->images.desc_size);
   memcpy(desc_map, tic, sizeof(tic));

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

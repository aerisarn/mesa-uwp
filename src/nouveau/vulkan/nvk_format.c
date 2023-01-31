#include "nvk_format.h"

#include "nvk_buffer_view.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"

#include "nvtypes.h"
#include "classes/cl902d.h"
#include "classes/cl90c0.h"
#include "vulkan/util/vk_enum_defines.h"
#include "vulkan/util/vk_format.h"

/*
 * nvidia names
 *   _: UNORM
 *   F: SFLOAT (and maybe UFLOAT?)
 *   L: SINT and UINT
 *   N: SNORM
 * and for whatever reason, 8 bit format names are in BE order
 *
 * TODO: swizzles
 * TODO: X formats
 * TODO: Y formats
 * TODO: Z formats
 * TODO: O formats
 */

struct nvk_format nvk_formats[] = {
   {
      .vk_format = VK_FORMAT_R8_UNORM,
      .hw_format = 0x0,
      .supports_2d_blit = false,
   },

   {
      .vk_format = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A1R5G5B5,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A2B10G10R10,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A2R10G10B10,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A8B8G8R8_SINT_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8BL8GL8RL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A8B8G8R8_SNORM_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_AN8BN8GN8RN8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A8B8G8R8_UINT_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8BL8GL8RL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_A8B8G8R8_UNORM_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8B8G8R8,
      .supports_2d_blit = true,
   },

   {
      .vk_format = VK_FORMAT_B8G8R8A8_SINT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8RL8GL8BL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_B8G8R8A8_UINT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8RL8GL8BL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_B8G8R8A8_UNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8R8G8B8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
      .hw_format = NV902D_SET_SRC_FORMAT_V_BF10GF11RF11,
      .supports_2d_blit = true,
   },

   {
      .vk_format = VK_FORMAT_R5G6B5_UNORM_PACK16,
      .hw_format = NV902D_SET_SRC_FORMAT_V_R5G6B5,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8_SNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_GN8RN8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8_UNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_G8R8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8B8A8_SINT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8BL8GL8RL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8B8A8_SNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_AN8BN8GN8RN8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8B8A8_UINT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8BL8GL8RL8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R8G8B8A8_UNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_A8B8G8R8,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16_SFLOAT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RF16_GF16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16_SNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RN16_GN16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16_UNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_R16_G16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16B16A16_SFLOAT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RF16_GF16_BF16_AF16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16B16A16_SNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RN16_GN16_BN16_AN16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R16G16B16A16_UNORM,
      .hw_format = NV902D_SET_SRC_FORMAT_V_R16_G16_B16_A16,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R32G32_SFLOAT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RF32_GF32,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .hw_format = NV902D_SET_SRC_FORMAT_V_RF32_GF32_BF32_AF32,
      .supports_2d_blit = true,
   },
   {
      .vk_format = VK_FORMAT_R32_UINT,
      .hw_format = NV90C0_SET_SU_LD_ST_TARGET_FORMAT_COLOR_RU32,
      .supports_2d_blit = false,
   },
   {
      .vk_format = VK_FORMAT_R16_UINT,
      .hw_format = NV90C0_SET_SU_LD_ST_TARGET_FORMAT_COLOR_RU16,
      .supports_2d_blit = false,
   },
};

const struct nvk_format *
nvk_get_format(VkFormat vk_format)
{
   for (unsigned i = 0; i < ARRAY_SIZE(nvk_formats); i++) {
      if (nvk_formats[i].vk_format == vk_format)
         return &nvk_formats[i];
   }

   return NULL;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                       VkFormat format,
                                       VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);

   VkFormatFeatureFlags2 linear2, optimal2, buffer2;
   linear2 = nvk_get_image_format_features(pdevice, format,
                                           VK_IMAGE_TILING_LINEAR);
   optimal2 = nvk_get_image_format_features(pdevice, format,
                                            VK_IMAGE_TILING_OPTIMAL);
   buffer2 = nvk_get_buffer_format_features(pdevice, format);

   pFormatProperties->formatProperties = (VkFormatProperties) {
      .linearTilingFeatures = vk_format_features2_to_features(linear2),
      .optimalTilingFeatures = vk_format_features2_to_features(optimal2),
      .bufferFeatures = vk_format_features2_to_features(buffer2),
   };

   vk_foreach_struct(ext, pFormatProperties->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR: {
         VkFormatProperties3KHR *props = (VkFormatProperties3KHR *)ext;
         props->linearTilingFeatures = linear2;
         props->optimalTilingFeatures = optimal2;
         props->bufferFeatures = buffer2;
         break;
      }

      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

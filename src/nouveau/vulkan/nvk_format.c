#include "nvk_format.h"

#include "nvk_buffer_view.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"

#include "nvtypes.h"
#include "classes/cl902d.h"
#include "classes/cl9097.h"
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

#define VA_FMT(vk_fmt, widths, swap_rb, type) \
   [VK_FORMAT_##vk_fmt] = \
   { NV9097_SET_VERTEX_ATTRIBUTE_A_COMPONENT_BIT_WIDTHS_##widths, \
     NV9097_SET_VERTEX_ATTRIBUTE_A_SWAP_R_AND_B_##swap_rb, \
     NV9097_SET_VERTEX_ATTRIBUTE_A_NUMERICAL_TYPE_NUM_##type }

static const struct nvk_va_format nvk_vf_formats[] = {
   VA_FMT(R8_UNORM,                    R8,               FALSE,   UNORM),
   VA_FMT(R8_SNORM,                    R8,               FALSE,   SNORM),
   VA_FMT(R8_USCALED,                  R8,               FALSE,   USCALED),
   VA_FMT(R8_SSCALED,                  R8,               FALSE,   SSCALED),
   VA_FMT(R8_UINT,                     R8,               FALSE,   UINT),
   VA_FMT(R8_SINT,                     R8,               FALSE,   SINT),

   VA_FMT(R8G8_UNORM,                  R8_G8,            FALSE,   UNORM),
   VA_FMT(R8G8_SNORM,                  R8_G8,            FALSE,   SNORM),
   VA_FMT(R8G8_USCALED,                R8_G8,            FALSE,   USCALED),
   VA_FMT(R8G8_SSCALED,                R8_G8,            FALSE,   SSCALED),
   VA_FMT(R8G8_UINT,                   R8_G8,            FALSE,   UINT),
   VA_FMT(R8G8_SINT,                   R8_G8,            FALSE,   SINT),

   VA_FMT(R8G8B8_UNORM,                R8_G8_B8,         FALSE,   UNORM),
   VA_FMT(R8G8B8_SNORM,                R8_G8_B8,         FALSE,   SNORM),
   VA_FMT(R8G8B8_USCALED,              R8_G8_B8,         FALSE,   USCALED),
   VA_FMT(R8G8B8_SSCALED,              R8_G8_B8,         FALSE,   SSCALED),
   VA_FMT(R8G8B8_UINT,                 R8_G8_B8,         FALSE,   UINT),
   VA_FMT(R8G8B8_SINT,                 R8_G8_B8,         FALSE,   SINT),

   VA_FMT(B8G8R8_UNORM,                R8_G8_B8,         TRUE,    UNORM),
   VA_FMT(B8G8R8_SNORM,                R8_G8_B8,         TRUE,    SNORM),
   VA_FMT(B8G8R8_USCALED,              R8_G8_B8,         TRUE,    USCALED),
   VA_FMT(B8G8R8_SSCALED,              R8_G8_B8,         TRUE,    SSCALED),
   VA_FMT(B8G8R8_UINT,                 R8_G8_B8,         TRUE,    UINT),
   VA_FMT(B8G8R8_SINT,                 R8_G8_B8,         TRUE,    SINT),

   VA_FMT(R8G8B8A8_UNORM,              R8_G8_B8_A8,      FALSE,   UNORM),
   VA_FMT(R8G8B8A8_SNORM,              R8_G8_B8_A8,      FALSE,   SNORM),
   VA_FMT(R8G8B8A8_USCALED,            R8_G8_B8_A8,      FALSE,   USCALED),
   VA_FMT(R8G8B8A8_SSCALED,            R8_G8_B8_A8,      FALSE,   SSCALED),
   VA_FMT(R8G8B8A8_UINT,               R8_G8_B8_A8,      FALSE,   UINT),
   VA_FMT(R8G8B8A8_SINT,               R8_G8_B8_A8,      FALSE,   SINT),

   VA_FMT(B8G8R8A8_UNORM,              R8_G8_B8_A8,      TRUE,   UNORM),
   VA_FMT(B8G8R8A8_SNORM,              R8_G8_B8_A8,      TRUE,   SNORM),
   VA_FMT(B8G8R8A8_USCALED,            R8_G8_B8_A8,      TRUE,   USCALED),
   VA_FMT(B8G8R8A8_SSCALED,            R8_G8_B8_A8,      TRUE,   SSCALED),
   VA_FMT(B8G8R8A8_UINT,               R8_G8_B8_A8,      TRUE,   UINT),
   VA_FMT(B8G8R8A8_SINT,               R8_G8_B8_A8,      TRUE,   SINT),

   VA_FMT(A2R10G10B10_UNORM_PACK32,    A2B10G10R10,      TRUE,    UNORM),
   VA_FMT(A2R10G10B10_SNORM_PACK32,    A2B10G10R10,      TRUE,    SNORM),
   VA_FMT(A2R10G10B10_USCALED_PACK32,  A2B10G10R10,      TRUE,    USCALED),
   VA_FMT(A2R10G10B10_SSCALED_PACK32,  A2B10G10R10,      TRUE,    SSCALED),
   VA_FMT(A2R10G10B10_UINT_PACK32,     A2B10G10R10,      TRUE,    UINT),
   VA_FMT(A2R10G10B10_SINT_PACK32,     A2B10G10R10,      TRUE,    SINT),

   VA_FMT(A2B10G10R10_UNORM_PACK32,    A2B10G10R10,      FALSE,   UNORM),
   VA_FMT(A2B10G10R10_SNORM_PACK32,    A2B10G10R10,      FALSE,   SNORM),
   VA_FMT(A2B10G10R10_USCALED_PACK32,  A2B10G10R10,      FALSE,   USCALED),
   VA_FMT(A2B10G10R10_SSCALED_PACK32,  A2B10G10R10,      FALSE,   SSCALED),
   VA_FMT(A2B10G10R10_UINT_PACK32,     A2B10G10R10,      FALSE,   UINT),
   VA_FMT(A2B10G10R10_SINT_PACK32,     A2B10G10R10,      FALSE,   SINT),

   VA_FMT(R16_UNORM,                   R16,              FALSE,   UNORM),
   VA_FMT(R16_SNORM,                   R16,              FALSE,   SNORM),
   VA_FMT(R16_USCALED,                 R16,              FALSE,   USCALED),
   VA_FMT(R16_SSCALED,                 R16,              FALSE,   SSCALED),
   VA_FMT(R16_UINT,                    R16,              FALSE,   UINT),
   VA_FMT(R16_SINT,                    R16,              FALSE,   SINT),
   VA_FMT(R16_SFLOAT,                  R16,              FALSE,   FLOAT),

   VA_FMT(R16G16_UNORM,                R16_G16,          FALSE,   UNORM),
   VA_FMT(R16G16_SNORM,                R16_G16,          FALSE,   SNORM),
   VA_FMT(R16G16_USCALED,              R16_G16,          FALSE,   USCALED),
   VA_FMT(R16G16_SSCALED,              R16_G16,          FALSE,   SSCALED),
   VA_FMT(R16G16_UINT,                 R16_G16,          FALSE,   UINT),
   VA_FMT(R16G16_SINT,                 R16_G16,          FALSE,   SINT),
   VA_FMT(R16G16_SFLOAT,               R16_G16,          FALSE,   FLOAT),

   VA_FMT(R16G16B16_UNORM,             R16_G16_B16,      FALSE,   UNORM),
   VA_FMT(R16G16B16_SNORM,             R16_G16_B16,      FALSE,   SNORM),
   VA_FMT(R16G16B16_USCALED,           R16_G16_B16,      FALSE,   USCALED),
   VA_FMT(R16G16B16_SSCALED,           R16_G16_B16,      FALSE,   SSCALED),
   VA_FMT(R16G16B16_UINT,              R16_G16_B16,      FALSE,   UINT),
   VA_FMT(R16G16B16_SINT,              R16_G16_B16,      FALSE,   SINT),
   VA_FMT(R16G16B16_SFLOAT,            R16_G16_B16,      FALSE,   FLOAT),

   VA_FMT(R16G16B16A16_UNORM,          R16_G16_B16_A16,  FALSE,   UNORM),
   VA_FMT(R16G16B16A16_SNORM,          R16_G16_B16_A16,  FALSE,   SNORM),
   VA_FMT(R16G16B16A16_USCALED,        R16_G16_B16_A16,  FALSE,   USCALED),
   VA_FMT(R16G16B16A16_SSCALED,        R16_G16_B16_A16,  FALSE,   SSCALED),
   VA_FMT(R16G16B16A16_UINT,           R16_G16_B16_A16,  FALSE,   UINT),
   VA_FMT(R16G16B16A16_SINT,           R16_G16_B16_A16,  FALSE,   SINT),
   VA_FMT(R16G16B16A16_SFLOAT,         R16_G16_B16_A16,  FALSE,   FLOAT),

   VA_FMT(R32_UINT,                    R32,              FALSE,   UINT),
   VA_FMT(R32_SINT,                    R32,              FALSE,   SINT),
   VA_FMT(R32_SFLOAT,                  R32,              FALSE,   FLOAT),

   VA_FMT(R32G32_UINT,                 R32_G32,          FALSE,   UINT),
   VA_FMT(R32G32_SINT,                 R32_G32,          FALSE,   SINT),
   VA_FMT(R32G32_SFLOAT,               R32_G32,          FALSE,   FLOAT),

   VA_FMT(R32G32B32_UINT,              R32_G32_B32,      FALSE,   UINT),
   VA_FMT(R32G32B32_SINT,              R32_G32_B32,      FALSE,   SINT),
   VA_FMT(R32G32B32_SFLOAT,            R32_G32_B32,      FALSE,   FLOAT),

   VA_FMT(R32G32B32A32_UINT,           R32_G32_B32_A32,  FALSE,   UINT),
   VA_FMT(R32G32B32A32_SINT,           R32_G32_B32_A32,  FALSE,   SINT),
   VA_FMT(R32G32B32A32_SFLOAT,         R32_G32_B32_A32,  FALSE,   FLOAT),
};

#undef VA_FMT

const struct nvk_va_format *
nvk_get_va_format(const struct nvk_physical_device *pdev, VkFormat format)
{
   if (format > ARRAY_SIZE(nvk_vf_formats))
      return NULL;

   if (nvk_vf_formats[format].bit_widths == 0)
      return NULL;

   return &nvk_vf_formats[format];
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

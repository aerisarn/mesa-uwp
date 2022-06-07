#include "nvk_format.h"

#include "nvtypes.h"
#include "classes/cl902d.h"

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

struct nvk_format nvk_formats[NVK_FORMATS] = {
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
};

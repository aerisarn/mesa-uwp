#include "nvk_sampler.h"

#include "nvk_device.h"
#include "util/format_srgb.h"
#include "vulkan/runtime/vk_sampler.h"

#include "gallium/drivers/nouveau/nv50/g80_texture.xml.h"

static inline uint32_t
g80_tsc_wrap_mode(VkSamplerAddressMode addr_mode)
{
   switch (addr_mode) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:
      return G80_TSC_WRAP_WRAP;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
      return G80_TSC_WRAP_MIRROR;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
      return G80_TSC_WRAP_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      return G80_TSC_WRAP_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      return G80_TSC_WRAP_MIRROR_ONCE_CLAMP_TO_EDGE;
   default:
      unreachable("Invalid sampler address mode");
   }
}

static uint32_t
g80_tsc_0_depth_compare_func(VkCompareOp op)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_NEVER;
   case VK_COMPARE_OP_LESS:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_LESS;
   case VK_COMPARE_OP_EQUAL:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_LEQUAL;
   case VK_COMPARE_OP_GREATER:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_NOTEQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_GEQUAL;
   case VK_COMPARE_OP_ALWAYS:
      return G80_TSC_0_DEPTH_COMPARE_FUNC_ALWAYS;
   default:
      unreachable("Invalid compare op");
   }
}

static uint32_t
g80_tsc_0_max_anisotropy(float max_anisotropy)
{
   if (max_anisotropy >= 16)
      return G80_TSC_0_MAX_ANISOTROPY_16_TO_1;

   if (max_anisotropy >= 12)
      return G80_TSC_0_MAX_ANISOTROPY_12_TO_1;

   uint32_t aniso_u32 = MAX2(0.0f, max_anisotropy);
   return (aniso_u32 >> 1) << 20;
}

static uint32_t
g80_tsc_1_trilin_opt(float max_anisotropy)
{
   /* No idea if we want this but matching nouveau */
   if (max_anisotropy >= 12)
      return 0;

   if (max_anisotropy >= 4)
      return 6 << G80_TSC_1_TRILIN_OPT__SHIFT;

   if (max_anisotropy >= 2)
      return 4 << G80_TSC_1_TRILIN_OPT__SHIFT;

   return 0;
}

static VkSamplerReductionMode
vk_sampler_create_reduction_mode(const VkSamplerCreateInfo *pCreateInfo)
{
   const VkSamplerReductionModeCreateInfo *reduction =
      vk_find_struct_const(pCreateInfo->pNext,
                           SAMPLER_REDUCTION_MODE_CREATE_INFO);
   if (reduction == NULL)
      return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;

   return reduction->reductionMode;
}

static uint32_t
to_sfixed(float f, unsigned int_bits, unsigned frac_bits)
{
   int min = -(1 << (int_bits - 1));
   int max = (1 << (int_bits - 1)) - 1;
   f = CLAMP(f, (float)min, (float)max);

   int fixed = f * (float)(1 << frac_bits);

   /* It's a uint so mask of high bits */
   return fixed & ((1 << (int_bits + frac_bits)) - 1);
}

static uint32_t
to_ufixed(float f, unsigned int_bits, unsigned frac_bits)
{
   unsigned max = (1 << int_bits) - 1;
   f = CLAMP(f, 0.0f, (float)max);

   int fixed = f * (float)(1 << frac_bits);

   assert((uint32_t)fixed <= UINT32_MAX);
   return fixed;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateSampler(VkDevice _device,
                  const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkSampler *pSampler)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_sampler *sampler;

   sampler = vk_object_zalloc(&device->vk, pAllocator, sizeof(*sampler),
                              VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t *desc_map = nvk_descriptor_table_alloc(device, &device->samplers,
                                                   &sampler->desc_index);
   if (desc_map == NULL) {
      vk_object_free(&device->vk, pAllocator, sampler);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate image descriptor");
   }

   uint32_t tsc[8] = {};
   tsc[0] |= 0x00024000; /* Font filter? */
   tsc[0] |= G80_TSC_0_SRGB_CONVERSION;
   tsc[0] |= g80_tsc_wrap_mode(pCreateInfo->addressModeU)
             << G80_TSC_0_ADDRESS_U__SHIFT;
   tsc[0] |= g80_tsc_wrap_mode(pCreateInfo->addressModeV)
             << G80_TSC_0_ADDRESS_V__SHIFT;
   tsc[0] |= g80_tsc_wrap_mode(pCreateInfo->addressModeW)
             << G80_TSC_0_ADDRESS_P__SHIFT;

   if (pCreateInfo->compareEnable) {
      tsc[0] |= G80_TSC_0_DEPTH_COMPARE;
      tsc[0] |= g80_tsc_0_depth_compare_func(pCreateInfo->compareOp);
   }

   tsc[0] |= g80_tsc_0_max_anisotropy(pCreateInfo->maxAnisotropy);
   tsc[1] |= g80_tsc_1_trilin_opt(pCreateInfo->maxAnisotropy);

   switch (pCreateInfo->magFilter) {
   case VK_FILTER_NEAREST:
      tsc[1] |= G80_TSC_1_MAG_FILTER_NEAREST;
      break;
   case VK_FILTER_LINEAR:
      tsc[1] |= G80_TSC_1_MAG_FILTER_LINEAR;
      break;
   default:
      unreachable("Invalid filter");
   }

   switch (pCreateInfo->minFilter) {
   case VK_FILTER_NEAREST:
      tsc[1] |= G80_TSC_1_MIN_FILTER_NEAREST;
      break;
   case VK_FILTER_LINEAR:
      tsc[1] |= G80_TSC_1_MIN_FILTER_LINEAR;
      break;
   default:
      unreachable("Invalid filter");
   }

   switch (pCreateInfo->mipmapMode) {
   case VK_SAMPLER_MIPMAP_MODE_NEAREST:
      tsc[1] |= G80_TSC_1_MIP_FILTER_NEAREST;
      break;
   case VK_SAMPLER_MIPMAP_MODE_LINEAR:
      tsc[1] |= G80_TSC_1_MIP_FILTER_LINEAR;
      break;
   default:
      unreachable("Invalid mipmap mode");
   }

   tsc[1] |= GK104_TSC_1_CUBEMAP_INTERFACE_FILTERING;
   if (pCreateInfo->unnormalizedCoordinates)
      tsc[1] |= GK104_TSC_1_FLOAT_COORD_NORMALIZATION_FORCE_UNNORMALIZED_COORDS;

   switch (vk_sampler_create_reduction_mode(pCreateInfo)) {
   case VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE:
      tsc[1] |= GM204_TSC_1_REDUCTION_MODE_WEIGHTED_AVERAGE;
      break;
   case VK_SAMPLER_REDUCTION_MODE_MIN:
      tsc[1] |= GM204_TSC_1_REDUCTION_MODE_MIN;
      break;
   case VK_SAMPLER_REDUCTION_MODE_MAX:
      tsc[1] |= GM204_TSC_1_REDUCTION_MODE_MAX;
      break;
   default:
      unreachable("Invalid reduction mode");
   }

   tsc[1] |= to_sfixed(pCreateInfo->mipLodBias, 5, 8)
             << G80_TSC_1_MIP_LOD_BIAS__SHIFT;
   tsc[2] |= to_ufixed(pCreateInfo->minLod, 4, 8)
             << G80_TSC_2_MIN_LOD_CLAMP__SHIFT;
   tsc[2] |= to_ufixed(pCreateInfo->maxLod, 4, 8)
             << G80_TSC_2_MAX_LOD_CLAMP__SHIFT;

   const VkClearColorValue bc =
      vk_sampler_border_color_value(pCreateInfo, NULL);
   uint32_t bc_srgb[3];
   for (unsigned i = 0; i < 3; i++)
      bc_srgb[i] = util_format_linear_float_to_srgb_8unorm(bc.float32[i]);

   tsc[2] |= bc_srgb[0] << G80_TSC_2_SRGB_BORDER_COLOR_R__SHIFT;
   tsc[3] |= bc_srgb[1] << G80_TSC_3_SRGB_BORDER_COLOR_G__SHIFT;
   tsc[3] |= bc_srgb[2] << G80_TSC_3_SRGB_BORDER_COLOR_B__SHIFT;
   for (unsigned i = 0; i < 4; i++)
      tsc[i + 4] = bc.uint32[i];

   memcpy(desc_map, tsc, sizeof(tsc));

   *pSampler = nvk_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroySampler(VkDevice _device,
                   VkSampler _sampler,
                   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_sampler, sampler, _sampler);

   if (!sampler)
      return;

   vk_object_free(&device->vk, pAllocator, sampler);
}

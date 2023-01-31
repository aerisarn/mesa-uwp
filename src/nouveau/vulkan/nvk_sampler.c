#include "nvk_sampler.h"

#include "nvk_device.h"
#include "nouveau_context.h"

#include "util/bitpack_helpers.h"
#include "util/format_srgb.h"
#include "vulkan/runtime/vk_sampler.h"

#include "cla097.h"
#include "clb197.h"
#include "cl9097tex.h"
#include "cla097tex.h"
#include "clb197tex.h"
#include "drf.h"

ALWAYS_INLINE static void
__set_u32(uint32_t *o, uint32_t v, unsigned lo, unsigned hi)
{
   assert(lo <= hi && hi < 32);
   *o |= util_bitpack_uint(v, lo % 32, hi % 32);
}

#define FIXED_FRAC_BITS 8

ALWAYS_INLINE static void
__set_ufixed(uint32_t *o, float v, unsigned lo, unsigned hi)
{
   assert(lo <= hi && hi < 32);
   *o |= util_bitpack_ufixed_clamp(v, lo % 32, hi % 32, FIXED_FRAC_BITS);
}

ALWAYS_INLINE static void
__set_sfixed(uint32_t *o, float v, unsigned lo, unsigned hi)
{
   assert(lo <= hi && hi < 32);
   *o |= util_bitpack_sfixed_clamp(v, lo % 32, hi % 32, FIXED_FRAC_BITS);
}

ALWAYS_INLINE static void
__set_bool(uint32_t *o, bool b, unsigned lo, unsigned hi)
{
   assert(lo == hi && hi < 32);
   *o |= util_bitpack_uint(b, lo % 32, hi % 32);
}

#define MW(x) x

#define SAMP_SET_U(o, NV, i, FIELD, val) \
   __set_u32(&(o)[i], (val), DRF_LO(NV##_TEXSAMP##i##_##FIELD),\
                             DRF_HI(NV##_TEXSAMP##i##_##FIELD))

#define SAMP_SET_UF(o, NV, i, FIELD, val) \
   __set_ufixed(&(o)[i], (val), DRF_LO(NV##_TEXSAMP##i##_##FIELD),\
                                DRF_HI(NV##_TEXSAMP##i##_##FIELD))

#define SAMP_SET_SF(o, NV, i, FIELD, val) \
   __set_sfixed(&(o)[i], (val), DRF_LO(NV##_TEXSAMP##i##_##FIELD),\
                                DRF_HI(NV##_TEXSAMP##i##_##FIELD))

#define SAMP_SET_B(o, NV, i, FIELD, b) \
   __set_bool(&(o)[i], (b), DRF_LO(NV##_TEXSAMP##i##_##FIELD),\
                            DRF_HI(NV##_TEXSAMP##i##_##FIELD))

#define SAMP_SET_E(o, NV, i, FIELD, E) \
   SAMP_SET_U((o), NV, i, FIELD, NV##_TEXSAMP##i##_##FIELD##_##E)

static inline uint32_t
vk_to_9097_address_mode(VkSamplerAddressMode addr_mode)
{
#define MODE(VK, NV) \
   [VK_SAMPLER_ADDRESS_MODE_##VK] = NV9097_TEXSAMP0_ADDRESS_U_##NV
   static const uint8_t vk_to_9097[] = {
      MODE(REPEAT,               WRAP),
      MODE(MIRRORED_REPEAT,      MIRROR),
      MODE(CLAMP_TO_EDGE,        CLAMP_TO_EDGE),
      MODE(CLAMP_TO_BORDER,      BORDER),
      MODE(MIRROR_CLAMP_TO_EDGE, MIRROR_ONCE_CLAMP_TO_EDGE),
   };
#undef MODE

   assert(addr_mode < ARRAY_SIZE(vk_to_9097));
   return vk_to_9097[addr_mode];
}

static uint32_t
vk_to_9097_texsamp_compare_op(VkCompareOp op)
{
#define OP(VK, NV) \
   [VK_COMPARE_OP_##VK] = NV9097_TEXSAMP0_DEPTH_COMPARE_FUNC_##NV
   ASSERTED static const uint8_t vk_to_9097[] = {
      OP(NEVER,            ZC_NEVER),
      OP(LESS,             ZC_LESS),
      OP(EQUAL,            ZC_EQUAL),
      OP(LESS_OR_EQUAL,    ZC_LEQUAL),
      OP(GREATER,          ZC_GREATER),
      OP(NOT_EQUAL,        ZC_NOTEQUAL),
      OP(GREATER_OR_EQUAL, ZC_GEQUAL),
      OP(ALWAYS,           ZC_ALWAYS),
   };
#undef OP

   assert(op < ARRAY_SIZE(vk_to_9097));
   assert(op == vk_to_9097[op]);

   return op;
}

static uint32_t
vk_to_9097_max_anisotropy(float max_anisotropy)
{
   if (max_anisotropy >= 16)
      return NV9097_TEXSAMP0_MAX_ANISOTROPY_ANISO_16_TO_1;

   if (max_anisotropy >= 12)
      return NV9097_TEXSAMP0_MAX_ANISOTROPY_ANISO_12_TO_1;

   uint32_t aniso_u32 = MAX2(0.0f, max_anisotropy);
   return aniso_u32 >> 1;
}

static uint32_t
vk_to_9097_trilin_opt(float max_anisotropy)
{
   /* No idea if we want this but matching nouveau */
   if (max_anisotropy >= 12)
      return 0;

   if (max_anisotropy >= 4)
      return 6;

   if (max_anisotropy >= 2)
      return 4;

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

   uint32_t samp[8] = {};
   SAMP_SET_U(samp, NV9097, 0, ADDRESS_U,
              vk_to_9097_address_mode(pCreateInfo->addressModeU));
   SAMP_SET_U(samp, NV9097, 0, ADDRESS_V,
              vk_to_9097_address_mode(pCreateInfo->addressModeV));
   SAMP_SET_U(samp, NV9097, 0, ADDRESS_P,
              vk_to_9097_address_mode(pCreateInfo->addressModeW));

   if (pCreateInfo->compareEnable) {
      SAMP_SET_B(samp, NV9097, 0, DEPTH_COMPARE, true);
      SAMP_SET_U(samp, NV9097, 0, DEPTH_COMPARE_FUNC,
                 vk_to_9097_texsamp_compare_op(pCreateInfo->compareOp));
   }

   SAMP_SET_B(samp, NV9097, 0, S_R_G_B_CONVERSION, true);
   SAMP_SET_E(samp, NV9097, 0, FONT_FILTER_WIDTH, SIZE_2);
   SAMP_SET_E(samp, NV9097, 0, FONT_FILTER_HEIGHT, SIZE_2);

   if (pCreateInfo->anisotropyEnable) {
      SAMP_SET_U(samp, NV9097, 0, MAX_ANISOTROPY,
                 vk_to_9097_max_anisotropy(pCreateInfo->maxAnisotropy));
   }

   switch (pCreateInfo->magFilter) {
   case VK_FILTER_NEAREST:
      SAMP_SET_E(samp, NV9097, 1, MAG_FILTER, MAG_POINT);
      break;
   case VK_FILTER_LINEAR:
      SAMP_SET_E(samp, NV9097, 1, MAG_FILTER, MAG_LINEAR);
      break;
   default:
      unreachable("Invalid filter");
   }

   switch (pCreateInfo->minFilter) {
   case VK_FILTER_NEAREST:
      SAMP_SET_E(samp, NV9097, 1, MIN_FILTER, MIN_POINT);
      break;
   case VK_FILTER_LINEAR:
      if (pCreateInfo->anisotropyEnable)
         SAMP_SET_E(samp, NV9097, 1, MIN_FILTER, MIN_ANISO);
      else
         SAMP_SET_E(samp, NV9097, 1, MIN_FILTER, MIN_LINEAR);
      break;
   default:
      unreachable("Invalid filter");
   }

   switch (pCreateInfo->mipmapMode) {
   case VK_SAMPLER_MIPMAP_MODE_NEAREST:
      SAMP_SET_E(samp, NV9097, 1, MIP_FILTER, MIP_POINT);
      break;
   case VK_SAMPLER_MIPMAP_MODE_LINEAR:
      SAMP_SET_E(samp, NV9097, 1, MIP_FILTER, MIP_LINEAR);
      break;
   default:
      unreachable("Invalid mipmap mode");
   }

   assert(device->ctx->eng3d.cls >= KEPLER_A);
   SAMP_SET_E(samp, NVA097, 1, CUBEMAP_INTERFACE_FILTERING, AUTO_SPAN_SEAM);

   if (device->ctx->eng3d.cls >= MAXWELL_B) {
      switch (vk_sampler_create_reduction_mode(pCreateInfo)) {
      case VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE:
         SAMP_SET_E(samp, NVB197, 1, REDUCTION_FILTER, RED_NONE);
         break;
      case VK_SAMPLER_REDUCTION_MODE_MIN:
         SAMP_SET_E(samp, NVB197, 1, REDUCTION_FILTER, RED_MINIMUM);
         break;
      case VK_SAMPLER_REDUCTION_MODE_MAX:
         SAMP_SET_E(samp, NVB197, 1, REDUCTION_FILTER, RED_MAXIMUM);
         break;
      default:
         unreachable("Invalid reduction mode");
      }
   }

   SAMP_SET_SF(samp, NV9097, 1, MIP_LOD_BIAS, pCreateInfo->mipLodBias);

   assert(device->ctx->eng3d.cls >= KEPLER_A);
   if (pCreateInfo->unnormalizedCoordinates) {
      SAMP_SET_E(samp, NVA097, 1, FLOAT_COORD_NORMALIZATION,
                                  FORCE_UNNORMALIZED_COORDS);
   } else {
      SAMP_SET_E(samp, NVA097, 1, FLOAT_COORD_NORMALIZATION,
                                  USE_HEADER_SETTING);
   }
   SAMP_SET_U(samp, NV9097, 1, TRILIN_OPT,
              vk_to_9097_trilin_opt(pCreateInfo->maxAnisotropy));

   SAMP_SET_UF(samp, NV9097, 2, MIN_LOD_CLAMP, pCreateInfo->minLod);
   SAMP_SET_UF(samp, NV9097, 2, MAX_LOD_CLAMP, pCreateInfo->maxLod);

   const VkClearColorValue bc =
      vk_sampler_border_color_value(pCreateInfo, NULL);
   uint8_t bc_srgb[3];
   for (unsigned i = 0; i < 3; i++)
      bc_srgb[i] = util_format_linear_float_to_srgb_8unorm(bc.float32[i]);

   SAMP_SET_U(samp, NV9097, 2, S_R_G_B_BORDER_COLOR_R, bc_srgb[0]);
   SAMP_SET_U(samp, NV9097, 3, S_R_G_B_BORDER_COLOR_G, bc_srgb[1]);
   SAMP_SET_U(samp, NV9097, 3, S_R_G_B_BORDER_COLOR_B, bc_srgb[2]);

   SAMP_SET_U(samp, NV9097, 4, BORDER_COLOR_R, bc.uint32[0]);
   SAMP_SET_U(samp, NV9097, 5, BORDER_COLOR_G, bc.uint32[1]);
   SAMP_SET_U(samp, NV9097, 6, BORDER_COLOR_B, bc.uint32[2]);
   SAMP_SET_U(samp, NV9097, 7, BORDER_COLOR_A, bc.uint32[3]);

   memcpy(desc_map, samp, sizeof(samp));

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

   nvk_descriptor_table_free(device, &device->samplers, sampler->desc_index);
   vk_object_free(&device->vk, pAllocator, sampler);
}

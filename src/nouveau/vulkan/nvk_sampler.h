#ifndef NVK_SAMPLER_H
#define NVK_SAMPLER_H 1

#include "nvk_private.h"
#include "nvk_physical_device.h"

#include "vulkan/runtime/vk_sampler.h"

struct nvk_sampler {
   struct vk_sampler vk;

   uint32_t desc_index;

   /** Number of planes for multi-plane images.
    * Hard-coded as 1 as a placeholder until YCbCr conversion
    * structs are implemented
    */
   uint8_t plane_count;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

static void
nvk_sampler_fill_header(const struct nvk_physical_device *pdev,
                        const struct VkSamplerCreateInfo *info,
                        const struct vk_sampler *vk_sampler,
                        uint32_t *samp);

#endif

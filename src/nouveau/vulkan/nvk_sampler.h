#ifndef NVK_SAMPLER
#define NVK_SAMPLER 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_object.h"

struct nvk_sampler {
   struct vk_object_base base;
};

VK_DEFINE_HANDLE_CASTS(nvk_sampler, base, VkSampler, VK_OBJECT_TYPE_SAMPLER)

#endif

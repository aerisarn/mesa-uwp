#ifndef NVK_DESCRIPTOR_SET
#define NVK_DESCRIPTOR_SET 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_object.h"

struct nvk_descriptor_set_layout;

struct nvk_image_descriptor {
   unsigned image_index:20;
   unsigned sampler_index:12;
};

/* This has to match nir_address_format_64bit_bounded_global */
struct nvk_buffer_address {
   uint64_t base_addr;
   uint32_t size;
   uint32_t zero; /* Must be zero! */
};

struct nvk_descriptor_set {
   struct vk_object_base base;

   struct nvk_descriptor_set_layout *layout;

   void *map;
};

VK_DEFINE_HANDLE_CASTS(nvk_descriptor_set, base, VkDescriptorSet,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET)

#endif

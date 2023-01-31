#ifndef NVK_DESCRIPTOR_SET
#define NVK_DESCRIPTOR_SET 1

#include "nvk_private.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"
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

struct nvk_descriptor_pool_entry {
   uint32_t offset;
   uint32_t size;
   struct nvk_descriptor_set *set;
};

struct nvk_descriptor_pool {
   struct vk_object_base base;
   struct nouveau_ws_bo *bo;
   uint8_t *mapped_ptr;
   uint64_t current_offset;
   uint64_t size;
   uint32_t entry_count;
   uint32_t max_entry_count;
   struct nvk_descriptor_pool_entry entries[0];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

struct nvk_descriptor_set {
   struct vk_object_base base;
   struct nvk_descriptor_set_layout *layout;
   uint32_t buffer_count;
   uint32_t bo_offset;
   struct nouveau_ws_bo *bo;
   void *mapped_ptr;
};

VK_DEFINE_HANDLE_CASTS(nvk_descriptor_set, base, VkDescriptorSet,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET)

static void
nvk_push_descriptor_set_ref(struct nouveau_ws_push *push,
                            const struct nvk_descriptor_set *set)
{
   if (set->bo)
      nouveau_ws_push_ref(push, set->bo, NOUVEAU_WS_BO_RD);
}

static inline uint64_t
nvk_descriptor_set_addr(const struct nvk_descriptor_set *set)
{
   return set->bo->offset + set->bo_offset;
}

#endif

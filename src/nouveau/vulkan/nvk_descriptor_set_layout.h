#ifndef NVK_DESCRIPTOR_SET_LAYOUT
#define NVK_DESCRIPTOR_SET_LAYOUT 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_object.h"

struct nvk_device;
struct nvk_sampler;

struct nvk_descriptor_set_binding_layout {
  /* The type of the descriptors in this binding */
  VkDescriptorType type;

  /* Flags provided when this binding was created */
  VkDescriptorBindingFlags flags;

  /* Number of array elements in this binding (or size in bytes for inline
   * uniform data)
   */
  uint32_t array_size;

  /* Offset into the descriptor buffer where this descriptor lives */
  uint32_t offset;

  /* Stride between array elements in the descriptor buffer */
  uint32_t stride;

  /* Immutable samplers (or NULL if no immutable samplers) */
  struct nvk_sampler **immutable_samplers;
};

struct nvk_descriptor_set_layout {
  struct vk_object_base base;

  uint32_t ref_cnt;

  unsigned char sha1[20];

  /* Size of the descriptor buffer for this descriptor set */
  uint32_t descriptor_buffer_size;

  /* Number of bindings in this descriptor set */
  uint32_t binding_count;

  /* Bindings in this descriptor set */
  struct nvk_descriptor_set_binding_layout binding[0];
};

VK_DEFINE_HANDLE_CASTS(nvk_descriptor_set_layout, base, VkDescriptorSetLayout,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

void nvk_descriptor_set_layout_destroy(
    struct nvk_device *device, struct nvk_descriptor_set_layout *layout);

static inline struct nvk_descriptor_set_layout *
nvk_descriptor_set_layout_ref(struct nvk_descriptor_set_layout *layout) {
  assert(layout && layout->ref_cnt >= 1);
  p_atomic_inc(&layout->ref_cnt);
  return layout;
}

static inline void
nvk_descriptor_set_layout_unref(struct nvk_device *device,
                                struct nvk_descriptor_set_layout *layout) {
  assert(layout && layout->ref_cnt >= 1);
  if (p_atomic_dec_zero(&layout->ref_cnt))
    nvk_descriptor_set_layout_destroy(device, layout);
}

#endif

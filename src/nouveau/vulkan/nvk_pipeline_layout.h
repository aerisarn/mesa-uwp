#ifndef NVK_PIPELINE_LAYOUT
#define NVK_PIPELINE_LAYOUT 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_object.h"

struct nvk_descriptor_set_layout;

struct nvk_pipeline_layout {
  struct vk_object_base base;

  unsigned char sha1[20];

  uint32_t num_sets;

  struct {
    struct nvk_descriptor_set_layout *layout;
  } set[NVK_MAX_SETS];
};

VK_DEFINE_HANDLE_CASTS(nvk_pipeline_layout, base, VkPipelineLayout,
                       VK_OBJECT_TYPE_PIPELINE_LAYOUT)

#endif

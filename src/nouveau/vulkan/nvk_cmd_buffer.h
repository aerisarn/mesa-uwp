#ifndef NVK_CMD_BUFFER_H
#define NVK_CMD_BUFFER_H 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_command_buffer.h"
#include "vulkan/runtime/vk_command_pool.h"

#define NVK_CMD_BUF_SIZE 64*1024

struct nvk_cmd_pool {
   struct vk_command_pool vk;
   struct list_head cmd_buffers;
   struct list_head free_cmd_buffers;

   struct nvk_device *dev;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_cmd_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

/** Root descriptor table.  This gets pushed to the GPU directly */
struct nvk_root_descriptor_table {
   /* Client push constants */
   uint8_t push[128];

   /* Descriptor set base addresses */
   uint64_t sets[NVK_MAX_SETS];

   /* TODO: Dynamic buffer bindings */
};

struct nvk_descriptor_state {
   struct nvk_root_descriptor_table root;
   struct nvk_descriptor_set *sets[NVK_MAX_SETS];
   uint32_t sets_dirty;
};

struct nvk_compute_state {
   struct nvk_descriptor_state descriptors;
};

struct nvk_cmd_buffer {
   struct vk_command_buffer vk;

   struct nvk_cmd_pool *pool;
   struct list_head pool_link;

   struct {
      struct nvk_compute_state cs;
   } state;

   struct nouveau_ws_push *push;
   bool reset_on_submit;
};

VkResult nvk_reset_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer);

VK_DEFINE_HANDLE_CASTS(nvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

static inline struct nvk_descriptor_state *
nvk_get_descriptors_state(struct nvk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd->state.cs.descriptors;
   default:
      unreachable("Unhandled bind point");
   }
};

#endif

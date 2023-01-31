#ifndef NVK_CMD_BUFFER_H
#define NVK_CMD_BUFFER_H 1

#include "nvk_private.h"

#include "nouveau_push.h"
#include "nvk_descriptor_set.h"

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
   union {
      struct {
         uint32_t block_size[3];
         uint32_t grid_size[3];
         uint32_t _pad[2];
      } cs;
   };

   /* Client push constants */
   uint8_t push[128];

   /* Descriptor set base addresses */
   uint64_t sets[NVK_MAX_SETS];

   /* TODO: Dynamic buffer bindings */
   struct nvk_buffer_address dynamic_buffers[NVK_MAX_DYNAMIC_BUFFERS];
};

struct nvk_descriptor_state {
   struct nvk_root_descriptor_table root;
   struct nvk_descriptor_set *sets[NVK_MAX_SETS];
   uint32_t sets_dirty;
};

struct nvk_compute_state {
   struct nvk_compute_pipeline *pipeline;
   struct nvk_descriptor_state descriptors;
};

struct nvk_cmd_buffer_upload {
   uint8_t *map;
   unsigned offset;
   uint64_t size;
   struct nouveau_ws_bo *upload_bo;
   struct list_head list;
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
   VkResult record_result;

   struct nvk_cmd_buffer_upload upload;

   uint64_t tls_space_needed;
};

VkResult nvk_reset_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer);
void nvk_cmd_buffer_begin_compute(struct nvk_cmd_buffer *cmd,
                                  const VkCommandBufferBeginInfo *pBeginInfo);


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

bool
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd_buffer, unsigned size,
                            uint64_t *addr, void **ptr);

#endif

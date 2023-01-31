#ifndef NVK_CMD_BUFFER_H
#define NVK_CMD_BUFFER_H 1

#include "nvk_private.h"

#include "nouveau_push.h"
#include "nvk_descriptor_set.h"

#include "vk_command_buffer.h"

struct nvk_image_view;

#define NVK_CMD_BUF_SIZE 64*1024

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
   uint8_t push[NVK_MAX_PUSH_SIZE];

   /* Descriptor set base addresses */
   uint64_t sets[NVK_MAX_SETS];

   /* Dynamic buffer bindings */
   struct nvk_buffer_address dynamic_buffers[NVK_MAX_DYNAMIC_BUFFERS];
};

struct nvk_descriptor_state {
   struct nvk_root_descriptor_table root;
   struct nvk_descriptor_set *sets[NVK_MAX_SETS];
   uint32_t sets_dirty;
};

struct nvk_attachment {
   VkFormat vk_format;
   struct nvk_image_view *iview;

   VkResolveModeFlagBits resolve_mode;
   struct nvk_image_view *resolve_iview;
};

struct nvk_rendering_state {
   VkRenderingFlagBits flags;

   VkRect2D area;
   uint32_t layer_count;
   uint32_t view_mask;
   uint32_t samples;

   uint32_t color_att_count;
   struct nvk_attachment color_att[NVK_MAX_RTS];
   struct nvk_attachment depth_att;
   struct nvk_attachment stencil_att;
};

struct nvk_addr_range {
   uint64_t addr;
   uint64_t range;
};

struct nvk_graphics_state {
   struct nvk_rendering_state render;
   struct nvk_graphics_pipeline *pipeline;
   struct nvk_descriptor_state descriptors;

   /* Used for meta save/restore */
   struct nvk_addr_range vb0;

   /* Needed by vk_command_buffer::dynamic_graphics_state */
   struct vk_vertex_input_state _dynamic_vi;
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

   struct {
      struct nvk_graphics_state gfx;
      struct nvk_compute_state cs;
   } state;

   struct nouveau_ws_push *push;

   struct nvk_cmd_buffer_upload upload;

   uint64_t tls_space_needed;
};

VK_DEFINE_HANDLE_CASTS(nvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops nvk_cmd_buffer_ops;

static inline struct nvk_device *
nvk_cmd_buffer_device(struct nvk_cmd_buffer *cmd)
{
   return (struct nvk_device *)cmd->vk.base.device;
}


static inline struct nv_push *
nvk_cmd_buffer_push(struct nvk_cmd_buffer *cmd, uint32_t dw_count)
{
   return P_SPACE(cmd->push, dw_count);
}

static inline void
nvk_cmd_buffer_ref_bo(struct nvk_cmd_buffer *cmd,
                      struct nouveau_ws_bo *bo)
{
   nouveau_ws_push_ref(cmd->push, bo, NOUVEAU_WS_BO_RDWR);
}

void nvk_cmd_buffer_begin_graphics(struct nvk_cmd_buffer *cmd,
                                   const VkCommandBufferBeginInfo *pBeginInfo);
void nvk_cmd_buffer_begin_compute(struct nvk_cmd_buffer *cmd,
                                  const VkCommandBufferBeginInfo *pBeginInfo);

void nvk_cmd_bind_graphics_pipeline(struct nvk_cmd_buffer *cmd,
                                    struct nvk_graphics_pipeline *pipeline);
void nvk_cmd_bind_compute_pipeline(struct nvk_cmd_buffer *cmd,
                                   struct nvk_compute_pipeline *pipeline);

void nvk_cmd_bind_vertex_buffer(struct nvk_cmd_buffer *cmd, uint32_t vb_idx,
                                struct nvk_addr_range addr_range);

static inline struct nvk_descriptor_state *
nvk_get_descriptors_state(struct nvk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmd->state.gfx.descriptors;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd->state.cs.descriptors;
   default:
      unreachable("Unhandled bind point");
   }
};

VkResult nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd,
                                     uint32_t size, uint32_t alignment,
                                     uint64_t *addr, void **ptr);

VkResult nvk_cmd_buffer_upload_data(struct nvk_cmd_buffer *cmd,
                                    const void *data, uint32_t size,
                                    uint32_t alignment, uint64_t *addr);

#endif

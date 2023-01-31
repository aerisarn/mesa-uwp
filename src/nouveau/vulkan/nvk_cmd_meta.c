#include "nvk_buffer.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_image.h"

static VkResult
nvk_cmd_bind_map_buffer(struct vk_command_buffer *vk_cmd,
                        struct vk_meta_device *meta,
                        VkBuffer _buffer, void **map_out)
{
   struct nvk_cmd_buffer *cmd =
      container_of(vk_cmd, struct nvk_cmd_buffer, vk);
   VK_FROM_HANDLE(nvk_buffer, buffer, _buffer);

   uint64_t addr;
   assert(buffer->vk.size < UINT_MAX);
   if (!nvk_cmd_buffer_upload_alloc(cmd, buffer->vk.size, &addr, map_out))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   buffer->addr = addr;

   return VK_SUCCESS;
}

VkResult
nvk_device_init_meta(struct nvk_device *dev)
{
   VkResult result = vk_meta_device_init(&dev->vk, &dev->meta);
   if (result != VK_SUCCESS)
      return result;

   dev->meta.cmd_bind_map_buffer = nvk_cmd_bind_map_buffer;
   dev->meta.max_bind_map_buffer_size_B = 64 * 1024; /* TODO */

   return VK_SUCCESS;
}

void
nvk_device_finish_meta(struct nvk_device *dev)
{
   vk_meta_device_finish(&dev->vk, &dev->meta);
}

struct nvk_meta_save {
   struct vk_vertex_input_state _dynamic_vi;
   struct vk_dynamic_graphics_state dynamic;
   struct nvk_graphics_pipeline *pipeline;
   struct nvk_addr_range vb0;
   uint8_t push[128];
};

static void
nvk_meta_begin(struct nvk_cmd_buffer *cmd,
               struct nvk_meta_save *save)
{
   save->dynamic = cmd->vk.dynamic_graphics_state;
   save->_dynamic_vi = cmd->state.gfx._dynamic_vi;

   save->pipeline = cmd->state.gfx.pipeline;
   save->vb0 = cmd->state.gfx.vb0;

   /* TODO: Push */
}

static void
nvk_meta_init_render(struct nvk_cmd_buffer *cmd,
                     struct vk_meta_rendering_info *info)
{
   const struct nvk_rendering_state *render = &cmd->state.gfx.render;

   *info = (struct vk_meta_rendering_info) {
      .view_mask = render->view_mask,
      .samples = render->samples,
      .color_attachment_count = render->color_att_count,
      .depth_attachment_format = render->depth_att.vk_format,
      .stencil_attachment_format = render->stencil_att.vk_format,
   };
   for (uint32_t a = 0; a < render->color_att_count; a++)
      info->color_attachment_formats[a] = render->color_att[a].vk_format;
}

static void
nvk_meta_end(struct nvk_cmd_buffer *cmd,
             struct nvk_meta_save *save)
{
   /* Restore the dynamic state */
   assert(save->dynamic.vi == &cmd->state.gfx._dynamic_vi);
   cmd->vk.dynamic_graphics_state = save->dynamic;
   cmd->state.gfx._dynamic_vi = save->_dynamic_vi;
   memcpy(cmd->vk.dynamic_graphics_state.dirty,
          cmd->vk.dynamic_graphics_state.set,
          sizeof(cmd->vk.dynamic_graphics_state.set));

   if (save->pipeline)
      nvk_cmd_bind_graphics_pipeline(cmd, save->pipeline);

   nvk_cmd_bind_vertex_buffer(cmd, 0, save->vb0);

   /* TODO: Push */
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearAttachments(VkCommandBuffer commandBuffer,
                        uint32_t attachmentCount,
                        const VkClearAttachment *pAttachments,
                        uint32_t rectCount,
                        const VkClearRect *pRects)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   struct nvk_meta_save save;
   nvk_meta_begin(cmd, &save);

   struct vk_meta_rendering_info render;
   nvk_meta_init_render(cmd, &render);

   vk_meta_clear_attachments(&cmd->vk, &dev->meta, &render,
                             attachmentCount, pAttachments,
                             rectCount, pRects);

   nvk_meta_end(cmd, &save);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearColorImage(VkCommandBuffer commandBuffer,
                       VkImage _image,
                       VkImageLayout imageLayout,
                       const VkClearColorValue *pColor,
                       uint32_t rangeCount,
                       const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, image, _image);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   /* Cannot be rendering right now */
   assert(cmd->state.gfx.render.samples == 0);

   struct nvk_meta_save save;
   nvk_meta_begin(cmd, &save);

   vk_meta_clear_color_image(&cmd->vk, &dev->meta,
                             &image->vk, imageLayout, image->vk.format,
                             pColor, rangeCount, pRanges);

   nvk_meta_end(cmd, &save);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                              VkImage _image,
                              VkImageLayout imageLayout,
                              const VkClearDepthStencilValue *pDepthStencil,
                              uint32_t rangeCount,
                              const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, image, _image);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   /* Cannot be rendering right now */
   assert(cmd->state.gfx.render.samples == 0);

   struct nvk_meta_save save;
   nvk_meta_begin(cmd, &save);

   vk_meta_clear_depth_stencil_image(&cmd->vk, &dev->meta,
                                     &image->vk, imageLayout,
                                     pDepthStencil, rangeCount, pRanges);

   nvk_meta_end(cmd, &save);
}

#include "nvk_cmd_buffer.h"

#include "nvk_cl9097.h"

static void
emit_clear_rects(struct nvk_cmd_buffer *cmd,
                 int color_att,
                 bool clear_depth,
                 bool clear_stencil,
                 uint32_t rect_count,
                 const VkClearRect *rects)
{
   struct nvk_rendering_state *render = &cmd->state.gfx.render;
   struct nouveau_ws_push *p = cmd->push;

   for (uint32_t r = 0; r < rect_count; r++) {
      P_MTHD(p, NV9097, SET_CLEAR_RECT_HORIZONTAL);
      P_NV9097_SET_CLEAR_RECT_HORIZONTAL(p, {
         .xmin = rects[r].rect.offset.x,
         .xmax = rects[r].rect.offset.x + rects[r].rect.extent.width,
      });
      P_NV9097_SET_CLEAR_RECT_VERTICAL(p, {
         .ymin = rects[r].rect.offset.y,
         .ymax = rects[r].rect.offset.y + rects[r].rect.extent.height,
      });

      if (render->view_mask) {
         assert(rects[r].baseArrayLayer == 0);
         assert(rects[r].layerCount == 1);
         u_foreach_bit(view, render->view_mask) {
            P_IMMD(p, NV9097, CLEAR_SURFACE, {
               .z_enable       = clear_depth,
               .stencil_enable = clear_stencil,
               .r_enable       = color_att >= 0,
               .g_enable       = color_att >= 0,
               .b_enable       = color_att >= 0,
               .a_enable       = color_att >= 0,
               .mrt_select     = color_att >= 0 ? color_att : 0,
               .rt_array_index = view,
            });
         }
      } else {
         for (uint32_t l = 0; l < rects[r].layerCount; l++) {
            P_IMMD(p, NV9097, CLEAR_SURFACE, {
               .z_enable       = clear_depth,
               .stencil_enable = clear_stencil,
               .r_enable       = color_att >= 0,
               .g_enable       = color_att >= 0,
               .b_enable       = color_att >= 0,
               .a_enable       = color_att >= 0,
               .mrt_select     = color_att >= 0 ? color_att : 0,
               .rt_array_index = rects[r].baseArrayLayer + l,
            });
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearAttachments(VkCommandBuffer commandBuffer,
                        uint32_t attachmentCount,
                        const VkClearAttachment *pAttachments,
                        uint32_t rectCount,
                        const VkClearRect *pRects)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nouveau_ws_push *p = cmd->push;

   P_IMMD(p, NV9097, SET_CLEAR_SURFACE_CONTROL, {
      .respect_stencil_mask   = RESPECT_STENCIL_MASK_FALSE,
      .use_clear_rect         = USE_CLEAR_RECT_TRUE,
      .use_scissor0           = USE_SCISSOR0_FALSE,
      .use_viewport_clip0     = USE_VIEWPORT_CLIP0_FALSE,
   });

   bool clear_depth = false, clear_stencil = false;
   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         P_IMMD(p, NV9097, SET_Z_CLEAR_VALUE,
                fui(pAttachments[i].clearValue.depthStencil.depth));
         clear_depth = true;
      }

      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         P_IMMD(p, NV9097, SET_STENCIL_CLEAR_VALUE,
                pAttachments[i].clearValue.depthStencil.stencil & 0xff);
         clear_stencil = true;
      }
   }

   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pAttachments[i].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
         continue;

      if (pAttachments[i].colorAttachment == VK_ATTACHMENT_UNUSED)
         continue;

      VkClearColorValue color = pAttachments[i].clearValue.color;

      P_MTHD(p, NV9097, SET_COLOR_CLEAR_VALUE(0));
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 0, color.uint32[0]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 1, color.uint32[1]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 2, color.uint32[2]);
      P_NV9097_SET_COLOR_CLEAR_VALUE(p, 3, color.uint32[3]);

      emit_clear_rects(cmd, pAttachments[i].colorAttachment,
                       clear_depth, clear_stencil, rectCount, pRects);

      /* We only need to clear depth/stencil once */
      clear_depth = clear_stencil = false;
   }

   /* No color clears */
   if (clear_depth || clear_stencil)
      emit_clear_rects(cmd, -1, clear_depth, clear_stencil, rectCount, pRects);
}

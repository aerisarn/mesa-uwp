#include "nvk_cmd_buffer.h"

#include "vulkan/util/vk_format.h"

#include "nvk_buffer.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_image.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"

#include "nvtypes.h"
#include "nvk_cl902d.h"

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBlitImage2(
   VkCommandBuffer commandBuffer,
   const VkBlitImageInfo2 *pBlitImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pBlitImageInfo->srcImage);
   VK_FROM_HANDLE(nvk_image, dst, pBlitImageInfo->dstImage);
   struct nouveau_ws_push *push = cmd->push;

   uint32_t src_depth = src->vk.extent.depth * src->vk.array_layers;
   uint32_t dst_depth = dst->vk.extent.depth * dst->vk.array_layers;

   nouveau_ws_push_ref(push, src->mem->bo, NOUVEAU_WS_BO_RD);
   nouveau_ws_push_ref(push, dst->mem->bo, NOUVEAU_WS_BO_WR);

   VkDeviceSize src_addr = src->mem->bo->offset + src->offset;
   VkDeviceSize dst_addr = dst->mem->bo->offset + dst->offset;

   P_IMMD(push, NV902D, SET_CLIP_ENABLE, V_FALSE);
   P_IMMD(push, NV902D, SET_COLOR_KEY_ENABLE, V_FALSE);
   P_IMMD(push, NV902D, SET_RENDER_ENABLE_C, MODE_TRUE);

   P_IMMD(push, NV902D, SET_SRC_FORMAT, src->format->hw_format);
   if (src->tile.is_tiled) {
      P_MTHD(push, NV902D, SET_SRC_MEMORY_LAYOUT);
      P_NV902D_SET_SRC_MEMORY_LAYOUT(push, V_BLOCKLINEAR);
      P_NV902D_SET_SRC_BLOCK_SIZE(push, {
         .height = src->tile.y,
         .depth = src->tile.z,
      });
   } else {
      P_IMMD(push, NV902D, SET_SRC_MEMORY_LAYOUT, V_PITCH);
   }

   P_MTHD(push, NV902D, SET_SRC_DEPTH);
   P_NV902D_SET_SRC_DEPTH(push, src_depth);

   P_MTHD(push, NV902D, SET_SRC_PITCH);
   P_NV902D_SET_SRC_PITCH(push, src->row_stride);
   P_NV902D_SET_SRC_WIDTH(push, src->vk.extent.width);
   P_NV902D_SET_SRC_HEIGHT(push, src->vk.extent.height);
   P_NV902D_SET_SRC_OFFSET_UPPER(push, src_addr >> 32);
   P_NV902D_SET_SRC_OFFSET_LOWER(push, src_addr & 0xffffffff);

   P_IMMD(push, NV902D, SET_DST_FORMAT, dst->format->hw_format);
   if (dst->tile.is_tiled) {
      P_MTHD(push, NV902D, SET_DST_MEMORY_LAYOUT);
      P_NV902D_SET_DST_MEMORY_LAYOUT(push, V_BLOCKLINEAR);
      P_NV902D_SET_DST_BLOCK_SIZE(push, {
         .height = dst->tile.y,
         .depth = dst->tile.z,
      });
   } else {
      P_IMMD(push, NV902D, SET_DST_MEMORY_LAYOUT, V_PITCH);
   }

   P_MTHD(push, NV902D, SET_DST_DEPTH);
   P_NV902D_SET_DST_DEPTH(push, dst_depth);
   P_NV902D_SET_DST_LAYER(push, 0);
   P_NV902D_SET_DST_PITCH(push, dst->row_stride);
   P_NV902D_SET_DST_WIDTH(push, dst->vk.extent.width);
   P_NV902D_SET_DST_HEIGHT(push, dst->vk.extent.height);
   P_NV902D_SET_DST_OFFSET_UPPER(push, dst_addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(push, dst_addr & 0xffffffff);

   if (pBlitImageInfo->filter == VK_FILTER_NEAREST) {
      P_IMMD(push, NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, {
         .origin = ORIGIN_CORNER,
         .filter = FILTER_POINT,
      });
   } else {
      P_IMMD(push, NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, {
         .origin = ORIGIN_CORNER,
         .filter = FILTER_BILINEAR,
      });
   }

   /* for single channel sources we have to disable channels, we can use SRCCOPY_PREMULT:
    *   DST = SRC * BETA4
    * otherwise all channels of the destinations are filled
    *
    * NOTE: this only works for blits to 8 bit or packed formats
    */
   if (vk_format_get_nr_components(src->format->vk_format) == 1 &&
       src->format->hw_format != dst->format->hw_format) {
      uint8_t mask = vk_format_is_snorm(dst->format->vk_format) ? 0x7f : 0xff;
      P_MTHD(push, NV902D, SET_BETA4);
      P_NV902D_SET_BETA4(push, {
         .r = mask,
         .a = mask,
      });
      P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY_PREMULT);
   } else {
      P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY);
   }

   for (unsigned r = 0; r < pBlitImageInfo->regionCount; r++) {
      const VkImageBlit2 *region = &pBlitImageInfo->pRegions[r];

      unsigned x_i = region->dstOffsets[0].x < region->dstOffsets[1].x ? 0 : 1;
      unsigned y_i = region->dstOffsets[0].y < region->dstOffsets[1].y ? 0 : 1;

      /* All src_* are in 32.32 fixed-point */
      int64_t src_start_x_fp = (int64_t)region->srcOffsets[x_i].x << 32;
      int64_t src_start_y_fp = (int64_t)region->srcOffsets[y_i].y << 32;
      int64_t src_end_x_fp = (int64_t)region->srcOffsets[1 - x_i].x << 32;
      int64_t src_end_y_fp = (int64_t)region->srcOffsets[1 - y_i].y << 32;
      int64_t src_width_fp = src_end_x_fp - src_start_x_fp;
      int64_t src_height_fp = src_end_y_fp - src_start_y_fp;

      uint32_t dst_start_x = region->dstOffsets[x_i].x;
      uint32_t dst_start_y = region->dstOffsets[y_i].y;
      uint32_t dst_end_x = region->dstOffsets[1 - x_i].x;
      uint32_t dst_end_y = region->dstOffsets[1 - y_i].y;
      uint32_t dst_width = dst_end_x - dst_start_x;
      uint32_t dst_height = dst_end_y - dst_start_y;

      int64_t scaling_x_fp = src_width_fp / dst_width;
      int64_t scaling_y_fp = src_height_fp / dst_height;

      /* move the src by half a fraction.
       * Alternatively I am sure there is a way to make that work with CENTER SAMPLE_MODE, but
       * that didn't really pan out
       */
      src_start_x_fp += scaling_x_fp / 2;
      src_start_y_fp += scaling_y_fp / 2;

      P_MTHD(push, NV902D, SET_PIXELS_FROM_MEMORY_DST_X0);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_X0(push, dst_start_x);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_Y0(push, dst_start_y);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH(push, dst_width);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT(push, dst_height);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC(push, scaling_x_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT(push, scaling_x_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_FRAC(push, scaling_y_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT(push, scaling_y_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC(push, src_start_x_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_INT(push, src_start_x_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC(push, src_start_y_fp & 0xffffffff);
      P_NV902D_PIXELS_FROM_MEMORY_SRC_Y0_INT(push, src_start_y_fp >> 32);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdFillBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize fillSize,
   uint32_t data)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, dst, dstBuffer);
   struct nouveau_ws_push *push = cmd->push;
   fillSize = vk_buffer_range(&dst->vk, dstOffset, fillSize);

   VkDeviceSize dst_addr = nvk_buffer_address(dst, 0);
   VkDeviceSize start = dstOffset / 4;
   VkDeviceSize end = start + fillSize / 4;

   /* can't go higher for whatever reason */
   uint32_t pitch = 1 << 21;
   uint32_t line = pitch / 4;

   nouveau_ws_push_ref(push, dst->mem->bo, NOUVEAU_WS_BO_WR);

   P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY);

   P_MTHD(push, NV902D, SET_DST_FORMAT);
   P_NV902D_SET_DST_FORMAT(push, V_A8B8G8R8);
   P_NV902D_SET_DST_MEMORY_LAYOUT(push, V_PITCH);

   P_MTHD(push, NV902D, SET_DST_PITCH);
   P_NV902D_SET_DST_PITCH(push, pitch);

   P_MTHD(push, NV902D, SET_DST_OFFSET_UPPER);
   P_NV902D_SET_DST_OFFSET_UPPER(push, dst_addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(push, dst_addr & 0xffffffff);

   P_MTHD(push, NV902D, RENDER_SOLID_PRIM_MODE);
   P_NV902D_RENDER_SOLID_PRIM_MODE(push, V_LINES);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT(push, V_A8B8G8R8);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR(push, data);

   /*
    * In order to support CPU efficient fills, we'll draw up to three primitives:
    *   1. rest of the first line
    *   2. a rect filling up the space between the start and end
    *   3. begining of last line
    */

   uint32_t y_0 = start / line;
   uint32_t y_1 = end / line;

   uint32_t x_0 = start % line;
   uint32_t x_1 = end % line;

   P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, x_0);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, y_0);
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, y_0 == y_1 ? x_1 : line);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, y_0);

   if (y_0 + 1 < y_1) {
      P_IMMD(push, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

      P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, y_0 + 1);
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, line);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, y_1);

      P_IMMD(push, NV902D, RENDER_SOLID_PRIM_MODE, V_LINES);
   }

   if (y_0 < y_1) {
      P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, y_1);
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, x_1);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, y_1);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdUpdateBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize dataSize,
   const void *pData)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, dst, dstBuffer);
   struct nouveau_ws_push *push = cmd->push;
   uint32_t pitch = 65536;

   assert(dataSize <= 65536);

   VkDeviceSize dst_addr = nvk_buffer_address(dst, 0);

   nouveau_ws_push_ref(push, dst->mem->bo, NOUVEAU_WS_BO_WR);

   P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY);

   P_MTHD(push, NV902D, SET_DST_OFFSET_UPPER);
   P_NV902D_SET_DST_OFFSET_UPPER(push, dst_addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(push, dst_addr & 0xffffffff);

   P_MTHD(push, NV902D, SET_DST_FORMAT);
   P_NV902D_SET_DST_FORMAT(push, V_A8B8G8R8);
   P_NV902D_SET_DST_MEMORY_LAYOUT(push, V_PITCH);

   P_MTHD(push, NV902D, SET_DST_PITCH);
   P_NV902D_SET_DST_PITCH(push, pitch);

   P_IMMD(push, NV902D, SET_PIXELS_FROM_CPU_DATA_TYPE, V_COLOR);
   P_IMMD(push, NV902D, SET_PIXELS_FROM_CPU_COLOR_FORMAT, V_A8B8G8R8);

   P_MTHD(push, NV902D, SET_PIXELS_FROM_CPU_SRC_WIDTH);
   P_NV902D_SET_PIXELS_FROM_CPU_SRC_WIDTH(push, dataSize / 4);
   P_NV902D_SET_PIXELS_FROM_CPU_SRC_HEIGHT(push, 1);
   P_NV902D_SET_PIXELS_FROM_CPU_DX_DU_FRAC(push, 0);
   P_NV902D_SET_PIXELS_FROM_CPU_DX_DU_INT(push, 1);
   P_NV902D_SET_PIXELS_FROM_CPU_DY_DV_FRAC(push, 0);
   P_NV902D_SET_PIXELS_FROM_CPU_DY_DV_INT(push, 1);
   P_NV902D_SET_PIXELS_FROM_CPU_DST_X0_FRAC(push, 0);
   P_NV902D_SET_PIXELS_FROM_CPU_DST_X0_INT(push, (dstOffset % pitch) / 4);
   P_NV902D_SET_PIXELS_FROM_CPU_DST_Y0_FRAC(push, 0);
   P_NV902D_SET_PIXELS_FROM_CPU_DST_Y0_INT(push, (dstOffset / pitch) / 4);

   P_0INC(push, NV902D, PIXELS_FROM_CPU_DATA);
   P_INLINE_ARRAY(push, pData, dataSize / 4);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearColorImage(
   VkCommandBuffer commandBuffer,
   VkImage _image,
   VkImageLayout imageLayout,
   const VkClearColorValue *pColor,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
}

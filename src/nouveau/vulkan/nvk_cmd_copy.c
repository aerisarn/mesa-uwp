#include "nvk_cmd_buffer.h"

#include "vulkan/util/vk_format.h"

#include "nvk_buffer.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"

#include "nouveau_bo.h"
#include "nouveau_context.h"
#include "nouveau_push.h"

#include "nvtypes.h"
#include "nvk_cl902d.h"
#include "nvk_cl90b5.h"
#include "nvk_clc1b5.h"

static void
nouveau_copy_linear(struct nouveau_ws_push *push,
                    uint64_t src_addr, uint64_t dst_addr, uint64_t size)
{
   while (size) {
      unsigned bytes = MIN2(size, 1 << 17);

      P_MTHD(push, NV90B5, OFFSET_IN_UPPER);
      P_NV90B5_OFFSET_IN_UPPER(push, src_addr >> 32);
      P_NV90B5_OFFSET_IN_LOWER(push, src_addr & 0xffffffff);
      P_NV90B5_OFFSET_OUT_UPPER(push, dst_addr >> 32);
      P_NV90B5_OFFSET_OUT_LOWER(push, dst_addr & 0xffffffff);

      P_MTHD(push, NV90B5, LINE_LENGTH_IN);
      P_NV90B5_LINE_LENGTH_IN(push, bytes);
      P_NV90B5_LINE_COUNT(push, 1);

      P_IMMD(push, NV90B5, LAUNCH_DMA, {
             .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
             .multi_line_enable = MULTI_LINE_ENABLE_TRUE,
             .flush_enable = FLUSH_ENABLE_TRUE,
             .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
             .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
      });

      src_addr += bytes;
      dst_addr += bytes;
      size -= bytes;
   }
}

struct nouveau_copy_buffer {
   uint64_t base_addr;
   VkOffset3D offset_el;
   uint32_t base_array_layer;
   VkExtent3D extent_el;
   uint32_t row_stride;
   uint32_t array_stride;
   struct nil_tiling tiling;
};

struct nouveau_copy {
   struct nouveau_copy_buffer src;
   struct nouveau_copy_buffer dst;
   uint32_t bpp;
   VkExtent3D extent_el;
   uint32_t layer_count;
};

static struct nouveau_copy_buffer
nouveau_copy_rect_buffer(
   struct nvk_buffer *buf,
   VkDeviceSize offset,
   struct vk_image_buffer_layout buffer_layout)
{
   return (struct nouveau_copy_buffer) {
      .base_addr = nvk_buffer_address(buf, offset),
      .row_stride = buffer_layout.row_stride_B,
      .array_stride = buffer_layout.image_stride_B,
   };
}

static VkOffset3D
vk_offset_px_to_el(VkOffset3D offset, VkFormat format)
{
   const struct util_format_description *desc = vk_format_description(format);

   assert(offset.x % desc->block.width == 0);
   assert(offset.y % desc->block.height == 0);
   assert(offset.z % desc->block.depth == 0);

   offset.x /= desc->block.width;
   offset.y /= desc->block.height;
   offset.z /= desc->block.depth;

   return offset;
}

static VkExtent3D
vk_extent_px_to_el(VkExtent3D extent, VkFormat format)
{
   const struct util_format_description *desc = vk_format_description(format);

   extent.width = DIV_ROUND_UP(extent.width, desc->block.width);
   extent.height = DIV_ROUND_UP(extent.height, desc->block.height);
   extent.depth = DIV_ROUND_UP(extent.depth, desc->block.depth);

   return extent;
}

static struct nouveau_copy_buffer
nouveau_copy_rect_image(
   struct nvk_image *img,
   VkOffset3D offset_px,
   const VkImageSubresourceLayers *sub_res)
{
   const VkExtent3D lvl_extent_px =
      vk_image_mip_level_extent(&img->vk, sub_res->mipLevel);
   offset_px = vk_image_sanitize_offset(&img->vk, offset_px);

   struct nouveau_copy_buffer buf = {
      .base_addr = nvk_image_base_address(img) +
                   img->nil.levels[sub_res->mipLevel].offset_B,
      .offset_el = vk_offset_px_to_el(offset_px, img->vk.format),
      .base_array_layer = sub_res->baseArrayLayer,
      .extent_el = vk_extent_px_to_el(lvl_extent_px, img->vk.format),
      .row_stride = img->nil.levels[sub_res->mipLevel].row_stride_B,
      .array_stride = img->nil.array_stride_B,
      .tiling = img->nil.levels[sub_res->mipLevel].tiling,
   };

   return buf;
}

static void
nouveau_copy_rect(struct nvk_cmd_buffer *cmd, struct nouveau_copy *copy)
{
   struct nouveau_ws_push *push = cmd->push;

   for (unsigned w = 0; w < copy->layer_count; w++) {
      VkDeviceSize src_addr = copy->src.base_addr;
      VkDeviceSize dst_addr = copy->dst.base_addr;

      src_addr += (w + copy->src.base_array_layer) * copy->src.array_stride;
      dst_addr += (w + copy->dst.base_array_layer) * copy->dst.array_stride;

      if (!copy->src.tiling.is_tiled) {
         src_addr += copy->src.offset_el.x * copy->bpp +
                     copy->src.offset_el.y * copy->src.row_stride;
      }

      if (!copy->dst.tiling.is_tiled) {
         dst_addr += copy->dst.offset_el.x * copy->bpp +
                     copy->dst.offset_el.y * copy->dst.row_stride;
      }

      for (unsigned z = 0; z < copy->extent_el.depth; z++) {
         P_MTHD(push, NV90B5, OFFSET_IN_UPPER);
         P_NV90B5_OFFSET_IN_UPPER(push, src_addr >> 32);
         P_NV90B5_OFFSET_IN_LOWER(push, src_addr & 0xffffffff);
         P_NV90B5_OFFSET_OUT_UPPER(push, dst_addr >> 32);
         P_NV90B5_OFFSET_OUT_LOWER(push, dst_addr & 0xfffffff);
         P_NV90B5_PITCH_IN(push, copy->src.row_stride);
         P_NV90B5_PITCH_OUT(push, copy->dst.row_stride);
         P_NV90B5_LINE_LENGTH_IN(push, copy->extent_el.width * copy->bpp);
         P_NV90B5_LINE_COUNT(push, copy->extent_el.height);

         uint32_t src_layout = 0, dst_layout = 0;
         if (copy->src.tiling.is_tiled) {
            P_MTHD(push, NV90B5, SET_SRC_BLOCK_SIZE);
            P_NV90B5_SET_SRC_BLOCK_SIZE(push, {
               .width = 0, /* Tiles are always 1 GOB wide */
               .height = copy->src.tiling.y_log2,
               .depth = copy->src.tiling.z_log2,
               .gob_height = copy->src.tiling.gob_height_8 ?
                             GOB_HEIGHT_GOB_HEIGHT_FERMI_8 :
                             GOB_HEIGHT_GOB_HEIGHT_TESLA_4,
            });
            P_NV90B5_SET_SRC_WIDTH(push, copy->src.extent_el.width * copy->bpp);
            P_NV90B5_SET_SRC_HEIGHT(push, copy->src.extent_el.height);
            P_NV90B5_SET_SRC_DEPTH(push, copy->src.extent_el.depth);
            P_NV90B5_SET_SRC_LAYER(push, z + copy->src.offset_el.z);

            if (cmd->pool->dev->ctx->copy.cls >= 0xc1b5) {
               P_MTHD(push, NVC1B5, SRC_ORIGIN_X);
               P_NVC1B5_SRC_ORIGIN_X(push, copy->src.offset_el.x * copy->bpp);
               P_NVC1B5_SRC_ORIGIN_Y(push, copy->src.offset_el.y);
            } else {
               P_MTHD(push, NV90B5, SET_SRC_ORIGIN);
               P_NV90B5_SET_SRC_ORIGIN(push, {
                  .x = copy->src.offset_el.x * copy->bpp,
                  .y = copy->src.offset_el.y
               });
            }

            src_layout = NV90B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_BLOCKLINEAR;
         } else {
            src_addr += copy->src.array_stride;
            src_layout = NV90B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH;
         }

         if (copy->dst.tiling.is_tiled) {
            P_MTHD(push, NV90B5, SET_DST_BLOCK_SIZE);
            P_NV90B5_SET_DST_BLOCK_SIZE(push, {
               .width = 0, /* Tiles are always 1 GOB wide */
               .height = copy->dst.tiling.y_log2,
               .depth = copy->dst.tiling.z_log2,
               .gob_height = copy->dst.tiling.gob_height_8 ?
                             GOB_HEIGHT_GOB_HEIGHT_FERMI_8 :
                             GOB_HEIGHT_GOB_HEIGHT_TESLA_4,
            });
            P_NV90B5_SET_DST_WIDTH(push, copy->dst.extent_el.width * copy->bpp);
            P_NV90B5_SET_DST_HEIGHT(push, copy->dst.extent_el.height);
            P_NV90B5_SET_DST_DEPTH(push, copy->dst.extent_el.depth);
            P_NV90B5_SET_DST_LAYER(push, z + copy->dst.offset_el.z);

            if (cmd->pool->dev->ctx->copy.cls >= 0xc1b5) {
               P_MTHD(push, NVC1B5, DST_ORIGIN_X);
               P_NVC1B5_DST_ORIGIN_X(push, copy->dst.offset_el.x * copy->bpp);
               P_NVC1B5_DST_ORIGIN_Y(push, copy->dst.offset_el.y);
            } else {
               P_MTHD(push, NV90B5, SET_DST_ORIGIN);
               P_NV90B5_SET_DST_ORIGIN(push, {
                  .x = copy->dst.offset_el.x * copy->bpp,
                  .y = copy->dst.offset_el.y
               });
            }

            dst_layout = NV90B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_BLOCKLINEAR;
         } else {
            dst_addr += copy->dst.array_stride;
            dst_layout = NV90B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH;
         }

         P_IMMD(push, NV90B5, LAUNCH_DMA, {
            .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
            .multi_line_enable = MULTI_LINE_ENABLE_TRUE,
            .flush_enable = FLUSH_ENABLE_TRUE,
            .src_memory_layout = src_layout,
            .dst_memory_layout = dst_layout
         });
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyBuffer2(VkCommandBuffer commandBuffer,
                   const VkCopyBufferInfo2 *pCopyBufferInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src, pCopyBufferInfo->srcBuffer);
   VK_FROM_HANDLE(nvk_buffer, dst, pCopyBufferInfo->dstBuffer);

   nvk_push_buffer_ref(cmd->push, src, NOUVEAU_WS_BO_RD);
   nvk_push_buffer_ref(cmd->push, dst, NOUVEAU_WS_BO_WR);

   for (unsigned r = 0; r < pCopyBufferInfo->regionCount; r++) {
      const VkBufferCopy2 *region = &pCopyBufferInfo->pRegions[r];

      nouveau_copy_linear(cmd->push,
                          nvk_buffer_address(src, region->srcOffset),
                          nvk_buffer_address(dst, region->dstOffset),
                          region->size);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                          const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src, pCopyBufferToImageInfo->srcBuffer);
   VK_FROM_HANDLE(nvk_image, dst, pCopyBufferToImageInfo->dstImage);

   nvk_push_buffer_ref(cmd->push, src, NOUVEAU_WS_BO_RD);
   nvk_push_image_ref(cmd->push, dst, NOUVEAU_WS_BO_WR);

   for (unsigned r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
      const VkBufferImageCopy2 *region = &pCopyBufferToImageInfo->pRegions[r];
      struct vk_image_buffer_layout buffer_layout =
         vk_image_buffer_copy_layout(&dst->vk, region);

      const VkExtent3D extent_px =
         vk_image_sanitize_extent(&dst->vk, region->imageExtent);
      const VkExtent3D extent_el =
         vk_extent_px_to_el(extent_px, dst->vk.format);

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_buffer(src, region->bufferOffset,
                                         buffer_layout),
         .dst = nouveau_copy_rect_image(dst, region->imageOffset,
                                        &region->imageSubresource),
         .bpp = buffer_layout.element_size_B,
         .extent_el = extent_el,
         .layer_count = region->imageSubresource.layerCount,
      };

      nouveau_copy_rect(cmd, &copy);

      vk_foreach_struct_const(ext, region->pNext) {
         switch (ext->sType) {
         default:
            nvk_debug_ignored_stype(ext->sType);
            break;
         }
      }
   }

   vk_foreach_struct_const(ext, pCopyBufferToImageInfo->pNext) {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                          const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(nvk_buffer, dst, pCopyImageToBufferInfo->dstBuffer);

   nvk_push_image_ref(cmd->push, src, NOUVEAU_WS_BO_RD);
   nvk_push_buffer_ref(cmd->push, dst, NOUVEAU_WS_BO_WR);

   for (unsigned r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
      const VkBufferImageCopy2 *region = &pCopyImageToBufferInfo->pRegions[r];
      struct vk_image_buffer_layout buffer_layout =
         vk_image_buffer_copy_layout(&src->vk, region);

      const VkExtent3D extent_px =
         vk_image_sanitize_extent(&src->vk, region->imageExtent);
      const VkExtent3D extent_el =
         vk_extent_px_to_el(extent_px, src->vk.format);

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_image(src, region->imageOffset,
                                        &region->imageSubresource),
         .dst = nouveau_copy_rect_buffer(dst, region->bufferOffset,
                                         buffer_layout),
         .bpp = buffer_layout.element_size_B,
         .extent_el = extent_el,
         .layer_count = region->imageSubresource.layerCount,
      };

      nouveau_copy_rect(cmd, &copy);

      vk_foreach_struct_const(ext, region->pNext) {
         switch (ext->sType) {
         default:
            nvk_debug_ignored_stype(ext->sType);
            break;
         }
      }
   }

   vk_foreach_struct_const(ext, pCopyImageToBufferInfo->pNext) {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyImage2(VkCommandBuffer commandBuffer,
                  const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(nvk_image, dst, pCopyImageInfo->dstImage);

   nvk_push_image_ref(cmd->push, src, NOUVEAU_WS_BO_RD);
   nvk_push_image_ref(cmd->push, dst, NOUVEAU_WS_BO_WR);

   uint16_t bpp = vk_format_description(src->vk.format)->block.bits;
   assert(bpp == vk_format_description(dst->vk.format)->block.bits);
   bpp /= 8;

   for (unsigned r = 0; r < pCopyImageInfo->regionCount; r++) {
      const VkImageCopy2 *region = &pCopyImageInfo->pRegions[r];

      /* From the Vulkan 1.3.217 spec:
       *
       *    "When copying between compressed and uncompressed formats the
       *    extent members represent the texel dimensions of the source image
       *    and not the destination."
       */
      const VkExtent3D extent_px =
         vk_image_sanitize_extent(&src->vk, region->extent);
      const VkExtent3D extent_el =
         vk_extent_px_to_el(extent_px, src->vk.format);

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_image(src, region->srcOffset,
                                        &region->srcSubresource),
         .dst = nouveau_copy_rect_image(dst, region->dstOffset,
                                        &region->dstSubresource),
         .bpp = bpp,
         .extent_el = extent_el,
         .layer_count = region->srcSubresource.layerCount,
      };

      nouveau_copy_rect(cmd, &copy);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdClearColorImage(VkCommandBuffer commandBuffer,
                       VkImage image,
                       VkImageLayout imageLayout,
                       const VkClearColorValue *pColor,
                       uint32_t rangeCount,
                       const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, dst, image);
   struct nouveau_ws_push *push = cmd->push;

   nvk_push_image_ref(push, dst, NOUVEAU_WS_BO_WR);

   P_IMMD(push, NV902D, SET_OPERATION, V_SRCCOPY);

   P_IMMD(push, NV902D, SET_CLIP_ENABLE, V_FALSE);
   P_IMMD(push, NV902D, SET_COLOR_KEY_ENABLE, V_FALSE);
   P_IMMD(push, NV902D, SET_RENDER_ENABLE_C, MODE_TRUE);

   uint32_t packed_color[4] = { };
   util_format_pack_rgba(vk_format_to_pipe_format(dst->vk.format),
                         packed_color, pColor, 1);

   switch (vk_format_get_blocksize(dst->vk.format)) {
   case 1:
      P_IMMD(push, NV902D, SET_DST_FORMAT, V_Y8);
      P_IMMD(push, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT, V_Y8);
      break;
   case 2:
      P_IMMD(push, NV902D, SET_DST_FORMAT, V_Y16);
      P_IMMD(push, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT, V_Y16);
      break;
   case 4:
      P_IMMD(push, NV902D, SET_DST_FORMAT, V_A8B8G8R8);
      P_IMMD(push, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT, V_A8B8G8R8);
      break;
   default:
      unreachable("TODO: More formats in CmdClearColorImage");
   }

   P_MTHD(push, NV902D, SET_RENDER_SOLID_PRIM_COLOR0);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR0(push, packed_color[0]);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR1(push, packed_color[1]);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR2(push, packed_color[2]);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR3(push, packed_color[3]);

   P_IMMD(push, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

   for (uint32_t r = 0; r < rangeCount; r++) {
      const uint32_t layer_count =
         vk_image_subresource_layer_count(&dst->vk, &pRanges[r]);
      const uint32_t level_count =
         vk_image_subresource_level_count(&dst->vk, &pRanges[r]);

      for (uint32_t w = 0; w < layer_count; w++) {
         const uint32_t layer = w + pRanges[r].baseArrayLayer;

         for (uint32_t l = 0; l < level_count; l++) {
            const uint32_t level = l + pRanges[r].baseMipLevel;

            const struct nil_image_level *dst_level = &dst->nil.levels[level];
            const VkDeviceSize dst_addr = nvk_image_base_address(dst) +
                                          layer * dst->nil.array_stride_B +
                                          dst_level->offset_B;

            P_MTHD(push, NV902D, SET_DST_OFFSET_UPPER);
            P_NV902D_SET_DST_OFFSET_UPPER(push, dst_addr >> 32);
            P_NV902D_SET_DST_OFFSET_LOWER(push, dst_addr & 0xffffffff);

            if (dst_level->tiling.is_tiled) {
               P_MTHD(push, NV902D, SET_DST_MEMORY_LAYOUT);
               P_NV902D_SET_DST_MEMORY_LAYOUT(push, V_BLOCKLINEAR);
               P_NV902D_SET_DST_BLOCK_SIZE(push, {
                  .height = dst_level->tiling.y_log2,
                  .depth = dst_level->tiling.z_log2,
               });
            } else {
               P_IMMD(push, NV902D, SET_DST_MEMORY_LAYOUT, V_PITCH);
            }

            const VkExtent3D dst_level_extent =
               vk_image_mip_level_extent(&dst->vk, level);

            P_MTHD(push, NV902D, SET_DST_DEPTH);
            P_NV902D_SET_DST_DEPTH(push, dst_level_extent.depth);

            P_MTHD(push, NV902D, SET_DST_PITCH);
            P_NV902D_SET_DST_PITCH(push, dst_level->row_stride_B);
            P_NV902D_SET_DST_WIDTH(push, dst_level_extent.width);
            P_NV902D_SET_DST_HEIGHT(push, dst_level_extent.height);

            for (uint32_t z = 0; z < dst_level_extent.depth; z++) {
               P_MTHD(push, NV902D, SET_DST_LAYER);
               P_NV902D_SET_DST_LAYER(push, z);

               const uint32_t x0 = 0;
               const uint32_t y0 = 0;
               const uint32_t x1 = dst_level_extent.width + 1;
               const uint32_t y1 = dst_level_extent.height + 1;

               P_MTHD(push, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
               P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 0, x0);
               P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 0, y0);
               P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(push, 1, x1);
               P_NV902D_RENDER_SOLID_PRIM_POINT_Y(push, 1, y1);
            }
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdFillBuffer(VkCommandBuffer commandBuffer,
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
   uint32_t pitch = 1 << 19;
   uint32_t line = pitch / 4;

   nvk_push_buffer_ref(push, dst, NOUVEAU_WS_BO_WR);

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
nvk_CmdUpdateBuffer(VkCommandBuffer commandBuffer,
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

   nvk_push_buffer_ref(push, dst, NOUVEAU_WS_BO_WR);

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

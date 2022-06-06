#include "nvk_cmd_buffer.h"

#include "vulkan/util/vk_format.h"

#include "nvk_buffer.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"

#include "nvtypes.h"
#include "classes/cl90b5.h"
#include "classes/clc1b5.h"
#include "push906f.h"

static void
nouveau_copy_linear(
   struct nouveau_ws_push *push,
   struct nouveau_ws_bo *src, uint64_t src_offset,
   struct nouveau_ws_bo *dst, uint64_t dst_offset,
   uint64_t size)
{
   VkDeviceSize src_addr = src->offset + src_offset;
   VkDeviceSize dst_addr = dst->offset + dst_offset;

   nouveau_ws_push_ref(push, src, NOUVEAU_WS_BO_RD);
   nouveau_ws_push_ref(push, dst, NOUVEAU_WS_BO_WR);

   while (size) {
      unsigned bytes = MIN2(size, 1 << 17);

      PUSH_MTHD(push, NV90B5, OFFSET_IN_UPPER,
                NVVAL(NV90B5, OFFSET_IN_UPPER, UPPER, src_addr >> 32),
                              OFFSET_IN_LOWER, src_addr & 0xffffffff,

                              OFFSET_OUT_UPPER,
                NVVAL(NV90B5, OFFSET_OUT_UPPER, UPPER, dst_addr >> 32),
                              OFFSET_OUT_LOWER, dst_addr & 0xffffffff);

      PUSH_MTHD(push, NV90B5, LINE_LENGTH_IN, bytes,
                              LINE_COUNT, 1);
      PUSH_IMMD(push, NV90B5, LAUNCH_DMA,
                NVDEF(NV90B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                NVDEF(NV90B5, LAUNCH_DMA, MULTI_LINE_ENABLE, FALSE) |
                NVDEF(NV90B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                NVDEF(NV90B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                NVDEF(NV90B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH));

      src_addr += bytes;
      dst_addr += bytes;
      size -= bytes;
   }
}

struct nouveau_copy_buffer {
   struct nouveau_ws_bo *bo;
   uint64_t bo_offset;
   VkOffset3D offset;
   VkExtent3D extent;
   uint32_t row_stride;
   uint32_t layer_stride;
   struct nvk_tile tile;
};

struct nouveau_copy {
   struct nouveau_copy_buffer src;
   struct nouveau_copy_buffer dst;
   uint32_t bpp;
   VkExtent3D extent;
};

static struct nouveau_copy_buffer
nouveau_copy_rect_buffer(
   struct nvk_buffer *buf,
   VkDeviceSize offset,
   struct vk_image_buffer_layout buffer_layout)
{
   return (struct nouveau_copy_buffer) {
      .bo = buf->mem->bo,
      .bo_offset = buf->offset + offset,
      .row_stride = buffer_layout.row_stride_B,
      .layer_stride = buffer_layout.image_stride_B,
   };
}

static struct nouveau_copy_buffer
nouveau_copy_rect_image(
   struct nvk_image *img,
   VkOffset3D offset,
   const VkImageSubresourceLayers *sub_res)
{
   struct nouveau_copy_buffer buf = {
      .bo = img->mem->bo,
      .bo_offset = img->offset,
      .offset = vk_image_sanitize_offset(&img->vk, offset),
      .extent = img->vk.extent,
      .row_stride = img->row_stride,
      .layer_stride = img->layer_stride,
      .tile = img->tile,
   };

   buf.extent.depth *= img->vk.array_layers;
   buf.offset.z += sub_res->baseArrayLayer;

   return buf;
}

static void
nouveau_copy_rect(struct nvk_cmd_buffer *cmd, struct nouveau_copy *copy)
{
   VkDeviceSize src_addr = copy->src.bo->offset + copy->src.bo_offset;
   VkDeviceSize dst_addr = copy->dst.bo->offset + copy->dst.bo_offset;

   if (!copy->src.tile.is_tiled) {
      src_addr +=
         copy->src.offset.x * copy->bpp +
         copy->src.offset.y * copy->src.row_stride +
         copy->src.offset.z * copy->src.layer_stride;
   }

   if (!copy->dst.tile.is_tiled) {
      dst_addr +=
         copy->dst.offset.x * copy->bpp +
         copy->dst.offset.y * copy->dst.row_stride +
         copy->dst.offset.z * copy->dst.layer_stride;
   }

   struct nouveau_ws_push *push = cmd->push;

   nouveau_ws_push_ref(push, copy->src.bo, NOUVEAU_WS_BO_RD);
   nouveau_ws_push_ref(push, copy->dst.bo, NOUVEAU_WS_BO_WR);

   for (unsigned z = 0; z < copy->extent.depth; z++) {
      PUSH_MTHD(push, NV90B5, OFFSET_IN_UPPER,
                NVVAL(NV90B5, OFFSET_IN_UPPER, UPPER, src_addr >> 32),
                              OFFSET_IN_LOWER, src_addr & 0xffffffff,

                              OFFSET_OUT_UPPER,
                NVVAL(NV90B5, OFFSET_OUT_UPPER, UPPER, dst_addr >> 32),
                              OFFSET_OUT_LOWER, dst_addr & 0xffffffff,
                              PITCH_IN, copy->src.row_stride,
                              PITCH_OUT, copy->dst.row_stride,
                              LINE_LENGTH_IN, copy->extent.width * copy->bpp,
                              LINE_COUNT, copy->extent.height);

      uint32_t pitch_cmd = 0;
      if (copy->src.tile.is_tiled) {
         assert(copy->src.tile.is_fermi);
         PUSH_MTHD(push, NV90B5, SET_SRC_BLOCK_SIZE,
                   NVVAL(NV90B5, SET_SRC_BLOCK_SIZE, WIDTH, copy->src.tile.x) |
                   NVVAL(NV90B5, SET_SRC_BLOCK_SIZE, HEIGHT, copy->src.tile.y) |
                   NVVAL(NV90B5, SET_SRC_BLOCK_SIZE, DEPTH, copy->src.tile.z) |
                   NVDEF(NV90B5, SET_SRC_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8),

                                 SET_SRC_WIDTH, copy->src.extent.width * copy->bpp,
                                 SET_SRC_HEIGHT, copy->src.extent.height,
                                 SET_SRC_DEPTH, copy->src.extent.depth,
                                 SET_SRC_LAYER, z + copy->src.offset.z);

         if (cmd->pool->dev->pdev->dev->cls >= 0xc1) {
            PUSH_MTHD(push, NVC1B5, SRC_ORIGIN_X,
                      NVVAL(NVC1B5, SRC_ORIGIN_X, VALUE, copy->src.offset.x * copy->bpp),
                                    SRC_ORIGIN_Y,
                      NVVAL(NVC1B5, SRC_ORIGIN_Y, VALUE, copy->src.offset.y));
         } else {
            PUSH_MTHD(push, NV90B5, SET_SRC_ORIGIN,
                      NVVAL(NV90B5, SET_SRC_ORIGIN, X, copy->src.offset.x * copy->bpp) |
                      NVVAL(NV90B5, SET_SRC_ORIGIN, Y, copy->src.offset.y));
         }

         pitch_cmd |= NVDEF(NV90B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, BLOCKLINEAR);
      } else {
         src_addr += copy->src.layer_stride;
         pitch_cmd |= NVDEF(NV90B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH);
      }

      if (copy->dst.tile.is_tiled) {
         assert(copy->dst.tile.is_fermi);
         PUSH_MTHD(push, NV90B5, SET_DST_BLOCK_SIZE,
                   NVVAL(NV90B5, SET_DST_BLOCK_SIZE, WIDTH, copy->dst.tile.x) |
                   NVVAL(NV90B5, SET_DST_BLOCK_SIZE, HEIGHT, copy->dst.tile.y) |
                   NVVAL(NV90B5, SET_DST_BLOCK_SIZE, DEPTH, copy->dst.tile.z) |
                   NVDEF(NV90B5, SET_DST_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8),

                                 SET_DST_WIDTH, copy->dst.extent.width * copy->bpp,
                                 SET_DST_HEIGHT, copy->dst.extent.height,
                                 SET_DST_DEPTH, copy->dst.extent.depth,
                                 SET_DST_LAYER, z + copy->dst.offset.z);

         if (cmd->pool->dev->pdev->dev->cls >= 0xc1) {
            PUSH_MTHD(push, NVC1B5, DST_ORIGIN_X,
                      NVVAL(NVC1B5, DST_ORIGIN_X, VALUE, copy->dst.offset.x * copy->bpp),
                                    DST_ORIGIN_Y,
                      NVVAL(NVC1B5, DST_ORIGIN_Y, VALUE, copy->dst.offset.y));
         } else {
            PUSH_MTHD(push, NV90B5, SET_DST_ORIGIN,
                      NVVAL(NV90B5, SET_DST_ORIGIN, X, copy->dst.offset.x * copy->bpp) |
                      NVVAL(NV90B5, SET_DST_ORIGIN, Y, copy->dst.offset.y));
         }

         pitch_cmd |= NVDEF(NV90B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, BLOCKLINEAR);
      } else {
         dst_addr += copy->dst.layer_stride;
         pitch_cmd |= NVDEF(NV90B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
      }

      PUSH_IMMD(push, NV90B5, LAUNCH_DMA,
                NVDEF(NV90B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                NVDEF(NV90B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
                NVDEF(NV90B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                pitch_cmd);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyBuffer(VkCommandBuffer commandBuffer,
                  VkBuffer srcBuffer, VkBuffer dstBuffer,
                  uint32_t regionCount, const VkBufferCopy* pRegions)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src, srcBuffer);
   VK_FROM_HANDLE(nvk_buffer, dst, dstBuffer);
   struct nouveau_ws_push *push = cmd->push;

   for (unsigned r = 0; r < regionCount; r++) {
      const VkBufferCopy *region = &pRegions[r];

      nouveau_copy_linear(
         push,
         src->mem->bo, src->offset + region->srcOffset,
         dst->mem->bo, dst->offset + region->dstOffset,
         region->size);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdCopyBufferToImage2(
   VkCommandBuffer commandBuffer,
   const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src, pCopyBufferToImageInfo->srcBuffer);
   VK_FROM_HANDLE(nvk_image, dst, pCopyBufferToImageInfo->dstImage);

   for (unsigned r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
      const VkBufferImageCopy2 *region = &pCopyBufferToImageInfo->pRegions[r];
      struct vk_image_buffer_layout buffer_layout = vk_image_buffer_copy_layout(&dst->vk, region);

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_buffer(src, region->bufferOffset, buffer_layout),
         .dst = nouveau_copy_rect_image(dst, region->imageOffset, &region->imageSubresource),
         .bpp = buffer_layout.element_size_B,
         .extent = vk_image_sanitize_extent(&dst->vk, region->imageExtent),
      };
      copy.extent.depth *= region->imageSubresource.layerCount;

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
nvk_CmdCopyImageToBuffer2(
   VkCommandBuffer commandBuffer,
   const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(nvk_buffer, dst, pCopyImageToBufferInfo->dstBuffer);

   for (unsigned r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
      const VkBufferImageCopy2 *region = &pCopyImageToBufferInfo->pRegions[r];
      struct vk_image_buffer_layout buffer_layout = vk_image_buffer_copy_layout(&src->vk, region);

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_image(src, region->imageOffset, &region->imageSubresource),
         .dst = nouveau_copy_rect_buffer(dst, region->bufferOffset, buffer_layout),
         .bpp = buffer_layout.element_size_B,
         .extent = vk_image_sanitize_extent(&src->vk, region->imageExtent),
      };
      copy.extent.depth *= region->imageSubresource.layerCount;

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
nvk_CmdCopyImage2(
   VkCommandBuffer commandBuffer,
   const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(nvk_image, dst, pCopyImageInfo->dstImage);

   uint16_t bpp = vk_format_description(src->vk.format)->block.bits;
   assert(bpp == vk_format_description(dst->vk.format)->block.bits);
   bpp /= 8;

   for (unsigned r = 0; r < pCopyImageInfo->regionCount; r++) {
      const VkImageCopy2 *region = &pCopyImageInfo->pRegions[r];

      struct nouveau_copy copy = {
         .src = nouveau_copy_rect_image(src, region->srcOffset, &region->srcSubresource),
         .dst = nouveau_copy_rect_image(dst, region->dstOffset, &region->dstSubresource),
         .bpp = bpp,
         .extent = vk_image_sanitize_extent(&src->vk, region->extent),
      };
      copy.extent.depth *= region->srcSubresource.layerCount;

      nouveau_copy_rect(cmd, &copy);
   }
}

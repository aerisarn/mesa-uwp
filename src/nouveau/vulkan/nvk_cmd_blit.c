#include "nvk_cmd_buffer.h"

#include "vulkan/util/vk_format.h"

#include "nvk_buffer.h"
#include "nvk_device_memory.h"
#include "nvk_format.h"
#include "nvk_image.h"

#include "nouveau_bo.h"

#include "nvtypes.h"
#include "nvk_cl902d.h"

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBlitImage2(VkCommandBuffer commandBuffer,
                  const VkBlitImageInfo2 *pBlitImageInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_image, src, pBlitImageInfo->srcImage);
   VK_FROM_HANDLE(nvk_image, dst, pBlitImageInfo->dstImage);

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 16);

   assert(nvk_get_format(src->vk.format)->supports_2d_blit);
   assert(nvk_get_format(dst->vk.format)->supports_2d_blit);

   P_IMMD(p, NV902D, SET_CLIP_ENABLE, V_FALSE);
   P_IMMD(p, NV902D, SET_COLOR_KEY_ENABLE, V_FALSE);
   P_IMMD(p, NV902D, SET_RENDER_ENABLE_C, MODE_TRUE);

   const uint32_t src_hw_format = nvk_get_format(src->vk.format)->hw_format;
   const uint32_t dst_hw_format = nvk_get_format(dst->vk.format)->hw_format;

   P_IMMD(p, NV902D, SET_SRC_FORMAT, src_hw_format);
   P_IMMD(p, NV902D, SET_DST_FORMAT, dst_hw_format);

   if (pBlitImageInfo->filter == VK_FILTER_NEAREST) {
      P_IMMD(p, NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, {
         .origin = ORIGIN_CORNER,
         .filter = FILTER_POINT,
      });
   } else {
      P_IMMD(p, NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, {
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
   if (vk_format_get_nr_components(src->vk.format) == 1 &&
       src_hw_format != dst_hw_format) {
      uint8_t mask = vk_format_is_snorm(dst->vk.format) ? 0x7f : 0xff;
      P_MTHD(p, NV902D, SET_BETA4);
      P_NV902D_SET_BETA4(p, {
         .r = mask,
         .a = mask,
      });
      P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY_PREMULT);
   } else {
      P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);
   }

   for (unsigned r = 0; r < pBlitImageInfo->regionCount; r++) {
      const VkImageBlit2 *region = &pBlitImageInfo->pRegions[r];
      p = nvk_cmd_buffer_push(cmd, 30 + region->srcSubresource.layerCount * 10);

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

      const struct nil_image_level *src_level =
         &src->nil.levels[region->srcSubresource.mipLevel];
      const VkExtent3D src_level_extent =
         vk_image_mip_level_extent(&src->vk, region->srcSubresource.mipLevel);

      if (src_level->tiling.is_tiled) {
         P_MTHD(p, NV902D, SET_SRC_MEMORY_LAYOUT);
         P_NV902D_SET_SRC_MEMORY_LAYOUT(p, V_BLOCKLINEAR);
         P_NV902D_SET_SRC_BLOCK_SIZE(p, {
            .height = src_level->tiling.y_log2,
            .depth = src_level->tiling.z_log2,
         });
      } else {
         P_IMMD(p, NV902D, SET_SRC_MEMORY_LAYOUT, V_PITCH);
      }

      P_MTHD(p, NV902D, SET_SRC_DEPTH);
      P_NV902D_SET_SRC_DEPTH(p, src_level_extent.depth);

      P_MTHD(p, NV902D, SET_SRC_PITCH);
      P_NV902D_SET_SRC_PITCH(p, src_level->row_stride_B);
      P_NV902D_SET_SRC_WIDTH(p, src_level_extent.width);
      P_NV902D_SET_SRC_HEIGHT(p, src_level_extent.height);

      const struct nil_image_level *dst_level =
         &dst->nil.levels[region->dstSubresource.mipLevel];
      const VkExtent3D dst_level_extent =
         vk_image_mip_level_extent(&dst->vk, region->dstSubresource.mipLevel);

      if (dst_level->tiling.is_tiled) {
         P_MTHD(p, NV902D, SET_DST_MEMORY_LAYOUT);
         P_NV902D_SET_DST_MEMORY_LAYOUT(p, V_BLOCKLINEAR);
         P_NV902D_SET_DST_BLOCK_SIZE(p, {
            .height = dst_level->tiling.y_log2,
            .depth = dst_level->tiling.z_log2,
         });
      } else {
         P_IMMD(p, NV902D, SET_DST_MEMORY_LAYOUT, V_PITCH);
      }

      P_MTHD(p, NV902D, SET_DST_DEPTH);
      P_NV902D_SET_DST_DEPTH(p, dst_level_extent.depth);

      P_MTHD(p, NV902D, SET_DST_PITCH);
      P_NV902D_SET_DST_PITCH(p, dst_level->row_stride_B);
      P_NV902D_SET_DST_WIDTH(p, dst_level_extent.width);
      P_NV902D_SET_DST_HEIGHT(p, dst_level_extent.height);

      P_MTHD(p, NV902D, SET_PIXELS_FROM_MEMORY_DST_X0);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_X0(p, dst_start_x);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_Y0(p, dst_start_y);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH(p, dst_width);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT(p, dst_height);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC(p, scaling_x_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT(p, scaling_x_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_FRAC(p, scaling_y_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT(p, scaling_y_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC(p, src_start_x_fp & 0xffffffff);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_INT(p, src_start_x_fp >> 32);
      P_NV902D_SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC(p, src_start_y_fp & 0xffffffff);

      assert(src->vk.image_type != VK_IMAGE_TYPE_3D);
      assert(dst->vk.image_type != VK_IMAGE_TYPE_3D);
      for (unsigned w = 0; w < region->srcSubresource.layerCount; w++) {
         const uint32_t src_layer = w + region->srcSubresource.baseArrayLayer;
         const VkDeviceSize src_addr = nvk_image_base_address(src) +
                                       src_layer * src->nil.array_stride_B +
                                       src_level->offset_B;

         const uint32_t dst_layer = w + region->dstSubresource.baseArrayLayer;
         const VkDeviceSize dst_addr = nvk_image_base_address(dst) +
                                       dst_layer * dst->nil.array_stride_B +
                                       dst_level->offset_B;

         P_MTHD(p, NV902D, SET_SRC_OFFSET_UPPER);
         P_NV902D_SET_SRC_OFFSET_UPPER(p, src_addr >> 32);
         P_NV902D_SET_SRC_OFFSET_LOWER(p, src_addr & 0xffffffff);

         P_MTHD(p, NV902D, SET_DST_OFFSET_UPPER);
         P_NV902D_SET_DST_OFFSET_UPPER(p, dst_addr >> 32);
         P_NV902D_SET_DST_OFFSET_LOWER(p, dst_addr & 0xffffffff);

         P_MTHD(p, NV902D, SET_DST_LAYER);
         P_NV902D_SET_DST_LAYER(p, 0);

         P_MTHD(p, NV902D, PIXELS_FROM_MEMORY_SRC_Y0_INT);
         P_NV902D_PIXELS_FROM_MEMORY_SRC_Y0_INT(p, src_start_y_fp >> 32);
      }
   }
}

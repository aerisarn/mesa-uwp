/*
 * Copyright © 2023 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_SDMA_H
#define RADV_SDMA_H

#include "radv_private.h"

#ifdef __cplusplus
extern "C" {
#endif

struct radv_sdma_surf {
   VkExtent3D extent; /* Image extent. */
   uint64_t va;       /* Virtual address of image data. */
   unsigned bpp;      /* Bytes per pixel. */
   unsigned blk_w;    /* Image format block width in pixels. */
   unsigned blk_h;    /* Image format block height in pixels. */

   union {
      /* linear images only */
      struct {
         unsigned pitch;       /* Row pitch in bytes. */
         unsigned slice_pitch; /* Slice pitch in bytes. */
      };
      /* tiled images only */
      struct {
         uint64_t meta_va;      /* Virtual address of metadata. */
         uint32_t meta_config;  /* Metadata configuration DWORD. */
         uint32_t header_dword; /* Extra bits for the copy packet header. */
         uint32_t info_dword;   /* Image information DWORD. */
      };
   };
};

void radv_sdma_copy_buffer_image(const struct radv_device *device, struct radeon_cmdbuf *cs, struct radv_image *image,
                                 struct radv_buffer *buffer, const VkBufferImageCopy2 *region, bool to_image);
bool radv_sdma_use_unaligned_buffer_image_copy(const struct radv_device *device, const struct radv_image *image,
                                               const struct radv_buffer *buffer, const VkBufferImageCopy2 *region);
void radv_sdma_copy_buffer_image_unaligned(const struct radv_device *device, struct radeon_cmdbuf *cs,
                                           struct radv_image *image, struct radv_buffer *buffer,
                                           const VkBufferImageCopy2 *region, struct radeon_winsys_bo *temp_bo,
                                           bool to_image);
void radv_sdma_copy_buffer(const struct radv_device *device, struct radeon_cmdbuf *cs, uint64_t src_va, uint64_t dst_va,
                           uint64_t size);
void radv_sdma_fill_buffer(const struct radv_device *device, struct radeon_cmdbuf *cs, const uint64_t va,
                           const uint64_t size, const uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* RADV_SDMA_H */

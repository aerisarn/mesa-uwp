/*
 * Copyright (C) 2022 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pan_texture.h"

#include <gtest/gtest.h>

TEST(BlockSize, Linear)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R8G8B8_UNORM,
      PIPE_FORMAT_ETC2_RGB8,
      PIPE_FORMAT_ASTC_5x5
   };

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_block_size blk = panfrost_block_size(DRM_FORMAT_MOD_LINEAR, format[i]);

      EXPECT_EQ(blk.width, 1);
      EXPECT_EQ(blk.height, 1);
   }
}

TEST(BlockSize, UInterleavedRegular)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R8G8B8_UNORM,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_block_size blk = panfrost_block_size(DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED, format[i]);

      EXPECT_EQ(blk.width, 16);
      EXPECT_EQ(blk.height, 16);
   }
}

TEST(BlockSize, UInterleavedBlockCompressed)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_ETC2_RGB8,
      PIPE_FORMAT_ASTC_5x5
   };

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_block_size blk = panfrost_block_size(DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED, format[i]);

      EXPECT_EQ(blk.width, 4);
      EXPECT_EQ(blk.height, 4);
   }
}

TEST(BlockSize, AFBCFormatInvariant16x16)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R8G8B8_UNORM,
      PIPE_FORMAT_ETC2_RGB8,
      PIPE_FORMAT_ASTC_5x5
   };

   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE |
                AFBC_FORMAT_MOD_YTR);

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_block_size blk = panfrost_block_size(modifier, format[i]);

      EXPECT_EQ(blk.width, 16);
      EXPECT_EQ(blk.height, 16);
   }
}

TEST(BlockSize, AFBCFormatInvariant32x8)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R8G8B8_UNORM,
      PIPE_FORMAT_ETC2_RGB8,
      PIPE_FORMAT_ASTC_5x5
   };

   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                AFBC_FORMAT_MOD_SPARSE |
                AFBC_FORMAT_MOD_YTR);

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_block_size blk = panfrost_block_size(modifier, format[i]);

      EXPECT_EQ(blk.width, 32);
      EXPECT_EQ(blk.height, 8);
   }
}

TEST(BlockSize, AFBCSuperblock16x16)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE |
                AFBC_FORMAT_MOD_YTR);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).width, 16);
   EXPECT_EQ(panfrost_afbc_superblock_width(modifier), 16);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).height, 16);
   EXPECT_EQ(panfrost_afbc_superblock_height(modifier), 16);

   EXPECT_FALSE(panfrost_afbc_is_wide(modifier));
}

TEST(BlockSize, AFBCSuperblock32x8)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                AFBC_FORMAT_MOD_SPARSE);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).width, 32);
   EXPECT_EQ(panfrost_afbc_superblock_width(modifier), 32);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).height, 8);
   EXPECT_EQ(panfrost_afbc_superblock_height(modifier), 8);

   EXPECT_TRUE(panfrost_afbc_is_wide(modifier));
}

TEST(BlockSize, AFBCSuperblock64x4)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_64x4 |
                AFBC_FORMAT_MOD_SPARSE);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).width, 64);
   EXPECT_EQ(panfrost_afbc_superblock_width(modifier), 64);

   EXPECT_EQ(panfrost_afbc_superblock_size(modifier).height, 4);
   EXPECT_EQ(panfrost_afbc_superblock_height(modifier), 4);

   EXPECT_TRUE(panfrost_afbc_is_wide(modifier));
}

/*
 * Copyright © 2021 Collabora Ltd.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "gen_macros.h"

#include "panvk_private.h"

void
panvk_per_arch(CmdCopyImage)(VkCommandBuffer commandBuffer,
                             VkImage srcImage,
                             VkImageLayout srcImageLayout,
                             VkImage destImage,
                             VkImageLayout destImageLayout,
                             uint32_t regionCount,
                             const VkImageCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdCopyBufferToImage)(VkCommandBuffer commandBuffer,
                                     VkBuffer srcBuffer,
                                     VkImage destImage,
                                     VkImageLayout destImageLayout,
                                     uint32_t regionCount,
                                     const VkBufferImageCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdCopyImageToBuffer)(VkCommandBuffer commandBuffer,
                                     VkImage srcImage,
                                     VkImageLayout srcImageLayout,
                                     VkBuffer destBuffer,
                                     uint32_t regionCount,
                                     const VkBufferImageCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdCopyBuffer)(VkCommandBuffer commandBuffer,
                              VkBuffer srcBuffer,
                              VkBuffer destBuffer,
                              uint32_t regionCount,
                              const VkBufferCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdFillBuffer)(VkCommandBuffer commandBuffer,
                              VkBuffer dstBuffer,
                              VkDeviceSize dstOffset,
                              VkDeviceSize fillSize,
                              uint32_t data)
{
   panvk_stub();
}

void
panvk_per_arch(CmdUpdateBuffer)(VkCommandBuffer commandBuffer,
                                VkBuffer dstBuffer,
                                VkDeviceSize dstOffset,
                                VkDeviceSize dataSize,
                                const void *pData)
{
   panvk_stub();
}

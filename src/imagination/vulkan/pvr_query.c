/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pvr_private.h"

VkResult pvr_CreateQueryPool(VkDevice _device,
                             const VkQueryPoolCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkQueryPool *pQueryPool)
{
   assert(!"Unimplemented");
   return VK_SUCCESS;
}

void pvr_DestroyQueryPool(VkDevice _device,
                          VkQueryPool queryPool,
                          const VkAllocationCallbacks *pAllocator)
{
   assert(!"Unimplemented");
}

VkResult pvr_GetQueryPoolResults(VkDevice _device,
                                 VkQueryPool queryPool,
                                 uint32_t firstQuery,
                                 uint32_t queryCount,
                                 size_t dataSize,
                                 void *pData,
                                 VkDeviceSize stride,
                                 VkQueryResultFlags flags)
{
   assert(!"Unimplemented");
   return VK_SUCCESS;
}

void pvr_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                           VkQueryPool queryPool,
                           uint32_t firstQuery,
                           uint32_t queryCount)
{
   assert(!"Unimplemented");
}

void pvr_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                                 VkQueryPool queryPool,
                                 uint32_t firstQuery,
                                 uint32_t queryCount,
                                 VkBuffer dstBuffer,
                                 VkDeviceSize dstOffset,
                                 VkDeviceSize stride,
                                 VkQueryResultFlags flags)
{
   assert(!"Unimplemented");
}

void pvr_CmdBeginQuery(VkCommandBuffer commandBuffer,
                       VkQueryPool queryPool,
                       uint32_t query,
                       VkQueryControlFlags flags)
{
   assert(!"Unimplemented");
}

void pvr_CmdEndQuery(VkCommandBuffer commandBuffer,
                     VkQueryPool queryPool,
                     uint32_t query)
{
   assert(!"Unimplemented");
}

/*
 * Copyright © 2015 Intel Corporation
 * Copyright © 2022 Collabora, Ltd
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

#include "vk_command_pool.h"

#include "vk_common_entrypoints.h"
#include "vk_device.h"

VkResult MUST_CHECK
vk_command_pool_init(struct vk_command_pool *pool,
                     struct vk_device *device,
                     const VkCommandPoolCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator)
{
   memset(pool, 0, sizeof(*pool));
   vk_object_base_init(device, &pool->base,
                       VK_OBJECT_TYPE_COMMAND_POOL);

   pool->flags = pCreateInfo->flags;
   pool->queue_family_index = pCreateInfo->queueFamilyIndex;
   pool->alloc = pAllocator ? *pAllocator : device->alloc;

   return VK_SUCCESS;
}

void
vk_command_pool_finish(struct vk_command_pool *pool)
{
   vk_object_base_finish(&pool->base);
}

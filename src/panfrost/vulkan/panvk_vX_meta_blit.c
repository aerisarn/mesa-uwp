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

#include "pan_blitter.h"

#include "panvk_private.h"

void
panvk_per_arch(CmdBlitImage)(VkCommandBuffer commandBuffer,
                             VkImage srcImage,
                             VkImageLayout srcImageLayout,
                             VkImage destImage,
                             VkImageLayout destImageLayout,
                             uint32_t regionCount,
                             const VkImageBlit *pRegions,
                             VkFilter filter)

{
   panvk_stub();
}

void
panvk_per_arch(CmdResolveImage)(VkCommandBuffer commandBuffer,
                                VkImage srcImage,
                                VkImageLayout srcImageLayout,
                                VkImage destImage,
                                VkImageLayout destImageLayout,
                                uint32_t regionCount,
                                const VkImageResolve *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(meta_blit_init)(struct panvk_physical_device *dev)
{
   panvk_pool_init(&dev->meta.blitter.bin_pool, &dev->pdev, NULL,
                   PAN_BO_EXECUTE, 16 * 1024,
                   "panvk_meta blitter binary pool", false);
   panvk_pool_init(&dev->meta.blitter.desc_pool, &dev->pdev, NULL,
                   0, 16 * 1024, "panvk_meta blitter descriptor pool",
                   false);
   pan_blend_shaders_init(&dev->pdev);
   GENX(pan_blitter_init)(&dev->pdev, &dev->meta.blitter.bin_pool.base,
                          &dev->meta.blitter.desc_pool.base);
}

void
panvk_per_arch(meta_blit_cleanup)(struct panvk_physical_device *dev)
{
   GENX(pan_blitter_cleanup)(&dev->pdev);
   pan_blend_shaders_cleanup(&dev->pdev);
   panvk_pool_cleanup(&dev->meta.blitter.desc_pool);
   panvk_pool_cleanup(&dev->meta.blitter.bin_pool);
}

/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
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
 *
 */

#ifndef __PANVK_POOL_H__
#define __PANVK_POOL_H__

#include "pan_pool.h"

/* Represents grow-only memory. It may be owned by the batch (OpenGL), or may
   be unowned for persistent uploads. */

struct panvk_pool {
   /* Inherit from pan_pool */
   struct pan_pool base;

   /* BOs allocated by this pool */
   struct util_dynarray bos;

   /* Current transient BO */
   struct panfrost_bo *transient_bo;

   /* Within the topmost transient BO, how much has been used? */
   unsigned transient_offset;

};

static inline struct panvk_pool *
to_panvk_pool(struct pan_pool *pool)
{
   return container_of(pool, struct panvk_pool, base);
}

void
panvk_pool_init(struct panvk_pool *pool, struct panfrost_device *dev,
                unsigned create_flags, size_t slab_size, const char *label,
                bool prealloc);

void
panvk_pool_cleanup(struct panvk_pool *pool);

static inline unsigned
panvk_pool_num_bos(struct panvk_pool *pool)
{
   return util_dynarray_num_elements(&pool->bos, struct panfrost_bo *);
}

void
panvk_pool_get_bo_handles(struct panvk_pool *pool, uint32_t *handles);

#endif

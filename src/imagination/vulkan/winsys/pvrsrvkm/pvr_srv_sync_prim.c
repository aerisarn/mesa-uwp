/*
 * Copyright © 2023 Imagination Technologies Ltd.
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

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "pvr_srv.h"
#include "pvr_srv_bridge.h"
#include "pvr_srv_sync_prim.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_log.h"

/* Amount of space used to hold sync prim values (in bytes). */
#define PVR_SRV_SYNC_PRIM_VALUE_SIZE 4

VkResult pvr_srv_sync_prim_block_init(struct pvr_srv_winsys *srv_ws)
{
   /* We don't currently make use of this value, but we're required to provide
    * a valid pointer to pvr_srv_alloc_sync_primitive_block.
    */
   void *sync_block_pmr;

   return pvr_srv_alloc_sync_primitive_block(
      srv_ws->render_fd,
      &srv_ws->sync_prim_ctx.block_handle,
      &sync_block_pmr,
      &srv_ws->sync_prim_ctx.block_size,
      &srv_ws->sync_prim_ctx.block_fw_addr);
}

void pvr_srv_sync_prim_block_finish(struct pvr_srv_winsys *srv_ws)
{
   pvr_srv_free_sync_primitive_block(srv_ws->render_fd,
                                     srv_ws->sync_prim_ctx.block_handle);
   srv_ws->sync_prim_ctx.block_handle = NULL;
}

struct pvr_srv_sync_prim *pvr_srv_sync_prim_alloc(struct pvr_srv_winsys *srv_ws)
{
   struct pvr_srv_sync_prim *sync_prim;

   if (p_atomic_read(&srv_ws->sync_prim_ctx.block_offset) ==
       srv_ws->sync_prim_ctx.block_size) {
      vk_error(NULL, VK_ERROR_UNKNOWN);
      return NULL;
   }

   sync_prim = vk_alloc(srv_ws->alloc,
                        sizeof(*sync_prim),
                        8,
                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!sync_prim) {
      vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   /* p_atomic_add_return() returns the new value rather than the old one, so
    * we have to subtract PVR_SRV_SYNC_PRIM_VALUE_SIZE to get the old value.
    */
   sync_prim->offset = p_atomic_add_return(&srv_ws->sync_prim_ctx.block_offset,
                                           PVR_SRV_SYNC_PRIM_VALUE_SIZE);
   sync_prim->offset -= PVR_SRV_SYNC_PRIM_VALUE_SIZE;
   if (sync_prim->offset == srv_ws->sync_prim_ctx.block_size) {
      /* FIXME: need to free offset back to srv_ws->sync_block_offset. */
      vk_free(srv_ws->alloc, sync_prim);

      vk_error(NULL, VK_ERROR_UNKNOWN);

      return NULL;
   }

   sync_prim->ctx = &srv_ws->sync_prim_ctx;

   return sync_prim;
}

/* FIXME: Add support for freeing offsets back to the sync block. */
void pvr_srv_sync_prim_free(struct pvr_srv_winsys *srv_ws,
                            struct pvr_srv_sync_prim *sync_prim)
{
   vk_free(srv_ws->alloc, sync_prim);
}

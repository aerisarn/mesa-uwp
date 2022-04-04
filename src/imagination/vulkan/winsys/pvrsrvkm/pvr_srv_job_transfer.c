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

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "fw-api/pvr_rogue_fwif_rf.h"
#include "pvr_private.h"
#include "pvr_srv.h"
#include "pvr_srv_bridge.h"
#include "pvr_srv_job_common.h"
#include "pvr_srv_job_transfer.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#define PVR_SRV_TRANSFER_CONTEXT_INITIAL_CCB_SIZE_LOG2 16U
#define PVR_SRV_TRANSFER_CONTEXT_MAX_CCB_SIZE_LOG2 0U

struct pvr_srv_winsys_transfer_ctx {
   struct pvr_winsys_transfer_ctx base;

   void *handle;

   int timeline;
};

#define to_pvr_srv_winsys_transfer_ctx(ctx) \
   container_of(ctx, struct pvr_srv_winsys_transfer_ctx, base)

VkResult pvr_srv_winsys_transfer_ctx_create(
   struct pvr_winsys *ws,
   const struct pvr_winsys_transfer_ctx_create_info *create_info,
   struct pvr_winsys_transfer_ctx **const ctx_out)
{
   struct pvr_srv_winsys *srv_ws = to_pvr_srv_winsys(ws);
   struct pvr_srv_winsys_transfer_ctx *srv_ctx;
   struct rogue_fwif_rf_cmd reset_cmd = { 0 };
   VkResult result;

   /* First 2 U8s are 2d work load related, and the last 2 are 3d workload
    * related.
    */
   const uint32_t packed_ccb_size =
      PVR_U8888_TO_U32(PVR_SRV_TRANSFER_CONTEXT_INITIAL_CCB_SIZE_LOG2,
                       PVR_SRV_TRANSFER_CONTEXT_MAX_CCB_SIZE_LOG2,
                       PVR_SRV_TRANSFER_CONTEXT_INITIAL_CCB_SIZE_LOG2,
                       PVR_SRV_TRANSFER_CONTEXT_MAX_CCB_SIZE_LOG2);

   srv_ctx = vk_alloc(srv_ws->alloc,
                      sizeof(*srv_ctx),
                      8U,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!srv_ctx)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = pvr_srv_create_timeline(srv_ws->render_fd, &srv_ctx->timeline);
   if (result != VK_SUCCESS)
      goto err_free_srv_ctx;

   /* TODO: Add support for reset framework. Currently we subtract
    * reset_cmd.regs size from reset_cmd size to only pass empty flags field.
    */
   result = pvr_srv_rgx_create_transfer_context(
      srv_ws->render_fd,
      pvr_srv_from_winsys_priority(create_info->priority),
      sizeof(reset_cmd) - sizeof(reset_cmd.regs),
      (uint8_t *)&reset_cmd,
      srv_ws->server_memctx_data,
      packed_ccb_size,
      RGX_CONTEXT_FLAG_DISABLESLR,
      0U,
      NULL,
      NULL,
      &srv_ctx->handle);
   if (result != VK_SUCCESS)
      goto err_close_timeline;

   srv_ctx->base.ws = ws;
   *ctx_out = &srv_ctx->base;

   return VK_SUCCESS;

err_close_timeline:
   close(srv_ctx->timeline);

err_free_srv_ctx:
   vk_free(srv_ws->alloc, srv_ctx);

   return result;
}

void pvr_srv_winsys_transfer_ctx_destroy(struct pvr_winsys_transfer_ctx *ctx)
{
   struct pvr_srv_winsys *srv_ws = to_pvr_srv_winsys(ctx->ws);
   struct pvr_srv_winsys_transfer_ctx *srv_ctx =
      to_pvr_srv_winsys_transfer_ctx(ctx);

   pvr_srv_rgx_destroy_transfer_context(srv_ws->render_fd, srv_ctx->handle);
   close(srv_ctx->timeline);
   vk_free(srv_ws->alloc, srv_ctx);
}

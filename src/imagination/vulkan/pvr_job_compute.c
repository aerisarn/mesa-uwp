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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "pvr_csb.h"
#include "pvr_job_common.h"
#include "pvr_job_context.h"
#include "pvr_job_compute.h"
#include "pvr_private.h"
#include "pvr_winsys.h"
#include "util/macros.h"

static void pvr_compute_job_ws_submit_info_init(
   struct pvr_compute_ctx *ctx,
   struct pvr_sub_cmd_compute *sub_cmd,
   struct vk_sync *barrier,
   struct vk_sync **waits,
   uint32_t wait_count,
   uint32_t *stage_flags,
   struct pvr_winsys_compute_submit_info *submit_info)
{
   const struct pvr_device *const device = ctx->device;
   const struct pvr_physical_device *const pdevice = device->pdevice;
   const struct pvr_device_runtime_info *const dev_runtime_info =
      &pdevice->dev_runtime_info;
   const struct pvr_device_info *const dev_info = &pdevice->dev_info;
   const struct pvr_compute_ctx_switch *const ctx_switch = &ctx->ctx_switch;
   uint32_t shared_regs = sub_cmd->num_shared_regs;

   submit_info->frame_num = device->global_queue_present_count;
   submit_info->job_num = device->global_queue_job_count;

   submit_info->barrier = barrier;

   submit_info->waits = waits;
   submit_info->wait_count = wait_count;
   submit_info->stage_flags = stage_flags;

   pvr_csb_pack (&submit_info->regs.cdm_ctrl_stream_base,
                 CR_CDM_CTRL_STREAM_BASE,
                 value) {
      value.addr = pvr_csb_get_start_address(&sub_cmd->control_stream);
   }

   /* FIXME: Need to set up the border color table at device creation
    * time. Set to invalid for the time being.
    */
   pvr_csb_pack (&submit_info->regs.tpu_border_colour_table,
                 CR_TPU_BORDER_COLOUR_TABLE_CDM,
                 value) {
      value.border_colour_table_address = PVR_DEV_ADDR_INVALID;
   }

   if (PVR_HAS_FEATURE(dev_info, compute_morton_capable))
      submit_info->regs.cdm_item = 0;

   pvr_csb_pack (&submit_info->regs.tpu, CR_TPU, value) {
      value.tag_cem_4k_face_packing = true;
   }

   pvr_csb_pack (&submit_info->regs.compute_cluster,
                 CR_COMPUTE_CLUSTER,
                 value) {
      if (PVR_HAS_FEATURE(dev_info, cluster_grouping) &&
          PVR_HAS_FEATURE(dev_info, slc_mcu_cache_controls) &&
          dev_runtime_info->num_phantoms > 1 && sub_cmd->uses_atomic_ops) {
         /* Each phantom has its own MCU, so atomicity can only be guaranteed
          * when all work items are processed on the same phantom. This means we
          * need to disable all USCs other than those of the first phantom,
          * which has 4 clusters.
          */
         value.mask = 0xFU;
      } else {
         value.mask = 0U;
      }
   }

   pvr_csb_pack (&submit_info->regs.cdm_ctx_state_base_addr,
                 CR_CDM_CONTEXT_STATE_BASE,
                 state) {
      state.addr = ctx_switch->compute_state_bo->vma->dev_addr;
   }

   pvr_csb_pack (&submit_info->regs.cdm_resume_pds1,
                 CR_CDM_CONTEXT_PDS1,
                 state) {
      /* Convert the data size from dwords to bytes. */
      const uint32_t load_program_data_size =
         ctx_switch->sr[0].pds.load_program.data_size * 4U;

      state.pds_seq_dep = false;
      state.usc_seq_dep = false;
      state.target = false;
      state.unified_size = ctx_switch->sr[0].usc.unified_size;
      state.common_shared = true;
      state.common_size =
         DIV_ROUND_UP(shared_regs << 2,
                      PVRX(CR_CDM_CONTEXT_PDS1_COMMON_SIZE_UNIT_SIZE));
      state.temp_size = 0;

      assert(load_program_data_size %
                PVRX(CR_CDM_CONTEXT_PDS1_DATA_SIZE_UNIT_SIZE) ==
             0);
      state.data_size =
         load_program_data_size / PVRX(CR_CDM_CONTEXT_PDS1_DATA_SIZE_UNIT_SIZE);
      state.fence = false;
   }

   if (sub_cmd->uses_barrier)
      submit_info->flags |= PVR_WINSYS_COMPUTE_FLAG_PREVENT_ALL_OVERLAP;

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support) &&
       sub_cmd->uses_atomic_ops) {
      submit_info->flags |= PVR_WINSYS_COMPUTE_FLAG_SINGLE_CORE;
   }
}

VkResult pvr_compute_job_submit(struct pvr_compute_ctx *ctx,
                                struct pvr_sub_cmd_compute *sub_cmd,
                                struct vk_sync *barrier,
                                struct vk_sync **waits,
                                uint32_t wait_count,
                                uint32_t *stage_flags,
                                struct vk_sync *signal_sync)
{
   struct pvr_winsys_compute_submit_info submit_info;
   struct pvr_device *device = ctx->device;

   pvr_compute_job_ws_submit_info_init(ctx,
                                       sub_cmd,
                                       barrier,
                                       waits,
                                       wait_count,
                                       stage_flags,
                                       &submit_info);

   return device->ws->ops->compute_submit(ctx->ws_ctx,
                                          &submit_info,
                                          signal_sync);
}

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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "hwdef/rogue_hw_utils.h"
#include "pvr_hardcode.h"
#include "pvr_private.h"
#include "rogue/rogue.h"
#include "usc/hardcoded_apps/pvr_simple_compute.h"
#include "util/macros.h"
#include "util/u_process.h"

/**
 * \file pvr_hardcode.c
 *
 * \brief Contains hard coding functions.
 * This should eventually be deleted as the compiler becomes more capable.
 */

/* Applications for which the compiler is capable of generating valid shaders.
 */
static const char *const compilable_progs[] = {
   "triangle",
};

static const struct pvr_hard_coding_data {
   const char *const name;

   union {
      struct {
         const uint8_t *const shader;
         size_t shader_size;

         /* Note that the bo field will be unused. */
         const struct pvr_compute_pipeline_shader_state shader_info;

         const struct pvr_hard_code_compute_build_info build_info;
      } compute;
   };

} hard_coding_table[] = {
   {
      .name = "simple-compute",
      .compute = {
         .shader = pvr_simple_compute_shader,
         .shader_size = sizeof(pvr_simple_compute_shader),

         .shader_info = {
            .uses_atomic_ops = false,
            .uses_barrier = false,
            .uses_num_workgroups = false,

            .const_shared_reg_count = 4,
            .input_register_count = 8,
            .work_size = 1 * 1 * 1,
            .coefficient_register_count = 4,
         },

         .build_info = {
            .ubo_data = { 0 },

            .local_invocation_regs = { 0, 1 },
            .work_group_regs = { 0, 1, 2 },
            .barrier_reg = ROGUE_REG_UNUSED,
            .usc_temps = 0,

            .explicit_conts_usage = {
               .start_offset = 0,
            },
         },
      }
   },
};

bool pvr_hard_code_shader_required(void)
{
   const char *const program = util_get_process_name();

   for (uint32_t i = 0; i < ARRAY_SIZE(compilable_progs); i++) {
      if (strcmp(program, compilable_progs[i]) == 0)
         return false;
   }

   return true;
}

static const struct pvr_hard_coding_data *pvr_get_hard_coding_data()
{
   const char *const program = util_get_process_name();

   for (uint32_t i = 0; i < ARRAY_SIZE(hard_coding_table); i++) {
      if (strcmp(program, hard_coding_table[i].name) == 0)
         return &hard_coding_table[i];
   }

   mesa_loge("Could not find hard coding data for %s", program);

   return NULL;
}

VkResult pvr_hard_code_compute_pipeline(
   struct pvr_device *const device,
   struct pvr_compute_pipeline_shader_state *const shader_state_out,
   struct pvr_hard_code_compute_build_info *const build_info_out)
{
   const uint32_t cache_line_size =
      rogue_get_slc_cache_line_size(&device->pdevice->dev_info);
   const struct pvr_hard_coding_data *const data = pvr_get_hard_coding_data();

   mesa_logd("Hard coding compute pipeline for %s", data->name);

   *build_info_out = data->compute.build_info;
   *shader_state_out = data->compute.shader_info;

   return pvr_gpu_upload_usc(device,
                             data->compute.shader,
                             data->compute.shader_size,
                             cache_line_size,
                             &shader_state_out->bo);
}

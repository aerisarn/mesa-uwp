/*
 * Copyright © 2006-2022 Intel Corporation
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

#include "brw_fs.h"

using namespace brw;

tcs_thread_payload::tcs_thread_payload(const fs_visitor &v)
{
   struct brw_vue_prog_data *vue_prog_data = brw_vue_prog_data(v.prog_data);
   struct brw_tcs_prog_data *tcs_prog_data = brw_tcs_prog_data(v.prog_data);
   struct brw_tcs_prog_key *tcs_key = (struct brw_tcs_prog_key *) v.key;

   if (vue_prog_data->dispatch_mode == DISPATCH_MODE_TCS_SINGLE_PATCH) {
      patch_urb_output = retype(brw_vec1_grf(0, 0), BRW_REGISTER_TYPE_UD);
      primitive_id = brw_vec1_grf(0, 1);

      /* r1-r4 contain the ICP handles. */
      icp_handle_start = retype(brw_vec8_grf(1, 0), BRW_REGISTER_TYPE_UD);

      num_regs = 5;
   } else {
      assert(vue_prog_data->dispatch_mode == DISPATCH_MODE_TCS_MULTI_PATCH);
      assert(tcs_key->input_vertices > 0);

      patch_urb_output = retype(brw_vec8_grf(1, 0), BRW_REGISTER_TYPE_UD);

      unsigned r = 2;

      if (tcs_prog_data->include_primitive_id)
         primitive_id = brw_vec8_grf(r++, 0);

      /* ICP handles occupy the next 1-32 registers. */
      icp_handle_start = retype(brw_vec8_grf(r, 0), BRW_REGISTER_TYPE_UD);
      r += tcs_key->input_vertices;

      num_regs = r;
   }
}

static inline void
setup_fs_payload_gfx6(fs_thread_payload &payload,
                      const fs_visitor &v,
                      bool &source_depth_to_render_target)
{
   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(v.prog_data);

   const unsigned payload_width = MIN2(16, v.dispatch_width);
   assert(v.dispatch_width % payload_width == 0);
   assert(v.devinfo->ver >= 6);

   payload.num_regs = 0;

   /* R0: PS thread payload header. */
   payload.num_regs++;

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R1: masks, pixel X/Y coordinates. */
      payload.subspan_coord_reg[j] = payload.num_regs++;
   }

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R3-26: barycentric interpolation coordinates.  These appear in the
       * same order that they appear in the brw_barycentric_mode enum.  Each
       * set of coordinates occupies 2 registers if dispatch width == 8 and 4
       * registers if dispatch width == 16.  Coordinates only appear if they
       * were enabled using the "Barycentric Interpolation Mode" bits in
       * WM_STATE.
       */
      for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
         if (prog_data->barycentric_interp_modes & (1 << i)) {
            payload.barycentric_coord_reg[i][j] = payload.num_regs;
            payload.num_regs += payload_width / 4;
         }
      }

      /* R27-28: interpolated depth if uses source depth */
      if (prog_data->uses_src_depth) {
         payload.source_depth_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R29-30: interpolated W set if GFX6_WM_USES_SOURCE_W. */
      if (prog_data->uses_src_w) {
         payload.source_w_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R31: MSAA position offsets. */
      if (prog_data->uses_pos_offset) {
         payload.sample_pos_reg[j] = payload.num_regs;
         payload.num_regs++;
      }

      /* R32-33: MSAA input coverage mask */
      if (prog_data->uses_sample_mask) {
         assert(v.devinfo->ver >= 7);
         payload.sample_mask_in_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R66: Source Depth and/or W Attribute Vertex Deltas */
      if (prog_data->uses_depth_w_coefficients) {
         payload.depth_w_coef_reg[j] = payload.num_regs;
         payload.num_regs++;
      }
   }

   if (v.nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      source_depth_to_render_target = true;
   }
}

#undef P                        /* prompted depth */
#undef C                        /* computed */
#undef N                        /* non-promoted? */

#define P 0
#define C 1
#define N 2

static const struct {
   GLuint mode:2;
   GLuint sd_present:1;
   GLuint sd_to_rt:1;
   GLuint dd_present:1;
   GLuint ds_present:1;
} wm_iz_table[BRW_WM_IZ_BIT_MAX] =
{
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 0 },
 { N, 0, 1, 0, 0 },
 { N, 0, 1, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { C, 0, 1, 1, 0 },
 { C, 0, 1, 1, 0 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 0 },
 { C, 0, 1, 1, 0 },
 { C, 0, 1, 1, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 0 },
 { N, 0, 1, 0, 0 },
 { N, 0, 1, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { C, 0, 1, 1, 0 },
 { C, 0, 1, 1, 0 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 0 },
 { C, 0, 1, 1, 0 },
 { C, 0, 1, 1, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 1 },
 { N, 0, 1, 0, 1 },
 { N, 0, 1, 0, 1 },
 { P, 0, 0, 0, 0 },
 { P, 0, 0, 0, 0 },
 { C, 0, 1, 1, 1 },
 { C, 0, 1, 1, 1 },
 { P, 0, 0, 0, 0 },
 { N, 1, 1, 0, 1 },
 { C, 0, 1, 1, 1 },
 { C, 0, 1, 1, 1 },
 { P, 0, 0, 0, 0 },
 { C, 0, 0, 0, 1 },
 { P, 0, 0, 0, 0 },
 { C, 0, 1, 0, 1 },
 { P, 0, 0, 0, 0 },
 { C, 1, 1, 0, 1 },
 { C, 0, 1, 0, 1 },
 { C, 0, 1, 0, 1 },
 { P, 0, 0, 0, 0 },
 { C, 1, 1, 1, 1 },
 { C, 0, 1, 1, 1 },
 { C, 0, 1, 1, 1 },
 { P, 0, 0, 0, 0 },
 { C, 1, 1, 1, 1 },
 { C, 0, 1, 1, 1 },
 { C, 0, 1, 1, 1 }
};

/**
 * \param line_aa  BRW_WM_AA_NEVER, BRW_WM_AA_ALWAYS or BRW_WM_AA_SOMETIMES
 * \param lookup  bitmask of BRW_WM_IZ_* flags
 */
static inline void
setup_fs_payload_gfx4(fs_thread_payload &payload,
                      const fs_visitor &v,
                      bool &source_depth_to_render_target,
                      bool &runtime_check_aads_emit)
{
   assert(v.dispatch_width <= 16);

   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(v.prog_data);
   brw_wm_prog_key *key = (brw_wm_prog_key *) v.key;

   GLuint reg = 1;
   bool kill_stats_promoted_workaround = false;
   int lookup = key->iz_lookup;

   assert(lookup < BRW_WM_IZ_BIT_MAX);

   /* Crazy workaround in the windowizer, which we need to track in
    * our register allocation and render target writes.  See the "If
    * statistics are enabled..." paragraph of 11.5.3.2: Early Depth
    * Test Cases [Pre-DevGT] of the 3D Pipeline - Windower B-Spec.
    */
   if (key->stats_wm &&
       (lookup & BRW_WM_IZ_PS_KILL_ALPHATEST_BIT) &&
       wm_iz_table[lookup].mode == P) {
      kill_stats_promoted_workaround = true;
   }

   payload.subspan_coord_reg[0] = reg++;

   if (wm_iz_table[lookup].sd_present || prog_data->uses_src_depth ||
       kill_stats_promoted_workaround) {
      payload.source_depth_reg[0] = reg;
      reg += 2;
   }

   if (wm_iz_table[lookup].sd_to_rt || kill_stats_promoted_workaround)
      source_depth_to_render_target = true;

   if (wm_iz_table[lookup].ds_present || key->line_aa != BRW_WM_AA_NEVER) {
      payload.aa_dest_stencil_reg[0] = reg;
      runtime_check_aads_emit =
         !wm_iz_table[lookup].ds_present && key->line_aa == BRW_WM_AA_SOMETIMES;
      reg++;
   }

   if (wm_iz_table[lookup].dd_present) {
      payload.dest_depth_reg[0] = reg;
      reg+=2;
   }

   payload.num_regs = reg;
}

#undef P                        /* prompted depth */
#undef C                        /* computed */
#undef N                        /* non-promoted? */

fs_thread_payload::fs_thread_payload(const fs_visitor &v,
                                     bool &source_depth_to_render_target,
                                     bool &runtime_check_aads_emit)
  : subspan_coord_reg(),
    source_depth_reg(),
    source_w_reg(),
    aa_dest_stencil_reg(),
    dest_depth_reg(),
    sample_pos_reg(),
    sample_mask_in_reg(),
    depth_w_coef_reg(),
    barycentric_coord_reg(),
    local_invocation_id_reg()
{
   if (v.devinfo->ver >= 6)
      setup_fs_payload_gfx6(*this, v, source_depth_to_render_target);
   else
      setup_fs_payload_gfx4(*this, v, source_depth_to_render_target,
                            runtime_check_aads_emit);
}

/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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
#ifndef ACO_SHADER_INFO_H
#define ACO_SHADER_INFO_H

#include "shader_enums.h"
/* temporary */
#include "vulkan/radv_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

struct aco_shader_info {
   bool has_ngg_culling;
   bool has_ngg_early_prim_export;
   uint32_t num_tess_patches;
   unsigned workgroup_size;
   struct {
      struct radv_vs_output_info outinfo;
      bool tcs_in_out_eq;
      uint64_t tcs_temp_only_input_mask;
      bool use_per_attribute_vb_descs;
      uint32_t vb_desc_usage_mask;
      bool has_prolog;
      bool dynamic_inputs;
   } vs;
   struct {
      uint8_t output_usage_mask[VARYING_SLOT_VAR31 + 1];
      uint8_t num_stream_output_components[4];
      uint8_t output_streams[VARYING_SLOT_VAR31 + 1];
      unsigned vertices_out;
   } gs;
   struct {
      uint32_t num_lds_blocks;
   } tcs;
   struct {
      struct radv_vs_output_info outinfo;
   } tes;
   struct {
      bool writes_z;
      bool writes_stencil;
      bool writes_sample_mask;
      uint32_t num_interp;
      unsigned spi_ps_input;
   } ps;
   struct {
      uint8_t subgroup_size;
   } cs;
   struct {
      struct radv_vs_output_info outinfo;
   } ms;
   struct radv_streamout_info so;

   uint32_t gfx9_gs_ring_lds_size;
};

#ifdef __cplusplus
}
#endif
#endif

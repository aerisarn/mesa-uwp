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
#ifndef RADV_ACO_SHADER_INFO_H
#define RADV_ACO_SHADER_INFO_H

/* this will convert from radv shader info to the ACO one. */

#include "aco_shader_info.h"

#define ASSIGN_FIELD(x) aco_info->x = radv->x
#define ASSIGN_FIELD_CP(x) memcpy(&aco_info->x, &radv->x, sizeof(radv->x))

static inline void
radv_aco_convert_shader_so_info(struct aco_shader_info *aco_info,
                       const struct radv_shader_info *radv)
{
   ASSIGN_FIELD(so.num_outputs);
   ASSIGN_FIELD_CP(so.outputs);
   ASSIGN_FIELD_CP(so.strides);
   /* enabled_stream_buffers_mask unused */
}

static inline void
radv_aco_convert_shader_info(struct aco_shader_info *aco_info,
			     const struct radv_shader_info *radv)
{
   ASSIGN_FIELD(has_ngg_culling);
   ASSIGN_FIELD(has_ngg_early_prim_export);
   ASSIGN_FIELD(num_tess_patches);
   ASSIGN_FIELD(workgroup_size);
   ASSIGN_FIELD(vs.outinfo);
   ASSIGN_FIELD(vs.tcs_in_out_eq);
   ASSIGN_FIELD(vs.tcs_temp_only_input_mask);
   ASSIGN_FIELD(vs.use_per_attribute_vb_descs);
   ASSIGN_FIELD(vs.vb_desc_usage_mask);
   ASSIGN_FIELD(vs.has_prolog);
   ASSIGN_FIELD(vs.dynamic_inputs);
   ASSIGN_FIELD_CP(gs.output_usage_mask);
   ASSIGN_FIELD_CP(gs.num_stream_output_components);
   ASSIGN_FIELD_CP(gs.output_streams);
   ASSIGN_FIELD(gs.vertices_out);
   ASSIGN_FIELD(tcs.num_lds_blocks);
   ASSIGN_FIELD(tes.outinfo);
   ASSIGN_FIELD(ps.writes_z);
   ASSIGN_FIELD(ps.writes_stencil);
   ASSIGN_FIELD(ps.writes_sample_mask);
   ASSIGN_FIELD(ps.num_interp);
   ASSIGN_FIELD(ps.spi_ps_input);
   ASSIGN_FIELD(cs.subgroup_size);
   ASSIGN_FIELD(ms.outinfo);
   radv_aco_convert_shader_so_info(aco_info, radv);
   aco_info->gfx9_gs_ring_lds_size = radv->gs_ring_info.lds_size;
}
#undef ASSIGN_FIELD
#undef ASSIGN_FIELD_CP

#endif

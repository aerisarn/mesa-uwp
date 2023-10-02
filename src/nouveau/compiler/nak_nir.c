/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "nir_xfb_info.h"

#include "util/u_math.h"

#define OPT(nir, pass, ...) ({                           \
   bool this_progress = false;                           \
   NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);    \
   if (this_progress)                                    \
      progress = true;                                   \
   this_progress;                                        \
})

#define OPT_V(nir, pass, ...) NIR_PASS_V(nir, pass, ##__VA_ARGS__)

static void
optimize_nir(nir_shader *nir, const struct nak_compiler *nak, bool allow_copies)
{
   bool progress;

   unsigned lower_flrp =
      (nir->options->lower_flrp16 ? 16 : 0) |
      (nir->options->lower_flrp32 ? 32 : 0) |
      (nir->options->lower_flrp64 ? 64 : 0);

   do {
      progress = false;

      /* This pass is causing problems with types used by OpenCL :
       *    https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/13955
       *
       * Running with it disabled made no difference in the resulting assembly
       * code.
       */
      if (nir->info.stage != MESA_SHADER_KERNEL)
         OPT(nir, nir_split_array_vars, nir_var_function_temp);

      OPT(nir, nir_shrink_vec_array_vars, nir_var_function_temp);
      OPT(nir, nir_opt_deref);
      if (OPT(nir, nir_opt_memcpy))
         OPT(nir, nir_split_var_copies);

      OPT(nir, nir_lower_vars_to_ssa);

      if (allow_copies) {
         /* Only run this pass in the first call to brw_nir_optimize.  Later
          * calls assume that we've lowered away any copy_deref instructions
          * and we don't want to introduce any more.
          */
         OPT(nir, nir_opt_find_array_copies);
      }
      OPT(nir, nir_opt_copy_prop_vars);
      OPT(nir, nir_opt_dead_write_vars);
      OPT(nir, nir_opt_combine_stores, nir_var_all);

      OPT(nir, nir_lower_alu_to_scalar, NULL, NULL);
      OPT(nir, nir_lower_phis_to_scalar, false);
      OPT(nir, nir_lower_frexp);
      OPT(nir, nir_copy_prop);
      OPT(nir, nir_opt_dce);
      OPT(nir, nir_opt_cse);

      OPT(nir, nir_opt_peephole_select, 0, false, false);
      OPT(nir, nir_opt_intrinsics);
      OPT(nir, nir_opt_idiv_const, 32);
      OPT(nir, nir_opt_algebraic);
      OPT(nir, nir_lower_constant_convert_alu_types);
      OPT(nir, nir_opt_constant_folding);

      if (lower_flrp != 0) {
         if (OPT(nir, nir_lower_flrp, lower_flrp, false /* always_precise */))
            OPT(nir, nir_opt_constant_folding);
         /* Nothing should rematerialize any flrps */
         lower_flrp = 0;
      }

      OPT(nir, nir_opt_dead_cf);
      if (OPT(nir, nir_opt_trivial_continues)) {
         /* If nir_opt_trivial_continues makes progress, then we need to clean
          * things up if we want any hope of nir_opt_if or nir_opt_loop_unroll
          * to make progress.
          */
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
      }
      OPT(nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      OPT(nir, nir_opt_conditional_discard);
      if (nir->options->max_unroll_iterations != 0) {
         OPT(nir, nir_opt_loop_unroll);
      }
      OPT(nir, nir_opt_remove_phis);
      OPT(nir, nir_opt_gcm, false);
      OPT(nir, nir_opt_undef);
      OPT(nir, nir_lower_pack);
   } while (progress);

   OPT(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
}

void
nak_optimize_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   optimize_nir(nir, nak, false);
}

static unsigned
lower_bit_size_cb(const nir_instr *instr, void *_data)
{
   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      switch (alu->op) {
      case nir_op_bit_count:
      case nir_op_ufind_msb:
      case nir_op_ifind_msb:
      case nir_op_find_lsb:
         /* These are handled specially because the destination is always
          * 32-bit and so the bit size of the instruction is given by the
          * source.
          */
         return alu->src[0].src.ssa->bit_size == 32 ? 0 : 32;
      default:
         break;
      }

      if (alu->def.bit_size >= 32)
         return 0;

      /* TODO: Some hardware has native 16-bit support */
      if (alu->def.bit_size & (8 | 16))
         return 32;

      return 0;
   }

   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_vote_feq:
      case nir_intrinsic_vote_ieq:
      case nir_intrinsic_read_invocation:
      case nir_intrinsic_read_first_invocation:
      case nir_intrinsic_shuffle:
      case nir_intrinsic_shuffle_xor:
      case nir_intrinsic_shuffle_up:
      case nir_intrinsic_shuffle_down:
      case nir_intrinsic_quad_broadcast:
      case nir_intrinsic_quad_swap_horizontal:
      case nir_intrinsic_quad_swap_vertical:
      case nir_intrinsic_quad_swap_diagonal:
      case nir_intrinsic_reduce:
      case nir_intrinsic_inclusive_scan:
      case nir_intrinsic_exclusive_scan:
         if (intrin->src[0].ssa->bit_size != 32)
            return 32;
         return 0;

      default:
         return 0;
      }
   }

   case nir_instr_type_phi: {
      nir_phi_instr *phi = nir_instr_as_phi(instr);
      if (phi->def.bit_size != 32)
         return 32;
      return 0;
   }

   default:
      return 0;
   }
}

void
nak_preprocess_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   UNUSED bool progress = false;

   nir_validate_ssa_dominance(nir, "before nak_preprocess_nir");

   OPT(nir, nir_lower_bit_size, lower_bit_size_cb, (void *)nak);

   const nir_lower_tex_options tex_options = {
      .lower_txd_3d = true,
      .lower_txd_cube_map = true,
      .lower_txd_clamp = true,
      .lower_txd_shadow = true,
      .lower_txp = ~0,
      /* TODO: More lowering */
   };
   OPT(nir, nir_lower_tex, &tex_options);
   OPT(nir, nir_normalize_cubemap_coords);

   nir_lower_image_options image_options = {
      .lower_cube_size = true,
   };
   OPT(nir, nir_lower_image, &image_options);

   OPT(nir, nir_lower_global_vars_to_local);

   OPT(nir, nir_split_var_copies);
   OPT(nir, nir_split_struct_vars, nir_var_function_temp);

   /* Optimize but allow copies because we haven't lowered them yet */
   optimize_nir(nir, nak, true /* allow_copies */);

   OPT(nir, nir_lower_load_const_to_scalar);
   OPT(nir, nir_lower_var_copies);
   OPT(nir, nir_lower_system_values);
   OPT(nir, nir_lower_compute_system_values, NULL);
}

static uint16_t
nak_attribute_attr_addr(gl_vert_attrib attrib)
{
   assert(attrib >= VERT_ATTRIB_GENERIC0);
   return NAK_ATTR_GENERIC_START + (attrib - VERT_ATTRIB_GENERIC0) * 0x10;
}

static int
count_location_bytes(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false) * 16;
}

static bool
nak_nir_lower_vs_inputs(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_shader_in_variable(var, nir) {
      var->data.driver_location =
         nak_attribute_attr_addr(var->data.location);
   }

   progress |= OPT(nir, nir_lower_io, nir_var_shader_in, count_location_bytes,
                        nir_lower_io_lower_64bit_to_32);

   return progress;
}

static uint16_t
nak_varying_attr_addr(gl_varying_slot slot)
{
   if (slot >= VARYING_SLOT_PATCH0) {
      return NAK_ATTR_PATCH_START + (slot - VARYING_SLOT_PATCH0) * 0x10;
   } else if (slot >= VARYING_SLOT_VAR0) {
      return NAK_ATTR_GENERIC_START + (slot - VARYING_SLOT_VAR0) * 0x10;
   } else {
      switch (slot) {
      case VARYING_SLOT_TESS_LEVEL_OUTER: return NAK_ATTR_TESS_LOD;
      case VARYING_SLOT_TESS_LEVEL_INNER: return NAK_ATTR_TESS_INTERRIOR;
      case VARYING_SLOT_PRIMITIVE_ID:     return NAK_ATTR_PRIMITIVE_ID;
      case VARYING_SLOT_LAYER:            return NAK_ATTR_RT_ARRAY_INDEX;
      case VARYING_SLOT_VIEWPORT:         return NAK_ATTR_VIEWPORT_INDEX;
      case VARYING_SLOT_PSIZ:             return NAK_ATTR_POINT_SIZE;
      case VARYING_SLOT_POS:              return NAK_ATTR_POSITION;
      case VARYING_SLOT_CLIP_DIST0:       return NAK_ATTR_CLIP_CULL_DIST_0;
      case VARYING_SLOT_CLIP_DIST1:       return NAK_ATTR_CLIP_CULL_DIST_4;
      default: unreachable("Invalid varying slot");
      }
   }
}

static uint16_t
nak_sysval_attr_addr(gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_PRIMITIVE_ID:  return NAK_ATTR_PRIMITIVE_ID;
   case SYSTEM_VALUE_FRAG_COORD:    return NAK_ATTR_POSITION;
   case SYSTEM_VALUE_POINT_COORD:   return NAK_ATTR_POINT_SPRITE;
   case SYSTEM_VALUE_TESS_COORD:    return NAK_ATTR_TESS_COORD;
   case SYSTEM_VALUE_INSTANCE_ID:   return NAK_ATTR_INSTANCE_ID;
   case SYSTEM_VALUE_VERTEX_ID:     return NAK_ATTR_VERTEX_ID;
   case SYSTEM_VALUE_FRONT_FACE:    return NAK_ATTR_FRONT_FACE;
   default: unreachable("Invalid system value");
   }
}

static uint8_t
nak_sysval_sysval_idx(gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_SUBGROUP_INVOCATION:    return NAK_SV_LANE_ID;
   case SYSTEM_VALUE_VERTICES_IN:            return NAK_SV_VERTEX_COUNT;
   case SYSTEM_VALUE_INVOCATION_ID:          return NAK_SV_INVOCATION_ID;
   case SYSTEM_VALUE_HELPER_INVOCATION:      return NAK_SV_THREAD_KILL;
   case SYSTEM_VALUE_LOCAL_INVOCATION_INDEX: return NAK_SV_COMBINED_TID;
   case SYSTEM_VALUE_LOCAL_INVOCATION_ID:    return NAK_SV_TID;
   case SYSTEM_VALUE_WORKGROUP_ID:           return NAK_SV_CTAID;
   case SYSTEM_VALUE_SUBGROUP_EQ_MASK:       return NAK_SV_LANEMASK_EQ;
   case SYSTEM_VALUE_SUBGROUP_LT_MASK:       return NAK_SV_LANEMASK_LT;
   case SYSTEM_VALUE_SUBGROUP_LE_MASK:       return NAK_SV_LANEMASK_LE;
   case SYSTEM_VALUE_SUBGROUP_GT_MASK:       return NAK_SV_LANEMASK_GT;
   case SYSTEM_VALUE_SUBGROUP_GE_MASK:       return NAK_SV_LANEMASK_GE;
   default: unreachable("Invalid system value");
   }
}

static nir_def *
nak_nir_isberd(nir_builder *b, nir_def *vertex)
{
   nir_def *info = nir_load_sysval_nv(b, 32, .base = NAK_SV_INVOCATION_INFO,
                                      .access = ACCESS_CAN_REORDER);
   nir_def *lo = nir_extract_u8_imm(b, info, 0);
   nir_def *hi = nir_extract_u8_imm(b, info, 2);
   nir_def *idx = nir_iadd(b, nir_imul(b, lo, hi), vertex);

   return nir_isberd_nv(b, idx);
}

static bool
lower_per_vertex_io_intrin(nir_builder *b,
                           nir_intrinsic_instr *intrin,
                           void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_per_vertex_input)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_src *vertex = &intrin->src[0];
   nir_def *idx = nak_nir_isberd(b, vertex->ssa);
   nir_src_rewrite(vertex, idx);

   return true;
}

static bool
nak_nir_lower_varyings(nir_shader *nir, nir_variable_mode modes)
{
   bool progress = false;

   assert(!(modes & ~(nir_var_shader_in | nir_var_shader_out)));

   nir_foreach_variable_with_modes(var, nir, modes)
      var->data.driver_location = nak_varying_attr_addr(var->data.location);

   OPT(nir, nir_lower_io, modes, count_location_bytes, 0);

   switch (nir->info.stage) {
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
      OPT(nir, nir_shader_intrinsics_pass, lower_per_vertex_io_intrin,
          nir_metadata_block_index | nir_metadata_dominance, NULL);
   default:
      break;
   }

   return progress;
}

struct nvk_xfb_info
nak_xfb_from_nir(const struct nir_xfb_info *nir_xfb)
{
   if (nir_xfb == NULL)
      return (struct nvk_xfb_info) { };

   struct nvk_xfb_info nak_xfb = { };

   u_foreach_bit(b, nir_xfb->buffers_written) {
      nak_xfb.stride[b] = nir_xfb->buffers[b].stride;
      nak_xfb.stream[b] = nir_xfb->buffer_to_stream[b];
   }

   for (unsigned o = 0; o < nir_xfb->output_count; o++) {
      const nir_xfb_output_info *out = &nir_xfb->outputs[o];
      const uint8_t b = out->buffer;
      assert(nir_xfb->buffers_written & BITFIELD_BIT(b));

      const uint16_t attr_addr = nak_varying_attr_addr(out->location);
      assert(attr_addr % 4 == 0);
      const uint16_t attr_idx = attr_addr / 4;

      assert(out->offset % 4 == 0);
      uint8_t out_idx = out->offset / 4;

      u_foreach_bit(c, out->component_mask)
         nak_xfb.attr_index[b][out_idx++] = attr_idx + c;

      nak_xfb.attr_count[b] = MAX2(nak_xfb.attr_count[b], out_idx);
   }

   return nak_xfb;
}

static nir_def *
load_frag_w(nir_builder *b, nir_def *bary)
{
   const uint16_t w_addr =
      nak_sysval_attr_addr(SYSTEM_VALUE_FRAG_COORD) + 12;

   return nir_load_interpolated_input(b, 1, 32, bary,
                                      nir_imm_int(b, 0), .base = w_addr,
                                      .dest_type = nir_type_float32);
}

static bool
lower_fs_input_intrin(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   const struct nak_fs_key *fs_key = data;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_barycentric_pixel: {
      if (!(fs_key && fs_key->force_sample_shading))
         return false;

      intrin->intrinsic = nir_intrinsic_load_barycentric_sample;
      return true;
   }

   case nir_intrinsic_load_barycentric_at_offset: {
      b->cursor = nir_before_instr(&intrin->instr);

      nir_def *offset_f = intrin->src[0].ssa;
      offset_f = nir_fclamp(b, offset_f, nir_imm_float(b, -0.5),
                            nir_imm_float(b, 0.437500));
      nir_def *offset_fixed =
         nir_f2i32(b, nir_fmul_imm(b, offset_f, 4096.0));
      nir_def *offset_packed =
         nir_ior(b, nir_ishl_imm(b, nir_channel(b, offset_fixed, 1), 16),
                    nir_iand_imm(b, nir_channel(b, offset_fixed, 0), 0xffff));

      intrin->intrinsic = nir_intrinsic_load_barycentric_at_offset_nv;
      nir_src_rewrite(&intrin->src[0], offset_packed);

      return true;
   }

   case nir_intrinsic_load_frag_coord:
   case nir_intrinsic_load_point_coord:
   case nir_intrinsic_load_sample_pos: {
      b->cursor = nir_before_instr(&intrin->instr);

      nir_def *bary;
      if (b->shader->info.fs.uses_sample_shading) {
         bary = nir_load_barycentric_sample(b, 32,
            .interp_mode = INTERP_MODE_SMOOTH);
      } else {
         bary = nir_load_barycentric_pixel(b, 32,
            .interp_mode = INTERP_MODE_SMOOTH);
      }

      const uint32_t addr =
         intrin->intrinsic == nir_intrinsic_load_point_coord ?
         nak_sysval_attr_addr(SYSTEM_VALUE_POINT_COORD) :
         nak_sysval_attr_addr(SYSTEM_VALUE_FRAG_COORD);

      nir_def *coord =
         nir_load_interpolated_input(b, intrin->def.num_components, 32,
                                     bary, nir_imm_int(b, 0),
                                     .base = addr,
                                     .dest_type = nir_type_float32);

      nir_def *w = load_frag_w(b, bary);
      coord = nir_fdiv(b, coord, w);

      switch (intrin->intrinsic) {
      case nir_intrinsic_load_frag_coord:
         coord = nir_vector_insert_imm(b, coord, w, 3);
         break;
      case nir_intrinsic_load_point_coord:
         break;
      case nir_intrinsic_load_sample_pos:
         coord = nir_ffract(b, coord);
         break;
      default:
         unreachable("Unknown intrinsic");
      }

      nir_def_rewrite_uses(&intrin->def, coord);
      nir_instr_remove(&intrin->instr);

      return true;
   }

   case nir_intrinsic_load_interpolated_input: {
      nir_intrinsic_instr *bary = nir_src_as_intrinsic(intrin->src[0]);
      if (nir_intrinsic_interp_mode(bary) != INTERP_MODE_SMOOTH &&
          nir_intrinsic_interp_mode(bary) != INTERP_MODE_NONE)
         return false;

      b->cursor = nir_after_instr(&intrin->instr);

      /* Perspective-correct interpolated inputs need to be divided by .w */
      nir_def *res = nir_fdiv(b, &intrin->def, load_frag_w(b, &bary->def));
      nir_def_rewrite_uses_after(&intrin->def, res, res->parent_instr);

      return true;
   }

   case nir_intrinsic_load_sample_mask_in: {
      if (!b->shader->info.fs.uses_sample_shading &&
          !(fs_key && fs_key->force_sample_shading))
         return false;

      b->cursor = nir_after_instr(&intrin->instr);

      /* Mask off just the current sample */
      nir_def *sample = nir_load_sample_id(b);
      nir_def *mask = nir_ishl(b, nir_imm_int(b, 1), sample);
      mask = nir_iand(b, &intrin->def, mask);
      nir_def_rewrite_uses_after(&intrin->def, mask, mask->parent_instr);

      return true;
   }

   default:
      return false;
   }
}

static bool
nak_nir_lower_fs_inputs(nir_shader *nir,
                        const struct nak_fs_key *fs_key)
{
   NIR_PASS_V(nir, nak_nir_lower_varyings, nir_var_shader_in);
   NIR_PASS_V(nir, nir_shader_intrinsics_pass, lower_fs_input_intrin,
              nir_metadata_block_index | nir_metadata_dominance,
              (void *)fs_key);

   return true;
}

static int
fs_out_size(const struct glsl_type *type, bool bindless)
{
   assert(glsl_type_is_vector_or_scalar(type));
   return 16;
}

static bool
nak_nir_lower_fs_outputs(nir_shader *nir)
{
   if (nir->info.outputs_written == 0)
      return false;

   NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, true);

   nir->num_outputs = 0;
   nir_foreach_shader_out_variable(var, nir) {
      assert(nir->info.outputs_written & BITFIELD_BIT(var->data.location));
      switch (var->data.location) {
      case FRAG_RESULT_DEPTH:
         assert(var->data.index == 0);
         assert(var->data.location_frac == 0);
         var->data.driver_location = NAK_FS_OUT_DEPTH;
         break;
      case FRAG_RESULT_STENCIL:
         unreachable("EXT_shader_stencil_export not supported");
         break;
      case FRAG_RESULT_COLOR:
         assert(var->data.index == 0);
         var->data.driver_location = NAK_FS_OUT_COLOR0;
         break;
      case FRAG_RESULT_SAMPLE_MASK:
         assert(var->data.index == 0);
         assert(var->data.location_frac == 0);
         var->data.driver_location = NAK_FS_OUT_SAMPLE_MASK;
         break;
      default: {
         assert(var->data.location >= FRAG_RESULT_DATA0);
         assert(var->data.index < 2);
         const unsigned out =
            (var->data.location - FRAG_RESULT_DATA0) + var->data.index;
         var->data.driver_location = NAK_FS_OUT_COLOR(out);
         break;
      }
      }
   }

   NIR_PASS_V(nir, nir_lower_io, nir_var_shader_out, fs_out_size, 0);

   return true;
}

static bool
nak_nir_lower_system_value_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_def *val;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_layer_id: {
      const uint32_t addr = nak_varying_attr_addr(VARYING_SLOT_LAYER);
      val = nir_load_input(b, intrin->def.num_components, 32,
                           nir_imm_int(b, 0), .base = addr,
                           .dest_type = nir_type_int32);
      break;
   }

   case nir_intrinsic_load_primitive_id: {
      assert(b->shader->info.stage == MESA_SHADER_TESS_CTRL ||
             b->shader->info.stage == MESA_SHADER_TESS_EVAL);
      nir_def *idx = nak_nir_isberd(b, nir_imm_int(b, 0));
      val = nir_load_per_vertex_input(b, 1, 32, idx, nir_imm_int(b, 0),
                                      .base = NAK_ATTR_PRIMITIVE_ID,
                                      .dest_type = nir_type_int32);
      break;
   }

   case nir_intrinsic_load_front_face:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_vertex_id: {
      const gl_system_value sysval =
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t addr = nak_sysval_attr_addr(sysval);
      val = nir_load_input(b, intrin->def.num_components, 32,
                           nir_imm_int(b, 0), .base = addr,
                           .dest_type = nir_type_int32);
      break;
   }

   case nir_intrinsic_load_subgroup_invocation:
   case nir_intrinsic_load_patch_vertices_in:
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_load_invocation_id:
   case nir_intrinsic_load_local_invocation_index:
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_workgroup_id:
   case nir_intrinsic_load_workgroup_id_zero_base:
   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_ge_mask: {
      const gl_system_value sysval =
         intrin->intrinsic == nir_intrinsic_load_workgroup_id_zero_base ?
         SYSTEM_VALUE_WORKGROUP_ID :
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t idx = nak_sysval_sysval_idx(sysval);
      nir_def *comps[3];
      assert(intrin->def.num_components <= 3);
      for (unsigned c = 0; c < intrin->def.num_components; c++) {
         comps[c] = nir_load_sysval_nv(b, 32, .base = idx + c,
                                       .access = ACCESS_CAN_REORDER);
      }
      val = nir_vec(b, comps, intrin->def.num_components);
      break;
   }

   case nir_intrinsic_is_helper_invocation: {
      /* Unlike load_helper_invocation, this one isn't re-orderable */
      val = nir_load_sysval_nv(b, 32, .base = NAK_SV_THREAD_KILL);
      break;
   }

   case nir_intrinsic_shader_clock:
      val = nir_load_sysval_nv(b, 64, .base = NAK_SV_CLOCK);
      val = nir_unpack_64_2x32(b, val);
      break;

   default:
      return false;
   }

   if (intrin->def.bit_size == 1)
      val = nir_i2b(b, val);

   nir_def_rewrite_uses(&intrin->def, val);

   return true;
}

static bool
nak_nir_lower_system_values(nir_shader *nir)
{
   return nir_shader_instructions_pass(nir, nak_nir_lower_system_value_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       NULL);
}

static nir_mem_access_size_align
nak_mem_access_size_align(nir_intrinsic_op intrin,
                          uint8_t bytes, uint8_t bit_size,
                          uint32_t align_mul, uint32_t align_offset,
                          bool offset_is_const, const void *cb_data)
{
   assert(align_offset < align_mul);
   const uint32_t align =
      align_offset ? 1 << (ffs(align_offset) - 1) : align_mul;
   assert(util_is_power_of_two_nonzero(align));

   unsigned bytes_pow2;
   if (nir_intrinsic_infos[intrin].has_dest) {
      /* Reads can over-fetch a bit if the alignment is okay. */
      bytes_pow2 = util_next_power_of_two(bytes);
   } else {
      bytes_pow2 = 1 << (util_last_bit(bytes) - 1);
   }

   unsigned chunk_bytes = MIN3(bytes_pow2, align, 16);
   assert(util_is_power_of_two_nonzero(chunk_bytes));
   if (intrin == nir_intrinsic_load_ubo)
      chunk_bytes = MIN2(chunk_bytes, 8);

   if (chunk_bytes < 4) {
      return (nir_mem_access_size_align) {
         .bit_size = chunk_bytes * 8,
         .num_components = 1,
         .align = align,
      };
   } else {
      return (nir_mem_access_size_align) {
         .bit_size = 32,
         .num_components = chunk_bytes / 4,
         .align = align,
      };
   }
}

static bool
nir_shader_has_local_variables(const nir_shader *nir)
{
   nir_foreach_function(func, nir) {
      if (func->impl && !exec_list_is_empty(&func->impl->locals))
         return true;
   }

   return false;
}

void
nak_postprocess_nir(nir_shader *nir,
                    const struct nak_compiler *nak,
                    const struct nak_fs_key *fs_key)
{
   UNUSED bool progress = false;

   nak_optimize_nir(nir, nak);

   if (nir_shader_has_local_variables(nir)) {
      OPT(nir, nir_lower_vars_to_explicit_types, nir_var_function_temp,
          glsl_get_natural_size_align_bytes);
      OPT(nir, nir_lower_explicit_io, nir_var_function_temp,
          nir_address_format_32bit_offset);
   }

   nir_lower_mem_access_bit_sizes_options mem_bit_size_options = {
      .modes = nir_var_mem_ubo | nir_var_mem_generic,
      .callback = nak_mem_access_size_align,
   };
   OPT(nir, nir_lower_mem_access_bit_sizes, &mem_bit_size_options);

   nak_optimize_nir(nir, nak);

   OPT(nir, nak_nir_lower_tex, nak);
   OPT(nir, nir_lower_idiv, NULL);
   OPT(nir, nir_lower_int64);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   OPT(nir, nir_lower_indirect_derefs, 0, UINT32_MAX);

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      OPT(nir, nak_nir_lower_vs_inputs);
      OPT(nir, nak_nir_lower_varyings, nir_var_shader_out);
      break;

   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
      OPT(nir, nak_nir_lower_varyings, nir_var_shader_in | nir_var_shader_out);
      break;

   case MESA_SHADER_FRAGMENT:
      OPT(nir, nak_nir_lower_fs_inputs, fs_key);
      OPT(nir, nak_nir_lower_fs_outputs);
      break;

   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      break;

   default:
      unreachable("Unsupported shader stage");
   }

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      OPT(nir, nir_lower_tess_coord_z,
          nir->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES);
   }

   OPT(nir, nak_nir_lower_system_values);

   nak_optimize_nir(nir, nak);

   do {
      progress = false;
      if (OPT(nir, nir_opt_algebraic_late)) {
         OPT(nir, nir_opt_constant_folding);
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
         OPT(nir, nir_opt_cse);
      }
   } while (progress);

   nir_divergence_analysis(nir);

   /* Re-index blocks and compact SSA defs because we'll use them to index
    * arrays
    */
   nir_foreach_function(func, nir) {
      if (func->impl) {
         nir_index_blocks(func->impl);
         nir_index_ssa_defs(func->impl);
      }
   }

   if (nak_should_print_nir())
      nir_print_shader(nir, stderr);
}

static bool
scalar_is_imm_int(nir_scalar x, unsigned bits)
{
   if (!nir_scalar_is_const(x))
      return false;

   int64_t imm = nir_scalar_as_int(x);
   return u_intN_min(bits) <= imm && imm <= u_intN_max(bits);
}

struct nak_io_addr_offset
nak_get_io_addr_offset(nir_def *addr, uint8_t imm_bits)
{
   nir_scalar addr_s = {
      .def = addr,
      .comp = 0,
   };
   if (scalar_is_imm_int(addr_s, imm_bits)) {
      /* Base is a dumb name for this.  It should be offset */
      return (struct nak_io_addr_offset) {
         .offset = nir_scalar_as_int(addr_s),
      };
   }

   addr_s = nir_scalar_chase_movs(addr_s);
   if (!nir_scalar_is_alu(addr_s) ||
       nir_scalar_alu_op(addr_s) != nir_op_iadd) {
      return (struct nak_io_addr_offset) {
         .base = addr_s,
      };
   }

   for (unsigned i = 0; i < 2; i++) {
      nir_scalar off_s = nir_scalar_chase_alu_src(addr_s, i);
      off_s = nir_scalar_chase_movs(off_s);
      if (scalar_is_imm_int(off_s, imm_bits)) {
         return (struct nak_io_addr_offset) {
            .base = nir_scalar_chase_alu_src(addr_s, 1 - i),
            .offset = nir_scalar_as_int(off_s),
         };
      }
   }

   return (struct nak_io_addr_offset) {
      .base = addr_s,
   };
}

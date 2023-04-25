/*
 * Copyright © 2023 Valve Corporation
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
 *
 */

#include "ac_nir.h"
#include "nir_builder.h"

/**
 * Build a manual selection sequence for cube face sc/tc coordinates and
 * major axis vector (multiplied by 2 for consistency) for the given
 * vec3 \p coords, for the face implied by \p selcoords.
 *
 * For the major axis, we always adjust the sign to be in the direction of
 * selcoords.ma; i.e., a positive out_ma means that coords is pointed towards
 * the selcoords major axis.
 */
static void
build_cube_select(nir_builder *b, nir_ssa_def *ma, nir_ssa_def *id, nir_ssa_def *deriv,
                  nir_ssa_def **out_ma, nir_ssa_def **out_sc, nir_ssa_def **out_tc)
{
   nir_ssa_def *deriv_x = nir_channel(b, deriv, 0);
   nir_ssa_def *deriv_y = nir_channel(b, deriv, 1);
   nir_ssa_def *deriv_z = nir_channel(b, deriv, 2);

   nir_ssa_def *is_ma_positive = nir_fge(b, ma, nir_imm_float(b, 0.0));
   nir_ssa_def *sgn_ma =
      nir_bcsel(b, is_ma_positive, nir_imm_float(b, 1.0), nir_imm_float(b, -1.0));
   nir_ssa_def *neg_sgn_ma = nir_fneg(b, sgn_ma);

   nir_ssa_def *is_ma_z = nir_fge(b, id, nir_imm_float(b, 4.0));
   nir_ssa_def *is_ma_y = nir_fge(b, id, nir_imm_float(b, 2.0));
   is_ma_y = nir_iand(b, is_ma_y, nir_inot(b, is_ma_z));
   nir_ssa_def *is_not_ma_x = nir_ior(b, is_ma_z, is_ma_y);

   /* Select sc */
   nir_ssa_def *tmp = nir_bcsel(b, is_not_ma_x, deriv_x, deriv_z);
   nir_ssa_def *sgn =
      nir_bcsel(b, is_ma_y, nir_imm_float(b, 1.0), nir_bcsel(b, is_ma_z, sgn_ma, neg_sgn_ma));
   *out_sc = nir_fmul(b, tmp, sgn);

   /* Select tc */
   tmp = nir_bcsel(b, is_ma_y, deriv_z, deriv_y);
   sgn = nir_bcsel(b, is_ma_y, sgn_ma, nir_imm_float(b, -1.0));
   *out_tc = nir_fmul(b, tmp, sgn);

   /* Select ma */
   tmp = nir_bcsel(b, is_ma_z, deriv_z, nir_bcsel(b, is_ma_y, deriv_y, deriv_x));
   *out_ma = nir_fmul_imm(b, nir_fabs(b, tmp), 2.0);
}

static void
prepare_cube_coords(nir_builder *b, nir_tex_instr *tex, nir_ssa_def **coord, nir_src *ddx,
                    nir_src *ddy, const ac_nir_lower_tex_options *options)
{
   nir_ssa_def *coords[NIR_MAX_VEC_COMPONENTS] = {0};
   for (unsigned i = 0; i < (*coord)->num_components; i++)
      coords[i] = nir_channel(b, *coord, i);

   /* Section 8.9 (Texture Functions) of the GLSL 4.50 spec says:
    *
    *    "For Array forms, the array layer used will be
    *
    *       max(0, min(d−1, floor(layer+0.5)))
    *
    *     where d is the depth of the texture array and layer
    *     comes from the component indicated in the tables below.
    *     Workaroudn for an issue where the layer is taken from a
    *     helper invocation which happens to fall on a different
    *     layer due to extrapolation."
    *
    * GFX8 and earlier attempt to implement this in hardware by
    * clamping the value of coords[2] = (8 * layer) + face.
    * Unfortunately, this means that the we end up with the wrong
    * face when clamping occurs.
    *
    * Clamp the layer earlier to work around the issue.
    */
   if (tex->is_array && options->gfx_level <= GFX8 && coords[3])
      coords[3] = nir_fmax(b, coords[3], nir_imm_float(b, 0.0));

   nir_ssa_def *cube_coords = nir_cube_face_coord_amd(b, nir_vec(b, coords, 3));
   nir_ssa_def *sc = nir_channel(b, cube_coords, 0);
   nir_ssa_def *tc = nir_channel(b, cube_coords, 1);
   nir_ssa_def *ma = nir_channel(b, cube_coords, 2);
   nir_ssa_def *invma = nir_frcp(b, nir_fabs(b, ma));
   nir_ssa_def *id = nir_cube_face_index_amd(b, nir_vec(b, coords, 3));

   if (ddx || ddy) {
      sc = nir_fmul(b, sc, invma);
      tc = nir_fmul(b, tc, invma);

      /* Convert cube derivatives to 2D derivatives. */
      for (unsigned i = 0; i < 2; i++) {
         /* Transform the derivative alongside the texture
          * coordinate. Mathematically, the correct formula is
          * as follows. Assume we're projecting onto the +Z face
          * and denote by dx/dh the derivative of the (original)
          * X texture coordinate with respect to horizontal
          * window coordinates. The projection onto the +Z face
          * plane is:
          *
          *   f(x,z) = x/z
          *
          * Then df/dh = df/dx * dx/dh + df/dz * dz/dh
          *            = 1/z * dx/dh - x/z * 1/z * dz/dh.
          *
          * This motivatives the implementation below.
          *
          * Whether this actually gives the expected results for
          * apps that might feed in derivatives obtained via
          * finite differences is anyone's guess. The OpenGL spec
          * seems awfully quiet about how textureGrad for cube
          * maps should be handled.
          */
         nir_ssa_def *deriv_ma, *deriv_sc, *deriv_tc;
         build_cube_select(b, ma, id, i ? ddy->ssa : ddx->ssa, &deriv_ma, &deriv_sc, &deriv_tc);

         deriv_ma = nir_fmul(b, deriv_ma, invma);

         nir_ssa_def *x = nir_fsub(b, nir_fmul(b, deriv_sc, invma), nir_fmul(b, deriv_ma, sc));
         nir_ssa_def *y = nir_fsub(b, nir_fmul(b, deriv_tc, invma), nir_fmul(b, deriv_ma, tc));

         nir_instr_rewrite_src_ssa(&tex->instr, i ? ddy : ddx, nir_vec2(b, x, y));
      }

      sc = nir_fadd_imm(b, sc, 1.5);
      tc = nir_fadd_imm(b, tc, 1.5);
   } else {
      sc = nir_ffma_imm2(b, sc, invma, 1.5);
      tc = nir_ffma_imm2(b, tc, invma, 1.5);
   }

   if (tex->is_array && coords[3])
      id = nir_ffma_imm1(b, coords[3], 8.0, id);

   *coord = nir_vec3(b, sc, tc, id);

   tex->is_array = true;
}

static bool
lower_array_layer_round_even(nir_builder *b, nir_tex_instr *tex, nir_ssa_def **coords)
{
   int coord_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (coord_index < 0 || nir_tex_instr_src_type(tex, coord_index) != nir_type_float)
      return false;

   unsigned layer = tex->coord_components - 1;
   nir_ssa_def *rounded_layer = nir_fround_even(b, nir_channel(b, *coords, layer));
   *coords = nir_vector_insert_imm(b, *coords, rounded_layer, layer);
   return true;
}

static bool
lower_tex_coords(nir_builder *b, nir_tex_instr *tex, nir_ssa_def **coords,
                 const ac_nir_lower_tex_options *options)
{
   bool progress = false;
   if (options->lower_array_layer_round_even && tex->is_array && tex->op != nir_texop_lod)
      progress |= lower_array_layer_round_even(b, tex, coords);

   if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE &&
       !(tex->sampler_dim == GLSL_SAMPLER_DIM_1D && options->gfx_level == GFX9))
      return progress;

   int ddx_idx = nir_tex_instr_src_index(tex, nir_tex_src_ddx);
   int ddy_idx = nir_tex_instr_src_index(tex, nir_tex_src_ddy);
   nir_src *ddx = ddx_idx >= 0 ? &tex->src[ddx_idx].src : NULL;
   nir_src *ddy = ddy_idx >= 0 ? &tex->src[ddy_idx].src : NULL;

   if (tex->sampler_dim == GLSL_SAMPLER_DIM_1D) {
      nir_ssa_def *y =
         nir_imm_floatN_t(b, tex->op == nir_texop_txf ? 0.0 : 0.5, (*coords)->bit_size);
      if (tex->is_array && (*coords)->num_components > 1) {
         nir_ssa_def *x = nir_channel(b, *coords, 0);
         nir_ssa_def *idx = nir_channel(b, *coords, 1);
         *coords = nir_vec3(b, x, y, idx);
      } else {
         *coords = nir_vec2(b, *coords, y);
      }

      int offset_src = nir_tex_instr_src_index(tex, nir_tex_src_offset);
      if (offset_src >= 0) {
         nir_src *offset = &tex->src[offset_src].src;
         nir_ssa_def *zero = nir_imm_intN_t(b, 0, offset->ssa->bit_size);
         nir_instr_rewrite_src_ssa(&tex->instr, offset, nir_vec2(b, offset->ssa, zero));
      }

      if (ddx || ddy) {
         nir_ssa_def *def = nir_vec2(b, ddx->ssa, nir_imm_floatN_t(b, 0.0, ddx->ssa->bit_size));
         nir_instr_rewrite_src_ssa(&tex->instr, ddx, def);
         def = nir_vec2(b, ddy->ssa, nir_imm_floatN_t(b, 0.0, ddy->ssa->bit_size));
         nir_instr_rewrite_src_ssa(&tex->instr, ddy, def);
      }
   } else if (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE) {
      prepare_cube_coords(b, tex, coords, ddx, ddy, options);
   }

   return true;
}

static bool
lower_tex(nir_builder *b, nir_instr *instr, void *options_)
{
   const ac_nir_lower_tex_options *options = options_;
   if (instr->type != nir_instr_type_tex)
      return false;

   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int coord_idx = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (coord_idx < 0 || nir_tex_instr_src_index(tex, nir_tex_src_backend1) >= 0)
      return false;

   b->cursor = nir_before_instr(instr);
   nir_ssa_def *coords = tex->src[coord_idx].src.ssa;
   if (lower_tex_coords(b, tex, &coords, options)) {
      tex->coord_components = coords->num_components;
      nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[coord_idx].src, coords);
      return true;
   }

   return false;
}

bool
ac_nir_lower_tex(nir_shader *nir, const ac_nir_lower_tex_options *options)
{
   return nir_shader_instructions_pass(
      nir, lower_tex, nir_metadata_block_index | nir_metadata_dominance, (void *)options);
}

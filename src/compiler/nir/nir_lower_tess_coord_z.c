/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_intrinsics.h"
#include "shader_enums.h"

static bool
lower_tess_coord_z(nir_builder *b, nir_instr *instr, void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_tess_coord)
      return false;

   b->cursor = nir_instr_remove(instr);
   nir_ssa_def *xy = nir_load_tess_coord_xy(b);
   nir_ssa_def *x = nir_channel(b, xy, 0);
   nir_ssa_def *y = nir_channel(b, xy, 1);
   nir_ssa_def *z;

   bool *triangles = state;
   if (*triangles)
      z = nir_fsub(b, nir_fsub_imm(b, 1.0f, y), x);
   else
      z = nir_imm_float(b, 0.0f);

   nir_ssa_def_rewrite_uses(&intr->dest.ssa, nir_vec3(b, x, y, z));
   return true;
}

bool
nir_lower_tess_coord_z(nir_shader *shader, bool triangles)
{
   return nir_shader_instructions_pass(shader, lower_tess_coord_z,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance, &triangles);
}

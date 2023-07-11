/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "etnaviv_nir.h"

bool
etna_nir_lower_texture(nir_shader *s, struct etna_shader_key *key)
{
   bool progress = false;

   nir_lower_tex_options lower_tex_options = {
      .lower_txp = ~0u,
      .lower_invalid_implicit_lod = true,
   };

   NIR_PASS(progress, s, nir_lower_tex, &lower_tex_options);

   if (key->has_sample_tex_compare)
      NIR_PASS(progress, s, nir_lower_tex_shadow, key->num_texture_states,
                                                  key->tex_compare_func,
                                                  key->tex_swizzle);

   return progress;
}

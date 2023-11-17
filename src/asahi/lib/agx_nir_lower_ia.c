/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "asahi/compiler/agx_compile.h"
#include "compiler/nir/nir_builder.h"
#include "shaders/geometry.h"
#include "agx_nir_lower_gs.h"
#include "libagx_shaders.h"
#include "nir.h"
#include "nir_builder_opcodes.h"

static nir_def *
load_vertex_id(nir_builder *b, struct agx_ia_key *key)
{
   /* Tessellate by primitive mode */
   nir_def *id = libagx_vertex_id_for_topology(
      b, nir_imm_int(b, key->mode), nir_imm_bool(b, key->flatshade_first),
      nir_load_primitive_id(b), nir_load_vertex_id_in_primitive_agx(b),
      nir_load_num_vertices(b));

   /* If drawing with an index buffer, pull the vertex ID. Otherwise, the
    * vertex ID is just the index as-is.
    */
   if (key->index_size) {
      nir_def *address =
         libagx_index_buffer(b, nir_load_input_assembly_buffer_agx(b), id,
                             nir_imm_int(b, key->index_size));

      nir_def *index = nir_load_global_constant(b, address, key->index_size, 1,
                                                key->index_size * 8);

      id = nir_u2uN(b, index, id->bit_size);
   }

   /* Add the "start", either an index bias or a base vertex. This must happen
    * after indexing for proper index bias behaviour.
    */
   return nir_iadd(b, id, nir_load_first_vertex(b));
}

static bool
lower_vertex_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);
   assert(intr->def.bit_size == 32);
   nir_def_rewrite_uses(&intr->def, load_vertex_id(b, data));
   return true;
}

void
agx_nir_lower_ia(nir_shader *s, struct agx_ia_key *ia)
{
   nir_shader_intrinsics_pass(s, lower_vertex_id,
                              nir_metadata_block_index | nir_metadata_dominance,
                              ia);
}

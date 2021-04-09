/*
 * Copyright © 2021 Valve Corporation
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
#include "u_math.h"

typedef struct
{
   nir_variable *position_value_var;
   nir_variable *prim_exp_arg_var;

   bool passthrough;
   bool export_prim_id;
   bool early_prim_export;
   unsigned max_num_waves;
   unsigned num_vertices_per_primitives;
   unsigned provoking_vtx_idx;
   unsigned max_es_num_vertices;
   unsigned total_lds_bytes;
} lower_ngg_nogs_state;

static nir_ssa_def *
pervertex_lds_addr(nir_builder *b, nir_ssa_def *vertex_idx, unsigned per_vtx_bytes)
{
   return nir_imul_imm(b, vertex_idx, per_vtx_bytes);
}

static nir_ssa_def *
emit_pack_ngg_prim_exp_arg(nir_builder *b, unsigned num_vertices_per_primitives,
                           nir_ssa_def *vertex_indices[3], nir_ssa_def *is_null_prim)
{
   nir_ssa_def *arg = vertex_indices[0];

   for (unsigned i = 0; i < num_vertices_per_primitives; ++i) {
      assert(vertex_indices[i]);

      if (i)
         arg = nir_ior(b, arg, nir_ishl(b, vertex_indices[i], nir_imm_int(b, 10u * i)));

      if (b->shader->info.stage == MESA_SHADER_VERTEX) {
         nir_ssa_def *edgeflag = nir_build_load_initial_edgeflag_amd(b, 32, nir_imm_int(b, i));
         arg = nir_ior(b, arg, nir_ishl(b, edgeflag, nir_imm_int(b, 10u * i + 9u)));
      }
   }

   if (is_null_prim) {
      if (is_null_prim->bit_size == 1)
         is_null_prim = nir_b2i32(b, is_null_prim);
      assert(is_null_prim->bit_size == 32);
      arg = nir_ior(b, arg, nir_ishl(b, is_null_prim, nir_imm_int(b, 31u)));
   }

   return arg;
}

static nir_ssa_def *
ngg_input_primitive_vertex_index(nir_builder *b, unsigned vertex)
{
   /* TODO: This is RADV specific. We'll need to refactor RADV and/or RadeonSI to match. */
   return nir_ubfe(b, nir_build_load_gs_vertex_offset_amd(b, .base = vertex / 2u * 2u),
                      nir_imm_int(b, (vertex % 2u) * 16u), nir_imm_int(b, 16u));
}

static nir_ssa_def *
emit_ngg_nogs_prim_exp_arg(nir_builder *b, lower_ngg_nogs_state *st)
{
   if (st->passthrough) {
      assert(!st->export_prim_id || b->shader->info.stage != MESA_SHADER_VERTEX);
      return nir_build_load_packed_passthrough_primitive_amd(b);
   } else {
      nir_ssa_def *vtx_idx[3] = {0};

      vtx_idx[0] = ngg_input_primitive_vertex_index(b, 0);
      vtx_idx[1] = st->num_vertices_per_primitives >= 2
               ? ngg_input_primitive_vertex_index(b, 1)
               : nir_imm_zero(b, 1, 32);
      vtx_idx[2] = st->num_vertices_per_primitives >= 3
               ? ngg_input_primitive_vertex_index(b, 2)
               : nir_imm_zero(b, 1, 32);

      return emit_pack_ngg_prim_exp_arg(b, st->num_vertices_per_primitives, vtx_idx, NULL);
   }
}

static void
emit_ngg_nogs_prim_export(nir_builder *b, lower_ngg_nogs_state *st, nir_ssa_def *arg)
{
   nir_if *if_gs_thread = nir_push_if(b, nir_build_has_input_primitive_amd(b));
   {
      if (!arg)
         arg = emit_ngg_nogs_prim_exp_arg(b, st);

      if (st->export_prim_id && b->shader->info.stage == MESA_SHADER_VERTEX) {
         /* Copy Primitive IDs from GS threads to the LDS address corresponding to the ES thread of the provoking vertex. */
         nir_ssa_def *prim_id = nir_build_load_primitive_id(b);
         nir_ssa_def *provoking_vtx_idx = ngg_input_primitive_vertex_index(b, st->provoking_vtx_idx);
         nir_ssa_def *addr = pervertex_lds_addr(b, provoking_vtx_idx, 4u);

         nir_build_store_shared(b,  prim_id, addr, .write_mask = 1u, .align_mul = 4u);
      }

      nir_build_export_primitive_amd(b, arg);
   }
   nir_pop_if(b, if_gs_thread);
}

static void
emit_store_ngg_nogs_es_primitive_id(nir_builder *b)
{
   nir_ssa_def *prim_id = NULL;

   if (b->shader->info.stage == MESA_SHADER_VERTEX) {
      /* Workgroup barrier - wait for GS threads to store primitive ID in LDS. */
      nir_scoped_barrier(b, .execution_scope = NIR_SCOPE_WORKGROUP, .memory_scope = NIR_SCOPE_WORKGROUP,
                            .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);

      /* LDS address where the primitive ID is stored */
      nir_ssa_def *thread_id_in_threadgroup = nir_build_load_local_invocation_index(b);
      nir_ssa_def *addr =  pervertex_lds_addr(b, thread_id_in_threadgroup, 4u);

      /* Load primitive ID from LDS */
      prim_id = nir_build_load_shared(b, 1, 32, addr, .align_mul = 4u);
   } else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL) {
      /* Just use tess eval primitive ID, which is the same as the patch ID. */
      prim_id = nir_build_load_primitive_id(b);
   }

   nir_io_semantics io_sem = {
      .location = VARYING_SLOT_PRIMITIVE_ID,
      .num_slots = 1,
   };

   nir_build_store_output(b, prim_id, nir_imm_zero(b, 1, 32),
                          .base = io_sem.location,
                          .write_mask = 1u, .src_type = nir_type_uint32, .io_semantics = io_sem);
}

ac_nir_ngg_config
ac_nir_lower_ngg_nogs(nir_shader *shader,
                      unsigned max_num_es_vertices,
                      unsigned num_vertices_per_primitives,
                      unsigned max_workgroup_size,
                      unsigned wave_size,
                      bool consider_culling,
                      bool consider_passthrough,
                      bool export_prim_id,
                      bool provoking_vtx_last)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl);
   assert(max_num_es_vertices && max_workgroup_size && wave_size);

   bool can_cull = false; /* TODO */
   bool passthrough = consider_passthrough && !can_cull &&
                      !(shader->info.stage == MESA_SHADER_VERTEX && export_prim_id);

   nir_variable *position_value_var = nir_local_variable_create(impl, glsl_vec4_type(), "position_value");
   nir_variable *prim_exp_arg_var = nir_local_variable_create(impl, glsl_uint_type(), "prim_exp_arg");

   lower_ngg_nogs_state state = {
      .passthrough = passthrough,
      .export_prim_id = export_prim_id,
      .early_prim_export = exec_list_is_singular(&impl->body),
      .num_vertices_per_primitives = num_vertices_per_primitives,
      .provoking_vtx_idx = provoking_vtx_last ? (num_vertices_per_primitives - 1) : 0,
      .position_value_var = position_value_var,
      .prim_exp_arg_var = prim_exp_arg_var,
      .max_num_waves = DIV_ROUND_UP(max_workgroup_size, wave_size),
      .max_es_num_vertices = max_num_es_vertices,
   };

   /* We need LDS space when VS needs to export the primitive ID. */
   if (shader->info.stage == MESA_SHADER_VERTEX && export_prim_id)
      state.total_lds_bytes = max_num_es_vertices * 4u;

   nir_cf_list extracted;
   nir_cf_extract(&extracted, nir_before_cf_list(&impl->body), nir_after_cf_list(&impl->body));

   nir_builder builder;
   nir_builder *b = &builder; /* This is to avoid the & */
   nir_builder_init(b, impl);
   b->cursor = nir_before_cf_list(&impl->body);

   if (!can_cull) {
      /* Allocate export space on wave 0 - confirm to the HW that we want to use all possible space */
      nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_int(b, 0)));
      {
         nir_ssa_def *vtx_cnt = nir_build_load_workgroup_num_input_vertices_amd(b);
         nir_ssa_def *prim_cnt = nir_build_load_workgroup_num_input_primitives_amd(b);
         nir_build_alloc_vertices_and_primitives_amd(b, vtx_cnt, prim_cnt);
      }
      nir_pop_if(b, if_wave_0);

      /* Take care of early primitive export, otherwise just pack the primitive export argument */
      if (state.early_prim_export)
         emit_ngg_nogs_prim_export(b, &state, NULL);
      else
         nir_store_var(b, prim_exp_arg_var, emit_ngg_nogs_prim_exp_arg(b, &state), 0x1u);
   } else {
      abort(); /* TODO */
   }

   nir_if *if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
   {
      if (can_cull) {
         nir_ssa_def *pos_val = nir_load_var(b, state.position_value_var);
         nir_io_semantics io_sem = { .location = VARYING_SLOT_POS, .num_slots = 1 };
         nir_build_store_output(b, pos_val, nir_imm_int(b, 0), .base = VARYING_SLOT_POS, .component = 0, .io_semantics = io_sem, .write_mask = 0xfu);
      }

      /* Run the actual shader */
      nir_cf_reinsert(&extracted, b->cursor);
      b->cursor = nir_after_cf_list(&if_es_thread->then_list);

      /* Export all vertex attributes (except primitive ID) */
      nir_build_export_vertex_amd(b);

      /* Export primitive ID (in case of early primitive export or TES) */
      if (state.export_prim_id && (state.early_prim_export || shader->info.stage != MESA_SHADER_VERTEX))
         emit_store_ngg_nogs_es_primitive_id(b);
   }
   nir_pop_if(b, if_es_thread);

   /* Take care of late primitive export */
   if (!state.early_prim_export) {
      emit_ngg_nogs_prim_export(b, &state, nir_load_var(b, prim_exp_arg_var));
      if (state.export_prim_id && shader->info.stage == MESA_SHADER_VERTEX) {
         if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
         emit_store_ngg_nogs_es_primitive_id(b);
         nir_pop_if(b, if_es_thread);
      }
   }

   nir_metadata_preserve(impl, nir_metadata_none);
   nir_validate_shader(shader, "after emitting NGG VS/TES");

   /* Cleanup */
   nir_lower_vars_to_ssa(shader);
   nir_remove_dead_variables(shader, nir_var_function_temp, NULL);
   nir_opt_undef(shader);

   shader->info.shared_size = state.total_lds_bytes;

   ac_nir_ngg_config ret = {
      .can_cull = can_cull,
      .passthrough = passthrough,
   };

   return ret;
}

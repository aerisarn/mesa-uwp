/*
 * Copyright © 2021 Intel Corporation
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

#include "brw_compiler.h"
#include "brw_fs.h"
#include "brw_nir.h"
#include "brw_private.h"
#include "compiler/nir/nir_builder.h"
#include "dev/intel_debug.h"

using namespace brw;

const unsigned *
brw_compile_task(const struct brw_compiler *compiler,
                 void *mem_ctx,
                 struct brw_compile_task_params *params)
{
   struct nir_shader *nir = params->nir;
   const struct brw_task_prog_key *key = params->key;
   struct brw_task_prog_data *prog_data = params->prog_data;
   const bool debug_enabled = INTEL_DEBUG(DEBUG_TASK);

   prog_data->base.base.stage = MESA_SHADER_TASK;
   prog_data->base.base.total_shared = nir->info.shared_size;

   prog_data->base.local_size[0] = nir->info.workgroup_size[0];
   prog_data->base.local_size[1] = nir->info.workgroup_size[1];
   prog_data->base.local_size[2] = nir->info.workgroup_size[2];

   const unsigned required_dispatch_width =
      brw_required_dispatch_width(&nir->info, key->base.subgroup_size_type);

   fs_visitor *v[3]     = {0};
   const char *error[3] = {0};

   for (unsigned simd = 0; simd < 3; simd++) {
      if (!brw_simd_should_compile(mem_ctx, simd, compiler->devinfo, &prog_data->base,
                                   required_dispatch_width, &error[simd]))
         continue;

      const unsigned dispatch_width = 8 << simd;

      nir_shader *shader = nir_shader_clone(mem_ctx, nir);
      brw_nir_apply_key(shader, compiler, &key->base, dispatch_width, true /* is_scalar */);

      NIR_PASS_V(shader, brw_nir_lower_simd, dispatch_width);

      brw_postprocess_nir(shader, compiler, true /* is_scalar */, debug_enabled,
                          key->base.robust_buffer_access);

      v[simd] = new fs_visitor(compiler, params->log_data, mem_ctx, &key->base,
                               &prog_data->base.base, shader, dispatch_width,
                               -1 /* shader_time_index */, debug_enabled);

      if (prog_data->base.prog_mask) {
         unsigned first = ffs(prog_data->base.prog_mask) - 1;
         v[simd]->import_uniforms(v[first]);
      }

      const bool allow_spilling = !prog_data->base.prog_mask;

      if (v[simd]->run_task(allow_spilling))
         brw_simd_mark_compiled(simd, &prog_data->base, v[simd]->spilled_any_registers);
      else
         error[simd] = ralloc_strdup(mem_ctx, v[simd]->fail_msg);
   }

   int selected_simd = brw_simd_select(&prog_data->base);
   if (selected_simd < 0) {
      params->error_str = ralloc_asprintf(mem_ctx, "Can't compile shader: %s, %s and %s.\n",
                                          error[0], error[1], error[2]);;
      return NULL;
   }

   fs_visitor *selected = v[selected_simd];
   prog_data->base.prog_mask = 1 << selected_simd;

   fs_generator g(compiler, params->log_data, mem_ctx,
                  &prog_data->base.base, false, MESA_SHADER_TASK);
   if (unlikely(debug_enabled)) {
      g.enable_debug(ralloc_asprintf(mem_ctx,
                                     "%s task shader %s",
                                     nir->info.label ? nir->info.label
                                                     : "unnamed",
                                     nir->info.name));
   }

   g.generate_code(selected->cfg, selected->dispatch_width, selected->shader_stats,
                   selected->performance_analysis.require(), params->stats);

   delete v[0];
   delete v[1];
   delete v[2];

   return g.get_assembly();
}

const unsigned *
brw_compile_mesh(const struct brw_compiler *compiler,
                 void *mem_ctx,
                 struct brw_compile_mesh_params *params)
{
   struct nir_shader *nir = params->nir;
   const struct brw_mesh_prog_key *key = params->key;
   struct brw_mesh_prog_data *prog_data = params->prog_data;
   const bool debug_enabled = INTEL_DEBUG(DEBUG_MESH);

   prog_data->base.base.stage = MESA_SHADER_MESH;
   prog_data->base.base.total_shared = nir->info.shared_size;

   prog_data->base.local_size[0] = nir->info.workgroup_size[0];
   prog_data->base.local_size[1] = nir->info.workgroup_size[1];
   prog_data->base.local_size[2] = nir->info.workgroup_size[2];

   prog_data->primitive_type = nir->info.mesh.primitive_type;

   /* TODO(mesh): Use other index formats (that are more compact) for optimization. */
   prog_data->index_format = BRW_INDEX_FORMAT_U32;

   const unsigned required_dispatch_width =
      brw_required_dispatch_width(&nir->info, key->base.subgroup_size_type);

   fs_visitor *v[3]     = {0};
   const char *error[3] = {0};

   for (int simd = 0; simd < 3; simd++) {
      if (!brw_simd_should_compile(mem_ctx, simd, compiler->devinfo, &prog_data->base,
                                   required_dispatch_width, &error[simd]))
         continue;

      const unsigned dispatch_width = 8 << simd;

      nir_shader *shader = nir_shader_clone(mem_ctx, nir);
      brw_nir_apply_key(shader, compiler, &key->base, dispatch_width, true /* is_scalar */);

      NIR_PASS_V(shader, brw_nir_lower_simd, dispatch_width);

      brw_postprocess_nir(shader, compiler, true /* is_scalar */, debug_enabled,
                          key->base.robust_buffer_access);

      v[simd] = new fs_visitor(compiler, params->log_data, mem_ctx, &key->base,
                               &prog_data->base.base, shader, dispatch_width,
                               -1 /* shader_time_index */, debug_enabled);

      if (prog_data->base.prog_mask) {
         unsigned first = ffs(prog_data->base.prog_mask) - 1;
         v[simd]->import_uniforms(v[first]);
      }

      const bool allow_spilling = !prog_data->base.prog_mask;

      if (v[simd]->run_mesh(allow_spilling))
         brw_simd_mark_compiled(simd, &prog_data->base, v[simd]->spilled_any_registers);
      else
         error[simd] = ralloc_strdup(mem_ctx, v[simd]->fail_msg);
   }

   int selected_simd = brw_simd_select(&prog_data->base);
   if (selected_simd < 0) {
      params->error_str = ralloc_asprintf(mem_ctx, "Can't compile shader: %s, %s and %s.\n",
                                          error[0], error[1], error[2]);;
      return NULL;
   }

   fs_visitor *selected = v[selected_simd];
   prog_data->base.prog_mask = 1 << selected_simd;

   fs_generator g(compiler, params->log_data, mem_ctx,
                  &prog_data->base.base, false, MESA_SHADER_MESH);
   if (unlikely(debug_enabled)) {
      g.enable_debug(ralloc_asprintf(mem_ctx,
                                     "%s mesh shader %s",
                                     nir->info.label ? nir->info.label
                                                     : "unnamed",
                                     nir->info.name));
   }

   g.generate_code(selected->cfg, selected->dispatch_width, selected->shader_stats,
                   selected->performance_analysis.require(), params->stats);

   delete v[0];
   delete v[1];
   delete v[2];

   return g.get_assembly();
}

void
fs_visitor::nir_emit_task_intrinsic(const fs_builder &bld,
                                    nir_intrinsic_instr *instr)
{
   assert(stage == MESA_SHADER_TASK);

   switch (instr->intrinsic) {
   case nir_intrinsic_store_output:
   case nir_intrinsic_load_output:
      /* TODO(mesh): Task Output. */
      break;

   default:
      nir_emit_task_mesh_intrinsic(bld, instr);
      break;
   }
}

void
fs_visitor::nir_emit_mesh_intrinsic(const fs_builder &bld,
                                    nir_intrinsic_instr *instr)
{
   assert(stage == MESA_SHADER_MESH);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_input:
      /* TODO(mesh): Mesh Input. */
      break;

   case nir_intrinsic_store_per_primitive_output:
   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_output:
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_load_per_primitive_output:
   case nir_intrinsic_load_output:
      /* TODO(mesh): Mesh Output. */
      break;

   default:
      nir_emit_task_mesh_intrinsic(bld, instr);
      break;
   }
}

void
fs_visitor::nir_emit_task_mesh_intrinsic(const fs_builder &bld,
                                         nir_intrinsic_instr *instr)
{
   assert(stage == MESA_SHADER_MESH || stage == MESA_SHADER_TASK);

   switch (instr->intrinsic) {
   default:
      nir_emit_cs_intrinsic(bld, instr);
      break;
   }
}

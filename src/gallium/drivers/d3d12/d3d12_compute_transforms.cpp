/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_compute_transforms.h"
#include "d3d12_nir_passes.h"

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_dxil.h"

#include "util/u_memory.h"

nir_shader *
get_indirect_draw_base_vertex_transform(const d3d12_compute_transform_key *args)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, 
      dxil_get_nir_compiler_options(), "TransformIndirectDrawBaseVertex");

   if (args->base_vertex.dynamic_count) {
      nir_variable *count_ubo = nir_variable_create(b.shader, nir_var_mem_ubo,
         glsl_uint_type(), "in_count");
      count_ubo->data.driver_location = 0;
   }

   nir_variable *input_ssbo = nir_variable_create(b.shader, nir_var_mem_ssbo,
      glsl_array_type(glsl_uint_type(), 0, 0), "input");
   nir_variable *output_ssbo = nir_variable_create(b.shader, nir_var_mem_ssbo,
      input_ssbo->type, "output");
   input_ssbo->data.driver_location = 0;
   output_ssbo->data.driver_location = 1;

   nir_ssa_def *draw_id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   if (args->base_vertex.dynamic_count) {
      nir_ssa_def *count = nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
         (gl_access_qualifier)0, 4, 0, 0, 4);
      nir_push_if(&b, nir_ilt(&b, draw_id, count));
   }

   nir_variable *stride_ubo = NULL;
   nir_ssa_def *in_stride_offset_and_base_drawid = d3d12_get_state_var(&b, D3D12_STATE_VAR_TRANSFORM_GENERIC0, "d3d12_Stride",
      glsl_uvec4_type(), &stride_ubo);
   nir_ssa_def *in_offset = nir_iadd(&b, nir_channel(&b, in_stride_offset_and_base_drawid, 1),
      nir_imul(&b, nir_channel(&b, in_stride_offset_and_base_drawid, 0), draw_id));
   nir_ssa_def *in_data0 = nir_load_ssbo(&b, 4, 32, nir_imm_int(&b, 0), in_offset, (gl_access_qualifier)0, 4, 0);

   nir_ssa_def *in_data1 = NULL;
   nir_ssa_def *base_vertex = NULL, *base_instance = NULL;
   if (args->base_vertex.indexed) {
      nir_ssa_def *in_offset1 = nir_iadd(&b, in_offset, nir_imm_int(&b, 16));
      in_data1 = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 0), in_offset1, (gl_access_qualifier)0, 4, 0);
      base_vertex = nir_channel(&b, in_data0, 3);
      base_instance = in_data1;
   } else {
      base_vertex = nir_channel(&b, in_data0, 2);
      base_instance = nir_channel(&b, in_data0, 3);
   }

   /* 4 additional uints for base vertex, base instance, draw ID, and a bool for indexed draw */
   unsigned out_stride = sizeof(uint32_t) * ((args->base_vertex.indexed ? 5 : 4) + 4);

   nir_ssa_def *out_offset = nir_imul(&b, draw_id, nir_imm_int(&b, out_stride));
   nir_ssa_def *out_data0 = nir_vec4(&b, base_vertex, base_instance,
      nir_iadd(&b, draw_id, nir_channel(&b, in_stride_offset_and_base_drawid, 2)),
      nir_imm_int(&b, args->base_vertex.indexed ? -1 : 0));
   nir_ssa_def *out_data1 = in_data0;

   nir_store_ssbo(&b, out_data0, nir_imm_int(&b, 1), out_offset, 0xf, (gl_access_qualifier)0, 4, 0);
   nir_store_ssbo(&b, out_data1, nir_imm_int(&b, 1), nir_iadd(&b, out_offset, nir_imm_int(&b, 16)),
      (1u << out_data1->num_components) - 1, (gl_access_qualifier)0, 4, 0);
   if (args->base_vertex.indexed)
      nir_store_ssbo(&b, in_data1, nir_imm_int(&b, 1), nir_iadd(&b, out_offset, nir_imm_int(&b, 32)), 1, (gl_access_qualifier)0, 4, 0);

   if (args->base_vertex.dynamic_count)
      nir_pop_if(&b, NULL);

   nir_validate_shader(b.shader, "creation");
   b.shader->info.num_ssbos = 2;
   b.shader->info.num_ubos = (args->base_vertex.dynamic_count ? 1 : 0);

   return b.shader;
}

static struct nir_shader *
create_compute_transform(const d3d12_compute_transform_key *key)
{
   switch (key->type) {
   case d3d12_compute_transform_type::base_vertex:
      return get_indirect_draw_base_vertex_transform(key);
   default:
      unreachable("Invalid transform");
   }
}

struct compute_transform
{
   d3d12_compute_transform_key key;
   d3d12_shader_selector *shader;
};

d3d12_shader_selector *
d3d12_get_compute_transform(struct d3d12_context *ctx, const d3d12_compute_transform_key *key)
{
   struct hash_entry *entry = _mesa_hash_table_search(ctx->compute_transform_cache, key);
   if (!entry) {
      compute_transform *data = (compute_transform *)MALLOC(sizeof(compute_transform));
      if (!data)
         return NULL;

      memcpy(&data->key, key, sizeof(*key));
      nir_shader *s = create_compute_transform(key);
      if (!s) {
         FREE(data);
         return NULL;
      }
      struct pipe_compute_state shader_args = { PIPE_SHADER_IR_NIR, s };
      data->shader = d3d12_create_compute_shader(ctx, &shader_args);
      if (!data->shader) {
         ralloc_free(s);
         FREE(data);
         return NULL;
      }

      data->shader->is_variant = true;
      entry = _mesa_hash_table_insert(ctx->compute_transform_cache, &data->key, data);
      assert(entry);
   }

   return ((struct compute_transform *)entry->data)->shader;
}

static uint32_t
hash_compute_transform_key(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct d3d12_compute_transform_key));
}

static bool
equals_compute_transform_key(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct d3d12_compute_transform_key)) == 0;
}

void
d3d12_compute_transform_cache_init(struct d3d12_context *ctx)
{
   ctx->compute_transform_cache = _mesa_hash_table_create(NULL,
                                                          hash_compute_transform_key,
                                                          equals_compute_transform_key);
}

static void
delete_entry(struct hash_entry *entry)
{
   struct compute_transform *data = (struct compute_transform *)entry->data;
   d3d12_shader_free(data->shader);
   FREE(data);
}

void
d3d12_compute_transform_cache_destroy(struct d3d12_context *ctx)
{
   _mesa_hash_table_destroy(ctx->compute_transform_cache, delete_entry);
}

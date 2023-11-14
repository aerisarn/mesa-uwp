/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_physical_device.h"
#include "nvk_shader.h"

#include "nir.h"
#include "nir_builder.h"

#include "nv50_ir_driver.h"

uint64_t
nvk_cg_get_prog_debug(void)
{
   return debug_get_num_option("NV50_PROG_DEBUG", 0);
}

uint64_t
nvk_cg_get_prog_optimize(void)
{
   return debug_get_num_option("NV50_PROG_OPTIMIZE", 3);
}

static inline enum pipe_shader_type
pipe_shader_type_from_mesa(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return PIPE_SHADER_VERTEX;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_FRAGMENT:
      return PIPE_SHADER_FRAGMENT;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return PIPE_SHADER_COMPUTE;
   default:
      unreachable("bad shader stage");
   }
}

const nir_shader_compiler_options *
nvk_cg_nir_options(const struct nvk_physical_device *pdev,
                   gl_shader_stage stage)
{
   enum pipe_shader_type p_stage = pipe_shader_type_from_mesa(stage);
   return nv50_ir_nir_shader_compiler_options(pdev->info.chipset, p_stage);
}

static nir_variable *
find_or_create_input(nir_builder *b, const struct glsl_type *type,
                     const char *name, unsigned location)
{
   nir_foreach_shader_in_variable(in, b->shader) {
      if (in->data.location == location)
         return in;
   }
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                          type, name);
   in->data.location = location;
   if (glsl_type_is_integer(type))
      in->data.interpolation = INTERP_MODE_FLAT;

   return in;
}

static bool
lower_fragcoord_instr(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   assert(b->shader->info.stage == MESA_SHADER_FRAGMENT);
   nir_variable *var;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *val;
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_frag_coord:
      var = find_or_create_input(b, glsl_vec4_type(),
                                 "gl_FragCoord",
                                 VARYING_SLOT_POS);
      val = nir_load_var(b, var);
      break;
   case nir_intrinsic_load_point_coord:
      var = find_or_create_input(b, glsl_vector_type(GLSL_TYPE_FLOAT, 2),
                                 "gl_PointCoord",
                                 VARYING_SLOT_PNTC);
      val = nir_load_var(b, var);
      break;
   case nir_intrinsic_load_sample_pos:
      var = find_or_create_input(b, glsl_vec4_type(),
                                 "gl_FragCoord",
                                 VARYING_SLOT_POS);
      val = nir_ffract(b, nir_trim_vector(b, nir_load_var(b, var), 2));
      break;
   case nir_intrinsic_load_layer_id:
      var = find_or_create_input(b, glsl_int_type(),
                                 "gl_Layer", VARYING_SLOT_LAYER);
      val = nir_load_var(b, var);
      break;

   default:
      return false;
   }

   nir_def_rewrite_uses(&intrin->def, val);

   return true;
}

void
nvk_cg_preprocess_nir(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);

   NIR_PASS(_, nir, nir_lower_system_values);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_shader_instructions_pass, lower_fragcoord_instr,
               nir_metadata_block_index | nir_metadata_dominance, NULL);
   }
}

/*
 * Copyright (C) 2005-2007  Brian Paul   All Rights Reserved.
 * Copyright (C) 2008  VMware, Inc.   All Rights Reserved.
 * Copyright © 2010 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ir_to_mesa.cpp
 *
 * Translate GLSL IR to Mesa's gl_program representation.
 */

#include <stdio.h>
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/shaderapi.h"
#include "main/shaderobj.h"
#include "main/uniforms.h"
#include "main/glspirv.h"
#include "compiler/glsl/ast.h"
#include "compiler/glsl/ir.h"
#include "compiler/glsl/ir_expression_flattening.h"
#include "compiler/glsl/ir_optimization.h"
#include "compiler/glsl/ir_uniform.h"
#include "compiler/glsl/glsl_parser_extras.h"
#include "compiler/glsl_types.h"
#include "compiler/glsl/linker.h"
#include "compiler/glsl/program.h"
#include "compiler/glsl/shader_cache.h"
#include "program/prog_instruction.h"
#include "program/prog_optimize.h"
#include "program/prog_print.h"
#include "program/program.h"
#include "program/prog_parameter.h"


static void
associate_uniform_storage(struct gl_context *ctx,
                          struct gl_shader_program *shader_program,
                          struct gl_program *prog)
{
   struct gl_program_parameter_list *params = prog->Parameters;
   gl_shader_stage shader_type = prog->info.stage;

   _mesa_disallow_parameter_storage_realloc(params);

   /* After adding each uniform to the parameter list, connect the storage for
    * the parameter with the tracking structure used by the API for the
    * uniform.
    */
   unsigned last_location = unsigned(~0);
   for (unsigned i = 0; i < params->NumParameters; i++) {
      if (params->Parameters[i].Type != PROGRAM_UNIFORM)
         continue;

      unsigned location = params->Parameters[i].UniformStorageIndex;

      struct gl_uniform_storage *storage =
         &shader_program->data->UniformStorage[location];

      /* Do not associate any uniform storage to built-in uniforms */
      if (storage->builtin)
         continue;

      if (location != last_location) {
         enum gl_uniform_driver_format format = uniform_native;
         unsigned columns = 0;

         int dmul;
         if (ctx->Const.PackedDriverUniformStorage && !prog->info.is_arb_asm) {
            dmul = storage->type->vector_elements * sizeof(float);
         } else {
            dmul = 4 * sizeof(float);
         }

         switch (storage->type->base_type) {
         case GLSL_TYPE_UINT64:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            FALLTHROUGH;
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_UINT16:
         case GLSL_TYPE_UINT8:
            assert(ctx->Const.NativeIntegers);
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_INT64:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            FALLTHROUGH;
         case GLSL_TYPE_INT:
         case GLSL_TYPE_INT16:
         case GLSL_TYPE_INT8:
            format =
               (ctx->Const.NativeIntegers) ? uniform_native : uniform_int_float;
            columns = 1;
            break;
         case GLSL_TYPE_DOUBLE:
            if (storage->type->vector_elements > 2)
               dmul *= 2;
            FALLTHROUGH;
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_FLOAT16:
            format = uniform_native;
            columns = storage->type->matrix_columns;
            break;
         case GLSL_TYPE_BOOL:
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_SAMPLER:
         case GLSL_TYPE_TEXTURE:
         case GLSL_TYPE_IMAGE:
         case GLSL_TYPE_SUBROUTINE:
            format = uniform_native;
            columns = 1;
            break;
         case GLSL_TYPE_ATOMIC_UINT:
         case GLSL_TYPE_ARRAY:
         case GLSL_TYPE_VOID:
         case GLSL_TYPE_STRUCT:
         case GLSL_TYPE_ERROR:
         case GLSL_TYPE_INTERFACE:
         case GLSL_TYPE_FUNCTION:
            assert(!"Should not get here.");
            break;
         }

         unsigned pvo = params->Parameters[i].ValueOffset;
         _mesa_uniform_attach_driver_storage(storage, dmul * columns, dmul,
                                             format,
                                             &params->ParameterValues[pvo]);

         /* When a bindless sampler/image is bound to a texture/image unit, we
          * have to overwrite the constant value by the resident handle
          * directly in the constant buffer before the next draw. One solution
          * is to keep track a pointer to the base of the data.
          */
         if (storage->is_bindless && (prog->sh.NumBindlessSamplers ||
                                      prog->sh.NumBindlessImages)) {
            unsigned array_elements = MAX2(1, storage->array_elements);

            for (unsigned j = 0; j < array_elements; ++j) {
               unsigned unit = storage->opaque[shader_type].index + j;

               if (storage->type->without_array()->is_sampler()) {
                  assert(unit >= 0 && unit < prog->sh.NumBindlessSamplers);
                  prog->sh.BindlessSamplers[unit].data =
                     &params->ParameterValues[pvo] + 4 * j;
               } else if (storage->type->without_array()->is_image()) {
                  assert(unit >= 0 && unit < prog->sh.NumBindlessImages);
                  prog->sh.BindlessImages[unit].data =
                     &params->ParameterValues[pvo] + 4 * j;
               }
            }
         }

         /* After attaching the driver's storage to the uniform, propagate any
          * data from the linker's backing store.  This will cause values from
          * initializers in the source code to be copied over.
          */
         unsigned array_elements = MAX2(1, storage->array_elements);
         if (ctx->Const.PackedDriverUniformStorage && !prog->info.is_arb_asm &&
             (storage->is_bindless || !storage->type->contains_opaque())) {
            const int dmul = storage->type->is_64bit() ? 2 : 1;
            const unsigned components =
               storage->type->vector_elements *
               storage->type->matrix_columns;

            for (unsigned s = 0; s < storage->num_driver_storage; s++) {
               gl_constant_value *uni_storage = (gl_constant_value *)
                  storage->driver_storage[s].data;
               memcpy(uni_storage, storage->storage,
                      sizeof(storage->storage[0]) * components *
                      array_elements * dmul);
            }
         } else {
            _mesa_propagate_uniforms_to_driver_storage(storage, 0,
                                                       array_elements);
         }

	      last_location = location;
      }
   }
}

void
_mesa_ensure_and_associate_uniform_storage(struct gl_context *ctx,
                              struct gl_shader_program *shader_program,
                              struct gl_program *prog, unsigned required_space)
{
   /* Avoid reallocation of the program parameter list, because the uniform
    * storage is only associated with the original parameter list.
    */
   _mesa_reserve_parameter_storage(prog->Parameters, required_space,
                                   required_space);

   /* This has to be done last.  Any operation the can cause
    * prog->ParameterValues to get reallocated (e.g., anything that adds a
    * program constant) has to happen before creating this linkage.
    */
   associate_uniform_storage(ctx, shader_program, prog);
}


extern "C" {

/**
 * Link a GLSL shader program.  Called via glLinkProgram().
 */
void
_mesa_glsl_link_shader(struct gl_context *ctx, struct gl_shader_program *prog)
{
   unsigned int i;
   bool spirv = false;

   _mesa_clear_shader_program_data(ctx, prog);

   prog->data = _mesa_create_shader_program_data();

   prog->data->LinkStatus = LINKING_SUCCESS;

   for (i = 0; i < prog->NumShaders; i++) {
      if (!prog->Shaders[i]->CompileStatus) {
	 linker_error(prog, "linking with uncompiled/unspecialized shader");
      }

      if (!i) {
         spirv = (prog->Shaders[i]->spirv_data != NULL);
      } else if (spirv && !prog->Shaders[i]->spirv_data) {
         /* The GL_ARB_gl_spirv spec adds a new bullet point to the list of
          * reasons LinkProgram can fail:
          *
          *    "All the shader objects attached to <program> do not have the
          *     same value for the SPIR_V_BINARY_ARB state."
          */
         linker_error(prog,
                      "not all attached shaders have the same "
                      "SPIR_V_BINARY_ARB state");
      }
   }
   prog->data->spirv = spirv;

   if (prog->data->LinkStatus) {
      if (!spirv)
         link_shaders(ctx, prog);
      else
         _mesa_spirv_link_shaders(ctx, prog);
   }

   /* If LinkStatus is LINKING_SUCCESS, then reset sampler validated to true.
    * Validation happens via the LinkShader call below. If LinkStatus is
    * LINKING_SKIPPED, then SamplersValidated will have been restored from the
    * shader cache.
    */
   if (prog->data->LinkStatus == LINKING_SUCCESS) {
      prog->SamplersValidated = GL_TRUE;
   }

   if (prog->data->LinkStatus && !ctx->Driver.LinkShader(ctx, prog)) {
      prog->data->LinkStatus = LINKING_FAILURE;
   }

   if (prog->data->LinkStatus != LINKING_FAILURE)
      _mesa_create_program_resource_hash(prog);

   /* Return early if we are loading the shader from on-disk cache */
   if (prog->data->LinkStatus == LINKING_SKIPPED)
      return;

   if (ctx->_Shader->Flags & GLSL_DUMP) {
      if (!prog->data->LinkStatus) {
	 fprintf(stderr, "GLSL shader program %d failed to link\n", prog->Name);
      }

      if (prog->data->InfoLog && prog->data->InfoLog[0] != 0) {
	 fprintf(stderr, "GLSL shader program %d info log:\n", prog->Name);
         fprintf(stderr, "%s\n", prog->data->InfoLog);
      }
   }

#ifdef ENABLE_SHADER_CACHE
   if (prog->data->LinkStatus)
      shader_cache_write_program_metadata(ctx, prog);
#endif
}

} /* extern "C" */

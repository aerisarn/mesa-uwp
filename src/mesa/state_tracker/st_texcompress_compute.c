/**************************************************************************
 *
 * Copyright © 2022 Intel Corporation
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
 *
 **************************************************************************/

#include "main/shaderapi.h"
#include "main/shaderobj.h"

#include "state_tracker/st_bc1_tables.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_texcompress_compute.h"

#include "util/u_string.h"

enum compute_program_id {
   COMPUTE_PROGRAM_COUNT
};

static struct gl_program * PRINTFLIKE(3, 4)
get_compute_program(struct st_context *st,
                    enum compute_program_id prog_id,
                    const char *source_fmt, ...)
{
   /* Try to get the program from the cache. */
   assert(prog_id < COMPUTE_PROGRAM_COUNT);
   if (st->texcompress_compute.progs[prog_id])
      return st->texcompress_compute.progs[prog_id];

   /* Cache miss. Create the final source string. */
   char *source_str;
   va_list ap;
   va_start(ap, source_fmt);
   int num_printed_bytes = vasprintf(&source_str, source_fmt, ap);
   va_end(ap);
   if (num_printed_bytes == -1)
      return NULL;

   /* Compile and link the shader. Then, destroy the shader string. */
   const char *strings[] = { source_str };
   GLuint program =
      _mesa_CreateShaderProgramv_impl(st->ctx, GL_COMPUTE_SHADER, 1, strings);
   free(source_str);

   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program(st->ctx, program);
   if (!shProg)
      return NULL;

   if (shProg->data->LinkStatus == LINKING_FAILURE) {
      fprintf(stderr, "Linking failed:\n%s\n", shProg->data->InfoLog);
      _mesa_reference_shader_program(st->ctx, &shProg, NULL);
      return NULL;
   }

   /* Cache the program and return it. */
   return st->texcompress_compute.progs[prog_id] =
          shProg->_LinkedShaders[MESA_SHADER_COMPUTE]->Program;
}

static struct pipe_resource *
create_bc1_endpoint_ssbo(struct pipe_context *pipe)
{
   struct pipe_resource *buffer =
      pipe_buffer_create(pipe->screen, PIPE_BIND_SHADER_BUFFER,
                         PIPE_USAGE_IMMUTABLE, sizeof(float) *
                         (sizeof(stb__OMatch5) + sizeof(stb__OMatch6)));

   if (!buffer)
      return NULL;

   struct pipe_transfer *transfer;
   float (*buffer_map)[2] = pipe_buffer_map(pipe, buffer,
                                            PIPE_MAP_WRITE |
                                            PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                                            &transfer);
   if (!buffer_map) {
      pipe_resource_reference(&buffer, NULL);
      return NULL;
   }

   for (int i = 0; i < 256; i++) {
      for (int j = 0; j < 2; j++) {
         buffer_map[i][j] = (float) stb__OMatch5[i][j];
         buffer_map[i + 256][j] = (float) stb__OMatch6[i][j];
      }
   }

   pipe_buffer_unmap(pipe, transfer);

   return buffer;
}

bool
st_init_texcompress_compute(struct st_context *st)
{
   st->texcompress_compute.progs =
      calloc(COMPUTE_PROGRAM_COUNT, sizeof(struct gl_program *));
   if (!st->texcompress_compute.progs)
      return false;

   st->texcompress_compute.bc1_endpoint_buf =
      create_bc1_endpoint_ssbo(st->pipe);
   if (!st->texcompress_compute.bc1_endpoint_buf)
      return false;

   return true;
}

void
st_destroy_texcompress_compute(struct st_context *st)
{
   /* The programs in the array are part of the gl_context (in st->ctx).They
    * are automatically destroyed when the context is destroyed (via
    * _mesa_free_context_data -> ... -> free_shader_program_data_cb).
    */
   free(st->texcompress_compute.progs);

   /* Destroy the SSBO used by the BC1 shader program. */
   pipe_resource_reference(&st->texcompress_compute.bc1_endpoint_buf, NULL);
}

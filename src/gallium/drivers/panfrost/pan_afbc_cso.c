/*
 * Copyright (C) 2023 Amazon.com, Inc. or its affiliates
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pan_afbc_cso.h"
#include "nir_builder.h"
#include "pan_context.h"
#include "pan_resource.h"
#include "pan_screen.h"

#define panfrost_afbc_add_info_ubo(name, b)                                    \
   nir_variable *info_ubo = nir_variable_create(                               \
      b.shader, nir_var_mem_ubo,                                               \
      glsl_array_type(glsl_uint_type(),                                        \
                      sizeof(struct panfrost_afbc_##name##_info) / 4, 0),      \
      "info_ubo");                                                             \
   info_ubo->data.driver_location = 0;

#define panfrost_afbc_get_info_field(name, b, field)                           \
   nir_load_ubo(                                                               \
      (b), 1, sizeof(((struct panfrost_afbc_##name##_info *)0)->field) * 8,    \
      nir_imm_int(b, 0),                                                       \
      nir_imm_int(b, offsetof(struct panfrost_afbc_##name##_info, field)),     \
      .align_mul = 4, .range = ~0)

struct pan_afbc_shader_data *
panfrost_afbc_get_shaders(struct panfrost_context *ctx,
                          struct panfrost_resource *rsrc, unsigned align)
{
   struct pipe_context *pctx = &ctx->base;
   struct panfrost_screen *screen = pan_screen(ctx->base.screen);
   bool tiled = rsrc->image.layout.modifier & AFBC_FORMAT_MOD_TILED;
   struct pan_afbc_shader_key key = {
      .bpp = util_format_get_blocksizebits(rsrc->base.format),
      .align = align,
      .tiled = tiled,
   };

   pthread_mutex_lock(&ctx->afbc_shaders.lock);
   struct hash_entry *he =
      _mesa_hash_table_search(ctx->afbc_shaders.shaders, &key);
   struct pan_afbc_shader_data *shader = he ? he->data : NULL;
   pthread_mutex_unlock(&ctx->afbc_shaders.lock);

   if (shader)
      return shader;

   shader = rzalloc(ctx->afbc_shaders.shaders, struct pan_afbc_shader_data);
   shader->key = key;
   _mesa_hash_table_insert(ctx->afbc_shaders.shaders, &shader->key, shader);

#define COMPILE_SHADER(name, ...)                                              \
   {                                                                           \
      nir_shader *nir =                                                        \
         panfrost_afbc_create_##name##_shader(screen, __VA_ARGS__);            \
      nir->info.num_ubos = 1;                                                  \
      struct pipe_compute_state cso = {PIPE_SHADER_IR_NIR, nir};               \
      shader->name##_cso = pctx->create_compute_state(pctx, &cso);             \
   }

#undef COMPILE_SHADER

   pthread_mutex_lock(&ctx->afbc_shaders.lock);
   _mesa_hash_table_insert(ctx->afbc_shaders.shaders, &shader->key, shader);
   pthread_mutex_unlock(&ctx->afbc_shaders.lock);

   return shader;
}

static uint32_t
panfrost_afbc_shader_key_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct pan_afbc_shader_key));
}

static bool
panfrost_afbc_shader_key_equal(const void *a, const void *b)
{
   return !memcmp(a, b, sizeof(struct pan_afbc_shader_key));
}

void
panfrost_afbc_context_init(struct panfrost_context *ctx)
{
   ctx->afbc_shaders.shaders = _mesa_hash_table_create(
      NULL, panfrost_afbc_shader_key_hash, panfrost_afbc_shader_key_equal);
   pthread_mutex_init(&ctx->afbc_shaders.lock, NULL);
}

void
panfrost_afbc_context_destroy(struct panfrost_context *ctx)
{
   _mesa_hash_table_destroy(ctx->afbc_shaders.shaders, NULL);
   pthread_mutex_destroy(&ctx->afbc_shaders.lock);
}

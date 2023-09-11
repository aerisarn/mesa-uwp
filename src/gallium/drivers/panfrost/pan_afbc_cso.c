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

static nir_def *
read_afbc_header(nir_builder *b, nir_def *buf, nir_def *idx)
{
   nir_def *offset = nir_imul_imm(b, idx, AFBC_HEADER_BYTES_PER_TILE);
   return nir_load_global(b, nir_iadd(b, buf, nir_u2u64(b, offset)), 16,
                          AFBC_HEADER_BYTES_PER_TILE / 4, 32);
}

static nir_def *
get_superblock_size(nir_builder *b, unsigned arch, nir_def *hdr,
                    nir_def *uncompressed_size)
{
   nir_def *size = nir_imm_int(b, 0);

   unsigned body_base_ptr_len = 32;
   unsigned nr_subblocks = 16;
   unsigned sz_len = 6; /* bits */
   nir_def *words[4];
   nir_def *mask = nir_imm_int(b, (1 << sz_len) - 1);
   nir_def *is_solid_color = nir_imm_bool(b, false);

   for (int i = 0; i < 4; i++)
      words[i] = nir_channel(b, hdr, i);

   /* Sum up all of the subblock sizes */
   for (int i = 0; i < nr_subblocks; i++) {
      nir_def *subblock_size;
      unsigned bitoffset = body_base_ptr_len + (i * sz_len);
      unsigned start = bitoffset / 32;
      unsigned end = (bitoffset + (sz_len - 1)) / 32;
      unsigned offset = bitoffset % 32;

      /* Handle differently if the size field is split between two words
       * of the header */
      if (start != end) {
         subblock_size = nir_ior(b, nir_ushr_imm(b, words[start], offset),
                                 nir_ishl_imm(b, words[end], 32 - offset));
         subblock_size = nir_iand(b, subblock_size, mask);
      } else {
         subblock_size =
            nir_ubitfield_extract_imm(b, words[start], offset, sz_len);
      }
      subblock_size = nir_bcsel(b, nir_ieq_imm(b, subblock_size, 1),
                                uncompressed_size, subblock_size);
      size = nir_iadd(b, size, subblock_size);

      /* When the first subblock size is set to zero, the whole superblock is
       * filled with a solid color specified in the header */
      if (arch >= 7 && i == 0)
         is_solid_color = nir_ieq_imm(b, size, 0);
   }

   return (arch >= 7)
             ? nir_bcsel(b, is_solid_color, nir_imm_zero(b, 1, 32), size)
             : size;
}

#define panfrost_afbc_size_get_info_field(b, field)                            \
   panfrost_afbc_get_info_field(size, b, field)

static nir_shader *
panfrost_afbc_create_size_shader(struct panfrost_screen *screen, unsigned bpp,
                                 unsigned align)
{
   struct panfrost_device *dev = pan_device(&screen->base);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, screen->vtbl.get_compiler_options(),
      "panfrost_afbc_size(bpp=%d)", bpp);

   panfrost_afbc_add_info_ubo(size, b);

   nir_def *coord = nir_load_global_invocation_id(&b, 32);
   nir_def *block_idx = nir_channel(&b, coord, 0);
   nir_def *src = panfrost_afbc_size_get_info_field(&b, src);
   nir_def *metadata = panfrost_afbc_size_get_info_field(&b, metadata);
   nir_def *uncompressed_size = nir_imm_int(&b, 4 * 4 * bpp / 8); /* bytes */

   nir_def *hdr = read_afbc_header(&b, src, block_idx);
   nir_def *size = get_superblock_size(&b, dev->arch, hdr, uncompressed_size);
   size = nir_iand(&b, nir_iadd(&b, size, nir_imm_int(&b, align - 1)),
                   nir_inot(&b, nir_imm_int(&b, align - 1)));

   nir_def *offset = nir_u2u64(
      &b,
      nir_iadd(&b,
               nir_imul_imm(&b, block_idx, sizeof(struct pan_afbc_block_info)),
               nir_imm_int(&b, offsetof(struct pan_afbc_block_info, size))));
   nir_store_global(&b, nir_iadd(&b, metadata, offset), 4, size, 0x1);

   return b.shader;
}

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

   COMPILE_SHADER(size, key.bpp, key.align);

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

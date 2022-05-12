/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 * All Rights Reserved.
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
 *
 */

/* This file implements randomized texture blit tests. */

#include "si_pipe.h"
#include "util/rand_xor.h"
#include "util/u_surface.h"
#include "amd/addrlib/inc/addrtypes.h"

static uint64_t seed_xorshift128plus[2];

#define RAND_NUM_SIZE 8

/* The GPU blits are emulated on the CPU using these CPU textures. */

struct cpu_texture {
   uint8_t *ptr;
   uint64_t size;
   uint64_t layer_stride;
   unsigned stride;
};

static void alloc_cpu_texture(struct cpu_texture *tex, struct pipe_resource *templ, unsigned level)
{
   unsigned width = u_minify(templ->width0, level);
   unsigned height = u_minify(templ->height0, level);

   tex->stride = align(util_format_get_stride(templ->format, width), RAND_NUM_SIZE);
   tex->layer_stride = util_format_get_2d_size(templ->format, tex->stride, height);
   tex->size = tex->layer_stride * util_num_layers(templ, level);
   tex->ptr = malloc(tex->size);
   assert(tex->ptr);
}

static void set_random_pixels(struct pipe_context *ctx, struct pipe_resource *tex,
                              struct cpu_texture *cpu, unsigned level)
{
   struct pipe_transfer *t;
   uint8_t *map;
   int x, y, z;
   unsigned width = u_minify(tex->width0, level);
   unsigned height = u_minify(tex->height0, level);
   unsigned num_y_blocks = util_format_get_nblocksy(tex->format, height);
   unsigned num_layers = util_num_layers(tex, level);

   map = pipe_texture_map_3d(ctx, tex, level, PIPE_MAP_WRITE, 0, 0, 0, width, height,
                             num_layers, &t);
   assert(map);

   for (z = 0; z < num_layers; z++) {
      for (y = 0; y < num_y_blocks; y++) {
         uint64_t *ptr = (uint64_t *)(map + t->layer_stride * z + t->stride * y);
         uint64_t *ptr_cpu = (uint64_t *)(cpu->ptr + cpu->layer_stride * z + cpu->stride * y);
         unsigned size = cpu->stride / RAND_NUM_SIZE;

         assert(t->stride % RAND_NUM_SIZE == 0);
         assert(cpu->stride % RAND_NUM_SIZE == 0);

         for (x = 0; x < size; x++) {
            *ptr++ = *ptr_cpu++ = rand_xorshift128plus(seed_xorshift128plus);
         }
      }
   }

   pipe_texture_unmap(ctx, t);
}

static bool compare_textures(struct pipe_context *ctx, struct pipe_resource *tex,
                             struct cpu_texture *cpu, unsigned level)
{
   struct pipe_transfer *t;
   uint8_t *map;
   int y, z;
   bool pass = true;
   unsigned width = u_minify(tex->width0, level);
   unsigned height = u_minify(tex->height0, level);
   unsigned stride = util_format_get_stride(tex->format, width);
   unsigned num_y_blocks = util_format_get_nblocksy(tex->format, height);
   unsigned num_layers = util_num_layers(tex, level);

   map = pipe_texture_map_3d(ctx, tex, level, PIPE_MAP_READ, 0, 0, 0, width, height,
                             num_layers, &t);
   assert(map);

   for (z = 0; z < num_layers; z++) {
      for (y = 0; y < num_y_blocks; y++) {
         uint8_t *ptr = map + t->layer_stride * z + t->stride * y;
         uint8_t *cpu_ptr = cpu->ptr + cpu->layer_stride * z + cpu->stride * y;

         if (memcmp(ptr, cpu_ptr, stride)) {
            pass = false;
            goto done;
         }
      }
   }
done:
   pipe_texture_unmap(ctx, t);
   return pass;
}

static enum pipe_format get_random_format(struct si_screen *sscreen)
{
   /* Keep generating formats until we get a supported one. */
   while (1) {
      /* Skip one format: PIPE_FORMAT_NONE */
      enum pipe_format format = (rand() % (PIPE_FORMAT_COUNT - 1)) + 1;
      const struct util_format_description *desc = util_format_description(format);

      if (desc->layout == UTIL_FORMAT_LAYOUT_PLAIN) {
         unsigned i;

         /* Don't test formats with X channels because cpu_texture doesn't emulate them. */
         for (i = 0; i < desc->nr_channels; i++) {
            if (desc->channel[i].type == UTIL_FORMAT_TYPE_VOID)
               break;
         }
         if (i != desc->nr_channels)
            continue;
      }

      if (desc->colorspace == UTIL_FORMAT_COLORSPACE_YUV)
         continue;

      if (sscreen->b.is_format_supported(&sscreen->b, format, PIPE_TEXTURE_2D, 1, 1,
                                         PIPE_BIND_SAMPLER_VIEW))
         return format;
   }
}

#define MAX_ALLOC_SIZE (64 * 1024 * 1024)

static void set_random_image_attrs(struct pipe_resource *templ)
{
   switch (rand() % 6) {
   case 0:
      templ->target = PIPE_TEXTURE_1D;
      break;
   case 1:
      templ->target = PIPE_TEXTURE_2D;
      break;
   case 2:
      if (util_format_is_depth_or_stencil(templ->format))
         templ->target = PIPE_TEXTURE_2D; /* 3D doesn't support Z/S */
      else
         templ->target = PIPE_TEXTURE_3D;
      break;
   case 3:
      templ->target = PIPE_TEXTURE_RECT;
      break;
   case 4:
      templ->target = PIPE_TEXTURE_1D_ARRAY;
      break;
   case 5:
   default:
      templ->target = PIPE_TEXTURE_2D_ARRAY;
      break;
   }

   templ->usage = PIPE_USAGE_DEFAULT;

   templ->height0 = 1;
   templ->depth0 = 1;
   templ->array_size = 1;

   /* Try to hit microtiling in 1/2 of the cases. */
   unsigned max_tex_size = rand() & 1 ? 128 : 1024;

   templ->width0 = (rand() % max_tex_size) + 1;

   if (templ->target != PIPE_TEXTURE_1D &&
       templ->target != PIPE_TEXTURE_1D_ARRAY)
      templ->height0 = (rand() % max_tex_size) + 1;

   if (templ->target == PIPE_TEXTURE_3D)
      templ->depth0 = (rand() % max_tex_size) + 1;

   if (templ->target == PIPE_TEXTURE_1D_ARRAY ||
       templ->target == PIPE_TEXTURE_2D_ARRAY)
      templ->array_size = (rand() % max_tex_size) + 1;

   /* Keep reducing the size until it we get a small enough size. */
   while ((uint64_t)util_format_get_nblocks(templ->format, templ->width0, templ->height0) *
          templ->depth0 * templ->array_size * util_format_get_blocksize(templ->format) >
          MAX_ALLOC_SIZE) {
      switch (rand() % 3) {
      case 0:
         if (templ->width0 > 1)
            templ->width0 /= 2;
         break;
      case 1:
         if (templ->height0 > 1)
            templ->height0 /= 2;
         break;
      case 2:
         if (templ->depth0 > 1)
            templ->depth0 /= 2;
         else if (templ->array_size > 1)
            templ->array_size /= 2;
         break;
      }
   }

   if (util_format_get_blockwidth(templ->format) == 2)
      templ->width0 = align(templ->width0, 2);

   if (templ->target != PIPE_TEXTURE_RECT &&
       util_format_description(templ->format)->layout != UTIL_FORMAT_LAYOUT_SUBSAMPLED) {
      unsigned max_dim = MAX3(templ->width0, templ->height0, templ->depth0);

      templ->last_level = rand() % (util_logbase2(max_dim) + 1);
   }
}

static void print_image_attrs(struct si_screen *sscreen, struct si_texture *tex)
{
   const char *mode;

   if (sscreen->info.gfx_level >= GFX9) {
      static const char *modes[32] = {
         [ADDR_SW_LINEAR] = "LINEAR",
         [ADDR_SW_4KB_S_X] = "4KB_S_X",
         [ADDR_SW_4KB_D_X] = "4KB_D_X",
         [ADDR_SW_64KB_Z_X] = "64KB_Z_X",
         [ADDR_SW_64KB_S_X] = "64KB_S_X",
         [ADDR_SW_64KB_D_X] = "64KB_D_X",
         [ADDR_SW_64KB_R_X] = "64KB_R_X",
      };
      mode = modes[tex->surface.u.gfx9.swizzle_mode];
   } else {
      static const char *modes[32] = {
         [RADEON_SURF_MODE_LINEAR_ALIGNED] = "LINEAR",
         [RADEON_SURF_MODE_1D] = "1D_TILED",
         [RADEON_SURF_MODE_2D] = "2D_TILED",
      };
      mode = modes[tex->surface.u.legacy.level[0].mode];
   }

   if (!mode)
      mode = "UNKNOWN";

   static const char *targets[PIPE_MAX_TEXTURE_TYPES] = {
      [PIPE_TEXTURE_1D] = "1D",
      [PIPE_TEXTURE_2D] = "2D",
      [PIPE_TEXTURE_3D] = "3D",
      [PIPE_TEXTURE_RECT] = "RECT",
      [PIPE_TEXTURE_1D_ARRAY] = "1D_ARRAY",
      [PIPE_TEXTURE_2D_ARRAY] = "2D_ARRAY",
   };

   char size[64];
   if (tex->buffer.b.b.target == PIPE_TEXTURE_1D)
      snprintf(size, sizeof(size), "%u", tex->buffer.b.b.width0);
   else if (tex->buffer.b.b.target == PIPE_TEXTURE_2D ||
            tex->buffer.b.b.target == PIPE_TEXTURE_RECT)
      snprintf(size, sizeof(size), "%ux%u", tex->buffer.b.b.width0, tex->buffer.b.b.height0);
   else
      snprintf(size, sizeof(size), "%ux%ux%u", tex->buffer.b.b.width0, tex->buffer.b.b.height0,
               util_num_layers(&tex->buffer.b.b, 0));

   printf("%8s, %14s, %2u levels, %8s", targets[tex->buffer.b.b.target], size,
          tex->buffer.b.b.last_level + 1, mode);
}

void si_test_image_copy_region(struct si_screen *sscreen)
{
   struct pipe_screen *screen = &sscreen->b;
   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   struct si_context *sctx = (struct si_context *)ctx;
   unsigned i, iterations, num_partial_copies;
   unsigned num_pass = 0, num_fail = 0;

   /* the seed for random test parameters */
   srand(0x9b47d95b);
   /* the seed for random pixel data */
   s_rand_xorshift128plus(seed_xorshift128plus, false);

   iterations = 1000000000; /* just kill it when you are bored */
   num_partial_copies = 30;

   /* These parameters are randomly generated per test:
    * - which texture dimensions to use
    * - random initial pixels in src
    * - execute multiple subrectangle copies for partial blits
    */
   for (i = 0; i < iterations; i++) {
      struct pipe_resource tsrc = {}, tdst = {}, *src, *dst;
      struct si_texture *sdst;
      struct si_texture *ssrc;
      struct cpu_texture src_cpu[RADEON_SURF_MAX_LEVELS], dst_cpu[RADEON_SURF_MAX_LEVELS];
      unsigned max_width, max_height, max_depth, j;
      unsigned gfx_blits = 0, cs_blits = 0;
      bool pass;

      /* generate a random test case */
      tsrc.format = tdst.format = get_random_format(sscreen);
      set_random_image_attrs(&tsrc);
      set_random_image_attrs(&tdst);

      /* Allocate textures (both the GPU and CPU copies).
       * The CPU will emulate what the GPU should be doing.
       */
      src = screen->resource_create(screen, &tsrc);
      dst = screen->resource_create(screen, &tdst);
      assert(src);
      assert(dst);
      sdst = (struct si_texture *)dst;
      ssrc = (struct si_texture *)src;

      printf("%4u: dst = (", i);
      print_image_attrs(sscreen, sdst);
      printf("), src = (");
      print_image_attrs(sscreen, ssrc);
      printf("), format = %18s, ", util_format_description(tsrc.format)->short_name);
      fflush(stdout);

      for (unsigned level = 0; level <= tsrc.last_level; level++) {
         alloc_cpu_texture(&src_cpu[level], &tsrc, level);
         set_random_pixels(ctx, src, &src_cpu[level], level);
      }
      for (unsigned level = 0; level <= tdst.last_level; level++) {
         alloc_cpu_texture(&dst_cpu[level], &tdst, level);
         memset(dst_cpu[level].ptr, 0, dst_cpu[level].layer_stride * util_num_layers(&tdst, level));
      }

      /* clear dst pixels */
      uint32_t zero = 0;
      si_clear_buffer(sctx, dst, 0, sdst->surface.surf_size, &zero, 4, SI_OP_SYNC_BEFORE_AFTER,
                      SI_COHERENCY_SHADER, SI_AUTO_SELECT_CLEAR_METHOD);

      for (j = 0; j < num_partial_copies; j++) {
         int width, height, depth;
         int srcx, srcy, srcz, dstx, dsty, dstz;
         struct pipe_box box;
         unsigned old_num_draw_calls = sctx->num_draw_calls;
         unsigned old_num_cs_calls = sctx->num_compute_calls;

         unsigned src_level = j % (tsrc.last_level + 1);
         unsigned dst_level = j % (tdst.last_level + 1);

         max_width = MIN2(u_minify(tsrc.width0, src_level), u_minify(tdst.width0, dst_level));
         max_height = MIN2(u_minify(tsrc.height0, src_level), u_minify(tdst.height0, dst_level));
         max_depth = MIN2(util_num_layers(&tsrc, src_level), util_num_layers(&tdst, dst_level));

         /* random sub-rectangle copies from src to dst */
         depth = (rand() % max_depth) + 1;
         srcz = rand() % (util_num_layers(&tsrc, src_level) - depth + 1);
         dstz = rand() % (util_num_layers(&tdst, dst_level) - depth + 1);

         /* just make sure that it doesn't divide by zero */
         assert(max_width > 0 && max_height > 0);

         width = (rand() % max_width) + 1;
         height = (rand() % max_height) + 1;

         srcx = rand() % (u_minify(tsrc.width0, src_level) - width + 1);
         srcy = rand() % (u_minify(tsrc.height0, src_level) - height + 1);

         dstx = rand() % (u_minify(tdst.width0, dst_level) - width + 1);
         dsty = rand() % (u_minify(tdst.height0, dst_level) - height + 1);

         /* Align the box to the format block size. */
         srcx &= ~(util_format_get_blockwidth(src->format) - 1);
         srcy &= ~(util_format_get_blockheight(src->format) - 1);

         dstx &= ~(util_format_get_blockwidth(dst->format) - 1);
         dsty &= ~(util_format_get_blockheight(dst->format) - 1);

         width = align(width, util_format_get_blockwidth(src->format));
         height = align(height, util_format_get_blockheight(src->format));

         /* GPU copy */
         u_box_3d(srcx, srcy, srcz, width, height, depth, &box);
         si_resource_copy_region(ctx, dst, dst_level, dstx, dsty, dstz, src, src_level, &box);

         /* See which engine was used. */
         gfx_blits += sctx->num_draw_calls > old_num_draw_calls;
         cs_blits += sctx->num_compute_calls > old_num_cs_calls;

         /* CPU copy */
         util_copy_box(dst_cpu[dst_level].ptr, tdst.format, dst_cpu[dst_level].stride,
                       dst_cpu[dst_level].layer_stride, dstx, dsty, dstz,
                       width, height, depth, src_cpu[src_level].ptr, src_cpu[src_level].stride,
                       src_cpu[src_level].layer_stride, srcx, srcy, srcz);
      }

      pass = true;
      for (unsigned level = 0; level <= tdst.last_level; level++)
         pass &= compare_textures(ctx, dst, &dst_cpu[level], level);

      if (pass)
         num_pass++;
      else
         num_fail++;

      printf("BLITs: GFX = %2u, CS = %2u, %s [%u/%u]\n", gfx_blits, cs_blits,
             pass ? "pass" : "fail", num_pass, num_pass + num_fail);

      /* cleanup */
      pipe_resource_reference(&src, NULL);
      pipe_resource_reference(&dst, NULL);
      for (unsigned level = 0; level <= tsrc.last_level; level++)
         free(src_cpu[level].ptr);
      for (unsigned level = 0; level <= tdst.last_level; level++)
         free(dst_cpu[level].ptr);
   }

   ctx->destroy(ctx);
   exit(0);
}

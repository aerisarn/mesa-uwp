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

static void alloc_cpu_texture(struct cpu_texture *tex, struct pipe_resource *templ)
{
   tex->stride = align(util_format_get_stride(templ->format, templ->width0), RAND_NUM_SIZE);
   tex->layer_stride = (uint64_t)tex->stride * templ->height0;
   tex->size = tex->layer_stride * templ->array_size;
   tex->ptr = malloc(tex->size);
   assert(tex->ptr);
}

static void set_random_pixels(struct pipe_context *ctx, struct pipe_resource *tex,
                              struct cpu_texture *cpu)
{
   struct pipe_transfer *t;
   uint8_t *map;
   int x, y, z;

   map = pipe_texture_map_3d(ctx, tex, 0, PIPE_MAP_WRITE, 0, 0, 0, tex->width0, tex->height0,
                              tex->array_size, &t);
   assert(map);

   for (z = 0; z < tex->array_size; z++) {
      for (y = 0; y < tex->height0; y++) {
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
                             struct cpu_texture *cpu)
{
   struct pipe_transfer *t;
   uint8_t *map;
   int y, z;
   bool pass = true;
   unsigned stride = util_format_get_stride(tex->format, tex->width0);

   map = pipe_texture_map_3d(ctx, tex, 0, PIPE_MAP_READ, 0, 0, 0, tex->width0, tex->height0,
                              tex->array_size, &t);
   assert(map);

   for (z = 0; z < tex->array_size; z++) {
      for (y = 0; y < tex->height0; y++) {
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

static enum pipe_format choose_format()
{
   enum pipe_format formats[] = {
      PIPE_FORMAT_R8_UINT,     PIPE_FORMAT_R16_UINT,          PIPE_FORMAT_R32_UINT,
      PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32B32A32_UINT, PIPE_FORMAT_G8R8_B8R8_UNORM,
   };
   return formats[rand() % ARRAY_SIZE(formats)];
}

#define MAX_ALLOC_SIZE (128 * 1024 * 1024)

static void set_random_image_attrs(struct pipe_resource *templ)
{
   templ->target = PIPE_TEXTURE_2D_ARRAY;
   templ->usage = PIPE_USAGE_DEFAULT;

   /* Try to hit microtiling in 1/2 of the cases. */
   unsigned max_tex_size = rand() & 1 ? 128 : 4096;
   unsigned max_tex_layers = rand() % 4 ? 1 : 3;

   /* keep generating the image size until it's less than the max size */
   while (1) {
      templ->width0 = (rand() % max_tex_size) + 1;
      templ->height0 = (rand() % max_tex_size) + 1;
      templ->depth0 = 1;
      templ->array_size = (rand() % max_tex_layers) + 1;

      if ((uint64_t)util_format_get_nblocks(templ->format, templ->width0, templ->height0) *
          templ->depth0 * templ->array_size * util_format_get_blocksize(templ->format) <
          MAX_ALLOC_SIZE)
         break;
   }

   if (util_format_get_blockwidth(templ->format) == 2)
      templ->width0 = align(templ->width0, 2);
}

static void print_image_attrs(struct si_screen *sscreen, struct si_texture *tex)
{
   const char *mode;

   if (sscreen->info.chip_class >= GFX9) {
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

   printf("%8s, %14s, %8s", targets[tex->buffer.b.b.target], size, mode);
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
      struct cpu_texture src_cpu, dst_cpu;
      unsigned max_width, max_height, max_depth, j;
      unsigned gfx_blits = 0, cs_blits = 0;
      bool pass;

      /* generate a random test case */
      tsrc.format = tdst.format = choose_format();
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
      alloc_cpu_texture(&src_cpu, &tsrc);
      alloc_cpu_texture(&dst_cpu, &tdst);

      printf("%4u: dst = (", i);
      print_image_attrs(sscreen, sdst);
      printf("), src = (");
      print_image_attrs(sscreen, ssrc);
      printf("), format = %17s, ", util_format_description(tsrc.format)->short_name);
      fflush(stdout);

      /* set src pixels */
      set_random_pixels(ctx, src, &src_cpu);

      /* clear dst pixels */
      uint32_t zero = 0;
      si_clear_buffer(sctx, dst, 0, sdst->surface.surf_size, &zero, 4, SI_OP_SYNC_BEFORE_AFTER,
                      SI_COHERENCY_SHADER, SI_AUTO_SELECT_CLEAR_METHOD);
      memset(dst_cpu.ptr, 0, dst_cpu.layer_stride * tdst.array_size);

      /* preparation */
      max_width = MIN2(tsrc.width0, tdst.width0);
      max_height = MIN2(tsrc.height0, tdst.height0);
      max_depth = MIN2(tsrc.array_size, tdst.array_size);

      for (j = 0; j < num_partial_copies; j++) {
         int width, height, depth;
         int srcx, srcy, srcz, dstx, dsty, dstz;
         struct pipe_box box;
         unsigned old_num_draw_calls = sctx->num_draw_calls;
         unsigned old_num_cs_calls = sctx->num_compute_calls;

         /* random sub-rectangle copies from src to dst */
         depth = (rand() % max_depth) + 1;
         srcz = rand() % (tsrc.array_size - depth + 1);
         dstz = rand() % (tdst.array_size - depth + 1);

         /* just make sure that it doesn't divide by zero */
         assert(max_width > 0 && max_height > 0);

         width = (rand() % max_width) + 1;
         height = (rand() % max_height) + 1;

         srcx = rand() % (tsrc.width0 - width + 1);
         srcy = rand() % (tsrc.height0 - height + 1);

         dstx = rand() % (tdst.width0 - width + 1);
         dsty = rand() % (tdst.height0 - height + 1);

         /* GPU copy */
         u_box_3d(srcx, srcy, srcz, width, height, depth, &box);
         si_resource_copy_region(ctx, dst, 0, dstx, dsty, dstz, src, 0, &box);

         /* See which engine was used. */
         gfx_blits += sctx->num_draw_calls > old_num_draw_calls;
         cs_blits += sctx->num_compute_calls > old_num_cs_calls;

         /* CPU copy */
         util_copy_box(dst_cpu.ptr, tdst.format, dst_cpu.stride, dst_cpu.layer_stride, dstx, dsty,
                       dstz, width, height, depth, src_cpu.ptr, src_cpu.stride,
                       src_cpu.layer_stride, srcx, srcy, srcz);
      }

      pass = compare_textures(ctx, dst, &dst_cpu);
      if (pass)
         num_pass++;
      else
         num_fail++;

      printf("BLITs: GFX = %2u, CS = %2u, %s [%u/%u]\n", gfx_blits, cs_blits,
             pass ? "pass" : "fail", num_pass, num_pass + num_fail);

      /* cleanup */
      pipe_resource_reference(&src, NULL);
      pipe_resource_reference(&dst, NULL);
      free(src_cpu.ptr);
      free(dst_cpu.ptr);
   }

   ctx->destroy(ctx);
   exit(0);
}

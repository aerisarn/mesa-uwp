/*
 * Copyright 2021 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#
#include <stdint.h>
#include "agx_state.h"
#include "magic.h"

/* The structures managed in this file appear to be software defined (either in
 * the macOS kernel driver or in the AGX firmware) */

/* Odd pattern */
static uint64_t
demo_unk6(struct agx_pool *pool)
{
   struct agx_ptr ptr = agx_pool_alloc_aligned(pool, 0x4000 * sizeof(uint64_t), 64);
   uint64_t *buf = ptr.cpu;
   memset(buf, 0, sizeof(*buf));

   for (unsigned i = 1; i < 0x3ff; ++i)
      buf[i] = (i + 1);

   return ptr.gpu;
}

static uint64_t
demo_zero(struct agx_pool *pool, unsigned count)
{
   struct agx_ptr ptr = agx_pool_alloc_aligned(pool, count, 64);
   memset(ptr.cpu, 0, count);
   return ptr.gpu;
}

unsigned
demo_cmdbuf(uint64_t *buf, size_t size,
            struct agx_pool *pool,
            uint64_t encoder_ptr,
            uint64_t encoder_id,
            uint64_t scissor_ptr,
            unsigned width, unsigned height, unsigned bpp,
            uint32_t pipeline_null,
            uint32_t pipeline_clear,
            uint32_t pipeline_store,
            uint64_t rt0,
            bool clear_pipeline_textures)
{
   uint32_t *map = (uint32_t *) buf;
   memset(map, 0, 474 * 4);

   map[54] = 0x6b0003;
   map[55] = 0x3a0012;
   map[56] = 1;

   map[106] = 1;
   map[108] = 0x1c;
   map[112] = 0xffffffff;
   map[113] = 0xffffffff;
   map[114] = 0xffffffff;

   uint64_t unk_buffer = demo_zero(pool, 0x1000);
   uint64_t unk_buffer_2 = demo_zero(pool, 0x8000);

   agx_pack(map + 156, IOGPU_INTERNAL_PIPELINES, cfg) {
      cfg.clear_pipeline_bind = 0xffff8002 | (clear_pipeline_textures ? 0x210 : 0);
      cfg.clear_pipeline = pipeline_clear;
      cfg.store_pipeline_bind = 0x12;
      cfg.store_pipeline = pipeline_store;
      cfg.scissor_array = scissor_ptr;
      cfg.unknown_buffer = unk_buffer;
   }

   agx_pack(map + 220, IOGPU_AUX_FRAMEBUFFER, cfg) {
      cfg.width = width;
      cfg.height = height;
      cfg.z16_unorm_attachment = false;
      cfg.pointer = unk_buffer_2;
   }

   agx_pack(map + 276, IOGPU_CLEAR_Z_S, cfg) {
      cfg.depth_clear_value = fui(1.0); // 32-bit float
      cfg.stencil_clear_value = 0;
      cfg.z16_unorm_attachment = false;
   }

   map[284] = 0xffffffff;
   map[285] = 0xffffffff;
   map[286] = 0xffffffff;

   map[298] = 0xffff8212;
   map[300] = pipeline_null | 0x4;
   map[305] = 0x12;
   map[306] = pipeline_store | 0x4;

   agx_pack(map + 344, IOGPU_MISC, cfg) {
      cfg.encoder_id = encoder_id;
      cfg.unknown_buffer = demo_unk6(pool);
      cfg.width = width;
      cfg.height = height;
   }

   unsigned offset_unk = (458 * 4);
   unsigned offset_attachments = (470 * 4);
   unsigned nr_attachments = 1;

   map[473] = nr_attachments;

   /* A single attachment follows, depth/stencil have their own attachments */
   agx_pack((map + (offset_attachments / 4) + 4), IOGPU_ATTACHMENT, cfg) {
      cfg.address = rt0;
      cfg.type = AGX_IOGPU_ATTACHMENT_TYPE_COLOUR;
      cfg.size = width * height * 4;
      cfg.percent = 100;
   }

   unsigned total_size = offset_attachments + (AGX_IOGPU_ATTACHMENT_LENGTH * nr_attachments) + 16;

   agx_pack(map, IOGPU_HEADER, cfg) {
      cfg.total_size = total_size;
      cfg.attachment_offset_1 = offset_attachments;
      cfg.attachment_offset_2 = offset_attachments;
      cfg.attachment_length = nr_attachments * AGX_IOGPU_ATTACHMENT_LENGTH;
      cfg.unknown_offset = offset_unk;
      cfg.encoder = encoder_ptr;

      cfg.deflake_1 = demo_zero(pool, 0x540);
      cfg.deflake_2 = demo_zero(pool, 0x280);
   }

   return total_size;
}

static struct agx_map_header
demo_map_header(uint64_t cmdbuf_id, uint64_t encoder_id, unsigned cmdbuf_size, unsigned count)
{
   return (struct agx_map_header) {
      .cmdbuf_id = cmdbuf_id,
      .unk2 = 0x1,
      .unk3 = 0x528, // 1320
      .encoder_id = encoder_id,
      .unk6 = 0x0,
      .cmdbuf_size = cmdbuf_size,

      /* +1 for the sentinel ending */
      .nr_entries = count,
      .nr_handles = count,
   };
}

void
demo_mem_map(void *map, size_t size, unsigned *handles, unsigned count,
             uint64_t cmdbuf_id, uint64_t encoder_id, unsigned cmdbuf_size)
{
   struct agx_map_header *header = map;
   struct agx_map_entry *entries = (struct agx_map_entry *) (((uint8_t *) map) + sizeof(*header));
   struct agx_map_entry *end = (struct agx_map_entry *) (((uint8_t *) map) + size);

   /* Header precedes the entry */
   *header = demo_map_header(cmdbuf_id, encoder_id, cmdbuf_size, count);

   /* Add an entry for each BO mapped */
   for (unsigned i = 0; i < count; ++i) {
	   assert((entries + i) < end);
      entries[i] = (struct agx_map_entry) {
         .indices = {handles[i]},
         .unkAAA = 0x20,
         .unkBBB = 0x1,
         .unka = 0x1ffff,
      };
   }
}

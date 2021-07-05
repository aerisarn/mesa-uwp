#include <stdint.h>
#include "agx_state.h"
#include "magic.h"

/* The structures managed in this file appear to be software defined (either in
 * the macOS kernel driver or in the AGX firmware) */

struct cmdbuf {
   uint32_t *map;
   unsigned offset;
};

static void
EMIT32(struct cmdbuf *cmdbuf, uint32_t val)
{
   cmdbuf->map[cmdbuf->offset++] = val;
}

static void
EMIT64(struct cmdbuf *cmdbuf, uint64_t val)
{
   EMIT32(cmdbuf, (val & 0xFFFFFFFF));
   EMIT32(cmdbuf, (val >> 32));
}

static void
EMIT_ZERO_WORDS(struct cmdbuf *cmdbuf, size_t words)
{
   memset(cmdbuf->map + cmdbuf->offset, 0, words * 4);
   cmdbuf->offset += words;
}

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
            uint64_t scissor_ptr,
            unsigned width, unsigned height,
            uint32_t pipeline_null,
            uint32_t pipeline_clear,
            uint32_t pipeline_store,
            uint64_t rt0,
            bool clear_pipeline_textures)
{
   struct cmdbuf _cmdbuf = {
      .map = (uint32_t *) buf,
      .offset = 0
   };

   struct cmdbuf *cmdbuf = &_cmdbuf;

   /* Vertex stuff */
   EMIT_ZERO_WORDS(cmdbuf, 12);

   EMIT32(cmdbuf, 0x30); /* 0x30 */
   EMIT32(cmdbuf, 0x01); /* 0x34. Compute: 0x03 */

   EMIT64(cmdbuf, encoder_ptr);

   EMIT_ZERO_WORDS(cmdbuf, 20);

   EMIT64(cmdbuf, 0); /* 0x90, compute blob - some zero */
   EMIT64(cmdbuf, 0); // blob - 0x540 bytes of zero, compute blob - null
   EMIT64(cmdbuf, 0); // blob - 0x280 bytes of zero
   EMIT64(cmdbuf, 0); // a8, compute blob - zero pointer

   EMIT64(cmdbuf, 0); // compute blob - zero pointer
   EMIT64(cmdbuf, 0); // compute blob - zero pointer
   EMIT64(cmdbuf, 0); // compute blob - zero pointer

   // while zero for vertex, used to include the odd unk6 pattern for compute
   EMIT64(cmdbuf, 0); // compute blob - 0x1
   EMIT64(cmdbuf, 0); // d0,  ompute blob - pointer to odd pattern, compare how it's done later for frag

   // compute 8 bytes of zero, then reconverge at *

   EMIT32(cmdbuf, 0x6b0003); // d8
   EMIT32(cmdbuf, 0x3a0012); // dc

   /* Possibly the funny pattern but not actually pointed to for vertex */
   EMIT64(cmdbuf, 1); // e0
   EMIT64(cmdbuf, 0); // e8

   EMIT_ZERO_WORDS(cmdbuf, 44);

   EMIT64(cmdbuf, 0); // blob - 0x20 bytes of zero
   EMIT64(cmdbuf, 1); // 1a8

   // * compute reconverges here at 0xe0 in my trace
   EMIT32(cmdbuf, 0x1c); // 1b0

   // compute 0xe4: [encoder ID -- from selector6 + 2 with blob], 0, 0, 0xffffffff, done for a while
   // compute 0x120: 0x9 | 0x128: 0x40

   EMIT32(cmdbuf, 0); // 1b0 - compute: 0x10000
   EMIT64(cmdbuf, 0x0); // 1b8 -- compute 0x10000
   EMIT32(cmdbuf, 0xffffffff); // note we can zero!
   EMIT32(cmdbuf, 0xffffffff); // note we can zero! compute 0
   EMIT32(cmdbuf, 0xffffffff); // note we can zero! compute 0
   EMIT32(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 40);

   // This is a pipeline bind
   EMIT32(cmdbuf, 0xffff8002 | (clear_pipeline_textures ? 0x210 : 0)); // 0x270
   EMIT32(cmdbuf, 0);
   EMIT64(cmdbuf, pipeline_clear | 0x4);
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0x12);
   EMIT64(cmdbuf, pipeline_store | 0x4);
   EMIT64(cmdbuf, scissor_ptr);
   EMIT64(cmdbuf, demo_zero(pool, 0x1000));
   EMIT64(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 48);

   EMIT64(cmdbuf, 4);
   EMIT64(cmdbuf, 0xc000);

   /* Note: making these smallers scissors polygons but not clear colour */
   EMIT32(cmdbuf, width);
   EMIT32(cmdbuf, height);
   EMIT64(cmdbuf, demo_zero(pool, 0x8000));

   EMIT_ZERO_WORDS(cmdbuf, 48);

   float depth_clear = 1.0;
   uint8_t stencil_clear = 0;

   EMIT64(cmdbuf, 0); // 0x450
   EMIT32(cmdbuf, fui(depth_clear));
   EMIT32(cmdbuf, (0x3 << 8) | stencil_clear);
   EMIT64(cmdbuf, 0);
   EMIT64(cmdbuf, 0x1000000);
   EMIT32(cmdbuf, 0xffffffff);
   EMIT32(cmdbuf, 0xffffffff);
   EMIT32(cmdbuf, 0xffffffff);
   EMIT32(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 8);

   EMIT64(cmdbuf, 0); // 0x4a0
   EMIT32(cmdbuf, 0xffff8212);
   EMIT32(cmdbuf, 0);

   EMIT64(cmdbuf, pipeline_null | 0x4);
   EMIT64(cmdbuf, 0);

   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0x12);
   EMIT32(cmdbuf, pipeline_store | 0x4);
   EMIT32(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 44);

   EMIT64(cmdbuf, 1); // 0x580
   EMIT64(cmdbuf, 0);
   EMIT_ZERO_WORDS(cmdbuf, 4);

   /* Compare compute case ,which has a bit of reordering, but we can swap */
   EMIT32(cmdbuf, 0x1c); // 0x5a0
   EMIT32(cmdbuf, 0);
   EMIT64(cmdbuf, 0xCAFECAFE); // encoder ID XXX: don't fix
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0xffffffff);

   // remark: opposite order for compute, but we can swap the orders
   EMIT32(cmdbuf, 1);
   EMIT32(cmdbuf, 0);
   EMIT64(cmdbuf, 0);
   EMIT64(cmdbuf, demo_unk6(pool));

   /* note: width/height act like scissor, but changing the 0s doesn't
    * seem to affect (maybe scissor enable bit missing), _and this affects
    * the clear_ .. bbox maybe */
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, width); // can increase up to 16384
   EMIT32(cmdbuf, height);

   EMIT32(cmdbuf, 1);
   EMIT32(cmdbuf, 8);
   EMIT32(cmdbuf, 8);
   EMIT32(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 12);

   EMIT32(cmdbuf, 0); // 0x620
   EMIT32(cmdbuf, 8);
   EMIT32(cmdbuf, 0x20);
   EMIT32(cmdbuf, 0x20);
   EMIT32(cmdbuf, 0x1);
   EMIT32(cmdbuf, 0);
   EMIT64(cmdbuf, 0);

   EMIT_ZERO_WORDS(cmdbuf, 58);

   unsigned offset_unk = (cmdbuf->offset * 4);
   EMIT_ZERO_WORDS(cmdbuf, 12);

   unsigned offset_attachments = (cmdbuf->offset * 4);
   EMIT32(cmdbuf, 0); // 0x758
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0);
   EMIT32(cmdbuf, 0x1); // number of attachments (includes depth/stencil) stored to

   /* A single attachment follows, depth/stencil have their own attachments */
   agx_pack((cmdbuf->map + cmdbuf->offset), IOGPU_ATTACHMENT, cfg) {
      cfg.address = rt0;
      cfg.type = AGX_IOGPU_ATTACHMENT_TYPE_COLOUR;
      cfg.unk_1 = 0x80000000;
      cfg.unk_2 = 0x5;
      cfg.bytes_per_pixel = 4;
      cfg.percent = 100;
   }

   cmdbuf->offset += (AGX_IOGPU_ATTACHMENT_LENGTH / 4);

   unsigned total_size = (cmdbuf->offset * 4);

   cmdbuf->map[0] = 0x10000;
   cmdbuf->map[1] = total_size;
   cmdbuf->map[2] = 7;
   cmdbuf->map[8] = offset_attachments;
   cmdbuf->map[9] = 0x18;
   cmdbuf->map[10] = offset_attachments;
   cmdbuf->map[11] = offset_unk;

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
      .nr_entries_1 = count + 1,
      .nr_entries_2 = count + 1,
      .unka = 0x0b,
   };
}

void
demo_mem_map(void *map, size_t size, unsigned *handles, unsigned count,
             uint64_t cmdbuf_id, uint64_t encoder_id, unsigned cmdbuf_size)
{
   struct agx_map_header *header = map;
   struct agx_map_entry *entries = (struct agx_map_entry *) (((uint8_t *) map) + 0x40);
   struct agx_map_entry *end = (struct agx_map_entry *) (((uint8_t *) map) + size);

   /* Header precedes the entry */
   *header = demo_map_header(cmdbuf_id, encoder_id, cmdbuf_size, count);

   /* Add an entry for each BO mapped */
   for (unsigned i = 0; i < count; ++i) {
	   assert((entries + i) < end);
      entries[i] = (struct agx_map_entry) {
         .unkAAA = 0x20,
         .unkBBB = 0x1,
         .unka = 0x1ffff,
         .index = handles[i]
      };
   }

   /* Final entry is a sentinel */
   assert((entries + count) < end);
   entries[count] = (struct agx_map_entry) {
      .unkAAA = 0x40,
      .unkBBB = 0x1,
      .unka = 0x1ffff,
      .index = 0
   };
}

#define AGX_STOP \
	0x88, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, \
	0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00 \

#define AGX_BLEND \
	0x09, 0x00, 0x00, 0x04, 0xf0, 0xfc, 0x80, 0x03

/* Clears the tilebuffer, where u6-u7 are preloaded with the FP16 clear colour

   0: 7e018c098040         bitop_mov        r0, u6
   6: 7e058e098000         bitop_mov        r1, u7
   c: 09000004f0fc8003     TODO.blend
   */

uint8_t shader_clear[] = {
   0x7e, 0x01, 0x8c, 0x09, 0x80, 0x40,
   0x7e, 0x05, 0x8e, 0x09, 0x80, 0x00,
   AGX_BLEND,
   AGX_STOP
};

uint8_t shader_store[] = {
   0x7e, 0x00, 0x04, 0x09, 0x80, 0x00,
   0xb1, 0x80, 0x00, 0x80, 0x00, 0x4a, 0x00, 0x00, 0x0a, 0x00,
   AGX_STOP
};

void
agx_internal_shaders(struct agx_device *dev)
{
   unsigned clear_offset = 0;
   unsigned store_offset = 1024;

   struct agx_bo *bo = agx_bo_create(dev, 4096, AGX_MEMORY_TYPE_SHADER);
   memcpy(((uint8_t *) bo->ptr.cpu) + clear_offset, shader_clear, sizeof(shader_clear));
   memcpy(((uint8_t *) bo->ptr.cpu) + store_offset, shader_store, sizeof(shader_store));

   dev->internal.bo = bo;
   dev->internal.clear = bo->ptr.gpu + clear_offset;
   dev->internal.store = bo->ptr.gpu + store_offset;

   agx_build_reload_shader(dev);
}

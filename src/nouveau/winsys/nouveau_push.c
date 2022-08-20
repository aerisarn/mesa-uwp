#include "nouveau_push.h"

#include <errno.h>
#include <inttypes.h>
#include <nouveau_drm.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "nouveau_bo.h"
#include "nouveau_context.h"

#include "nvtypes.h"
#include "nvk_cl9097.h"
#include "nvk_cl902d.h"
#include "nvk_cl90b5.h"
#include "nvk_cla097.h"
#include "nvk_cla0b5.h"
#include "nvk_cla0c0.h"
#include "nvk_clb197.h"
#include "nvk_clc0c0.h"
#include "nvk_clc1b5.h"
#include "nvk_clc397.h"
#include "nvk_clc3c0.h"
#include "nvk_clc597.h"

struct nouveau_ws_push*
nouveau_ws_push_new(struct nouveau_ws_device *dev, uint64_t size)
{
   uint32_t flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;

   struct nouveau_ws_bo *bo = nouveau_ws_bo_new(dev, size, 0, flags);
   if (!bo)
      goto fail_bo;

   void *map = nouveau_ws_bo_map(bo, NOUVEAU_WS_BO_RDWR);
   if (!map)
      goto fail_map;

   struct nouveau_ws_push *push = CALLOC_STRUCT(nouveau_ws_push);
   if (!push)
      goto fail_alloc;

   push->bo = bo;
   push->map = map;
   push->orig_map = map;
   push->end = map + size;

   struct nouveau_ws_push_bo push_bo;
   push_bo.bo = bo;
   push_bo.flags = NOUVEAU_WS_BO_RD;

   util_dynarray_init(&push->bos, NULL);

   return push;

fail_alloc:
   nouveau_ws_bo_unmap(bo, map);
fail_map:
   nouveau_ws_bo_destroy(bo);
fail_bo:
   return NULL;
}

void
nouveau_ws_push_init_cpu(struct nouveau_ws_push *push,
                         void *data, size_t size_bytes)
{
   push->map = data;
   push->orig_map = push->map;
   push->end = push->map + (size_bytes / sizeof(uint32_t));

   util_dynarray_init(&push->bos, NULL);
}

void
nouveau_ws_push_destroy(struct nouveau_ws_push *push)
{
   util_dynarray_fini(&push->bos);
   if (push->bo) {
      nouveau_ws_bo_unmap(push->bo, push->orig_map);
      nouveau_ws_bo_destroy(push->bo);
   }
}

void
nouveau_ws_push_append(struct nouveau_ws_push *push,
                       const struct nouveau_ws_push *other)
{
   /* We only use this for CPU pushes. */
   assert(other->bo == NULL);

   /* We don't support BO refs for now */
   assert(other->bos.size == 0);

   size_t count = other->map - other->orig_map;

   assert(push->map + count <= push->end);

   memcpy(push->map, other->orig_map, count * sizeof(*push->map));
   push->map += count;
   push->last_size = NULL;
}

static void
nouveau_ws_push_valid(struct nouveau_ws_push *push) {
   uint32_t *cur = push->orig_map;

   /* submitting empty push buffers is probably a bug */
   assert(push->map != push->orig_map);

   /* make sure we don't overrun the bo */
   assert(push->map <= push->end);

   /* parse all the headers to see if we get to push->map */
   while (cur < push->map) {
      uint32_t hdr = *cur;
      uint32_t mthd = hdr >> 29;

      switch (mthd) {
      /* immd */
      case 4:
         break;
      case 1:
      case 3:
      case 5: {
         uint32_t count = (hdr >> 16) & 0x1fff;
         assert(count);
         cur += count;
         break;
      }
      default:
         assert(!"unknown method found");
      }

      cur++;
      assert(cur <= push->map);
   }
}

static void
nouveau_ws_push_dump(struct nouveau_ws_push *push, struct nouveau_ws_context *ctx)
{
   uint32_t *cur = push->orig_map;

   while (cur < push->map) {
      uint32_t hdr = *cur;
      uint32_t type = hdr >> 29;
      uint32_t inc;
      uint32_t count = (hdr >> 16) & 0x1fff;
      uint32_t subchan = (hdr >> 13) & 0x7;
      uint32_t mthd = (hdr & 0xfff) << 2;
      uint32_t value = 0;
      bool is_immd = false;

      printf("[0x%08" PRIxPTR "] HDR %x subch %i", cur - push->orig_map, hdr, subchan);
      cur++;

      switch (type) {
      case 4:
         printf(" IMMD\n");
         inc = 0;
         is_immd = true;
         value = count;
         count = 1;
         break;
      case 1:
         printf(" NINC\n");
         inc = count;
         break;
      case 3:
         printf(" 0INC\n");
         inc = 0;
         break;
      case 5:
         printf(" 1INC\n");
         inc = 1;
         break;
      }

      while (count--) {
         const char *mthd_name = "";
         switch (subchan) {
         case 0:
            if (ctx->compute.cls >= 0xc597)
               mthd_name = P_PARSE_NVC597_MTHD(mthd);
            else if (ctx->compute.cls >= 0xc397)
               mthd_name = P_PARSE_NVC397_MTHD(mthd);
            else if (ctx->compute.cls >= 0xb197)
               mthd_name = P_PARSE_NVB197_MTHD(mthd);
            else if (ctx->compute.cls >= 0xa097)
               mthd_name = P_PARSE_NVA097_MTHD(mthd);
            else
               mthd_name = P_PARSE_NV9097_MTHD(mthd);
            break;
         case 1:
            if (ctx->compute.cls >= 0xc3c0)
               mthd_name = P_PARSE_NVC3C0_MTHD(mthd);
            else if (ctx->compute.cls >= 0xc0c0)
               mthd_name = P_PARSE_NVC0C0_MTHD(mthd);
            else
               mthd_name = P_PARSE_NVA0C0_MTHD(mthd);
            break;
         case 3:
            mthd_name = P_PARSE_NV902D_MTHD(mthd);
            break;
         case 4:
            if (ctx->copy.cls >= 0xc1b5)
               mthd_name = P_PARSE_NVC1B5_MTHD(mthd);
            else if (ctx->copy.cls >= 0xa0b5)
               mthd_name = P_PARSE_NVA0B5_MTHD(mthd);
            else
               mthd_name = P_PARSE_NV90B5_MTHD(mthd);
            break;
         default:
            mthd_name = "";
            break;
         }

         if (!is_immd)
            value = *cur;

         printf("\tmthd %04x %s\n", mthd, mthd_name);
         switch (subchan) {
         case 0:
            if (ctx->compute.cls >= 0xc597)
               P_DUMP_NVC597_MTHD_DATA(mthd, value, "\t\t");
            else if (ctx->compute.cls >= 0xc397)
               P_DUMP_NVC397_MTHD_DATA(mthd, value, "\t\t");
            else if (ctx->compute.cls >= 0xb197)
               P_DUMP_NVB197_MTHD_DATA(mthd, value, "\t\t");
            else if (ctx->compute.cls >= 0xa097)
               P_DUMP_NVA097_MTHD_DATA(mthd, value, "\t\t");
            else
               P_DUMP_NV9097_MTHD_DATA(mthd, value, "\t\t");
            break;
         case 1:
            if (ctx->compute.cls >= 0xc3c0)
               P_DUMP_NVC3C0_MTHD_DATA(mthd, value, "\t\t");
            else if (ctx->compute.cls >= 0xc0c0)
               P_DUMP_NVC0C0_MTHD_DATA(mthd, value, "\t\t");
            else
               P_DUMP_NVA0C0_MTHD_DATA(mthd, value, "\t\t");
            break;
         case 3:
            P_DUMP_NV902D_MTHD_DATA(mthd, value, "\t\t");
            break;
         case 4:
            if (ctx->copy.cls >= 0xc1b5)
               P_DUMP_NVC1B5_MTHD_DATA(mthd, value, "\t\t");
            else if (ctx->copy.cls >= 0xa0b5)
               P_DUMP_NVA0B5_MTHD_DATA(mthd, value, "\t\t");
            else
               P_DUMP_NV90B5_MTHD_DATA(mthd, value, "\t\t");
            break;
         default:
            mthd_name = "";
            break;
         }

         if (!is_immd)
            cur++;

         if (inc) {
            inc--;
            mthd += 4;
         }
      }

      printf("\n");
   }
}

int
nouveau_ws_push_submit(
   struct nouveau_ws_push *push,
   struct nouveau_ws_device *dev,
   struct nouveau_ws_context *ctx
) {
   struct drm_nouveau_gem_pushbuf_bo req_bo[NOUVEAU_GEM_MAX_BUFFERS] = {};
   struct drm_nouveau_gem_pushbuf req = {};
   struct drm_nouveau_gem_pushbuf_push req_push = {};

   /* Can't submit a CPU push */
   assert(push->bo);

   if (push->map == push->orig_map)
      return 0;

   /* make sure we don't submit nonsense */
   nouveau_ws_push_valid(push);

   int i = 0;
   util_dynarray_foreach(&push->bos, struct nouveau_ws_push_bo, push_bo) {
      struct nouveau_ws_bo *bo = push_bo->bo;
      enum nouveau_ws_bo_map_flags flags = push_bo->flags;

      req_bo[i].handle = bo->handle;

      if (flags & NOUVEAU_WS_BO_RD) {
         if (bo->flags & NOUVEAU_WS_BO_GART) {
            req_bo[i].valid_domains |= NOUVEAU_GEM_DOMAIN_GART;
            req_bo[i].read_domains |= NOUVEAU_GEM_DOMAIN_GART;
         } else {
            req_bo[i].valid_domains |= dev->local_mem_domain;
            req_bo[i].read_domains |= dev->local_mem_domain;
         }
      }

      if (flags & NOUVEAU_WS_BO_WR) {
         if (bo->flags & NOUVEAU_WS_BO_GART) {
            req_bo[i].valid_domains |= NOUVEAU_GEM_DOMAIN_GART;
            req_bo[i].write_domains |= NOUVEAU_GEM_DOMAIN_GART;
         } else {
            req_bo[i].valid_domains |= dev->local_mem_domain;
            req_bo[i].write_domains |= dev->local_mem_domain;
         }
      }

      i++;
   }

   req_bo[i].handle = push->bo->handle;
   req_bo[i].valid_domains |= NOUVEAU_GEM_DOMAIN_GART;
   req_bo[i].read_domains |= NOUVEAU_GEM_DOMAIN_GART;

   req_push.bo_index = i;
   req_push.offset = 0;
   req_push.length = (push->map - push->orig_map) * 4;

   req.channel = ctx->channel;
   req.nr_buffers = i;
   req.buffers = (uintptr_t)&req_bo;
   req.nr_push = 1;
   req.push = (uintptr_t)&req_push;

   if (dev->debug_flags & NVK_DEBUG_PUSH_SYNC)
      req.vram_available |= NOUVEAU_GEM_PUSHBUF_SYNC;

   int ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_GEM_PUSHBUF, &req, sizeof(req));

   if ((ret && (dev->debug_flags & NVK_DEBUG_PUSH_SYNC)) || dev->debug_flags & NVK_DEBUG_PUSH_DUMP) {
      printf("DRM_NOUVEAU_GEM_PUSHBUF returned %i, dumping pushbuffer\n", ret);
      nouveau_ws_push_dump(push, ctx);
   }

   /* TODO: later we want to report that the channel is gone, but for now just assert */
   assert(ret != -ENODEV);

   return ret;
}

void
nouveau_ws_push_ref(
   struct nouveau_ws_push *push,
   struct nouveau_ws_bo *bo,
   enum nouveau_ws_bo_map_flags flags
) {
   util_dynarray_foreach(&push->bos, struct nouveau_ws_push_bo, push_bo) {
      if (push_bo->bo == bo) {
         push_bo->flags |= flags;
         return;
      }
   }

   struct nouveau_ws_push_bo push_bo;
   push_bo.bo = bo;
   push_bo.flags = flags;

   util_dynarray_append(&push->bos, struct nouveau_ws_push_bo, push_bo);
}

void nouveau_ws_push_reset(struct nouveau_ws_push *push)
{
   util_dynarray_clear(&push->bos);
   push->map = push->orig_map;
}

unsigned nouveau_ws_push_num_refs(const struct nouveau_ws_push *push)
{
   return util_dynarray_num_elements(&push->bos, struct nouveau_ws_push_bo);
}

void nouveau_ws_push_reset_refs(struct nouveau_ws_push *push,
                                unsigned num_refs)
{
   assert(num_refs <= nouveau_ws_push_num_refs(push));
   util_dynarray_resize(&push->bos, struct nouveau_ws_push_bo, num_refs);
}

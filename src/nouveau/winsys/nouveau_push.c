#include "nouveau_push.h"

#include <errno.h>
#include <inttypes.h>
#include <nouveau_drm.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "nouveau_bo.h"
#include "nouveau_context.h"

#include "nv_device_info.h"

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

   struct nouveau_ws_push_buffer push_buf;
   push_buf.bo = bo;
   nv_push_init(&push_buf.push, map, 0);

   util_dynarray_init(&push->bos, NULL);
   util_dynarray_init(&push->pushs, NULL);
   util_dynarray_append(&push->pushs, struct nouveau_ws_push_buffer, push_buf);
   push->dev = dev;

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
   struct nouveau_ws_push_buffer push_buf;
   push_buf.bo = NULL;
   nv_push_init(&push_buf.push, data, size_bytes / sizeof(uint32_t));

   util_dynarray_init(&push->bos, NULL);
   util_dynarray_init(&push->pushs, NULL);
   util_dynarray_append(&push->pushs, struct nouveau_ws_push_buffer, push_buf);
}

void
nouveau_ws_push_destroy(struct nouveau_ws_push *push)
{
   util_dynarray_foreach(&push->pushs, struct nouveau_ws_push_buffer, buf) {
      nouveau_ws_bo_unmap(buf->bo, buf->push.start);
      nouveau_ws_bo_destroy(buf->bo);
   }

   util_dynarray_fini(&push->bos);
   util_dynarray_fini(&push->pushs);
}

struct nv_push *
nouveau_ws_push_space(struct nouveau_ws_push *push,
                      uint32_t count)
{
   struct nouveau_ws_push_buffer *buf = _nouveau_ws_push_top(push);

   if (!count)
      return &buf->push;

   if (buf->push.end + count < buf->push.start + (buf->bo->size / 4)) {
      buf->push.limit = buf->push.end + count;
      return &buf->push;
   }

   uint32_t flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
   uint32_t size = buf->bo->size;

   assert(count <= size / 4);

   struct nouveau_ws_bo *bo = nouveau_ws_bo_new(push->dev, size, 0, flags);
   if (!bo)
      return NULL;

   uint32_t *map = nouveau_ws_bo_map(bo, NOUVEAU_WS_BO_RDWR);
   if (!map)
      goto fail_map;

   struct nouveau_ws_push_buffer push_buf;
   push_buf.bo = bo;
   nv_push_init(&push_buf.push, map, count);

   util_dynarray_append(&push->pushs, struct nouveau_ws_push_buffer, push_buf);
   return &_nouveau_ws_push_top(push)->push;

fail_map:
   nouveau_ws_bo_destroy(bo);
   return NULL;
}

int
nouveau_ws_push_append(struct nouveau_ws_push *push,
                       const struct nouveau_ws_push *other)
{
   struct nouveau_ws_push_buffer *other_buf = _nouveau_ws_push_top(other);
   size_t count = nv_push_dw_count(&other_buf->push);

   struct nv_push *p = P_SPACE(push, count);
   if (!p)
      return -ENOMEM;

   /* We only use this for CPU pushes. */
   assert(other_buf->bo == NULL);

   /* We don't support BO refs for now */
   assert(other->bos.size == 0);

   memcpy(p->end, other_buf->push.start, count * sizeof(*p->end));
   p->end += count;
   p->last_size = NULL;

   return 0;
}

static void
nouveau_ws_push_valid(struct nouveau_ws_push *push) {
   util_dynarray_foreach(&push->pushs, struct nouveau_ws_push_buffer, buf)
      nv_push_validate(&buf->push);
}

static void
nouveau_ws_push_dump(struct nouveau_ws_push *push, struct nouveau_ws_context *ctx)
{
   struct nv_device_info devinfo = {
      .cls_copy = ctx->copy.cls,
      .cls_eng2d = ctx->eng2d.cls,
      .cls_eng3d = ctx->eng3d.cls,
      .cls_m2mf = ctx->m2mf.cls,
      .cls_compute = ctx->compute.cls,
   };
   util_dynarray_foreach(&push->pushs, struct nouveau_ws_push_buffer, buf)
      vk_push_print(stdout, &buf->push, &devinfo);
}

int
nouveau_ws_push_submit(
   struct nouveau_ws_push *push,
   struct nouveau_ws_device *dev,
   struct nouveau_ws_context *ctx
) {
   struct drm_nouveau_gem_pushbuf_bo req_bo[NOUVEAU_GEM_MAX_BUFFERS] = {};
   struct drm_nouveau_gem_pushbuf_push req_push[NOUVEAU_GEM_MAX_PUSH] = {};
   struct drm_nouveau_gem_pushbuf req = {};

   /* make sure we don't submit nonsense */
   nouveau_ws_push_valid(push);

   int i = 0;
   util_dynarray_foreach(&push->pushs, struct nouveau_ws_push_buffer, buf) {
      /* Can't submit a CPU push */
      assert(buf->bo);

      if (buf->push.end == buf->push.start)
         continue;

      req_bo[i].handle = buf->bo->handle;
      req_bo[i].valid_domains |= NOUVEAU_GEM_DOMAIN_GART;
      req_bo[i].read_domains |= NOUVEAU_GEM_DOMAIN_GART;

      req_push[i].bo_index = i;
      req_push[i].offset = 0;
      req_push[i].length = nv_push_dw_count(&buf->push) * 4;

      i++;
   }

   if (i == 0)
      return 0;

   uint32_t pushs = i;
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

   req.channel = ctx->channel;
   req.nr_buffers = i;
   req.buffers = (uintptr_t)&req_bo;
   req.nr_push = pushs;
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
   bool first = true;
   util_dynarray_foreach(&push->pushs, struct nouveau_ws_push_buffer, buf) {
      if (first) {
         buf->push.end = buf->push.start;
         first = false;
         continue;
      }

      nouveau_ws_bo_unmap(buf->bo, buf->push.start);
      nouveau_ws_bo_destroy(buf->bo);
   }

   util_dynarray_clear(&push->bos);
   ASSERTED void *result = util_dynarray_resize(&push->pushs, struct nouveau_ws_push_buffer, 1);
   /* A push always has at least 1 BO, so this couldn't have tried to resize and failed. */
   assert(result != NULL);
}

unsigned nouveau_ws_push_num_refs(const struct nouveau_ws_push *push)
{
   return util_dynarray_num_elements(&push->bos, struct nouveau_ws_push_bo);
}

void nouveau_ws_push_reset_refs(struct nouveau_ws_push *push,
                                unsigned num_refs)
{
   assert(num_refs <= nouveau_ws_push_num_refs(push));
   ASSERTED void *result = util_dynarray_resize(&push->bos, struct nouveau_ws_push_bo, num_refs);
   assert(result != NULL); /* We checked above that we wouldn't hit the resize path */
}

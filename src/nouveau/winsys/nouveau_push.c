#include "nouveau_push.h"

#include <nouveau_drm.h>
#include <nouveau/nouveau.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "nouveau_bo.h"
#include "nouveau_context.h"

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
   util_dynarray_append(&push->bos, struct nouveau_ws_push_bo, push_bo);

   return push;

fail_alloc:
   munmap(map, bo->size);
fail_map:
   nouveau_ws_bo_destroy(bo);
fail_bo:
   return NULL;
}

void
nouveau_ws_push_destroy(struct nouveau_ws_push *push)
{
   util_dynarray_fini(&push->bos);
   munmap(push->orig_map, push->bo->size);
   nouveau_ws_bo_destroy(push->bo);
}

int
nouveau_ws_push_submit(
   struct nouveau_ws_push *push,
   struct nouveau_ws_device *dev,
   struct nouveau_ws_context *ctx
) {
   struct nouveau_ws_device_priv *pdev = nouveau_ws_device(dev);
   struct nouveau_fifo *fifo = ctx->channel->data;

   struct drm_nouveau_gem_pushbuf_bo req_bo[NOUVEAU_GEM_MAX_BUFFERS] = {};
   struct drm_nouveau_gem_pushbuf req = {};
   struct drm_nouveau_gem_pushbuf_push req_push = {};

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
            req_bo[i].valid_domains |= pdev->local_mem_domain;
            req_bo[i].read_domains |= pdev->local_mem_domain;
         }
      }

      if (flags & NOUVEAU_WS_BO_WR) {
         if (bo->flags & NOUVEAU_WS_BO_GART) {
            req_bo[i].valid_domains |= NOUVEAU_GEM_DOMAIN_GART;
            req_bo[i].write_domains |= NOUVEAU_GEM_DOMAIN_GART;
         } else {
            req_bo[i].valid_domains |= pdev->local_mem_domain;
            req_bo[i].write_domains |= pdev->local_mem_domain;
         }
      }

      i++;
   }

   req_push.bo_index = 0;
   req_push.offset = 0;
   req_push.length = (push->map - push->orig_map) * 4;

   req.channel = fifo->channel;
   req.nr_buffers = i;
   req.buffers = (uintptr_t)&req_bo;
   req.nr_push = 1;
   req.push = (uintptr_t)&req_push;

   return drmCommandWriteRead(pdev->fd, DRM_NOUVEAU_GEM_PUSHBUF, &req, sizeof(req));
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

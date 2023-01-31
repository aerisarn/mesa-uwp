#include "nouveau_bo.h"

#include <nouveau_drm.h>
#include <xf86drm.h>

#include <stddef.h>
#include <sys/mman.h>

struct nouveau_ws_bo *
nouveau_ws_bo_new(struct nouveau_ws_device *dev,
                  uint64_t size, uint64_t align,
                  enum nouveau_ws_bo_flags flags)
{
   return nouveau_ws_bo_new_tiled(dev, size, align, 0, 0, flags);
}

struct nouveau_ws_bo *
nouveau_ws_bo_new_tiled(struct nouveau_ws_device *dev,
                        uint64_t size, uint64_t align,
                        uint8_t pte_kind, uint16_t tile_mode,
                        enum nouveau_ws_bo_flags flags)
{
   struct nouveau_ws_bo *bo = CALLOC_STRUCT(nouveau_ws_bo);
   struct drm_nouveau_gem_new req = {};

   /* if the caller doesn't care, use the GPU page size */
   if (align == 0)
      align = 0x1000;

   req.info.domain = NOUVEAU_GEM_TILE_NONCONTIG;
   if (flags & NOUVEAU_WS_BO_GART)
      req.info.domain |= NOUVEAU_GEM_DOMAIN_GART;
   else
      req.info.domain |= dev->local_mem_domain;

   if (flags & NOUVEAU_WS_BO_MAP)
      req.info.domain |= NOUVEAU_GEM_DOMAIN_MAPPABLE;

   assert(pte_kind == 0 || !(flags & NOUVEAU_WS_BO_GART));
   assert(tile_mode == 0 || !(flags & NOUVEAU_WS_BO_GART));
   req.info.tile_flags = (uint32_t)pte_kind << 8;
   req.info.tile_mode = tile_mode;

   req.info.size = size;
   req.align = align;

   int ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_GEM_NEW, &req, sizeof(req));
   if (ret) {
      FREE(bo);
      return NULL;
   }

   bo->size = req.info.size;
   bo->offset = req.info.offset;
   bo->handle = req.info.handle;
   bo->map_handle = req.info.map_handle;
   bo->fd = dev->fd;
   bo->flags = flags;
   bo->refcnt = 1;

   return bo;
}

void
nouveau_ws_bo_destroy(struct nouveau_ws_bo *bo)
{
   if (--bo->refcnt)
      return;

   drmCloseBufferHandle(bo->fd, bo->handle);
   FREE(bo);
}

void *
nouveau_ws_bo_map(struct nouveau_ws_bo *bo, enum nouveau_ws_bo_map_flags flags)
{
   size_t prot = 0;

   if (flags & NOUVEAU_WS_BO_RD)
      prot |= PROT_READ;
   if (flags & NOUVEAU_WS_BO_WR)
      prot |= PROT_WRITE;

   void *res = mmap64(NULL, bo->size, prot, MAP_SHARED, bo->fd, bo->map_handle);
   if (res == MAP_FAILED)
      return NULL;

   return res;
}

bool
nouveau_ws_bo_wait(struct nouveau_ws_bo *bo, enum nouveau_ws_bo_map_flags flags)
{
   struct drm_nouveau_gem_cpu_prep req = {};

   req.handle = bo->handle;
   if (flags & NOUVEAU_WS_BO_WR)
      req.flags |= NOUVEAU_GEM_CPU_PREP_WRITE;

   return !drmCommandWrite(bo->fd, DRM_NOUVEAU_GEM_CPU_PREP, &req, sizeof(req));
}

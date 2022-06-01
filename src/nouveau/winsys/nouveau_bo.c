#include "nouveau_bo.h"

#include <nouveau/nouveau.h>
#include <nouveau_drm.h>
#include <xf86drm.h>

#include <stddef.h>
#include <sys/mman.h>

struct nouveau_ws_bo *
nouveau_ws_bo_new(struct nouveau_ws_device *dev, uint64_t size, uint64_t align, enum nouveau_ws_bo_flags flags)
{
   struct nouveau_ws_device_priv *pdev = nouveau_ws_device(dev);
   struct nouveau_ws_bo *bo = CALLOC_STRUCT(nouveau_ws_bo);
   struct drm_nouveau_gem_new req = {};

   /* if the caller doesn't care, use the GPU page size */
   if (align == 0)
      align = 0x1000;

   req.info.domain = NOUVEAU_GEM_TILE_NONCONTIG;
   if (flags & NOUVEAU_WS_BO_GART)
      req.info.domain |= NOUVEAU_GEM_DOMAIN_GART;
   else
      req.info.domain |= pdev->local_mem_domain;

   if (flags & NOUVEAU_WS_BO_MAP)
      req.info.domain |= NOUVEAU_GEM_DOMAIN_MAPPABLE;

   req.info.size = size;
   req.align = align;

   int ret = drmCommandWriteRead(pdev->fd, DRM_NOUVEAU_GEM_NEW, &req, sizeof(req));
   if (ret) {
      FREE(bo);
      return NULL;
   }

   bo->size = req.info.size;
   bo->offset = req.info.offset;
   bo->handle = req.info.handle;
   bo->map_handle = req.info.map_handle;
   bo->fd = pdev->fd;
   bo->flags = flags;

   return bo;
}

void
nouveau_ws_bo_destroy(struct nouveau_ws_bo *bo)
{
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

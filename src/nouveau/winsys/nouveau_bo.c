#include "nouveau_bo.h"

#include "drm-uapi/nouveau_drm.h"
#include "util/hash_table.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <xf86drm.h>

struct nouveau_ws_bo *
nouveau_ws_bo_new(struct nouveau_ws_device *dev,
                  uint64_t size, uint64_t align,
                  enum nouveau_ws_bo_flags flags)
{
   return nouveau_ws_bo_new_tiled(dev, size, align, 0, 0, flags);
}

struct nouveau_ws_bo *
nouveau_ws_bo_new_mapped(struct nouveau_ws_device *dev,
                         uint64_t size, uint64_t align,
                         enum nouveau_ws_bo_flags flags,
                         enum nouveau_ws_bo_map_flags map_flags,
                         void **map_out)
{
   struct nouveau_ws_bo *bo = nouveau_ws_bo_new(dev, size, align,
                                                flags | NOUVEAU_WS_BO_MAP);
   if (!bo)
      return NULL;

   void *map = nouveau_ws_bo_map(bo, map_flags);
   if (map == NULL) {
      nouveau_ws_bo_destroy(bo);
      return NULL;
   }

   *map_out = map;
   return bo;
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

   simple_mtx_lock(&dev->bos_lock);

   int ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_GEM_NEW, &req, sizeof(req));
   if (ret == 0) {
      bo->size = req.info.size;
      bo->offset = req.info.offset;
      bo->handle = req.info.handle;
      bo->map_handle = req.info.map_handle;
      bo->dev = dev;
      bo->flags = flags;
      bo->refcnt = 1;

      _mesa_hash_table_insert(dev->bos, (void *)(uintptr_t)bo->handle, bo);
   } else {
      FREE(bo);
      bo = NULL;
   }

   simple_mtx_unlock(&dev->bos_lock);

   return bo;
}

struct nouveau_ws_bo *
nouveau_ws_bo_from_dma_buf(struct nouveau_ws_device *dev, int fd)
{
   struct nouveau_ws_bo *bo = NULL;

   simple_mtx_lock(&dev->bos_lock);

   uint32_t handle;
   int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
   if (ret == 0) {
      struct hash_entry *entry =
         _mesa_hash_table_search(dev->bos, (void *)(uintptr_t)handle);
      if (entry != NULL) {
         bo = entry->data;
      } else {
         struct drm_nouveau_gem_info info = {
            .handle = handle
         };
         ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_GEM_INFO,
                                   &info, sizeof(info));
         if (ret == 0) {
            enum nouveau_ws_bo_flags flags = 0;
            if (info.domain & NOUVEAU_GEM_DOMAIN_GART)
               flags |= NOUVEAU_WS_BO_GART;
            if (info.map_handle)
               flags |= NOUVEAU_WS_BO_MAP;

            bo = CALLOC_STRUCT(nouveau_ws_bo);
            bo->size = info.size;
            bo->offset = info.offset;
            bo->handle = info.handle;
            bo->map_handle = info.map_handle;
            bo->dev = dev;
            bo->flags = flags;
            bo->refcnt = 1;

            _mesa_hash_table_insert(dev->bos, (void *)(uintptr_t)handle, bo);
         }
      }
   }

   simple_mtx_unlock(&dev->bos_lock);

   return bo;
}

void
nouveau_ws_bo_destroy(struct nouveau_ws_bo *bo)
{
   if (--bo->refcnt)
      return;

   struct nouveau_ws_device *dev = bo->dev;

   simple_mtx_lock(&dev->bos_lock);

   _mesa_hash_table_remove_key(dev->bos, (void *)(uintptr_t)bo->handle);
   drmCloseBufferHandle(bo->dev->fd, bo->handle);
   FREE(bo);

   simple_mtx_unlock(&dev->bos_lock);
}

void *
nouveau_ws_bo_map(struct nouveau_ws_bo *bo, enum nouveau_ws_bo_map_flags flags)
{
   size_t prot = 0;

   if (flags & NOUVEAU_WS_BO_RD)
      prot |= PROT_READ;
   if (flags & NOUVEAU_WS_BO_WR)
      prot |= PROT_WRITE;

   void *res = mmap64(NULL, bo->size, prot, MAP_SHARED, bo->dev->fd, bo->map_handle);
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

   return !drmCommandWrite(bo->dev->fd, DRM_NOUVEAU_GEM_CPU_PREP, &req, sizeof(req));
}

int
nouveau_ws_bo_dma_buf(struct nouveau_ws_bo *bo, int *fd)
{
   return drmPrimeHandleToFD(bo->dev->fd, bo->handle, DRM_CLOEXEC, fd);
}

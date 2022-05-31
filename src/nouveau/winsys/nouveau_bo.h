#ifndef NOUVEAU_BO
#define NOUVEAU_BO 1

#include "nouveau_private.h"

#include "nouveau_device.h"

enum nouveau_ws_bo_flags {
   /* vram or gart depending on GPU */
   NOUVEAU_WS_BO_LOCAL = 0 << 0,
   NOUVEAU_WS_BO_GART  = 1 << 0,
   NOUVEAU_WS_BO_MAP   = 1 << 1,
};

enum nouveau_ws_bo_map_flags {
   NOUVEAU_WS_BO_RD   = 1 << 0,
   NOUVEAU_WS_BO_WR   = 1 << 1,
   NOUVEAU_WS_BO_RDWR = NOUVEAU_WS_BO_RD | NOUVEAU_WS_BO_WR,
};

struct nouveau_ws_bo {
   uint64_t size;
   uint64_t offset;
   uint64_t map_handle;
   int fd;
   uint32_t handle;
   enum nouveau_ws_bo_flags flags;
   _Atomic uint32_t refcnt;
};

struct nouveau_ws_bo *nouveau_ws_bo_new(struct nouveau_ws_device *, uint64_t size, uint64_t align, enum nouveau_ws_bo_flags);
void nouveau_ws_bo_destroy(struct nouveau_ws_bo *);
void *nouveau_ws_bo_map(struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags);
bool nouveau_ws_bo_wait(struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags flags);

static inline void
nouveau_ws_bo_ref(struct nouveau_ws_bo *bo)
{
   bo->refcnt++;
}

#endif

#ifndef NOUVEAU_PUSH
#define NOUVEAU_PUSH 1

#include "nouveau_private.h"

#include "nouveau_bo.h"
#include "nv_push.h"
#include "util/u_dynarray.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nouveau_ws_context;
struct nouveau_ws_device;

struct nouveau_ws_push_bo {
   struct nouveau_ws_bo *bo;
   enum nouveau_ws_bo_map_flags flags;
};

struct nouveau_ws_push_buffer {
   struct nouveau_ws_bo *bo;
   struct nv_push push;
};

struct nouveau_ws_push {
   /* struct nouveau_ws_push_bo */
   struct util_dynarray bos;

   /* struct nouveau_ws_push_buffer */
   struct util_dynarray pushs;

   struct nouveau_ws_device *dev;
};

struct nouveau_ws_push *nouveau_ws_push_new(struct nouveau_ws_device *, uint64_t size);
void nouveau_ws_push_init_cpu(struct nouveau_ws_push *push,
                              void *data, size_t size_bytes);
void nouveau_ws_push_destroy(struct nouveau_ws_push *);
int nouveau_ws_push_append(struct nouveau_ws_push *,
                            const struct nouveau_ws_push *);
struct nv_push *
nouveau_ws_push_space(struct nouveau_ws_push *, uint32_t size);
int nouveau_ws_push_submit(struct nouveau_ws_push *, struct nouveau_ws_device *, struct nouveau_ws_context *);
void nouveau_ws_push_ref(struct nouveau_ws_push *, struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags);
void nouveau_ws_push_reset(struct nouveau_ws_push *);
unsigned nouveau_ws_push_num_refs(const struct nouveau_ws_push *push);
void nouveau_ws_push_reset_refs(struct nouveau_ws_push *push,
                                unsigned num_refs);

static inline struct nouveau_ws_push_buffer *
_nouveau_ws_push_top(const struct nouveau_ws_push *push)
{
   return util_dynarray_top_ptr(&push->pushs, struct nouveau_ws_push_buffer);
}

static inline struct nv_push *
P_SPACE(struct nouveau_ws_push *push, uint32_t size)
{
   return nouveau_ws_push_space(push, size);
}

#ifdef __cplusplus
}
#endif

#endif /* NOUVEAU_PUSH */

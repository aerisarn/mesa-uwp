#ifndef NOUVEAU_CONTEXT
#define NOUVEAU_CONTEXT 1

#include "nouveau_private.h"

struct nouveau_ws_device;

struct nouveau_ws_context {
   struct nouveau_object *channel;
};

int nouveau_ws_context_create(struct nouveau_ws_device *, struct nouveau_ws_context **out);
void nouveau_ws_context_destroy(struct nouveau_ws_context *);

#endif

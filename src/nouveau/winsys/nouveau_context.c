#include "nouveau_context.h"

#include <errno.h>
#include <nouveau/nouveau.h>

#include "nouveau_device.h"

#include "nvtypes.h"
#include "classes/cl902d.h"

int
nouveau_ws_context_create(struct nouveau_ws_device *dev, struct nouveau_ws_context **out)
{
   struct nouveau_ws_device_priv *pdev = nouveau_ws_device(dev);
   struct nvc0_fifo nvc0_data = { };
   uint32_t size = sizeof(nvc0_data);

   *out = CALLOC_STRUCT(nouveau_ws_context);
   if (!*out)
      return -ENOMEM;

   int ret = nouveau_object_new(&pdev->dev->object, 0, NOUVEAU_FIFO_CHANNEL_CLASS,
                                &nvc0_data, size, &(*out)->channel);
   if (ret)
      goto fail_chan;

   ret = nouveau_object_new((*out)->channel, 0xbeef902d, FERMI_TWOD_A, NULL, 0,
                          &(*out)->eng2d);
   if (ret)
      goto fail_2d;

   return 0;

fail_2d:
   nouveau_object_del(&(*out)->channel);
fail_chan:
   FREE(*out);
   return ret;
}

void
nouveau_ws_context_destroy(struct nouveau_ws_context *context)
{
   nouveau_object_del(&context->eng2d);
   nouveau_object_del(&context->channel);
   FREE(context);
}

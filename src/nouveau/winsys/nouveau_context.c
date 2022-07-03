#include "nouveau_context.h"

#include <drm/drm.h>
#include <errno.h>
#include <nouveau_drm.h>
#include <nouveau/nvif/ioctl.h>
#include <xf86drm.h>

#include "nouveau_device.h"

#include "nvtypes.h"
#include "classes/cl902d.h"
#include "classes/clc5c0.h"

static void
nouveau_ws_subchan_dealloc(int fd, struct nouveau_ws_object *obj)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_del del;
   } args = {
      .ioctl = {
         .object = (uintptr_t)obj,
         .owner = NVIF_IOCTL_V0_OWNER_ANY,
         .route = 0x00,
         .type = NVIF_IOCTL_V0_DEL,
         .version = 0,
      },
   };

   /* TODO returns -ENOENT for unknown reasons */
   drmCommandWrite(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
}

static int
nouveau_ws_subchan_alloc(int fd, int channel, uint32_t handle, uint16_t oclass, struct nouveau_ws_object *obj)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_new_v0 new;
   } args = {
      .ioctl = {
         .route = 0xff,
         .token = channel,
         .type = NVIF_IOCTL_V0_NEW,
         .version = 0,
      },
      .new = {
         .handle = handle,
         .object = (uintptr_t)obj,
         .oclass = oclass,
         .route = NVIF_IOCTL_V0_ROUTE_NVIF,
         .token = (uintptr_t)obj,
         .version = 0,
      },
   };

   return drmCommandWrite(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
}

static void
nouveau_ws_channel_dealloc(int fd, int channel)
{
   struct drm_nouveau_channel_free req = {
      .channel = channel,
   };

   int ret = drmCommandWrite(fd, DRM_NOUVEAU_CHANNEL_FREE, &req, sizeof(req));
   assert(!ret);
}

int
nouveau_ws_context_create(struct nouveau_ws_device *dev, struct nouveau_ws_context **out)
{
   struct drm_nouveau_channel_alloc req = { };

   *out = CALLOC_STRUCT(nouveau_ws_context);
   if (!*out)
      return -ENOMEM;

   int ret = drmCommandWriteRead(dev->fd, DRM_NOUVEAU_CHANNEL_ALLOC, &req, sizeof(req));
   if (ret)
      goto fail_chan;

   ret = nouveau_ws_subchan_alloc(dev->fd, req.channel, 0xbeef902d, FERMI_TWOD_A, &(*out)->eng2d);
   if (ret)
      goto fail_2d;

   uint32_t obj_class = 0xa140;//NVF0_P2MF_CLASS;
   ret = nouveau_ws_subchan_alloc(dev->fd, req.channel, 0xbeef323f, obj_class, &(*out)->m2mf);
   if (ret)
      goto fail_subchan;

   obj_class = TURING_COMPUTE_A;
   ret = nouveau_ws_subchan_alloc(dev->fd, req.channel, 0xbeef00c0, obj_class, &(*out)->compute);
   if (ret)
      goto fail_subchan;

   (*out)->channel = req.channel;
   (*out)->dev = dev;
   return 0;

fail_subchan:
   nouveau_ws_subchan_dealloc(dev->fd, &(*out)->compute);
   nouveau_ws_subchan_dealloc(dev->fd, &(*out)->m2mf);
   nouveau_ws_subchan_dealloc(dev->fd, &(*out)->eng2d);
fail_2d:
   nouveau_ws_channel_dealloc(dev->fd, req.channel);
fail_chan:
   FREE(*out);
   return ret;
}

void
nouveau_ws_context_destroy(struct nouveau_ws_context *context)
{
   nouveau_ws_subchan_dealloc(context->dev->fd, &context->compute);
   nouveau_ws_subchan_dealloc(context->dev->fd, &context->m2mf);
   nouveau_ws_subchan_dealloc(context->dev->fd, &context->eng2d);
   nouveau_ws_channel_dealloc(context->dev->fd, context->channel);
   FREE(context);
}

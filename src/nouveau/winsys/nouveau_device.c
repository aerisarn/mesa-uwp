#include "nouveau_device.h"

#include <drm/drm.h>
#include <nouveau/nouveau.h>
#include <nouveau_drm.h>
#include <nvif/cl0080.h>
#include <nvif/class.h>

#include "util/os_file.h"
#include "util/os_misc.h"

struct nouveau_ws_device *
nouveau_ws_device_new(int fd)
{
   struct nouveau_ws_device_priv *device = CALLOC_STRUCT(nouveau_ws_device_priv);
   uint64_t device_id = 0;
   struct nouveau_drm *drm;
   struct nouveau_device *dev;
   int dup_fd = os_dupfd_cloexec(fd);

   if (nouveau_drm_new(dup_fd, &drm)) {
      return NULL;
   }

   struct nv_device_v0 v0 = {
      .device = ~0ULL,
   };

   if (nouveau_device_new(&drm->client, NV_DEVICE, &v0, sizeof(v0), &dev)) {
      goto out_drm;
   }

   if (nouveau_getparam(dev, NOUVEAU_GETPARAM_PCI_DEVICE, &device_id)) {
      goto out_dev;
   }

   device->base.vendor_id = 0x10de;
   device->base.device_id = device_id;
   device->base.chipset = dev->chipset;
   device->base.vram_size = dev->vram_size;
   os_get_available_system_memory(&device->base.gart_size);
   device->base.gart_size = MIN2(device->base.gart_size, dev->gart_size);
   device->base.is_integrated = dev->vram_size == 0;
   device->drm = drm;
   device->dev = dev;
   device->fd = dup_fd;

   return &device->base;

out_dev:
   nouveau_device_del(&dev);
out_drm:
   nouveau_drm_del(&drm);
   close(dup_fd);
   return NULL;
}

void
nouveau_ws_device_destroy(struct nouveau_ws_device *device)
{
   if (!device)
      return;

   struct nouveau_ws_device_priv *priv = nouveau_ws_device(device);

   nouveau_device_del(&priv->dev);
   nouveau_drm_del(&priv->drm);
   close(priv->fd);

   FREE(priv);
}

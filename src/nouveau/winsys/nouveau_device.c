#include "nouveau_device.h"

#include <drm/drm.h>
#include <nouveau/nouveau.h>
#include <nouveau_drm.h>
#include <nvif/cl0080.h>
#include <nvif/class.h>

#include "util/os_file.h"
#include "util/os_misc.h"

static uint8_t
sm_for_chipset(uint16_t chipset)
{
   if (chipset >= 0x180)
      return 0x90;
   else if (chipset == 0x17b)
      return 0x87;
   else if (chipset >= 0x172)
      return 0x86;
   else if (chipset >= 0x170)
      return 0x80;
   else if (chipset >= 0x160)
      return 0x75;
   else if (chipset >= 0x14b)
      return 0x72;
   else if (chipset >= 0x140)
      return 0x70;
   else if (chipset >= 0x13b)
      return 0x62;
   else if (chipset >= 0x132)
      return 0x61;
   else if (chipset >= 0x130)
      return 0x60;
   else if (chipset >= 0x12b)
      return 0x53;
   else if (chipset >= 0x120)
      return 0x52;
   else if (chipset >= 0x110)
      return 0x50;
   // TODO: 0x37
   else if (chipset >= 0x0f0)
      return 0x35;
   else if (chipset >= 0x0ea)
      return 0x32;
   else if (chipset >= 0x0e0)
      return 0x30;
   // GF110 is SM20
   else if (chipset == 0x0c8)
      return 0x20;
   else if (chipset >= 0x0c1)
      return 0x21;
   else if (chipset >= 0x0c0)
      return 0x20;
   else if (chipset >= 0x0a3)
      return 0x12;
   // GT200 is SM13
   else if (chipset >= 0x0a0)
      return 0x13;
   else if (chipset >= 0x080)
      return 0x11;
   // this has to be == because 0x63 is older than 0x50 and has no compute
   else if (chipset == 0x050)
      return 0x10;
   // no compute
   return 0x00;
}

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
   device->base.sm = sm_for_chipset(dev->chipset);
   device->base.vram_size = dev->vram_size;
   os_get_available_system_memory(&device->base.gart_size);
   device->base.gart_size = MIN2(device->base.gart_size, dev->gart_size);
   device->base.is_integrated = dev->vram_size == 0;
   device->drm = drm;
   device->dev = dev;
   device->fd = dup_fd;

   if (dev->vram_size == 0)
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_GART;
   else
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_VRAM;

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

#include "nouveau_device.h"

#include <drm/drm.h>
#include <nouveau_drm.h>
#include <nouveau/nvif/ioctl.h>
#include <nvif/cl0080.h>
#include <nvif/class.h>
#include <xf86drm.h>

#include "util/u_debug.h"
#include "util/os_file.h"
#include "util/os_misc.h"

static uint8_t
cls_for_chipset(uint16_t chipset)
{
   if (chipset >= 0x180)
      assert(!"unknown chipset");
   else if (chipset == 0x17b)
      return 0xc7;
   else if (chipset >= 0x172)
      return 0xc6;
   else if (chipset >= 0x170)
      return 0xc5;
   else if (chipset >= 0x160)
      return 0xc4;
   else if (chipset >= 0x140)
      return 0xc3;
   else if (chipset >= 0x132)
      return 0xc1;
   else if (chipset >= 0x130)
      return 0xc0;
   else if (chipset >= 0x120)
      return 0xb1;
   else if (chipset >= 0x110)
      return 0xb0;
   else if (chipset >= 0x0f0)
      return 0xa2;
   else if (chipset >= 0x0ea)
      return 0xa1;
   else if (chipset >= 0x0e0)
      return 0xa0;
   // GF110 is like GF100
   else if (chipset == 0x0c8)
      return 0x90;
   else if (chipset >= 0x0c1)
      return 0x91;
   else if (chipset >= 0x0c0)
      return 0x90;
   else if (chipset >= 0x0a3)
      return 0x85;
   // GT200 is special
   else if (chipset >= 0x0a0)
      return 0x86;
   else if (chipset >= 0x082)
      return 0x82;
   // this has to be == because 0x63 is older than 0x50
   else if (chipset == 0x050)
      return 0x50;
   else if (chipset >= 0x044)
      return 0x44;
   else if (chipset >= 0x040)
      return 0x40;
   else if (chipset >= 0x036)
      return 0x36;
   else if (chipset >= 0x020)
      return 0x20;
   return 0x00;
}

static uint8_t
sm_for_cls(uint8_t cls, uint16_t chipset)
{
   switch (cls) {
   case 0xc7:
      return 0x87;
   case 0xc6:
      return 0x86;
   case 0xc5:
      return 0x80;
   case 0xc4:
      return 0x75;
   case 0xc3:
      return chipset >= 0x14b ? 0x72 : 0x70;
   case 0xc1:
      // TODO maybe that's 0xc2?
      return chipset >= 0x13b ? 0x62 : 0x61;
   case 0xc0:
      return 0x60;
   case 0xb1:
      // TODO is there a 0xb2?
      return chipset >= 0x12b ? 0x53 : 0x52;
   case 0xb0:
      return 0x50;
   case 0xa2:
      return 0x35;
   case 0xa1:
      return 0x31;
   case 0xa0:
      return 0x30;
   case 0x91:
      return 0x21;
   case 0x90:
      return 0x20;
   case 0x86:
      return 0x13;
   case 0x85:
      return 0x12;
   case 0x82:
      return 0x11;
   case 0x50:
      return 0x10;
   default:
      return 0;
   }
}

static void
nouveau_ws_device_set_dbg_flags(struct nouveau_ws_device *dev)
{
   const struct debug_control flags[] = {
      { "push_dump", NVK_DEBUG_PUSH_DUMP },
      { "push_sync", NVK_DEBUG_PUSH_SYNC },
   };

   dev->debug_flags = parse_debug_string(getenv("NVK_DEBUG"), flags);
}

static int
nouveau_ws_param(int fd, uint64_t param, uint64_t *value)
{
   struct drm_nouveau_getparam data = { .param = param };

   int ret = drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM, &data, sizeof(data));
   if (ret)
      return ret;

   *value = data.value;
   return 0;
}

static int
nouveau_ws_device_alloc(int fd, struct nouveau_ws_device *dev)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_new_v0 new;
      struct nv_device_v0 dev;
   } args = {
      .ioctl = {
         .object = 0,
         .owner = NVIF_IOCTL_V0_OWNER_ANY,
         .route = 0x00,
         .type = NVIF_IOCTL_V0_NEW,
         .version = 0,
      },
      .new = {
         .handle = 0,
         .object = (uintptr_t)dev,
         .oclass = NV_DEVICE,
         .route = NVIF_IOCTL_V0_ROUTE_NVIF,
         .token = (uintptr_t)dev,
         .version = 0,
      },
      .dev = {
         .device = ~0ULL,
      },
   };

   return drmCommandWrite(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
}

static int
nouveau_ws_device_info(int fd, struct nouveau_ws_device *dev)
{
   struct {
      struct nvif_ioctl_v0 ioctl;
      struct nvif_ioctl_mthd_v0 mthd;
      struct nv_device_info_v0 info;
   } args = {
      .ioctl = {
         .object = (uintptr_t)dev,
         .owner = NVIF_IOCTL_V0_OWNER_ANY,
         .route = 0x00,
         .type = NVIF_IOCTL_V0_MTHD,
         .version = 0,
      },
      .mthd = {
         .method = NV_DEVICE_V0_INFO,
         .version = 0,
      },
      .info = {
         .version = 0,
      },
   };

   int ret = drmCommandWriteRead(fd, DRM_NOUVEAU_NVIF, &args, sizeof(args));
   if (ret)
      return ret;

   dev->chipset = args.info.chipset;
   dev->vram_size = args.info.ram_user;

   switch (args.info.platform) {
   case NV_DEVICE_INFO_V0_IGP:
      dev->device_type = NOUVEAU_WS_DEVICE_TYPE_IGP;
      break;
   case NV_DEVICE_INFO_V0_SOC:
      dev->device_type = NOUVEAU_WS_DEVICE_TYPE_SOC;
      break;
   case NV_DEVICE_INFO_V0_PCI:
   case NV_DEVICE_INFO_V0_AGP:
   case NV_DEVICE_INFO_V0_PCIE:
   default:
      dev->device_type = NOUVEAU_WS_DEVICE_TYPE_DIS;
      break;
   }

   dev->chipset_name = strndup(args.info.chip, sizeof(args.info.chip));
   dev->device_name = strndup(args.info.name, sizeof(args.info.name));

   return 0;
}

struct nouveau_ws_device *
nouveau_ws_device_new(int fd)
{
   struct nouveau_ws_device *device = CALLOC_STRUCT(nouveau_ws_device);
   uint64_t value = 0;
   int dup_fd = os_dupfd_cloexec(fd);
   drmVersionPtr ver;

   ver = drmGetVersion(dup_fd);
   if (!ver)
      return NULL;

   uint32_t version =
      ver->version_major << 24 |
      ver->version_minor << 8  |
      ver->version_patchlevel;
   drmFreeVersion(ver);

   if (version < 0x01000301)
      goto out_drm;

   if (nouveau_ws_device_alloc(dup_fd, device))
      goto out_drm;

   if (nouveau_ws_device_info(dup_fd, device))
      goto out_drm;

   if (nouveau_ws_param(dup_fd, NOUVEAU_GETPARAM_PCI_DEVICE, &value))
      goto out_dev;
   device->device_id = value;

   if (nouveau_ws_param(dup_fd, NOUVEAU_GETPARAM_AGP_SIZE, &value))
      goto out_dev;
   os_get_available_system_memory(&device->gart_size);
   device->gart_size = MIN2(device->gart_size, value);

   device->fd = dup_fd;
   device->vendor_id = 0x10de;
   device->cls = cls_for_chipset(device->chipset);
   device->cm = sm_for_cls(device->cls, device->chipset);
   device->is_integrated = device->vram_size == 0;

   if (device->vram_size == 0)
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_GART;
   else
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_VRAM;

   if (nouveau_ws_param(dup_fd, NOUVEAU_GETPARAM_GRAPH_UNITS, &value))
      goto out_dev;
   device->gpc_count = value & 0x000000ff;
   device->mp_count = value >> 8;

   nouveau_ws_device_set_dbg_flags(device);

   return device;

out_dev:
   FREE(device);
out_drm:
   close(dup_fd);
   return NULL;
}

void
nouveau_ws_device_destroy(struct nouveau_ws_device *device)
{
   if (!device)
      return;

   close(device->fd);
   FREE(device->chipset_name);
   FREE(device->device_name);
   FREE(device);
}

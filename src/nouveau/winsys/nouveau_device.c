#include "nouveau_device.h"

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <nouveau_drm.h>
#include <nouveau/nvif/ioctl.h>
#include <nvif/cl0080.h>
#include <nvif/class.h>
#include <util/hash_table.h>

#include "nouveau_context.h"

#include "util/u_debug.h"
#include "util/os_file.h"
#include "util/os_misc.h"

static uint8_t
sm_for_chipset(uint16_t chipset)
{
   if (chipset >= 0x190)
      return 89;
   // GH100 is older than AD10X, but is SM90
   else if (chipset >= 0x180)
      return 90;
   else if (chipset == 0x17b)
      return 87;
   else if (chipset >= 0x172)
      return 86;
   else if (chipset >= 0x170)
      return 80;
   else if (chipset >= 0x160)
      return 75;
   else if (chipset >= 0x14b)
      return 72;
   else if (chipset >= 0x140)
      return 70;
   else if (chipset >= 0x13b)
      return 62;
   else if (chipset >= 0x132)
      return 61;
   else if (chipset >= 0x130)
      return 60;
   else if (chipset >= 0x12b)
      return 53;
   else if (chipset >= 0x120)
      return 52;
   else if (chipset >= 0x110)
      return 50;
   // TODO: 37
   else if (chipset >= 0x0f0)
      return 35;
   else if (chipset >= 0x0ea)
      return 32;
   else if (chipset >= 0x0e0)
      return 30;
   // GF110 is SM20
   else if (chipset == 0x0c8)
      return 20;
   else if (chipset >= 0x0c1)
      return 21;
   else if (chipset >= 0x0c0)
      return 20;
   else if (chipset >= 0x0a3)
      return 12;
   // GT200 is SM13
   else if (chipset >= 0x0a0)
      return 13;
   else if (chipset >= 0x080)
      return 11;
   // this has to be == because 0x63 is older than 0x50 and has no compute
   else if (chipset == 0x050)
      return 10;
   // no compute
   return 0x00;
}

static void
nouveau_ws_device_set_dbg_flags(struct nouveau_ws_device *dev)
{
   const struct debug_control flags[] = {
      { "push_dump", NVK_DEBUG_PUSH_DUMP },
      { "push_sync", NVK_DEBUG_PUSH_SYNC },
      { "zero_memory", NVK_DEBUG_ZERO_MEMORY },
      { NULL, 0 },
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
      dev->info.type = NV_DEVICE_TYPE_IGP;
      break;
   case NV_DEVICE_INFO_V0_SOC:
      dev->info.type = NV_DEVICE_TYPE_SOC;
      break;
   case NV_DEVICE_INFO_V0_PCI:
   case NV_DEVICE_INFO_V0_AGP:
   case NV_DEVICE_INFO_V0_PCIE:
   default:
      dev->info.type = NV_DEVICE_TYPE_DIS;
      break;
   }

   dev->chipset_name = strndup(args.info.chip, sizeof(args.info.chip));
   dev->device_name = strndup(args.info.name, sizeof(args.info.name));

   return 0;
}

struct nouveau_ws_device *
nouveau_ws_device_new(drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   struct nouveau_ws_device *device = CALLOC_STRUCT(nouveau_ws_device);
   uint64_t value = 0;
   drmVersionPtr ver = NULL;

   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      goto out_open;

   ver = drmGetVersion(fd);
   if (!ver)
      goto out_err;

   if (strncmp("nouveau", ver->name, ver->name_len) != 0) {
      fprintf(stderr,
              "DRM kernel driver '%.*s' in use. NVK requires nouveau.\n",
              ver->name_len, ver->name);
      goto out_err;
   }

   uint32_t version =
      ver->version_major << 24 |
      ver->version_minor << 8  |
      ver->version_patchlevel;
   drmFreeVersion(ver);
   ver = NULL;

   if (version < 0x01000301)
      goto out_err;

   if (nouveau_ws_device_alloc(fd, device))
      goto out_err;

   if (nouveau_ws_device_info(fd, device))
      goto out_err;

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_PCI_DEVICE, &value))
      goto out_err;
   device->device_id = value;

   device->info.device_id = value;
   if (drm_device->bustype == DRM_BUS_PCI) {
      assert(device->info.type == NV_DEVICE_TYPE_DIS);
      assert(device->info.device_id == drm_device->deviceinfo.pci->device_id);

      device->info.pci.domain       = drm_device->businfo.pci->domain;
      device->info.pci.bus          = drm_device->businfo.pci->bus;
      device->info.pci.dev          = drm_device->businfo.pci->dev;
      device->info.pci.func         = drm_device->businfo.pci->func;
      device->info.pci.revision_id  = drm_device->deviceinfo.pci->revision_id;
   };

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_AGP_SIZE, &value))
      goto out_err;
   os_get_available_system_memory(&device->gart_size);
   device->gart_size = MIN2(device->gart_size, value);

   device->fd = fd;
   device->vendor_id = 0x10de;
   device->sm = sm_for_chipset(device->chipset);
   device->is_integrated = device->vram_size == 0;

   if (device->vram_size == 0)
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_GART;
   else
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_VRAM;

   if (nouveau_ws_param(fd, NOUVEAU_GETPARAM_GRAPH_UNITS, &value))
      goto out_err;
   device->gpc_count = value & 0x000000ff;
   device->mp_count = value >> 8;

   nouveau_ws_device_set_dbg_flags(device);

   struct nouveau_ws_context *tmp_ctx;
   if (nouveau_ws_context_create(device, &tmp_ctx))
      goto out_err;

   device->info.cls_copy = tmp_ctx->copy.cls;
   device->info.cls_eng2d = tmp_ctx->eng2d.cls;
   device->info.cls_eng3d = tmp_ctx->eng3d.cls;
   device->info.cls_m2mf = tmp_ctx->m2mf.cls;
   device->info.cls_compute = tmp_ctx->compute.cls;

   nouveau_ws_context_destroy(tmp_ctx);

   simple_mtx_init(&device->bos_lock, mtx_plain);
   device->bos = _mesa_pointer_hash_table_create(NULL);

   return device;

out_err:
   if (ver)
      drmFreeVersion(ver);
out_open:
   FREE(device);
   close(fd);
   return NULL;
}

void
nouveau_ws_device_destroy(struct nouveau_ws_device *device)
{
   if (!device)
      return;

   _mesa_hash_table_destroy(device->bos, NULL);
   simple_mtx_destroy(&device->bos_lock);
   close(device->fd);
   FREE(device->chipset_name);
   FREE(device->device_name);
   FREE(device);
}

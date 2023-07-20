#ifndef NV_DEVINFO_H
#define NV_DEVINFO_H

#include "util/macros.h"

#define NVIDIA_VENDOR_ID 0x10de

enum PACKED nv_device_type {
   NV_DEVICE_TYPE_IGP,
   NV_DEVICE_TYPE_DIS,
   NV_DEVICE_TYPE_SOC,
};

struct nv_device_info {
   enum nv_device_type type;

   uint16_t device_id;

   /* Populated if type == NV_DEVICE_TYPE_DIS */
   struct {
      uint16_t domain;
      uint8_t bus;
      uint8_t dev;
      uint8_t func;
      uint8_t revision_id;
   } pci;

   uint16_t cls_copy;
   uint16_t cls_eng2d;
   uint16_t cls_eng3d;
   uint16_t cls_m2mf;
   uint16_t cls_compute;
};

#endif /* NV_DEVINFO_H */

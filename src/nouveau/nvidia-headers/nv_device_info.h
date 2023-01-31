#ifndef NV_DEVINFO_H
#define NV_DEVINFO_H

enum nv_device_type {
   NV_DEVICE_TYPE_IGP,
   NV_DEVICE_TYPE_DIS,
   NV_DEVICE_TYPE_SOC,
};

struct nv_device_info {
   uint16_t pci_domain;
   uint8_t pci_bus;
   uint8_t pci_dev;
   uint8_t pci_func;
   uint16_t pci_device_id;
   uint8_t pci_revision_id;

   enum nv_device_type type;

   uint16_t cls_copy;
   uint16_t cls_eng2d;
   uint16_t cls_eng3d;
   uint16_t cls_m2mf;
   uint16_t cls_compute;
};

#endif /* NV_DEVINFO_H */

#ifndef NOUVEAU_DEVICE
#define NOUVEAU_DEVICE 1

#include "nouveau_private.h"

struct nouveau_ws_device {
   uint16_t vendor_id;
   uint16_t device_id;
   uint32_t chipset;
   uint64_t vram_size;
   uint64_t gart_size;
   bool is_integrated;
};

struct nouveau_ws_device *nouveau_ws_device_new(int fd);
void nouveau_ws_device_destroy(struct nouveau_ws_device *);

#endif

#ifndef NVK_WSI_H
#define NVK_WSI_H 1

#include "nvk_physical_device.h"

VkResult nvk_init_wsi(struct nvk_physical_device *physical_device);
void nvk_finish_wsi(struct nvk_physical_device *physical_device);

#endif

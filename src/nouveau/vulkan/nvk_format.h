#ifndef NVK_FORMAT_H
#define NVK_FORMAT_H 1

#include "nvk_private.h"

bool nvk_is_storage_image_format(VkFormat vk_format);

struct nvk_format {
   VkFormat vk_format;
   uint8_t hw_format;

   bool supports_2d_blit:1;
};

const struct nvk_format *nvk_get_format(VkFormat vk_format);

#endif

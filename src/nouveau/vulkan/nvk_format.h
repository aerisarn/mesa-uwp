#ifndef NVK_FORMAT_H
#define NVK_FORMAT_H 1

#include "nvk_private.h"

struct nvk_format {
   VkFormat vk_format;
   uint8_t hw_format;

   bool supports_2d_blit:1;
};

#define NVK_FORMATS 27
extern struct nvk_format nvk_formats[NVK_FORMATS];

#endif

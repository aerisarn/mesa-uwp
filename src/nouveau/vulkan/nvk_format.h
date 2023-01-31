#ifndef NVK_FORMAT_H
#define NVK_FORMAT_H 1

#include "nvk_private.h"

struct nvk_format {
   VkFormat vk_format;
   uint8_t hw_format;

   bool supports_2d_blit:1;
};

const struct nvk_format *nvk_get_format(VkFormat vk_format);

struct nvk_tic_format {
   unsigned comp_sizes:8;
   unsigned type_r:3;
   unsigned type_g:3;
   unsigned type_b:3;
   unsigned type_a:3;
   unsigned src_x:3;
   unsigned src_y:3;
   unsigned src_z:3;
   unsigned src_w:3;
};

extern const struct nvk_tic_format pipe_to_nvk_tic_format[];

#endif

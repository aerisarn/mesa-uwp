#ifndef NVK_DESCRIPTOR_SET
#define NVK_DESCRIPTOR_SET 1

#include "nvk_private.h"

struct nvk_image_descriptor {
  unsigned image_index:20;
  unsigned sampler_index:12;
};

/* This has to match nir_address_format_64bit_bounded_global */
struct nvk_buffer_address {
  uint64_t base_addr;
  uint32_t size;
  uint32_t zero; /* Must be zero! */
};

#endif

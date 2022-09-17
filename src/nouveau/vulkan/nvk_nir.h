#ifndef NVK_NIR
#define NVK_NIR 1

#include "compiler/nir/nir.h"

bool nvk_nir_lower_descriptors(nir_shader *nir,
                               const struct vk_pipeline_layout *layout,
                               bool robust_buffer_access);

#endif

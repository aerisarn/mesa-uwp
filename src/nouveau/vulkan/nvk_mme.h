#ifndef NVK_MME_H
#define NVK_MME_H 1

#include "mme_builder.h"

struct nvk_device;

enum nvk_mme {
   NVK_MME_COUNT,
};

typedef void (*nvk_mme_builder_func)(struct nvk_device *dev,
                                     struct mme_builder *b);

uint32_t *nvk_build_mme(struct nvk_device *dev, enum nvk_mme mme,
                        size_t *size_out);

#endif /* NVK_MME_H */

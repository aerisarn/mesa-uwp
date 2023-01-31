#include "nvk_mme.h"

#include "nvk_device.h"

static const nvk_mme_builder_func mme_builders[NVK_MME_COUNT] = {
   [NVK_MME_CLEAR_VIEWS]   = nvk_mme_clear_views,
   [NVK_MME_CLEAR_LAYERS]  = nvk_mme_clear_layers,
};

uint32_t *
nvk_build_mme(struct nvk_device *dev, enum nvk_mme mme, size_t *size_out)
{
   struct mme_builder b;
   mme_builder_init(&b);

   mme_builders[mme](dev, &b);

   return mme_builder_finish(&b, size_out);
}

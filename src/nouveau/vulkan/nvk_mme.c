#include "nvk_mme.h"

#include "nvk_private.h"

static const nvk_mme_builder_func mme_builders[NVK_MME_COUNT] = {
   [NVK_MME_CLEAR_VIEWS]            = nvk_mme_clear_views,
   [NVK_MME_CLEAR_LAYERS]           = nvk_mme_clear_layers,
   [NVK_MME_DRAW]                   = nvk_mme_draw,
   [NVK_MME_DRAW_INDEXED]           = nvk_mme_draw_indexed,
   [NVK_MME_DRAW_INDIRECT]          = nvk_mme_draw_indirect,
   [NVK_MME_DRAW_INDEXED_INDIRECT]  = nvk_mme_draw_indexed_indirect,
   [NVK_MME_ADD_CS_INVOCATIONS]     = nvk_mme_add_cs_invocations,
   [NVK_MME_DISPATCH_INDIRECT]      = nvk_mme_dispatch_indirect,
   [NVK_MME_WRITE_CS_INVOCATIONS]   = nvk_mme_write_cs_invocations,
   [NVK_MME_COPY_QUERIES]           = nvk_mme_copy_queries,
};

uint32_t *
nvk_build_mme(const struct nv_device_info *devinfo,
              enum nvk_mme mme, size_t *size_out)
{
   struct mme_builder b;
   mme_builder_init(&b, devinfo);

   mme_builders[mme](&b);

   return mme_builder_finish(&b, size_out);
}

void
nvk_test_build_all_mmes(const struct nv_device_info *devinfo)
{
   for (uint32_t mme = 0; mme < NVK_MME_COUNT; mme++) {
      size_t size;
      uint32_t *dw = nvk_build_mme(devinfo, mme, &size);
      assert(dw != NULL);
      free(dw);
   }
}

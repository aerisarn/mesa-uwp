#ifndef NVK_MME_H
#define NVK_MME_H 1

#include "mme_builder.h"

struct nv_device_info;

enum nvk_mme {
   NVK_MME_CLEAR_VIEWS,
   NVK_MME_CLEAR_LAYERS,
   NVK_MME_DRAW,
   NVK_MME_DRAW_INDEXED,
   NVK_MME_DRAW_INDIRECT,
   NVK_MME_DRAW_INDEXED_INDIRECT,
   NVK_MME_ADD_CS_INVOCATIONS,
   NVK_MME_DISPATCH_INDIRECT,
   NVK_MME_WRITE_CS_INVOCATIONS,
   NVK_MME_COPY_QUERIES,
   NVK_MME_COUNT,
};

enum nvk_mme_scratch {
   NVK_MME_SCRATCH_CS_INVOCATIONS_HI = 0,
   NVK_MME_SCRATCH_CS_INVOCATIONS_LO,

   /* Must be at the end */
   NVK_MME_NUM_SCRATCH,
};

typedef void (*nvk_mme_builder_func)(struct mme_builder *b);

uint32_t *nvk_build_mme(const struct nv_device_info *devinfo,
                        enum nvk_mme mme, size_t *size_out);

void nvk_test_build_all_mmes(const struct nv_device_info *devinfo);

void nvk_mme_clear_views(struct mme_builder *b);
void nvk_mme_clear_layers(struct mme_builder *b);
void nvk_mme_draw(struct mme_builder *b);
void nvk_mme_draw_indexed(struct mme_builder *b);
void nvk_mme_draw_indirect(struct mme_builder *b);
void nvk_mme_draw_indexed_indirect(struct mme_builder *b);
void nvk_mme_add_cs_invocations(struct mme_builder *b);
void nvk_mme_dispatch_indirect(struct mme_builder *b);
void nvk_mme_write_cs_invocations(struct mme_builder *b);
void nvk_mme_copy_queries(struct mme_builder *b);

#endif /* NVK_MME_H */

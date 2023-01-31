#ifndef NVK_DESCRIPTOR_TABLE_H
#define NVK_DESCRIPTOR_TABLE_H 1

#include "nvk_private.h"

#include "nouveau_bo.h"
#include "nouveau_push.h"
#include "util/simple_mtx.h"

struct nvk_device;

struct nvk_descriptor_table {
   simple_mtx_t mutex;

   uint32_t desc_size; /**< Size of a descriptor */
   uint32_t alloc; /**< Number of descriptors allocated */
   uint32_t next_desc; /**< Next unallocated descriptor */
   uint32_t free_count; /**< Size of free_table */

   struct nouveau_ws_bo *bo;
   void *map;

   /* Stack for free descriptor elements */
   uint32_t *free_table;
};

VkResult nvk_descriptor_table_init(struct nvk_device *device,
                                   struct nvk_descriptor_table *table,
                                   uint32_t descriptor_size,
                                   uint32_t min_descriptor_count,
                                   uint32_t max_descriptor_count);

void nvk_descriptor_table_finish(struct nvk_device *device,
                                 struct nvk_descriptor_table *table);

void *nvk_descriptor_table_alloc(struct nvk_device *device,
                                 struct nvk_descriptor_table *table,
                                 uint32_t *index_out);

void nvk_descriptor_table_free(struct nvk_device *device,
                               struct nvk_descriptor_table *table,
                               uint32_t index);

static inline void
nvk_push_descriptor_table_ref(struct nouveau_ws_push *push,
                              const struct nvk_descriptor_table *table)
{
   if (table->bo)
      nouveau_ws_push_ref(push, table->bo, NOUVEAU_WS_BO_RD);
}

static inline uint64_t
nvk_descriptor_table_base_address(const struct nvk_descriptor_table *table)
{
   return table->bo->offset;
}

#endif

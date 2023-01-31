#include "nvk_descriptor_table.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"

VkResult
nvk_descriptor_table_init(struct nvk_device *device,
                          struct nvk_descriptor_table *table,
                          uint32_t descriptor_size,
                          uint32_t min_descriptor_count,
                          uint32_t max_descriptor_count)
{
   struct nvk_physical_device *pdevice = nvk_device_physical(device);
   memset(table, 0, sizeof(*table));
   VkResult result;

   simple_mtx_init(&table->mutex, mtx_plain);

   /* TODO: Implement table growing.  This requires new uAPI */
   assert(min_descriptor_count == max_descriptor_count);

   table->desc_size = descriptor_size;
   table->alloc = min_descriptor_count;
   table->next_desc = 0;
   table->free_count = 0;

   const uint32_t bo_size = table->alloc * table->desc_size;
   table->bo = nouveau_ws_bo_new(pdevice->dev, bo_size, 256,
                                 NOUVEAU_WS_BO_LOCAL | NOUVEAU_WS_BO_MAP);
   if (table->bo == NULL) {
      result = vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to allocate the image descriptor table");
      goto fail;
   }

   table->map = nouveau_ws_bo_map(table->bo, NOUVEAU_WS_BO_WR);
   if (table->map == NULL) {
      result = vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Failed to map the image descriptor table");
      goto fail;
   }

   const size_t free_table_size = table->alloc * sizeof(uint32_t);
   table->free_table = vk_alloc(&device->vk.alloc, free_table_size, 4,
                                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (table->free_table == NULL) {
      result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                         "Failed to allocate image descriptor free table");
      goto fail;
   }

   return VK_SUCCESS;

fail:
   nvk_descriptor_table_finish(device, table);
   return result;
}

void
nvk_descriptor_table_finish(struct nvk_device *device,
                            struct nvk_descriptor_table *table)
{
   if (table->bo != NULL)
      nouveau_ws_bo_destroy(table->bo);
   vk_free(&device->vk.alloc, table->free_table);
   simple_mtx_destroy(&table->mutex);
}

#define NVK_IMAGE_DESC_INVALID

static void *
nvk_descriptor_table_alloc_locked(struct nvk_device *device,
                                  struct nvk_descriptor_table *table,
                                  uint32_t *index_out)
{
   if (table->free_count > 0) {
      *index_out = table->free_table[--table->free_count];
      return (char *)table->map + (*index_out * table->desc_size);
   }

   if (table->next_desc < table->alloc) {
      *index_out = table->next_desc++;
      return (char *)table->map + (*index_out * table->desc_size);
   }

   return NULL;
}

void *
nvk_descriptor_table_alloc(struct nvk_device *device,
                           struct nvk_descriptor_table *table,
                           uint32_t *index_out)
{
   simple_mtx_lock(&table->mutex);
   void *map = nvk_descriptor_table_alloc_locked(device, table, index_out);
   simple_mtx_unlock(&table->mutex);
   return map;
}

void
nvk_descriptor_table_free(struct nvk_device *device,
                          struct nvk_descriptor_table *table,
                          uint32_t index)
{
   simple_mtx_lock(&table->mutex);
   assert(table->free_count < table->alloc);
   for (uint32_t i = 0; i < table->free_count; i++)
      assert(table->free_table[i] != index);
   table->free_table[table->free_count++] = index;
   simple_mtx_unlock(&table->mutex);
}

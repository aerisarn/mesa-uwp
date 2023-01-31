#include "nvk_descriptor_set.h"

#include "nvk_buffer.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_image_view.h"
#include "nvk_physical_device.h"
#include "nvk_sampler.h"
#include "nouveau_bo.h"

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static void *
desc_ubo_data(struct nvk_descriptor_set *set, uint32_t binding,
              uint32_t elem)
{
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];

   return (char *)set->mapped_ptr + binding_layout->offset +
          elem * binding_layout->stride;
}

static void
write_sampler_desc(struct nvk_descriptor_set *set,
                   const VkDescriptorImageInfo *const info,
                   uint32_t binding, uint32_t elem)
{
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];

   if (binding_layout->immutable_samplers)
      return;

   VK_FROM_HANDLE(nvk_sampler, sampler, info->sampler);

   struct nvk_image_descriptor *desc = desc_ubo_data(set, binding, elem);
   assert(sampler->desc_index < (1 << 12));
   desc->sampler_index = sampler->desc_index;
}

static void
write_image_view_desc(struct nvk_descriptor_set *set,
                      const VkDescriptorImageInfo *const info,
                      uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_image_view, view, info->imageView);

   struct nvk_image_descriptor *desc = desc_ubo_data(set, binding, elem);
   assert(view->desc_index < (1 << 20));
   desc->image_index = view->desc_index;
}

static void
write_buffer_desc(struct nvk_descriptor_set *set,
                  const VkDescriptorBufferInfo *const info,
                  uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);

   struct nvk_buffer_address *desc = desc_ubo_data(set, binding, elem);
   *desc = (struct nvk_buffer_address){
      .base_addr = nvk_buffer_address(buffer) + info->offset,
      .size = vk_buffer_range(&buffer->vk, info->offset, info->range),
   };
}

static void
write_buffer_view_desc(struct nvk_descriptor_set *set,
                       const VkBufferView bufferView,
                       uint32_t binding, uint32_t elem)
{
   /* TODO */
}

static void
write_inline_uniform_data(struct nvk_descriptor_set *set,
                          const VkWriteDescriptorSetInlineUniformBlock *info,
                          uint32_t binding, uint32_t offset)
{
   memcpy((char *)desc_ubo_data(set, binding, 0) + offset, info->pData,
          info->dataSize);
}

VKAPI_ATTR void VKAPI_CALL
nvk_UpdateDescriptorSets(VkDevice device,
                         uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   for (uint32_t w = 0; w < descriptorWriteCount; w++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
      VK_FROM_HANDLE(nvk_descriptor_set, set, write->dstSet);

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_sampler_desc(set, write->pImageInfo + j, write->dstBinding,
                               write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_sampler_desc(set, write->pImageInfo + j, write->dstBinding,
                               write->dstArrayElement + j);
            write_image_view_desc(set, write->pImageInfo + j, write->dstBinding,
                                  write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_image_view_desc(set, write->pImageInfo + j, write->dstBinding,
                                  write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_buffer_view_desc(set, write->pTexelBufferView[j],
                                   write->dstBinding, write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_buffer_desc(set, write->pBufferInfo + j, write->dstBinding,
                              write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         unreachable("Dynamic buffers not yet supported");

      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
         const VkWriteDescriptorSetInlineUniformBlock *write_inline =
            vk_find_struct_const(write->pNext,
                                 WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
         assert(write_inline->dataSize == write->descriptorCount);
         write_inline_uniform_data(set, write_inline, write->dstBinding,
                                   write->dstArrayElement);
         break;
      }

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      VK_FROM_HANDLE(nvk_descriptor_set, src, copy->srcSet);
      VK_FROM_HANDLE(nvk_descriptor_set, dst, copy->dstSet);

      const struct nvk_descriptor_set_binding_layout *src_binding_layout =
         &src->layout->binding[copy->srcBinding];
      const struct nvk_descriptor_set_binding_layout *dst_binding_layout =
         &dst->layout->binding[copy->dstBinding];

      assert(dst_binding_layout->type == src_binding_layout->type);

      if (dst_binding_layout->stride > 0 && src_binding_layout->stride > 0) {
         for (uint32_t j = 0; j < copy->descriptorCount; j++) {
            memcpy(desc_ubo_data(dst, copy->dstBinding, copy->dstArrayElement + j),
                   desc_ubo_data(src, copy->srcBinding, copy->srcArrayElement + j),
                   MIN2(dst_binding_layout->stride, src_binding_layout->stride));
         }
      }

      switch (src_binding_layout->type) {
         /* Insert any special copy stuff here */

      default:
         break;
      }
   }
}

static void
nvk_descriptor_set_destroy(struct nvk_device *device, struct nvk_descriptor_pool *pool,
                           struct nvk_descriptor_set *set, bool free_bo)
{
   if (free_bo) {
      for (int i = 0; i < pool->entry_count; ++i) {
         if (pool->entries[i].set == set) {
            memmove(&pool->entries[i], &pool->entries[i + 1],
                    sizeof(pool->entries[i]) * (pool->entry_count - i - 1));
            --pool->entry_count;
            break;
         }
      }
   }

   vk_object_base_finish(&set->base);
   vk_free2(&device->vk.alloc, NULL, set);
}

static void
nvk_destroy_descriptor_pool(struct nvk_device *device, const VkAllocationCallbacks *pAllocator,
                            struct nvk_descriptor_pool *pool)
{
   for (int i = 0; i < pool->entry_count; ++i) {
      nvk_descriptor_set_destroy(device, pool, pool->entries[i].set, false);
   }

   nouveau_ws_bo_destroy(pool->bo);

   vk_object_base_finish(&pool->base);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_descriptor_pool *pool;
   uint64_t size = sizeof(struct nvk_descriptor_pool);
   uint64_t bo_size = 0;

   const VkMutableDescriptorTypeCreateInfoVALVE *mutable_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_VALVE);

   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      const VkMutableDescriptorTypeListVALVE *type_list = NULL;
      if (pCreateInfo->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_MUTABLE_VALVE) {
         assert(mutable_info != NULL);
         assert(i <= mutable_info->mutableDescriptorTypeListCount);
         type_list = &mutable_info->pMutableDescriptorTypeLists[i];
      }

      uint32_t stride, align;
      nvk_descriptor_stride_align_for_type(pCreateInfo->pPoolSizes[i].type,
                                           type_list, &stride, &align);
      bo_size += MAX2(stride, align) *
                 pCreateInfo->pPoolSizes[i].descriptorCount;
   }

   uint64_t entries_size = sizeof(struct nvk_descriptor_pool_entry) * pCreateInfo->maxSets;
   size += entries_size;

   pool = vk_zalloc2(&device->vk.alloc, pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);

   if (bo_size) {
      uint32_t flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
      pool->bo = nouveau_ws_bo_new(device->pdev->dev, bo_size, 0, flags);
      if (!pool->bo) {
         nvk_destroy_descriptor_pool(device, pAllocator, pool);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      pool->mapped_ptr = nouveau_ws_bo_map(pool->bo, NOUVEAU_WS_BO_WR);
      if (!pool->mapped_ptr) {
         nvk_destroy_descriptor_pool(device, pAllocator, pool);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   }

   pool->size = bo_size;
   pool->max_entry_count = pCreateInfo->maxSets;

   *pDescriptorPool = nvk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

static VkResult
nvk_descriptor_set_create(struct nvk_device *device, struct nvk_descriptor_pool *pool,
                          struct nvk_descriptor_set_layout *layout, const uint32_t *variable_count,
                          struct nvk_descriptor_set **out_set)
{
   struct nvk_descriptor_set *set;
   uint32_t mem_size = sizeof(struct nvk_descriptor_set);
   set = vk_zalloc2(&device->vk.alloc, NULL, mem_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

   set->layout = layout;
   if (pool->entry_count == pool->max_entry_count) {
      return VK_ERROR_OUT_OF_POOL_MEMORY;
   }

   if (pool->current_offset + layout->descriptor_buffer_size <= pool->size) {
      set->bo = pool->bo;
      set->mapped_ptr = (uint32_t *)(pool->mapped_ptr + pool->current_offset);
      set->bo_offset = pool->current_offset;
      pool->entries[pool->entry_count].offset = pool->current_offset;
      pool->entries[pool->entry_count].size = layout->descriptor_buffer_size;
      pool->entries[pool->entry_count].set = set;
      pool->current_offset += layout->descriptor_buffer_size;
   }
   pool->entry_count++;
   *out_set = set;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateDescriptorSets(VkDevice _device,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, pAllocateInfo->descriptorPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   struct nvk_descriptor_set *set = NULL;

   /* allocate a set of buffers for each shader to contain descriptors */
   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(nvk_descriptor_set_layout, layout, pAllocateInfo->pSetLayouts[i]);
      const uint32_t *variable_count = NULL;
      result = nvk_descriptor_set_create(device, pool, layout, variable_count, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = nvk_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS) {
      nvk_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool, i, pDescriptorSets);
      for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
         pDescriptorSets[i] = VK_NULL_HANDLE;
      }
   }
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FreeDescriptorSets(VkDevice _device, VkDescriptorPool descriptorPool, uint32_t count,
                       const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, descriptorPool);

   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(nvk_descriptor_set, set, pDescriptorSets[i]);

      if (set)
         nvk_descriptor_set_destroy(device, pool, set, true);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, _pool);

   nvk_destroy_descriptor_pool(device, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetDescriptorPool(VkDevice _device, VkDescriptorPool descriptorPool,
                        VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, descriptorPool);

   for (int i = 0; i < pool->entry_count; ++i) {
      nvk_descriptor_set_destroy(device, pool, pool->entries[i].set, false);
   }
   pool->entry_count = 0;

   return VK_SUCCESS;
}

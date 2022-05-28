#include "nvk_descriptor_set.h"

#include "nvk_buffer.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_image_view.h"
#include "nvk_sampler.h"

static void *desc_ubo_data(struct nvk_descriptor_set *set, uint32_t binding,
                           uint32_t elem) {
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];

   return (char *)set->map + binding_layout->offset +
          elem * binding_layout->stride;
}

static void write_sampler_desc(struct nvk_descriptor_set *set,
                               const VkDescriptorImageInfo *const info,
                               uint32_t binding, uint32_t elem) {
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];

   if (binding_layout->immutable_samplers)
      return;

   VK_FROM_HANDLE(nvk_sampler, sampler, info->sampler);

   struct nvk_image_descriptor *desc = desc_ubo_data(set, binding, elem);
   assert(sampler->desc_index < (1 << 12));
   desc->sampler_index = sampler->desc_index;
}

static void write_image_view_desc(struct nvk_descriptor_set *set,
                                  const VkDescriptorImageInfo *const info,
                                  uint32_t binding, uint32_t elem) {
   VK_FROM_HANDLE(nvk_image_view, view, info->imageView);

   struct nvk_image_descriptor *desc = desc_ubo_data(set, binding, elem);
   assert(view->desc_index < (1 << 20));
   desc->image_index = view->desc_index;
}

static void write_buffer_desc(struct nvk_descriptor_set *set,
                              const VkDescriptorBufferInfo *const info,
                              uint32_t binding, uint32_t elem) {
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);

   struct nvk_buffer_address *desc = desc_ubo_data(set, binding, elem);
   *desc = (struct nvk_buffer_address){
      .base_addr = nvk_buffer_address(buffer) + info->offset,
      .size = vk_buffer_range(&buffer->vk, info->offset, info->range),
   };
}

static void write_buffer_view_desc(struct nvk_descriptor_set *set,
                                   const VkBufferView bufferView,
                                   uint32_t binding, uint32_t elem) {
   /* TODO */
}

static void
write_inline_uniform_data(struct nvk_descriptor_set *set,
                          const VkWriteDescriptorSetInlineUniformBlock *info,
                          uint32_t binding, uint32_t offset) {
   memcpy((char *)desc_ubo_data(set, binding, 0) + offset, info->pData,
          info->dataSize);
}

VKAPI_ATTR void VKAPI_CALL nvk_UpdateDescriptorSets(
   VkDevice device, uint32_t descriptorWriteCount,
   const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
   const VkCopyDescriptorSet *pDescriptorCopies) {
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
nvk_destroy_descriptor_pool(struct nvk_device *device, const VkAllocationCallbacks *pAllocator,
                            struct nvk_descriptor_pool *pool)
{
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

   pool = vk_zalloc2(&device->vk.alloc, pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   *pDescriptorPool = nvk_descriptor_pool_to_handle(pool);
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

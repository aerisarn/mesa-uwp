/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vn_feedback.h"

#include "vn_device.h"
#include "vn_physical_device.h"

/* coherent buffer with bound and mapped memory */
struct vn_feedback_buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *data;

   struct list_head head;
};

static uint32_t
vn_get_memory_type_index(const VkPhysicalDeviceMemoryProperties *mem_props,
                         uint32_t mem_type_bits,
                         VkMemoryPropertyFlags required_mem_flags)
{
   u_foreach_bit(mem_type_index, mem_type_bits)
   {
      assert(mem_type_index < mem_props->memoryTypeCount);
      if ((mem_props->memoryTypes[mem_type_index].propertyFlags &
           required_mem_flags) == required_mem_flags)
         return mem_type_index;
   }

   return UINT32_MAX;
}

static VkResult
vn_feedback_buffer_create(struct vn_device *dev,
                          uint32_t size,
                          const VkAllocationCallbacks *alloc,
                          struct vn_feedback_buffer **out_feedback_buf)
{
   const bool exclusive = dev->queue_family_count == 1;
   const VkPhysicalDeviceMemoryProperties *mem_props =
      &dev->physical_device->memory_properties.memoryProperties;
   VkDevice dev_handle = vn_device_to_handle(dev);
   struct vn_feedback_buffer *feedback_buf;
   VkResult result;

   feedback_buf = vk_zalloc(alloc, sizeof(*feedback_buf), VN_DEFAULT_ALIGN,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!feedback_buf)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* use concurrent to avoid explicit queue family ownership transfer for
    * device created with queues from multiple queue families
    */
   const VkBufferCreateInfo buf_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode =
         exclusive ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
      /* below favors the current venus protocol */
      .queueFamilyIndexCount = exclusive ? 0 : dev->queue_family_count,
      .pQueueFamilyIndices = exclusive ? NULL : dev->queue_families,
   };
   result = vn_CreateBuffer(dev_handle, &buf_create_info, alloc,
                            &feedback_buf->buffer);
   if (result != VK_SUCCESS)
      goto out_free_feedback_buf;

   struct vn_buffer *buf = vn_buffer_from_handle(feedback_buf->buffer);
   const VkMemoryRequirements *mem_req =
      &buf->requirements.memory.memoryRequirements;
   const uint32_t mem_type_index =
      vn_get_memory_type_index(mem_props, mem_req->memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mem_type_index >= mem_props->memoryTypeCount) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto out_destroy_buffer;
   }

   const VkMemoryAllocateInfo mem_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_req->size,
      .memoryTypeIndex = mem_type_index,
   };
   result = vn_AllocateMemory(dev_handle, &mem_alloc_info, alloc,
                              &feedback_buf->memory);
   if (result != VK_SUCCESS)
      goto out_destroy_buffer;

   const VkBindBufferMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = feedback_buf->buffer,
      .memory = feedback_buf->memory,
      .memoryOffset = 0,
   };
   result = vn_BindBufferMemory2(dev_handle, 1, &bind_info);
   if (result != VK_SUCCESS)
      goto out_free_memory;

   result = vn_MapMemory(dev_handle, feedback_buf->memory, 0, VK_WHOLE_SIZE,
                         0, &feedback_buf->data);
   if (result != VK_SUCCESS)
      goto out_free_memory;

   *out_feedback_buf = feedback_buf;

   return VK_SUCCESS;

out_free_memory:
   vn_FreeMemory(dev_handle, feedback_buf->memory, alloc);

out_destroy_buffer:
   vn_DestroyBuffer(dev_handle, feedback_buf->buffer, alloc);

out_free_feedback_buf:
   vk_free(alloc, feedback_buf);

   return result;
}

static void
vn_feedback_buffer_destroy(struct vn_device *dev,
                           struct vn_feedback_buffer *feedback_buf,
                           const VkAllocationCallbacks *alloc)
{
   VkDevice dev_handle = vn_device_to_handle(dev);

   vn_UnmapMemory(dev_handle, feedback_buf->memory);
   vn_FreeMemory(dev_handle, feedback_buf->memory, alloc);
   vn_DestroyBuffer(dev_handle, feedback_buf->buffer, alloc);
   vk_free(alloc, feedback_buf);
}

static VkResult
vn_feedback_pool_grow(struct vn_feedback_pool *pool)
{
   VN_TRACE_FUNC();
   struct vn_feedback_buffer *feedback_buf = NULL;
   VkResult result;

   result = vn_feedback_buffer_create(pool->device, pool->size, pool->alloc,
                                      &feedback_buf);
   if (result != VK_SUCCESS)
      return result;

   pool->used = 0;

   list_add(&feedback_buf->head, &pool->feedback_buffers);

   return VK_SUCCESS;
}

VkResult
vn_feedback_pool_init(struct vn_device *dev,
                      struct vn_feedback_pool *pool,
                      uint32_t size,
                      const VkAllocationCallbacks *alloc)
{
   pool->device = dev;
   pool->alloc = alloc;
   pool->size = size;
   pool->used = size;
   list_inithead(&pool->feedback_buffers);

   return vn_feedback_pool_grow(pool);
}

void
vn_feedback_pool_fini(struct vn_feedback_pool *pool)
{
   list_for_each_entry_safe(struct vn_feedback_buffer, feedback_buf,
                            &pool->feedback_buffers, head)
      vn_feedback_buffer_destroy(pool->device, feedback_buf, pool->alloc);
}

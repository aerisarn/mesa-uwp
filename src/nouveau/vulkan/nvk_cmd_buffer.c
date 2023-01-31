#include "nvk_cmd_buffer.h"

#include "nvk_cmd_pool.h"
#include "nvk_descriptor_set.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_pipeline.h"
#include "nvk_pipeline_layout.h"
#include "nvk_physical_device.h"

#include "nouveau_push.h"
#include "nouveau_context.h"

#include "nouveau/nouveau.h"

#include "nvk_cl90b5.h"
#include "nvk_cla0c0.h"

static void
nvk_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct nvk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);

   nvk_cmd_pool_free_bo_list(pool, &cmd->bos);
   util_dynarray_fini(&cmd->pushes);
   util_dynarray_fini(&cmd->bo_refs);
   vk_command_buffer_finish(&cmd->vk);
   vk_free(&pool->vk.alloc, cmd);
}

static VkResult
nvk_create_cmd_buffer(struct vk_command_pool *vk_pool,
                      struct vk_command_buffer **cmd_buffer_out)
{
   struct nvk_cmd_pool *pool = container_of(vk_pool, struct nvk_cmd_pool, vk);
   struct nvk_device *device = nvk_cmd_pool_device(pool);
   struct nvk_cmd_buffer *cmd;
   VkResult result;

   cmd = vk_zalloc(&pool->vk.alloc, sizeof(*cmd), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = vk_command_buffer_init(&pool->vk, &cmd->vk,
                                   &nvk_cmd_buffer_ops, 0);
   if (result != VK_SUCCESS) {
      vk_free(&pool->vk.alloc, cmd);
      return result;
   }

   cmd->vk.dynamic_graphics_state.vi =
      &cmd->state.gfx._dynamic_vi;

   list_inithead(&cmd->bos);
   util_dynarray_init(&cmd->pushes, NULL);
   util_dynarray_init(&cmd->bo_refs, NULL);

   *cmd_buffer_out = &cmd->vk;

   return VK_SUCCESS;
}

static void
nvk_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer,
                     UNUSED VkCommandBufferResetFlags flags)
{
   struct nvk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);

   vk_command_buffer_reset(&cmd->vk);

   nvk_cmd_pool_free_bo_list(pool, &cmd->bos);
   cmd->upload_bo = NULL;
   cmd->push_bo = NULL;
   cmd->push_bo_limit = NULL;
   cmd->push = (struct nv_push) {0};

   util_dynarray_clear(&cmd->pushes);
   util_dynarray_clear(&cmd->bo_refs);

   memset(&cmd->state, 0, sizeof(cmd->state));
}

const struct vk_command_buffer_ops nvk_cmd_buffer_ops = {
   .create = nvk_create_cmd_buffer,
   .reset = nvk_reset_cmd_buffer,
   .destroy = nvk_destroy_cmd_buffer,
};

/* If we ever fail to allocate a push, we use this */
static uint32_t push_runout[NVK_CMD_BUFFER_MAX_PUSH];

void
nvk_cmd_buffer_new_push(struct nvk_cmd_buffer *cmd)
{
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);
   VkResult result;

   result = nvk_cmd_pool_alloc_bo(pool, &cmd->push_bo);
   if (unlikely(result != VK_SUCCESS)) {
      STATIC_ASSERT(NVK_CMD_BUFFER_MAX_PUSH <= NVK_CMD_BO_SIZE / 4);
      cmd->push_bo = NULL;
      nv_push_init(&cmd->push, push_runout, 0);
      cmd->push_bo_limit = &push_runout[NVK_CMD_BUFFER_MAX_PUSH];
   } else {
      nv_push_init(&cmd->push, cmd->push_bo->map, 0);
      cmd->push_bo_limit =
         (uint32_t *)((char *)cmd->push_bo->map + NVK_CMD_BO_SIZE);
   }
}

static void
nvk_cmd_buffer_flush_push(struct nvk_cmd_buffer *cmd)
{
   struct nvk_cmd_push push = {
      .bo = cmd->push_bo,
      .start_dw = cmd->push.start - (uint32_t *)cmd->push_bo->map,
      .dw_count = nv_push_dw_count(&cmd->push),
   };
   util_dynarray_append(&cmd->pushes, struct nvk_cmd_push, push);

   cmd->push.start = cmd->push.end;
}

VkResult
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd,
                            uint32_t size, uint32_t alignment,
                            uint64_t *addr, void **ptr)
{
   assert(size % 4 == 0);
   assert(size < NVK_CMD_BO_SIZE);

   uint32_t offset = cmd->upload_offset;
   if (align > 0)
      offset = align(offset, alignment);

   assert(offset <= NVK_CMD_BO_SIZE);
   if (cmd->upload_bo == NULL || size > NVK_CMD_BO_SIZE - offset) {
      struct nvk_cmd_bo *bo;
      VkResult result = nvk_cmd_pool_alloc_bo(nvk_cmd_buffer_pool(cmd), &bo);
      if (unlikely(result != VK_SUCCESS))
         return result;

      nvk_cmd_buffer_ref_bo(cmd, bo->bo);
      cmd->upload_bo = bo;
      offset = 0;
   }

   *addr = cmd->upload_bo->bo->offset + offset;
   *ptr = (char *)cmd->upload_bo->map + offset;

   cmd->upload_offset = offset + size;

   return VK_SUCCESS;
}

VkResult
nvk_cmd_buffer_upload_data(struct nvk_cmd_buffer *cmd,
                           const void *data, uint32_t size,
                           uint32_t alignment, uint64_t *addr)
{
   VkResult result;
   void *map;

   result = nvk_cmd_buffer_upload_alloc(cmd, size, alignment, addr, &map);
   if (unlikely(result != VK_SUCCESS))
      return result;

   memcpy(map, data, size);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   nvk_reset_cmd_buffer(&cmd->vk, 0);

   /* Start with a nop so we have at least something to submit */
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);
   P_MTHD(p, NV90B5, NOP);
   P_NV90B5_NOP(p, 0);

   nvk_cmd_buffer_begin_compute(cmd, pBeginInfo);
   nvk_cmd_buffer_begin_graphics(cmd, pBeginInfo);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   nvk_cmd_buffer_flush_push(cmd);

   return vk_command_buffer_get_record_result(&cmd->vk);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   unreachable("Secondary command buffers not yet supported");
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                        const VkDependencyInfo *pDependencyInfo)
{ }

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindPipeline(VkCommandBuffer commandBuffer,
                    VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_pipeline, pipeline, _pipeline);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   for (unsigned s = 0; s < ARRAY_SIZE(pipeline->shaders); s++) {
      if (!pipeline->shaders[s].bo)
         continue;

      nvk_cmd_buffer_ref_bo(cmd, pipeline->shaders[s].bo);

      if (pipeline->shaders[s].slm_size)
         nvk_device_ensure_slm(dev, pipeline->shaders[s].slm_size);
   }

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      assert(pipeline->type == NVK_PIPELINE_GRAPHICS);
      nvk_cmd_bind_graphics_pipeline(cmd, (void *)pipeline);
      break;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      assert(pipeline->type == NVK_PIPELINE_COMPUTE);
      nvk_cmd_bind_compute_pipeline(cmd, (void *)pipeline);
      break;
   default:
      unreachable("Unhandled bind point");
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout layout,
                          uint32_t firstSet,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets,
                          uint32_t dynamicOffsetCount,
                          const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_pipeline_layout, pipeline_layout, layout);
   struct nvk_descriptor_state *desc =
      nvk_get_descriptors_state(cmd, pipelineBindPoint);

   uint32_t next_dyn_offset = 0;
   for (uint32_t i = 0; i < descriptorSetCount; ++i) {
      unsigned set_idx = i + firstSet;
      VK_FROM_HANDLE(nvk_descriptor_set, set, pDescriptorSets[i]);
      const struct nvk_descriptor_set_layout *set_layout =
         pipeline_layout->set[set_idx].layout;

      if (desc->sets[set_idx] != set) {
         if (set->bo)
            nvk_cmd_buffer_ref_bo(cmd, set->bo);
         desc->root.sets[set_idx] = nvk_descriptor_set_addr(set);
         desc->sets[set_idx] = set;
         desc->sets_dirty |= BITFIELD_BIT(set_idx);
      }

      if (set_layout->dynamic_buffer_count > 0) {
         const uint32_t dynamic_buffer_start =
            pipeline_layout->set[set_idx].dynamic_buffer_start;

         for (uint32_t j = 0; j < set_layout->dynamic_buffer_count; j++) {
            struct nvk_buffer_address addr = set->dynamic_buffers[j];
            addr.base_addr += pDynamicOffsets[next_dyn_offset + j];
            desc->root.dynamic_buffers[dynamic_buffer_start + j] = addr;
         }
         next_dyn_offset += set->layout->dynamic_buffer_count;
      }
   }
   assert(next_dyn_offset <= dynamicOffsetCount);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPushConstants(VkCommandBuffer commandBuffer,
                     VkPipelineLayout layout,
                     VkShaderStageFlags stageFlags,
                     uint32_t offset,
                     uint32_t size,
                     const void *pValues)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) {
      struct nvk_descriptor_state *desc =
         nvk_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);

      memcpy(desc->root.push + offset, pValues, size);
   }

   if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct nvk_descriptor_state *desc =
         nvk_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);

      memcpy(desc->root.push + offset, pValues, size);
   }
}

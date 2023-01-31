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
nvk_cmd_buffer_upload_init(struct nvk_cmd_buffer_upload *upload)
{
   memset(upload, 0, sizeof(*upload));
   list_inithead(&upload->list);
}

static void
nvk_cmd_buffer_upload_reset(struct nvk_cmd_buffer_upload *upload)
{
   list_for_each_entry_safe(struct nvk_cmd_buffer_upload, child,
                            &upload->list, list) {
      nouveau_ws_bo_unmap(child->upload_bo, child->map);
      nouveau_ws_bo_destroy(child->upload_bo);
      free(child);
   }
   list_inithead(&upload->list);

   upload->offset = 0;
}

static void
nvk_cmd_buffer_upload_finish(struct nvk_cmd_buffer_upload *upload)
{
   nvk_cmd_buffer_upload_reset(upload);
   if (upload->upload_bo)
      nouveau_ws_bo_destroy(upload->upload_bo);
}

static void
nvk_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct nvk_cmd_buffer *cmd_buffer =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);

   nvk_cmd_buffer_upload_finish(&cmd_buffer->upload);
   nouveau_ws_push_destroy(cmd_buffer->push);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

static VkResult
nvk_create_cmd_buffer(struct vk_command_pool *vk_pool,
                      struct vk_command_buffer **cmd_buffer_out)
{
   struct nvk_cmd_pool *pool = container_of(vk_pool, struct nvk_cmd_pool, vk);
   struct nvk_device *device = nvk_cmd_pool_device(pool);
   struct nvk_cmd_buffer *cmd_buffer;
   VkResult result;

   cmd_buffer = vk_zalloc(&pool->vk.alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = vk_command_buffer_init(&pool->vk, &cmd_buffer->vk,
                                   &nvk_cmd_buffer_ops, 0);
   if (result != VK_SUCCESS) {
      vk_free(&pool->vk.alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->vk.dynamic_graphics_state.vi =
      &cmd_buffer->state.gfx._dynamic_vi;

   cmd_buffer->push = nouveau_ws_push_new(device->pdev->dev, NVK_CMD_BUF_SIZE);
   nvk_cmd_buffer_upload_init(&cmd_buffer->upload);

   *cmd_buffer_out = &cmd_buffer->vk;

   return VK_SUCCESS;
}

static void
nvk_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer,
                     UNUSED VkCommandBufferResetFlags flags)
{
   struct nvk_cmd_buffer *cmd_buffer =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);

   vk_command_buffer_reset(&cmd_buffer->vk);

   nouveau_ws_push_reset(cmd_buffer->push);
   nvk_cmd_buffer_upload_reset(&cmd_buffer->upload);
   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
}

const struct vk_command_buffer_ops nvk_cmd_buffer_ops = {
   .create = nvk_create_cmd_buffer,
   .reset = nvk_reset_cmd_buffer,
   .destroy = nvk_destroy_cmd_buffer,
};

static bool
nvk_cmd_buffer_resize_upload_buf(struct nvk_cmd_buffer *cmd_buffer,
                                 uint64_t min_needed)
{
   struct nvk_device *device = nvk_cmd_buffer_device(cmd_buffer);
   uint64_t new_size;
   struct nouveau_ws_bo *bo = NULL;
   struct nvk_cmd_buffer_upload *upload;

   new_size = MAX2(min_needed, 16 * 1024);
   new_size = MAX2(new_size, 2 * cmd_buffer->upload.size);

   uint32_t flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
   bo = nouveau_ws_bo_new(device->pdev->dev, new_size, 0, flags);

   nouveau_ws_push_ref(cmd_buffer->push, bo, NOUVEAU_WS_BO_RD);
   if (cmd_buffer->upload.upload_bo) {
      upload = malloc(sizeof(*upload));

      if (!upload) {
         vk_command_buffer_set_error(&cmd_buffer->vk,
                                     VK_ERROR_OUT_OF_HOST_MEMORY);
         nouveau_ws_bo_destroy(bo);
         return false;
      }

      memcpy(upload, &cmd_buffer->upload, sizeof(*upload));
      list_add(&upload->list, &cmd_buffer->upload.list);
   }

   cmd_buffer->upload.upload_bo = bo;
   cmd_buffer->upload.size = new_size;
   cmd_buffer->upload.offset = 0;
   cmd_buffer->upload.map = nouveau_ws_bo_map(cmd_buffer->upload.upload_bo, NOUVEAU_WS_BO_WR);

   if (!cmd_buffer->upload.map) {
      vk_command_buffer_set_error(&cmd_buffer->vk,
                                  VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }

   return true;
}

bool
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd_buffer, unsigned size,
                            uint64_t *addr, void **ptr)
{
   assert(size % 4 == 0);

   /* Align to the scalar cache line size if it results in this allocation
    * being placed in less of them.
    */
   unsigned offset = cmd_buffer->upload.offset;
   unsigned line_size = 256;//for compute dispatches
   unsigned gap = align(offset, line_size) - offset;
   if ((size & ~(line_size - 1)) > gap)
      offset = align(offset, line_size);

   if (offset + size > cmd_buffer->upload.size) {
      if (!nvk_cmd_buffer_resize_upload_buf(cmd_buffer, size))
         return false;
      offset = 0;
   }

   *addr = cmd_buffer->upload.upload_bo->offset + offset;
   *ptr = cmd_buffer->upload.map + offset;

   cmd_buffer->upload.offset = offset + size;
   return true;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   nvk_reset_cmd_buffer(&cmd->vk, 0);

   /* Start with a nop so we have at least something to submit */
   struct nv_push *p = P_SPACE(cmd->push, 2);
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

      nouveau_ws_push_ref(cmd->push, pipeline->shaders[s].bo,
                          NOUVEAU_WS_BO_RD);

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
         nvk_push_descriptor_set_ref(cmd->push, set);
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

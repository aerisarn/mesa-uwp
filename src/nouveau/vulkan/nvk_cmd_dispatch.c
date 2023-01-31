#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_set.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_pipeline.h"

#include "nouveau_context.h"

#include "classes/cla0b5.h"

#include "nvk_cla0c0.h"
#include "cla1c0.h"
#include "nvk_clc3c0.h"

#include "drf.h"
#include "cla0c0qmd.h"
#include "clc0c0qmd.h"
#include "clc3c0qmd.h"

#define NVA0C0_QMDV00_06_VAL_SET(p,a...) NVVAL_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVA0C0_QMDV00_06_DEF_SET(p,a...) NVDEF_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVC0C0_QMDV02_01_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC0C0_QMDV02_01_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC3C0_QMDV02_02_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC3C0, QMDV02_02, ##a)
#define NVC3C0_QMDV02_02_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC3C0, QMDV02_02, ##a)

void
nvk_cmd_buffer_begin_compute(struct nvk_cmd_buffer *cmd,
                             const VkCommandBufferBeginInfo *pBeginInfo)
{ }

static void
gv100_compute_setup_launch_desc(uint32_t *qmd,
                                uint32_t x, uint32_t y, uint32_t z)
{
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_WIDTH, x);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_HEIGHT, y);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_DEPTH, z);
}

static inline void
gp100_cp_launch_desc_set_cb(uint32_t *qmd, unsigned index,
                            uint32_t size, uint64_t address)
{
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_ADDR_LOWER, index, address);
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_ADDR_UPPER, index, address >> 32);
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_SIZE_SHIFTED4, index,
                                 DIV_ROUND_UP(size, 16));
   NVC0C0_QMDV02_01_DEF_SET(qmd, CONSTANT_BUFFER_VALID, index, TRUE);
}

void
nvk_cmd_bind_compute_pipeline(struct nvk_cmd_buffer *cmd,
                              struct nvk_compute_pipeline *pipeline)
{
   cmd->state.cs.pipeline = pipeline;
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDispatch(VkCommandBuffer commandBuffer,
                uint32_t groupCountX,
                uint32_t groupCountY,
                uint32_t groupCountZ)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   const struct nvk_compute_pipeline *pipeline = cmd->state.cs.pipeline;
   const struct nvk_shader *shader =
      &pipeline->base.shaders[MESA_SHADER_COMPUTE];
   struct nvk_descriptor_state *desc = &cmd->state.cs.descriptors;
   VkResult result;

   desc->root.cs.block_size[0] = shader->cp.block_size[0];
   desc->root.cs.block_size[1] = shader->cp.block_size[1];
   desc->root.cs.block_size[2] = shader->cp.block_size[2];
   desc->root.cs.grid_size[0] = groupCountX;
   desc->root.cs.grid_size[1] = groupCountY;
   desc->root.cs.grid_size[2] = groupCountZ;

   uint64_t root_table_addr;
   result = nvk_cmd_buffer_upload_data(cmd, &desc->root, sizeof(desc->root),
                                       NVK_MIN_UBO_ALIGNMENT,
                                       &root_table_addr);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   uint32_t qmd[128];
   memset(qmd, 0, sizeof(qmd));
   memcpy(qmd, pipeline->qmd_template, sizeof(pipeline->qmd_template));

   gv100_compute_setup_launch_desc(qmd, groupCountX, groupCountY, groupCountZ);

   gp100_cp_launch_desc_set_cb(qmd, 0, sizeof(desc->root), root_table_addr);
   gp100_cp_launch_desc_set_cb(qmd, 1, sizeof(desc->root), root_table_addr);

   uint64_t qmd_addr;
   result = nvk_cmd_buffer_upload_data(cmd, qmd, sizeof(qmd), 256, &qmd_addr);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   struct nv_push *p = P_SPACE(cmd->push, 6);

   P_MTHD(p, NVA0C0, INVALIDATE_SHADER_CACHES_NO_WFI);
   P_NVA0C0_INVALIDATE_SHADER_CACHES_NO_WFI(p, {
      .constant = CONSTANT_TRUE
   });

   P_MTHD(p, NVA0C0, SEND_PCAS_A);
   P_NVA0C0_SEND_PCAS_A(p, qmd_addr >> 8);
   P_IMMD(p, NVA0C0, SEND_SIGNALING_PCAS_B, {
      .invalidate = INVALIDATE_TRUE,
      .schedule = SCHEDULE_TRUE
   });
}

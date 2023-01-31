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

static uint64_t
calc_tls_size(struct nvk_device *device,
              uint32_t lpos, uint32_t lneg, uint32_t cstack)
{
   uint64_t size = (lpos + lneg) * 32 + cstack;

   assert (size < (1 << 20));

   size *= 64; /* max warps */
   size  = align(size, 0x8000);
   size *= device->pdev->dev->mp_count;

   size = align(size, 1 << 17);
   return size;
}

void
nvk_cmd_buffer_begin_compute(struct nvk_cmd_buffer *cmd,
                             const VkCommandBufferBeginInfo *pBeginInfo)
{
   struct nvk_device *dev = (struct nvk_device *)cmd->vk.base.device;

   if (dev->ctx->compute.cls < 0xa0c0)
      return;

   cmd->tls_space_needed = calc_tls_size(dev, 128 * 16, 0, 0x200);
}

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

   desc->root.cs.block_size[0] = shader->cp.block_size[0];
   desc->root.cs.block_size[1] = shader->cp.block_size[1];
   desc->root.cs.block_size[2] = shader->cp.block_size[2];
   desc->root.cs.grid_size[0] = groupCountX;
   desc->root.cs.grid_size[1] = groupCountY;
   desc->root.cs.grid_size[2] = groupCountZ;

   uint32_t root_table_size = sizeof(desc->root);
   void *root_table_map;
   uint64_t root_table_addr;
   if (!nvk_cmd_buffer_upload_alloc(cmd, root_table_size, &root_table_addr,
                                    &root_table_map))
      return; /* TODO: Error */

   P_MTHD(cmd->push, NVA0C0, OFFSET_OUT_UPPER);
   P_NVA0C0_OFFSET_OUT_UPPER(cmd->push, root_table_addr >> 32);
   P_NVA0C0_OFFSET_OUT(cmd->push, root_table_addr & 0xffffffff);
   P_MTHD(cmd->push, NVA0C0, LINE_LENGTH_IN);
   P_NVA0C0_LINE_LENGTH_IN(cmd->push, root_table_size);
   P_NVA0C0_LINE_COUNT(cmd->push, 0x1);

   P_1INC(cmd->push, NVA0C0, LAUNCH_DMA);
   P_NVA0C0_LAUNCH_DMA(cmd->push,
                       { .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
                         .sysmembar_disable = SYSMEMBAR_DISABLE_TRUE });
   P_INLINE_ARRAY(cmd->push, (uint32_t *)&desc->root, root_table_size / 4);

   uint32_t *qmd;
   uint64_t qmd_addr;
   if (!nvk_cmd_buffer_upload_alloc(cmd, 512, &qmd_addr, (void **)&qmd))
      return; /* TODO: Error */

   memcpy(qmd, pipeline->qmd_template, 256);
   gv100_compute_setup_launch_desc(qmd, groupCountX, groupCountY, groupCountZ);

   gp100_cp_launch_desc_set_cb(qmd, 0, root_table_size, root_table_addr);
   gp100_cp_launch_desc_set_cb(qmd, 1, root_table_size, root_table_addr);

   P_MTHD(cmd->push, NVA0C0, INVALIDATE_SHADER_CACHES_NO_WFI);
   P_NVA0C0_INVALIDATE_SHADER_CACHES_NO_WFI(cmd->push, { .constant = CONSTANT_TRUE });

   P_MTHD(cmd->push, NVA0C0, SEND_PCAS_A);
   P_NVA0C0_SEND_PCAS_A(cmd->push, qmd_addr >> 8);
   P_IMMD(cmd->push, NVA0C0, SEND_SIGNALING_PCAS_B,
          { .invalidate = INVALIDATE_TRUE,
            .schedule = SCHEDULE_TRUE });
}

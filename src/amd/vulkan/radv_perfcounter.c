/*
 * Copyright © 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <inttypes.h>

#include "ac_perfcounter.h"
#include "amdgfxregs.h"
#include "radv_cs.h"
#include "radv_private.h"
#include "sid.h"

void
radv_perfcounter_emit_shaders(struct radeon_cmdbuf *cs, unsigned shaders)
{
   radeon_set_uconfig_reg_seq(cs, R_036780_SQ_PERFCOUNTER_CTRL, 2);
   radeon_emit(cs, shaders & 0x7f);
   radeon_emit(cs, 0xffffffff);
}

void
radv_perfcounter_emit_spm_reset(struct radeon_cmdbuf *cs)
{
   radeon_set_uconfig_reg(cs, R_036020_CP_PERFMON_CNTL,
                              S_036020_PERFMON_STATE(V_036020_CP_PERFMON_STATE_DISABLE_AND_RESET) |
                              S_036020_SPM_PERFMON_STATE(V_036020_STRM_PERFMON_STATE_DISABLE_AND_RESET));
}

void
radv_perfcounter_emit_spm_start(struct radv_device *device, struct radeon_cmdbuf *cs, int family)
{
   /* Start SPM counters. */
   radeon_set_uconfig_reg(cs, R_036020_CP_PERFMON_CNTL,
                              S_036020_PERFMON_STATE(V_036020_CP_PERFMON_STATE_DISABLE_AND_RESET) |
                              S_036020_SPM_PERFMON_STATE(V_036020_STRM_PERFMON_STATE_START_COUNTING));

   /* Start windowed performance counters. */
   if (family == RADV_QUEUE_GENERAL) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PERFCOUNTER_START) | EVENT_INDEX(0));
   }
   radeon_set_sh_reg(cs, R_00B82C_COMPUTE_PERFCOUNT_ENABLE, S_00B82C_PERFCOUNT_ENABLE(1));
}

void
radv_perfcounter_emit_spm_stop(struct radv_device *device, struct radeon_cmdbuf *cs, int family)
{
   /* Stop windowed performance counters. */
   if (family == RADV_QUEUE_GENERAL) {
      if (!device->physical_device->rad_info.never_send_perfcounter_stop) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PERFCOUNTER_STOP) | EVENT_INDEX(0));
      }
   }
   radeon_set_sh_reg(cs, R_00B82C_COMPUTE_PERFCOUNT_ENABLE, S_00B82C_PERFCOUNT_ENABLE(0));

   /* Stop SPM counters. */
   radeon_set_uconfig_reg(cs, R_036020_CP_PERFMON_CNTL,
                              S_036020_PERFMON_STATE(V_036020_CP_PERFMON_STATE_DISABLE_AND_RESET) |
                              S_036020_SPM_PERFMON_STATE(device->physical_device->rad_info.never_stop_sq_perf_counters ?
                                                            V_036020_STRM_PERFMON_STATE_START_COUNTING :
                                                            V_036020_STRM_PERFMON_STATE_STOP_COUNTING));
}

enum radv_perfcounter_op {
   RADV_PC_OP_SUM,
   RADV_PC_OP_MAX,
   RADV_PC_OP_RATIO_DIVSCALE,
   RADV_PC_OP_REVERSE_RATIO, /* (reg1 - reg0) / reg1 */
   RADV_PC_OP_SUM_WEIGHTED_4,
};

#define S_REG_SEL(x)   ((x)&0xFFFF)
#define G_REG_SEL(x)   ((x)&0xFFFF)
#define S_REG_BLOCK(x) ((x) << 16)
#define G_REG_BLOCK(x) (((x) >> 16) & 0x7FFF)

#define S_REG_OFFSET(x)    ((x)&0xFFFF)
#define G_REG_OFFSET(x)    ((x)&0xFFFF)
#define S_REG_INSTANCES(x) ((x) << 16)
#define G_REG_INSTANCES(x) (((x) >> 16) & 0x7FFF)
#define S_REG_CONSTANT(x)  ((x) << 31)
#define G_REG_CONSTANT(x)  ((x) >> 31)

struct radv_perfcounter_impl {
   enum radv_perfcounter_op op;
   uint32_t regs[8];
};

/* Only append to this list, never insert into the middle or remove (but can rename).
 *
 * The invariant we're trying to get here is counters that have the same meaning, so
 * these can be shared between counters that have different implementations on different
 * GPUs, but should be unique within a GPU.
 */
enum radv_perfcounter_uuid {
   RADV_PC_UUID_GPU_CYCLES,
   RADV_PC_UUID_SHADER_WAVES,
   RADV_PC_UUID_SHADER_INSTRUCTIONS,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_VALU,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_SALU,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_VMEM_LOAD,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_SMEM_LOAD,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_VMEM_STORE,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_LDS,
   RADV_PC_UUID_SHADER_INSTRUCTIONS_GDS,
   RADV_PC_UUID_SHADER_VALU_BUSY,
   RADV_PC_UUID_SHADER_SALU_BUSY,
   RADV_PC_UUID_VRAM_READ_SIZE,
   RADV_PC_UUID_VRAM_WRITE_SIZE,
   RADV_PC_UUID_L0_CACHE_HIT_RATIO,
   RADV_PC_UUID_L1_CACHE_HIT_RATIO,
   RADV_PC_UUID_L2_CACHE_HIT_RATIO,
};

struct radv_perfcounter_desc {
   struct radv_perfcounter_impl impl;

   VkPerformanceCounterUnitKHR unit;

   char name[VK_MAX_DESCRIPTION_SIZE];
   char category[VK_MAX_DESCRIPTION_SIZE];
   char description[VK_MAX_DESCRIPTION_SIZE];
   enum radv_perfcounter_uuid uuid;
};

#define PC_DESC(arg_op, arg_unit, arg_name, arg_category, arg_description, arg_uuid, ...)          \
   (struct radv_perfcounter_desc)                                                                  \
   {                                                                                               \
      .impl = {.op = arg_op, .regs = {__VA_ARGS__}},                                               \
      .unit = VK_PERFORMANCE_COUNTER_UNIT_##arg_unit##_KHR, .name = arg_name,                      \
      .category = arg_category, .description = arg_description, .uuid = RADV_PC_UUID_##arg_uuid    \
   }

#define ADD_PC(op, unit, name, category, description, uuid, ...)                                   \
   do {                                                                                            \
      if (descs) {                                                                                 \
         descs[*count] = PC_DESC((op), unit, name, category, description, uuid, __VA_ARGS__);      \
      }                                                                                            \
      ++*count;                                                                                    \
   } while (0)
#define CTR(block, ctr) (S_REG_BLOCK(block) | S_REG_SEL(ctr))
#define CONSTANT(v)     (S_REG_CONSTANT(1) | (uint32_t)(v))

enum { GRBM_PERF_SEL_GUI_ACTIVE = CTR(GRBM, 2) };

enum { CPF_PERF_SEL_CPF_STAT_BUSY_GFX10 = CTR(CPF, 0x18) };

enum {
   GL1C_PERF_SEL_REQ = CTR(GL1C, 0xe),
   GL1C_PERF_SEL_REQ_MISS = CTR(GL1C, 0x12),
};

enum {
   GL2C_PERF_SEL_REQ = CTR(GL2C, 0x3),

   GL2C_PERF_SEL_MISS_GFX101 = CTR(GL2C, 0x23),
   GL2C_PERF_SEL_MC_WRREQ_GFX101 = CTR(GL2C, 0x4b),
   GL2C_PERF_SEL_EA_WRREQ_64B_GFX101 = CTR(GL2C, 0x4c),
   GL2C_PERF_SEL_EA_RDREQ_32B_GFX101 = CTR(GL2C, 0x59),
   GL2C_PERF_SEL_EA_RDREQ_64B_GFX101 = CTR(GL2C, 0x5a),
   GL2C_PERF_SEL_EA_RDREQ_96B_GFX101 = CTR(GL2C, 0x5b),
   GL2C_PERF_SEL_EA_RDREQ_128B_GFX101 = CTR(GL2C, 0x5c),

   GL2C_PERF_SEL_MISS_GFX103 = CTR(GL2C, 0x2b),
   GL2C_PERF_SEL_MC_WRREQ_GFX103 = CTR(GL2C, 0x53),
   GL2C_PERF_SEL_EA_WRREQ_64B_GFX103 = CTR(GL2C, 0x55),
   GL2C_PERF_SEL_EA_RDREQ_32B_GFX103 = CTR(GL2C, 0x63),
   GL2C_PERF_SEL_EA_RDREQ_64B_GFX103 = CTR(GL2C, 0x64),
   GL2C_PERF_SEL_EA_RDREQ_96B_GFX103 = CTR(GL2C, 0x65),
   GL2C_PERF_SEL_EA_RDREQ_128B_GFX103 = CTR(GL2C, 0x66),
};

enum {
   SQ_PERF_SEL_WAVES = CTR(SQ, 0x4),
   SQ_PERF_SEL_INSTS_ALL_GFX10 = CTR(SQ, 0x31),
   SQ_PERF_SEL_INSTS_GDS_GFX10 = CTR(SQ, 0x37),
   SQ_PERF_SEL_INSTS_LDS_GFX10 = CTR(SQ, 0x3b),
   SQ_PERF_SEL_INSTS_SALU_GFX10 = CTR(SQ, 0x3c),
   SQ_PERF_SEL_INSTS_SMEM_GFX10 = CTR(SQ, 0x3d),
   SQ_PERF_SEL_INSTS_VALU_GFX10 = CTR(SQ, 0x40),
   SQ_PERF_SEL_INSTS_TEX_LOAD_GFX10 = CTR(SQ, 0x45),
   SQ_PERF_SEL_INSTS_TEX_STORE_GFX10 = CTR(SQ, 0x46),
   SQ_PERF_SEL_INST_CYCLES_VALU_GFX10 = CTR(SQ, 0x75),
};

enum {
   TCP_PERF_SEL_REQ_GFX10 = CTR(TCP, 0x9),
   TCP_PERF_SEL_REQ_MISS_GFX10 = CTR(TCP, 0x12),
};

#define CTR_NUM_SIMD                                                                               \
   CONSTANT(pdev->rad_info.num_simd_per_compute_unit * pdev->rad_info.num_good_compute_units)
#define CTR_NUM_CUS CONSTANT(pdev->rad_info.num_good_compute_units)

static void
radv_query_perfcounter_descs(struct radv_physical_device *pdev, uint32_t *count,
                             struct radv_perfcounter_desc *descs)
{
   *count = 0;

   ADD_PC(RADV_PC_OP_MAX, CYCLES, "GPU active cycles", "GRBM",
          "cycles the GPU is active processing a command buffer.", GPU_CYCLES,
          GRBM_PERF_SEL_GUI_ACTIVE);

   ADD_PC(RADV_PC_OP_SUM, GENERIC, "Waves", "Shaders", "Number of waves executed", SHADER_WAVES,
          SQ_PERF_SEL_WAVES);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "Instructions", "Shaders", "Number of Instructions executed",
          SHADER_INSTRUCTIONS, SQ_PERF_SEL_INSTS_ALL_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "VALU Instructions", "Shaders",
          "Number of VALU Instructions executed", SHADER_INSTRUCTIONS_VALU,
          SQ_PERF_SEL_INSTS_VALU_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "SALU Instructions", "Shaders",
          "Number of SALU Instructions executed", SHADER_INSTRUCTIONS_SALU,
          SQ_PERF_SEL_INSTS_SALU_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "VMEM Load Instructions", "Shaders",
          "Number of VMEM load instructions executed", SHADER_INSTRUCTIONS_VMEM_LOAD,
          SQ_PERF_SEL_INSTS_TEX_LOAD_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "SMEM Load Instructions", "Shaders",
          "Number of SMEM load instructions executed", SHADER_INSTRUCTIONS_SMEM_LOAD,
          SQ_PERF_SEL_INSTS_SMEM_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "VMEM Store Instructions", "Shaders",
          "Number of VMEM store instructions executed", SHADER_INSTRUCTIONS_VMEM_STORE,
          SQ_PERF_SEL_INSTS_TEX_STORE_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "LDS Instructions", "Shaders",
          "Number of LDS Instructions executed", SHADER_INSTRUCTIONS_LDS,
          SQ_PERF_SEL_INSTS_LDS_GFX10);
   ADD_PC(RADV_PC_OP_SUM, GENERIC, "GDS Instructions", "Shaders",
          "Number of GDS Instructions executed", SHADER_INSTRUCTIONS_GDS,
          SQ_PERF_SEL_INSTS_GDS_GFX10);

   ADD_PC(RADV_PC_OP_RATIO_DIVSCALE, PERCENTAGE, "VALU Busy", "Shader Utilization",
          "Percentage of time the VALU units are busy", SHADER_VALU_BUSY,
          SQ_PERF_SEL_INST_CYCLES_VALU_GFX10, CPF_PERF_SEL_CPF_STAT_BUSY_GFX10, CTR_NUM_SIMD);
   ADD_PC(RADV_PC_OP_RATIO_DIVSCALE, PERCENTAGE, "SALU Busy", "Shader Utilization",
          "Percentage of time the SALU units are busy", SHADER_SALU_BUSY,
          SQ_PERF_SEL_INSTS_SALU_GFX10, CPF_PERF_SEL_CPF_STAT_BUSY_GFX10, CTR_NUM_CUS);

   if (pdev->rad_info.gfx_level >= GFX10_3) {
      ADD_PC(RADV_PC_OP_SUM_WEIGHTED_4, BYTES, "VRAM read size", "Memory",
             "Number of bytes read from VRAM", VRAM_READ_SIZE, GL2C_PERF_SEL_EA_RDREQ_32B_GFX103,
             CONSTANT(32), GL2C_PERF_SEL_EA_RDREQ_64B_GFX103, CONSTANT(64),
             GL2C_PERF_SEL_EA_RDREQ_96B_GFX103, CONSTANT(96), GL2C_PERF_SEL_EA_RDREQ_128B_GFX103,
             CONSTANT(128));
      ADD_PC(RADV_PC_OP_SUM_WEIGHTED_4, BYTES, "VRAM write size", "Memory",
             "Number of bytes written to VRAM", VRAM_WRITE_SIZE, GL2C_PERF_SEL_MC_WRREQ_GFX103,
             CONSTANT(32), GL2C_PERF_SEL_EA_WRREQ_64B_GFX103, CONSTANT(64), CONSTANT(0),
             CONSTANT(0), CONSTANT(0), CONSTANT(0));
   } else {
      ADD_PC(RADV_PC_OP_SUM_WEIGHTED_4, BYTES, "VRAM read size", "Memory",
             "Number of bytes read from VRAM", VRAM_READ_SIZE, GL2C_PERF_SEL_EA_RDREQ_32B_GFX101,
             CONSTANT(32), GL2C_PERF_SEL_EA_RDREQ_64B_GFX101, CONSTANT(64),
             GL2C_PERF_SEL_EA_RDREQ_96B_GFX101, CONSTANT(96), GL2C_PERF_SEL_EA_RDREQ_128B_GFX101,
             CONSTANT(128));
      ADD_PC(RADV_PC_OP_SUM_WEIGHTED_4, BYTES, "VRAM write size", "Memory",
             "Number of bytes written to VRAM", VRAM_WRITE_SIZE, GL2C_PERF_SEL_MC_WRREQ_GFX101,
             CONSTANT(32), GL2C_PERF_SEL_EA_WRREQ_64B_GFX101, CONSTANT(32), CONSTANT(0),
             CONSTANT(0), CONSTANT(0), CONSTANT(0));
   }

   ADD_PC(RADV_PC_OP_REVERSE_RATIO, BYTES, "L0 cache hit ratio", "Memory", "Hit ratio of L0 cache",
          L0_CACHE_HIT_RATIO, TCP_PERF_SEL_REQ_MISS_GFX10, TCP_PERF_SEL_REQ_GFX10);
   ADD_PC(RADV_PC_OP_REVERSE_RATIO, BYTES, "L1 cache hit ratio", "Memory", "Hit ratio of L1 cache",
          L1_CACHE_HIT_RATIO, GL1C_PERF_SEL_REQ_MISS, GL1C_PERF_SEL_REQ);
   if (pdev->rad_info.gfx_level >= GFX10_3) {
      ADD_PC(RADV_PC_OP_REVERSE_RATIO, BYTES, "L2 cache hit ratio", "Memory",
             "Hit ratio of L2 cache", L2_CACHE_HIT_RATIO, GL2C_PERF_SEL_MISS_GFX103,
             GL2C_PERF_SEL_REQ);
   } else {
      ADD_PC(RADV_PC_OP_REVERSE_RATIO, BYTES, "L2 cache hit ratio", "Memory",
             "Hit ratio of L2 cache", L2_CACHE_HIT_RATIO, GL2C_PERF_SEL_MISS_GFX101,
             GL2C_PERF_SEL_REQ);
   }
}

static bool
radv_init_perfcounter_descs(struct radv_physical_device *pdev)
{
   if (pdev->perfcounters)
      return true;

   uint32_t count;
   radv_query_perfcounter_descs(pdev, &count, NULL);

   struct radv_perfcounter_desc *descs = malloc(sizeof(*descs) * count);
   if (!descs)
      return false;

   radv_query_perfcounter_descs(pdev, &count, descs);
   pdev->num_perfcounters = count;
   pdev->perfcounters = descs;

   return true;
}

static int
cmp_uint32_t(const void *a, const void *b)
{
   uint32_t l = *(const uint32_t *)a;
   uint32_t r = *(const uint32_t *)b;

   return (l < r) ? -1 : (l > r) ? 1 : 0;
}

static VkResult
radv_get_counter_registers(const struct radv_physical_device *pdevice, uint32_t num_indices,
                           const uint32_t *indices, unsigned *out_num_regs, uint32_t **out_regs)
{
   ASSERTED uint32_t num_counters = pdevice->num_perfcounters;
   const struct radv_perfcounter_desc *descs = pdevice->perfcounters;

   unsigned full_reg_cnt = num_indices * ARRAY_SIZE(descs->impl.regs);
   uint32_t *regs = malloc(full_reg_cnt * sizeof(uint32_t));
   if (!regs)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   unsigned reg_cnt = 0;
   for (unsigned i = 0; i < num_indices; ++i) {
      uint32_t index = indices[i];
      assert(index < num_counters);
      for (unsigned j = 0; j < ARRAY_SIZE(descs[index].impl.regs) && descs[index].impl.regs[j];
           ++j) {
         if (!G_REG_CONSTANT(descs[index].impl.regs[j]))
            regs[reg_cnt++] = descs[index].impl.regs[j];
      }
   }

   qsort(regs, reg_cnt, sizeof(uint32_t), cmp_uint32_t);

   unsigned deduped_reg_cnt = 0;
   for (unsigned i = 1; i < reg_cnt; ++i) {
      if (regs[i] != regs[deduped_reg_cnt])
         regs[++deduped_reg_cnt] = regs[i];
   }
   ++deduped_reg_cnt;

   *out_num_regs = deduped_reg_cnt;
   *out_regs = regs;
   return VK_SUCCESS;
}

static unsigned
radv_pc_get_num_instances(const struct radv_physical_device *pdevice, struct ac_pc_block *ac_block)
{
   return ac_block->num_instances *
          ((ac_block->b->b->flags & AC_PC_BLOCK_SE) ? pdevice->rad_info.max_se : 1);
}

static unsigned
radv_get_num_counter_passes(const struct radv_physical_device *pdevice, unsigned num_regs,
                            const uint32_t *regs)
{
   enum ac_pc_gpu_block prev_block = NUM_GPU_BLOCK;
   unsigned block_reg_count = 0;
   struct ac_pc_block *ac_block = NULL;
   unsigned passes_needed = 1;

   for (unsigned i = 0; i < num_regs; ++i) {
      enum ac_pc_gpu_block block = G_REG_BLOCK(regs[i]);

      if (block != prev_block) {
         block_reg_count = 0;
         prev_block = block;
         ac_block = ac_pc_get_block(&pdevice->ac_perfcounters, block);
      }

      ++block_reg_count;

      passes_needed =
         MAX2(passes_needed, DIV_ROUND_UP(block_reg_count, ac_block->b->b->num_counters));
   }

   return passes_needed;
}

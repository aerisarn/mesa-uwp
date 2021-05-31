/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "ac_gpu_info.h"
#include "ac_perfcounter.h"

#include "util/u_memory.h"
#include "macros.h"

static struct ac_pc_block_base cik_CB = {
   .name = "CB",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = R_037000_CB_PERFCOUNTER_FILTER,
   .counter0_lo = R_035018_CB_PERFCOUNTER0_LO,
   .num_multi = 1,
   .num_prelude = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static unsigned cik_CPC_select[] = {
   R_036024_CPC_PERFCOUNTER0_SELECT,
   R_036010_CPC_PERFCOUNTER0_SELECT1,
   R_03600C_CPC_PERFCOUNTER1_SELECT,
};
static struct ac_pc_block_base cik_CPC = {
   .name = "CPC",
   .num_counters = 2,

   .select = cik_CPC_select,
   .counter0_lo = R_034018_CPC_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_CUSTOM | AC_PC_REG_REVERSE,
};

static struct ac_pc_block_base cik_CPF = {
   .name = "CPF",
   .num_counters = 2,

   .select0 = R_03601C_CPF_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034028_CPF_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE | AC_PC_REG_REVERSE,
};

static struct ac_pc_block_base cik_CPG = {
   .name = "CPG",
   .num_counters = 2,

   .select0 = R_036008_CPG_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034008_CPG_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE | AC_PC_REG_REVERSE,
};

static struct ac_pc_block_base cik_DB = {
   .name = "DB",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = R_037100_DB_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035100_DB_PERFCOUNTER0_LO,
   .num_multi = 3, // really only 2, but there's a gap between registers
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_GDS = {
   .name = "GDS",
   .num_counters = 4,

   .select0 = R_036A00_GDS_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034A00_GDS_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_TAIL,
};

static unsigned cik_GRBM_counters[] = {
   R_034100_GRBM_PERFCOUNTER0_LO,
   R_03410C_GRBM_PERFCOUNTER1_LO,
};
static struct ac_pc_block_base cik_GRBM = {
   .name = "GRBM",
   .num_counters = 2,

   .select0 = R_036100_GRBM_PERFCOUNTER0_SELECT,
   .counters = cik_GRBM_counters,
};

static struct ac_pc_block_base cik_GRBMSE = {
   .name = "GRBMSE",
   .num_counters = 4,

   .select0 = R_036108_GRBM_SE0_PERFCOUNTER_SELECT,
   .counter0_lo = R_034114_GRBM_SE0_PERFCOUNTER_LO,
};

static struct ac_pc_block_base cik_IA = {
   .name = "IA",
   .num_counters = 4,

   .select0 = R_036210_IA_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034220_IA_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_TAIL,
};

static struct ac_pc_block_base cik_PA_SC = {
   .name = "PA_SC",
   .num_counters = 8,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036500_PA_SC_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034500_PA_SC_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

/* According to docs, PA_SU counters are only 48 bits wide. */
static struct ac_pc_block_base cik_PA_SU = {
   .name = "PA_SU",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036400_PA_SU_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034400_PA_SU_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_SPI = {
   .name = "SPI",
   .num_counters = 6,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036600_SPI_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034604_SPI_PERFCOUNTER0_LO,
   .num_multi = 4,
   .layout = AC_PC_MULTI_BLOCK,
};

static struct ac_pc_block_base cik_SQ = {
   .name = "SQ",
   .num_counters = 16,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER,

   .select0 = R_036700_SQ_PERFCOUNTER0_SELECT,
   .select_or = S_036700_SQC_BANK_MASK(15) | S_036700_SQC_CLIENT_MASK(15) | S_036700_SIMD_MASK(15),
   .counter0_lo = R_034700_SQ_PERFCOUNTER0_LO,
};

static struct ac_pc_block_base cik_SX = {
   .name = "SX",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036900_SX_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034900_SX_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_TAIL,
};

static struct ac_pc_block_base cik_TA = {
   .name = "TA",
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_036B00_TA_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034B00_TA_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_TD = {
   .name = "TD",
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_036C00_TD_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034C00_TD_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_TCA = {
   .name = "TCA",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = R_036E40_TCA_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034E40_TCA_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_TCC = {
   .name = "TCC",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = R_036E00_TCC_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034E00_TCC_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_TCP = {
   .name = "TCP",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_036D00_TCP_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034D00_TCP_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base cik_VGT = {
   .name = "VGT",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036230_VGT_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034240_VGT_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_TAIL,
};

static struct ac_pc_block_base cik_WD = {
   .name = "WD",
   .num_counters = 4,

   .select0 = R_036200_WD_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034200_WD_PERFCOUNTER0_LO,
};

static struct ac_pc_block_base cik_MC = {
   .name = "MC",
   .num_counters = 4,

   .layout = AC_PC_FAKE,
};

static struct ac_pc_block_base cik_SRBM = {
   .name = "SRBM",
   .num_counters = 2,

   .layout = AC_PC_FAKE,
};

static struct ac_pc_block_base gfx10_CHA = {
   .name = "CHA",
   .num_counters = 4,

   .select0 = R_037780_CHA_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035800_CHA_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_CHCG = {
   .name = "CHCG",
   .num_counters = 4,

   .select0 = R_036F18_CHCG_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034F20_CHCG_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_CHC = {
   .name = "CHC",
   .num_counters = 4,

   .select0 = R_036F00_CHC_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034F00_CHC_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GCR = {
   .name = "GCR",
   .num_counters = 2,

   .select0 = R_037580_GCR_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035480_GCR_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GE = {
   .name = "GE",
   .num_counters = 12,

   .select0 = R_036200_GE_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034200_GE_PERFCOUNTER0_LO,
   .num_multi = 4,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GL1A = {
   .name = "GL1A",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_037700_GL1A_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035700_GL1A_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GL1C = {
   .name = "GL1C",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_036E80_GL1C_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034E80_GL1C_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GL2A = {
   .name = "GL2A",
   .num_counters = 4,

   .select0 = R_036E40_GL2A_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034E40_GL2A_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_GL2C = {
   .name = "GL2C",
   .num_counters = 4,

   .select0 = R_036E00_GL2C_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034E00_GL2C_PERFCOUNTER0_LO,
   .num_multi = 2,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static unsigned gfx10_PA_PH_select[] = {
   R_037600_PA_PH_PERFCOUNTER0_SELECT,
   R_037604_PA_PH_PERFCOUNTER0_SELECT1,
   R_037608_PA_PH_PERFCOUNTER1_SELECT,
   R_037640_PA_PH_PERFCOUNTER1_SELECT1,
   R_03760C_PA_PH_PERFCOUNTER2_SELECT,
   R_037644_PA_PH_PERFCOUNTER2_SELECT1,
   R_037610_PA_PH_PERFCOUNTER3_SELECT,
   R_037648_PA_PH_PERFCOUNTER3_SELECT1,
   R_037614_PA_PH_PERFCOUNTER4_SELECT,
   R_037618_PA_PH_PERFCOUNTER5_SELECT,
   R_03761C_PA_PH_PERFCOUNTER6_SELECT,
   R_037620_PA_PH_PERFCOUNTER7_SELECT,
};
static struct ac_pc_block_base gfx10_PA_PH = {
   .name = "PA_PH",
   .num_counters = 8,
   .flags = AC_PC_BLOCK_SE,

   .select = gfx10_PA_PH_select,
   .counter0_lo = R_035600_PA_PH_PERFCOUNTER0_LO,
   .num_multi = 4,
   .layout = AC_PC_MULTI_CUSTOM,
};

static struct ac_pc_block_base gfx10_PA_SU = {
   .name = "PA_SU",
   .num_counters = 4,
   .flags = AC_PC_BLOCK_SE,

   .select0 = R_036400_PA_SU_PERFCOUNTER0_SELECT,
   .counter0_lo = R_034400_PA_SU_PERFCOUNTER0_LO,
   .num_multi = 4,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_RLC = {
   .name = "RLC",
   .num_counters = 2,

   .select0 = R_037304_RLC_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035200_RLC_PERFCOUNTER0_LO,
   .num_multi = 0,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_RMI = {
   .name = "RMI",
   /* Actually 4, but the 2nd counter is missing the secondary selector while
    * the 3rd counter has it, which complicates the register layout. */
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_INSTANCE_GROUPS,

   .select0 = R_037400_RMI_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035300_RMI_PERFCOUNTER0_LO,
   .num_multi = 1,
   .layout = AC_PC_MULTI_ALTERNATE,
};

static struct ac_pc_block_base gfx10_UTCL1 = {
   .name = "UTCL1",
   .num_counters = 2,
   .flags = AC_PC_BLOCK_SE | AC_PC_BLOCK_SHADER_WINDOWED,

   .select0 = R_03758C_UTCL1_PERFCOUNTER0_SELECT,
   .counter0_lo = R_035470_UTCL1_PERFCOUNTER0_LO,
   .num_multi = 0,
   .layout = AC_PC_MULTI_ALTERNATE,
};

/* Both the number of instances and selectors varies between chips of the same
 * class. We only differentiate by class here and simply expose the maximum
 * number over all chips in a class.
 *
 * Unfortunately, GPUPerfStudio uses the order of performance counter groups
 * blindly once it believes it has identified the hardware, so the order of
 * blocks here matters.
 */
static struct ac_pc_block_gfxdescr groups_CIK[] = {
   {&cik_CB, 226},     {&cik_CPF, 17},    {&cik_DB, 257},  {&cik_GRBM, 34},   {&cik_GRBMSE, 15},
   {&cik_PA_SU, 153},  {&cik_PA_SC, 395}, {&cik_SPI, 186}, {&cik_SQ, 252},    {&cik_SX, 32},
   {&cik_TA, 111},     {&cik_TCA, 39, 2}, {&cik_TCC, 160}, {&cik_TD, 55},     {&cik_TCP, 154},
   {&cik_GDS, 121},    {&cik_VGT, 140},   {&cik_IA, 22},   {&cik_MC, 22},     {&cik_SRBM, 19},
   {&cik_WD, 22},      {&cik_CPG, 46},    {&cik_CPC, 22},

};

static struct ac_pc_block_gfxdescr groups_VI[] = {
   {&cik_CB, 405},     {&cik_CPF, 19},    {&cik_DB, 257},  {&cik_GRBM, 34},   {&cik_GRBMSE, 15},
   {&cik_PA_SU, 154},  {&cik_PA_SC, 397}, {&cik_SPI, 197}, {&cik_SQ, 273},    {&cik_SX, 34},
   {&cik_TA, 119},     {&cik_TCA, 35, 2}, {&cik_TCC, 192}, {&cik_TD, 55},     {&cik_TCP, 180},
   {&cik_GDS, 121},    {&cik_VGT, 147},   {&cik_IA, 24},   {&cik_MC, 22},     {&cik_SRBM, 27},
   {&cik_WD, 37},      {&cik_CPG, 48},    {&cik_CPC, 24},

};

static struct ac_pc_block_gfxdescr groups_gfx9[] = {
   {&cik_CB, 438},     {&cik_CPF, 32},    {&cik_DB, 328},  {&cik_GRBM, 38},   {&cik_GRBMSE, 16},
   {&cik_PA_SU, 292},  {&cik_PA_SC, 491}, {&cik_SPI, 196}, {&cik_SQ, 374},    {&cik_SX, 208},
   {&cik_TA, 119},     {&cik_TCA, 35, 2}, {&cik_TCC, 256}, {&cik_TD, 57},     {&cik_TCP, 85},
   {&cik_GDS, 121},    {&cik_VGT, 148},   {&cik_IA, 32},   {&cik_WD, 58},     {&cik_CPG, 59},
   {&cik_CPC, 35},
};

static struct ac_pc_block_gfxdescr groups_gfx10[] = {
   {&cik_CB, 461},
   {&gfx10_CHA, 45},
   {&gfx10_CHCG, 35},
   {&gfx10_CHC, 35},
   {&cik_CPC, 47},
   {&cik_CPF, 40},
   {&cik_CPG, 82},
   {&cik_DB, 370},
   {&gfx10_GCR, 94},
   {&cik_GDS, 123},
   {&gfx10_GE, 315},
   {&gfx10_GL1A, 36},
   {&gfx10_GL1C, 64},
   {&gfx10_GL2A, 91},
   {&gfx10_GL2C, 235},
   {&cik_GRBM, 47},
   {&cik_GRBMSE, 19},
   {&gfx10_PA_PH, 960},
   {&cik_PA_SC, 552},
   {&gfx10_PA_SU, 266},
   {&gfx10_RLC, 7},
   {&gfx10_RMI, 258},
   {&cik_SPI, 329},
   {&cik_SQ, 509},
   {&cik_SX, 225},
   {&cik_TA, 226},
   {&cik_TCP, 77},
   {&cik_TD, 61},
   {&gfx10_UTCL1, 15},
};

struct ac_pc_block *ac_lookup_counter(const struct ac_perfcounters *pc,
                                      unsigned index, unsigned *base_gid,
                                      unsigned *sub_index)
{
   struct ac_pc_block *block = pc->blocks;
   unsigned bid;

   *base_gid = 0;
   for (bid = 0; bid < pc->num_blocks; ++bid, ++block) {
      unsigned total = block->num_groups * block->b->selectors;

      if (index < total) {
         *sub_index = index;
         return block;
      }

      index -= total;
      *base_gid += block->num_groups;
   }

   return NULL;
}

struct ac_pc_block *ac_lookup_group(const struct ac_perfcounters *pc,
                                    unsigned *index)
{
   unsigned bid;
   struct ac_pc_block *block = pc->blocks;

   for (bid = 0; bid < pc->num_blocks; ++bid, ++block) {
      if (*index < block->num_groups)
         return block;
      *index -= block->num_groups;
   }

   return NULL;
}

bool ac_init_block_names(const struct radeon_info *info,
                         const struct ac_perfcounters *pc,
                         struct ac_pc_block *block)
{
   bool per_instance_groups = ac_pc_block_has_per_instance_groups(pc, block);
   bool per_se_groups = ac_pc_block_has_per_se_groups(pc, block);
   unsigned i, j, k;
   unsigned groups_shader = 1, groups_se = 1, groups_instance = 1;
   unsigned namelen;
   char *groupname;
   char *p;

   if (per_instance_groups)
      groups_instance = block->num_instances;
   if (per_se_groups)
      groups_se = info->max_se;
   if (block->b->b->flags & AC_PC_BLOCK_SHADER)
      groups_shader = ARRAY_SIZE(ac_pc_shader_type_bits);

   namelen = strlen(block->b->b->name);
   block->group_name_stride = namelen + 1;
   if (block->b->b->flags & AC_PC_BLOCK_SHADER)
      block->group_name_stride += 3;
   if (per_se_groups) {
      assert(groups_se <= 10);
      block->group_name_stride += 1;

      if (per_instance_groups)
         block->group_name_stride += 1;
   }
   if (per_instance_groups) {
      assert(groups_instance <= 100);
      block->group_name_stride += 2;
   }

   block->group_names = MALLOC(block->num_groups * block->group_name_stride);
   if (!block->group_names)
      return false;

   groupname = block->group_names;
   for (i = 0; i < groups_shader; ++i) {
      const char *shader_suffix = ac_pc_shader_type_suffixes[i];
      unsigned shaderlen = strlen(shader_suffix);
      for (j = 0; j < groups_se; ++j) {
         for (k = 0; k < groups_instance; ++k) {
            strcpy(groupname, block->b->b->name);
            p = groupname + namelen;

            if (block->b->b->flags & AC_PC_BLOCK_SHADER) {
               strcpy(p, shader_suffix);
               p += shaderlen;
            }

            if (per_se_groups) {
               p += sprintf(p, "%d", j);
               if (per_instance_groups)
                  *p++ = '_';
            }

            if (per_instance_groups)
               p += sprintf(p, "%d", k);

            groupname += block->group_name_stride;
         }
      }
   }

   assert(block->b->selectors <= 1000);
   block->selector_name_stride = block->group_name_stride + 4;
   block->selector_names =
      MALLOC(block->num_groups * block->b->selectors * block->selector_name_stride);
   if (!block->selector_names)
      return false;

   groupname = block->group_names;
   p = block->selector_names;
   for (i = 0; i < block->num_groups; ++i) {
      for (j = 0; j < block->b->selectors; ++j) {
         sprintf(p, "%s_%03d", groupname, j);
         p += block->selector_name_stride;
      }
      groupname += block->group_name_stride;
   }

   return true;
}

bool ac_init_perfcounters(const struct radeon_info *info,
                          bool separate_se,
                          bool separate_instance,
                          struct ac_perfcounters *pc)
{
   const struct ac_pc_block_gfxdescr *blocks;
   unsigned num_blocks;

   switch (info->chip_class) {
   case GFX7:
      blocks = groups_CIK;
      num_blocks = ARRAY_SIZE(groups_CIK);
      break;
   case GFX8:
      blocks = groups_VI;
      num_blocks = ARRAY_SIZE(groups_VI);
      break;
   case GFX9:
      blocks = groups_gfx9;
      num_blocks = ARRAY_SIZE(groups_gfx9);
      break;
   case GFX10:
   case GFX10_3:
      blocks = groups_gfx10;
      num_blocks = ARRAY_SIZE(groups_gfx10);
      break;
   case GFX6:
   default:
      return false; /* not implemented */
   }

   pc->separate_se = separate_se;
   pc->separate_instance = separate_instance;

   pc->blocks = CALLOC(num_blocks, sizeof(struct ac_pc_block));
   if (!pc->blocks)
      return false;
   pc->num_blocks = num_blocks;

   for (unsigned i = 0; i < num_blocks; i++) {
      struct ac_pc_block *block = &pc->blocks[i];

      block->b = &blocks[i];
      block->num_instances = MAX2(1, block->b->instances);

      if (!strcmp(block->b->b->name, "CB") ||
          !strcmp(block->b->b->name, "DB") ||
          !strcmp(block->b->b->name, "RMI"))
         block->num_instances = info->max_se;
      else if (!strcmp(block->b->b->name, "TCC"))
         block->num_instances = info->max_tcc_blocks;
      else if (!strcmp(block->b->b->name, "IA"))
         block->num_instances = MAX2(1, info->max_se / 2);
      else if (!strcmp(block->b->b->name, "TA") ||
               !strcmp(block->b->b->name, "TCP") ||
               !strcmp(block->b->b->name, "TD")) {
         block->num_instances = MAX2(1, info->max_good_cu_per_sa);
      }

      if (ac_pc_block_has_per_instance_groups(pc, block)) {
         block->num_groups = block->num_instances;
      } else {
         block->num_groups = 1;
      }

      if (ac_pc_block_has_per_se_groups(pc, block))
         block->num_groups *= info->max_se;
      if (block->b->b->flags & AC_PC_BLOCK_SHADER)
         block->num_groups *= ARRAY_SIZE(ac_pc_shader_type_bits);

      pc->num_groups += block->num_groups;
   }

   return true;
}

void ac_destroy_perfcounters(struct ac_perfcounters *pc)
{
   if (!pc)
      return;

   for (unsigned i = 0; i < pc->num_blocks; ++i) {
      FREE(pc->blocks[i].group_names);
      FREE(pc->blocks[i].selector_names);
   }
   FREE(pc->blocks);
}

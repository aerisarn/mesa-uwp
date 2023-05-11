/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "common/amd_family.h"
#include "drm-shim/drm_shim.h"
#include "drm-uapi/amdgpu_drm.h"
#include "util/log.h"

struct amdgpu_device {
   const char *name;
   enum radeon_family radeon_family;

   struct drm_amdgpu_info_hw_ip hw_ip_gfx;
   struct drm_amdgpu_info_hw_ip hw_ip_compute;

   struct drm_amdgpu_info_firmware fw_gfx_me;
   struct drm_amdgpu_info_firmware fw_gfx_pfp;
   struct drm_amdgpu_info_firmware fw_gfx_mec;

   uint32_t mmr_regs[256 * 3];
   uint32_t mmr_reg_count;

   struct drm_amdgpu_info_device dev;
   struct drm_amdgpu_memory_info mem;
};

static const struct amdgpu_device *amdgpu_dev;

bool drm_shim_driver_prefers_first_render_node = true;

static int
amdgpu_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
amdgpu_ioctl_gem_create(int fd, unsigned long request, void *_arg)
{
   union drm_amdgpu_gem_create *arg = _arg;
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = calloc(1, sizeof(*bo));
   int ret;

   ret = drm_shim_bo_init(bo, arg->in.bo_size);
   if (ret) {
      free(bo);
      return ret;
   }

   arg->out.handle = drm_shim_bo_get_handle(shim_fd, bo);

   drm_shim_bo_put(bo);

   return 0;
}

static int
amdgpu_ioctl_gem_mmap(int fd, unsigned long request, void *_arg)
{
   union drm_amdgpu_gem_mmap *arg = _arg;
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, arg->in.handle);

   arg->out.addr_ptr = drm_shim_bo_get_mmap_offset(shim_fd, bo);

   return 0;
}

static void
amdgpu_info_hw_ip_info(uint32_t type, struct drm_amdgpu_info_hw_ip *out)
{
   switch (type) {
   case AMDGPU_HW_IP_GFX:
      *out = amdgpu_dev->hw_ip_gfx;
      break;
   case AMDGPU_HW_IP_COMPUTE:
      *out = amdgpu_dev->hw_ip_compute;
      break;
   default:
      break;
   }
}

static void
amdgpu_info_fw_version(uint32_t type, struct drm_amdgpu_info_firmware *out)
{
   switch (type) {
   case AMDGPU_INFO_FW_GFX_ME:
      *out = amdgpu_dev->fw_gfx_me;
      break;
   case AMDGPU_INFO_FW_GFX_PFP:
      *out = amdgpu_dev->fw_gfx_pfp;
      break;
   case AMDGPU_INFO_FW_GFX_MEC:
      *out = amdgpu_dev->fw_gfx_mec;
      break;
   default:
      break;
   }
}

static void
amdgpu_info_read_mmr_reg(uint32_t reg, uint32_t count, uint32_t instance, uint32_t *vals)
{
   for (int i = 0; i < count; i++) {
      /* linear search */
      bool found = false;
      uint32_t val = 0;
      for (int j = 0; j < amdgpu_dev->mmr_reg_count; j++) {
         const uint32_t *triple = &amdgpu_dev->mmr_regs[j * 3];
         if (triple[0] == reg + i && triple[1] == instance) {
            val = triple[2];
            found = true;
            break;
         }
      }

      if (!found)
         mesa_logw("reg 0x%04x is unknown", reg + i);

      vals[i] = val;
   }
}

static void
amdgpu_info_dev_info(struct drm_amdgpu_info_device *out)
{
   *out = amdgpu_dev->dev;
}

static void
amdgpu_info_memory(struct drm_amdgpu_memory_info *out)
{
   *out = amdgpu_dev->mem;

   /* override all but total_heap_size */
   out->vram.usable_heap_size = out->vram.total_heap_size;
   out->vram.heap_usage = 0;
   out->vram.max_allocation = out->vram.total_heap_size * 3 / 4;
   out->cpu_accessible_vram.usable_heap_size = out->cpu_accessible_vram.total_heap_size;
   out->cpu_accessible_vram.heap_usage = 0;
   out->cpu_accessible_vram.max_allocation = out->cpu_accessible_vram.total_heap_size * 3 / 4;
   out->gtt.usable_heap_size = out->gtt.total_heap_size;
   out->gtt.heap_usage = 0;
   out->gtt.max_allocation = out->gtt.total_heap_size * 3 / 4;
}

static void
amdgpu_info_video_caps(uint32_t type, struct drm_amdgpu_info_video_caps *out)
{
   /* nop */
}

static int
amdgpu_ioctl_info(int fd, unsigned long request, void *arg)
{
   const struct drm_amdgpu_info *info = arg;
   union {
      void *ptr;
      uint32_t *ui32;
   } out = { .ptr = (void *)info->return_pointer };

   switch (info->query) {
   case AMDGPU_INFO_ACCEL_WORKING:
      *out.ui32 = 1;
      break;
   case AMDGPU_INFO_HW_IP_INFO:
      amdgpu_info_hw_ip_info(info->query_hw_ip.type, out.ptr);
      break;
   case AMDGPU_INFO_FW_VERSION:
      amdgpu_info_fw_version(info->query_fw.fw_type, out.ptr);
      break;
   case AMDGPU_INFO_READ_MMR_REG:
      amdgpu_info_read_mmr_reg(info->read_mmr_reg.dword_offset, info->read_mmr_reg.count,
                               info->read_mmr_reg.instance, out.ptr);
      break;
   case AMDGPU_INFO_DEV_INFO:
      amdgpu_info_dev_info(out.ptr);
      break;
   case AMDGPU_INFO_MEMORY:
      amdgpu_info_memory(out.ptr);
      break;
   case AMDGPU_INFO_VIDEO_CAPS:
      amdgpu_info_video_caps(info->video_cap.type, out.ptr);
      break;
   default:
      return -EINVAL;
   }

   return 0;
}

static ioctl_fn_t amdgpu_ioctls[] = {
   [DRM_AMDGPU_GEM_CREATE] = amdgpu_ioctl_gem_create,
   [DRM_AMDGPU_GEM_MMAP] = amdgpu_ioctl_gem_mmap,
   [DRM_AMDGPU_CTX] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_BO_LIST] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_CS] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_INFO] = amdgpu_ioctl_info,
   [DRM_AMDGPU_GEM_METADATA] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_GEM_WAIT_IDLE] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_GEM_VA] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_WAIT_CS] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_GEM_OP] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_GEM_USERPTR] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_WAIT_FENCES] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_VM] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_FENCE_TO_HANDLE] = amdgpu_ioctl_noop,
   [DRM_AMDGPU_SCHED] = amdgpu_ioctl_noop,
};

static const struct amdgpu_device amdgpu_devices[] = {
   {
      .name = "renoir",
      .radeon_family = CHIP_RENOIR,
      .hw_ip_gfx = {
         .hw_ip_version_major = 9,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0x90300,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 9,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0x90300,
      },
      .fw_gfx_me = {
         .ver = 166,
         .feature = 53,
      },
      .fw_gfx_pfp = {
         .ver = 194,
         .feature = 53,
      },
      .fw_gfx_mec = {
         .ver = 464,
         .feature = 53,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x26010042,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x15e7,
         .external_rev = 0xa1,
         .pci_rev = 0xe9,
         .family = AMDGPU_FAMILY_RV,
         .num_shader_engines = 1,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 100000,
         .max_engine_clock = 1800000,
         .max_memory_clock = 1333000,
         .cu_active_number = 7,
         .cu_ao_mask = 0xfe,
         .cu_bitmap[0][0] = 0xfe,
         .enabled_rb_pipes_mask = 0x3,
         .num_rb_pipes = 2,
         .num_hw_gfx_contexts = 8,
         .ids_flags = 0x5,
         .virtual_address_offset = 0x200000,
         .virtual_address_max = 0x800000000000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 32768,
         .vram_type = 11,
         .vram_bit_width = 128,
         .gc_double_offchip_lds_buf = 1,
         .wave_front_size = 64,
         .num_shader_visible_vgprs = 256,
         .num_cu_per_sh = 8,
         .num_tcc_blocks = 4,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .cu_ao_bitmap[0][0] = 0xfe,
         .high_va_offset = 0xffff800000000000llu,
         .high_va_max = 0xffffffffffe00000llu,
      },
      .mem = {
         .vram = {
            .total_heap_size = 64ull << 20,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 64ull << 20,
         },
         .gtt = {
            .total_heap_size = 4096ull << 20,
         },
      },
   },
   {
      .name = "raven",
      .radeon_family = CHIP_RAVEN,
      .hw_ip_gfx = {
         .hw_ip_version_major = 9,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 9,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
      },
      .fw_gfx_me = {
         .ver = 162,
         .feature = 47,
      },
      .fw_gfx_pfp = {
         .ver = 185,
         .feature = 47,
      },
      .fw_gfx_mec = {
         .ver = 427,
         .feature = 47,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x24000042,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x15d8,
         .chip_rev = 0x01,
         .external_rev = 0x42,
         .pci_rev = 0xc1,
         .family = AMDGPU_FAMILY_RV,
         .num_shader_engines = 1,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 25000,
         .max_engine_clock = 1400000,
         .max_memory_clock = 1200000,
         .cu_active_number = 10,
         .cu_ao_mask = 0x3ff,
         .cu_bitmap[0][0] = 0x3ff,
         .enabled_rb_pipes_mask = 0x3,
         .num_rb_pipes = 2,
         .num_hw_gfx_contexts = 8,
         .ids_flags = 0x1,
         .virtual_address_offset = 0x200000,
         .virtual_address_max = 0x800000000000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 32768,
         .vram_type = 8,
         .vram_bit_width = 128,
         .gc_double_offchip_lds_buf = 1,
         .wave_front_size = 64,
         .num_shader_visible_vgprs = 256,
         .num_cu_per_sh = 11,
         .num_tcc_blocks = 4,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .cu_ao_bitmap[0][0] = 0x3ff,
         .high_va_offset = 0xffff800000000000llu,
         .high_va_max = 0xffffffffffe00000llu,
      },
      .mem = {
         .vram = {
            .total_heap_size = 64ull << 20,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 64ull << 20,
         },
         .gtt = {
            .total_heap_size = 3072ull << 20,
         },
      },
   },
   {
      .name = "stoney",
      .radeon_family = CHIP_STONEY,
      .hw_ip_gfx = {
         .hw_ip_version_major = 8,
         .hw_ip_version_minor = 1,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 8,
         .hw_ip_version_minor = 1,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
      },
      .fw_gfx_me = {
         .ver = 52,
         .feature = 35,
      },
      .fw_gfx_pfp = {
         .ver = 77,
         .feature = 35,
      },
      .fw_gfx_mec = {
         .ver = 134,
         .feature = 35,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x02010001,
         0x263d, 0x0000ff00, 0x00000000,
         0xa0d4, 0x0000ff00, 0x00000000,
         0xa0d5, 0x0000ff00, 0x00000000,
         0x09d8, 0xffffffff, 0x00007111,
         0x2644, 0xffffffff, 0x00800010,
         0x2645, 0xffffffff, 0x00800810,
         0x2646, 0xffffffff, 0x00801010,
         0x2647, 0xffffffff, 0x00801810,
         0x2648, 0xffffffff, 0x00802810,
         0x2649, 0xffffffff, 0x00802808,
         0x264a, 0xffffffff, 0x00802814,
         0x264b, 0xffffffff, 0x00000000,
         0x264c, 0xffffffff, 0x00000004,
         0x264d, 0xffffffff, 0x02000008,
         0x264e, 0xffffffff, 0x02000010,
         0x264f, 0xffffffff, 0x06000014,
         0x2650, 0xffffffff, 0x00000000,
         0x2651, 0xffffffff, 0x02400008,
         0x2652, 0xffffffff, 0x02400010,
         0x2653, 0xffffffff, 0x02400030,
         0x2654, 0xffffffff, 0x06400014,
         0x2655, 0xffffffff, 0x00000000,
         0x2656, 0xffffffff, 0x0040000c,
         0x2657, 0xffffffff, 0x0100000c,
         0x2658, 0xffffffff, 0x0100001c,
         0x2659, 0xffffffff, 0x01000034,
         0x265a, 0xffffffff, 0x01000024,
         0x265b, 0xffffffff, 0x00000000,
         0x265c, 0xffffffff, 0x0040001c,
         0x265d, 0xffffffff, 0x01000020,
         0x265e, 0xffffffff, 0x01000038,
         0x265f, 0xffffffff, 0x02c00008,
         0x2660, 0xffffffff, 0x02c00010,
         0x2661, 0xffffffff, 0x06c00014,
         0x2662, 0xffffffff, 0x00000000,
         0x2663, 0xffffffff, 0x00000000,
         0x2664, 0xffffffff, 0x000000a8,
         0x2665, 0xffffffff, 0x000000a4,
         0x2666, 0xffffffff, 0x00000090,
         0x2667, 0xffffffff, 0x00000090,
         0x2668, 0xffffffff, 0x00000090,
         0x2669, 0xffffffff, 0x00000090,
         0x266a, 0xffffffff, 0x00000090,
         0x266b, 0xffffffff, 0x00000000,
         0x266c, 0xffffffff, 0x000000ee,
         0x266d, 0xffffffff, 0x000000ea,
         0x266e, 0xffffffff, 0x000000e9,
         0x266f, 0xffffffff, 0x000000e5,
         0x2670, 0xffffffff, 0x000000e4,
         0x2671, 0xffffffff, 0x000000e0,
         0x2672, 0xffffffff, 0x00000090,
         0x2673, 0xffffffff, 0x00000000,
      },
      .mmr_reg_count = 53,
      .dev = {
         .device_id = 0x98e4,
         .external_rev = 0x61,
         .pci_rev = 0xeb,
         .family = AMDGPU_FAMILY_CZ,
         .num_shader_engines = 1,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 48000,
         .max_engine_clock = 600000,
         .max_memory_clock = 933000,
         .cu_active_number = 3,
         .cu_ao_mask = 0x3,
         .cu_bitmap[0][0] = 0x7,
         .enabled_rb_pipes_mask = 0x1,
         .num_rb_pipes = 1,
         .num_hw_gfx_contexts = 8,
         .ids_flags = 0x1,
         .virtual_address_offset = 0x200000,
         .virtual_address_max = 0xfffe00000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 32768,
         .vram_bit_width = 64,
         .vce_harvest_config = 2,
         .wave_front_size = 64,
         .num_shader_visible_vgprs = 256,
         .num_cu_per_sh = 3,
         .num_tcc_blocks = 2,
         .max_gs_waves_per_vgt = 16,
         .cu_ao_bitmap[0][0] = 0x3,
      },
      .mem = {
         .vram = {
            .total_heap_size = 16ull << 20,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 16ull << 20,
         },
         .gtt = {
            .total_heap_size = 3072ull << 20,
         },
      },
   },
   {
      .name = "vangogh",
      .radeon_family = CHIP_VANGOGH,
      .hw_ip_gfx = {
         .hw_ip_version_major = 10,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0x0000,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 10,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0x0000,
      },
      .fw_gfx_me = {
         .ver = 64,
         .feature = 41,
      },
      .fw_gfx_pfp = {
         .ver = 95,
         .feature = 41,
      },
      .fw_gfx_mec = {
         .ver = 98,
         .feature = 41,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x00000142,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x163f,
         .chip_rev = 0x00,
         .external_rev = 0x01,
         .pci_rev = 0xae,
         .family = AMDGPU_FAMILY_VGH,
         .num_shader_engines = 1,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 100000,
         .max_engine_clock = 0llu,
         .max_memory_clock = 0llu,
         .cu_active_number = 8,
         .cu_ao_mask = 0xff,
         .cu_bitmap = {
            { 0xff, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .enabled_rb_pipes_mask = 0x3,
         .num_rb_pipes = 2,
         .num_hw_gfx_contexts = 8,
         .pcie_gen = 0,
         .ids_flags = 0x1llu,
         .virtual_address_offset = 0x200000llu,
         .virtual_address_max = 0x800000000000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 65536,
         .vram_type = 10,
         .vram_bit_width = 256,
         .vce_harvest_config = 0,
         .gc_double_offchip_lds_buf = 1,
         .prim_buf_gpu_addr = 0llu,
         .pos_buf_gpu_addr = 0llu,
         .cntl_sb_buf_gpu_addr = 0llu,
         .param_buf_gpu_addr = 0llu,
         .prim_buf_size = 0,
         .pos_buf_size = 0,
         .cntl_sb_buf_size = 0,
         .param_buf_size = 0,
         .wave_front_size = 32,
         .num_shader_visible_vgprs = 1024,
         .num_cu_per_sh = 8,
         .num_tcc_blocks = 4,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .pcie_num_lanes = 0,
         .cu_ao_bitmap = {
            { 0xff, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .high_va_offset = 0xffff800000000000llu,
         .high_va_max = 0xffffffffffe00000llu,
         .pa_sc_tile_steering_override = 0,
         .tcc_disabled_mask = 0llu,
         .min_engine_clock = 0llu,
         .min_memory_clock = 0llu,
         .tcp_cache_size = 0,
         .num_sqc_per_wgp = 0,
         .sqc_data_cache_size = 0,
         .sqc_inst_cache_size = 0,
         .gl1c_cache_size = 0,
         .gl2c_cache_size = 0,
         .mall_size = 0llu,
         .enabled_rb_pipes_mask_hi = 0,
      },
      .mem = {
         .vram = {
            .total_heap_size = 1073741824,
            .usable_heap_size = 1040584704,
            .heap_usage = 344141824,
            .max_allocation = 780438528,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 1073741824,
            .usable_heap_size = 1040584704,
            .heap_usage = 344141824,
            .max_allocation = 780438528,
         },
         .gtt = {
            .total_heap_size = 8522825728,
            .usable_heap_size = 8511004672,
            .heap_usage = 79179776,
            .max_allocation = 6383253504,
         },
      },
   },
   {
      .name = "raphael_mendocino",
      .radeon_family = CHIP_RAPHAEL_MENDOCINO,
      .hw_ip_gfx = {
         .hw_ip_version_major = 10,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0xa0306,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 10,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0xa0306,
      },
      .fw_gfx_me = {
         .ver = 13,
         .feature = 38,
      },
      .fw_gfx_pfp = {
         .ver = 13,
         .feature = 38,
      },
      .fw_gfx_mec = {
         .ver = 18,
         .feature = 38,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x00000042,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x164e,
         .chip_rev = 0x01,
         .external_rev = 0x02,
         .pci_rev = 0xc1,
         .family = AMDGPU_FAMILY_GC_10_3_6,
         .num_shader_engines = 1,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 100000,
         .max_engine_clock = 200000llu,
         .max_memory_clock = 2400000llu,
         .cu_active_number = 2,
         .cu_ao_mask = 0x3,
         .cu_bitmap = {
            { 0x3, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .enabled_rb_pipes_mask = 0x1,
         .num_rb_pipes = 1,
         .num_hw_gfx_contexts = 8,
         .pcie_gen = 4,
         .ids_flags = 0x1llu,
         .virtual_address_offset = 0x200000llu,
         .virtual_address_max = 0x800000000000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 65536,
         .vram_type = 10,
         .vram_bit_width = 128,
         .vce_harvest_config = 0,
         .gc_double_offchip_lds_buf = 1,
         .prim_buf_gpu_addr = 0llu,
         .pos_buf_gpu_addr = 0llu,
         .cntl_sb_buf_gpu_addr = 0llu,
         .param_buf_gpu_addr = 0llu,
         .prim_buf_size = 0,
         .pos_buf_size = 0,
         .cntl_sb_buf_size = 0,
         .param_buf_size = 0,
         .wave_front_size = 32,
         .num_shader_visible_vgprs = 1024,
         .num_cu_per_sh = 2,
         .num_tcc_blocks = 2,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .pcie_num_lanes = 16,
         .cu_ao_bitmap = {
            { 0x3, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .high_va_offset = 0xffff800000000000llu,
         .high_va_max = 0xffffffffffe00000llu,
         .pa_sc_tile_steering_override = 0,
         .tcc_disabled_mask = 0llu,
         .min_engine_clock = 200000llu,
         .min_memory_clock = 2400000llu,
         .tcp_cache_size = 0,
         .num_sqc_per_wgp = 0,
         .sqc_data_cache_size = 0,
         .sqc_inst_cache_size = 0,
         .gl1c_cache_size = 0,
         .gl2c_cache_size = 0,
         .mall_size = 0llu,
         .enabled_rb_pipes_mask_hi = 0,
      },
      .mem = {
         .vram = {
            .total_heap_size = 536870912,
            .usable_heap_size = 512081920,
            .heap_usage = 30093312,
            .max_allocation = 384061440,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 536870912,
            .usable_heap_size = 512081920,
            .heap_usage = 30093312,
            .max_allocation = 384061440,
         },
         .gtt = {
            .total_heap_size = 33254252544,
            .usable_heap_size = 33241997312,
            .heap_usage = 14360576,
            .max_allocation = 24931497984,
         },
      },
   },
   {
      .name = "polaris12",
      .radeon_family = CHIP_POLARIS12,
      .hw_ip_gfx = {
         .hw_ip_version_major = 8,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0x0000,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 8,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0x0000,
      },
      .fw_gfx_me = {
         .ver = 167,
         .feature = 49,
      },
      .fw_gfx_pfp = {
         .ver = 254,
         .feature = 49,
      },
      .fw_gfx_mec = {
         .ver = 730,
         .feature = 49,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x22011002,
         0x263d, 0x0000ff00, 0x00000001,
         0xa0d4, 0x0000ff00, 0x16000012,
         0xa0d5, 0x0000ff00, 0x00000000,
         0x263d, 0x0000ff01, 0x00000001,
         0xa0d4, 0x0000ff01, 0x16000012,
         0xa0d5, 0x0000ff01, 0x00000000,
         0x09d8, 0xffffffff, 0x000060a2,
         0x2644, 0xffffffff, 0x00800150,
         0x2645, 0xffffffff, 0x00800950,
         0x2646, 0xffffffff, 0x00801150,
         0x2647, 0xffffffff, 0x00801950,
         0x2648, 0xffffffff, 0x00802950,
         0x2649, 0xffffffff, 0x00802948,
         0x264a, 0xffffffff, 0x00802954,
         0x264b, 0xffffffff, 0x00802954,
         0x264c, 0xffffffff, 0x00000144,
         0x264d, 0xffffffff, 0x02000148,
         0x264e, 0xffffffff, 0x02000150,
         0x264f, 0xffffffff, 0x06000154,
         0x2650, 0xffffffff, 0x06000154,
         0x2651, 0xffffffff, 0x02400148,
         0x2652, 0xffffffff, 0x02400150,
         0x2653, 0xffffffff, 0x02400170,
         0x2654, 0xffffffff, 0x06400154,
         0x2655, 0xffffffff, 0x06400154,
         0x2656, 0xffffffff, 0x0040014c,
         0x2657, 0xffffffff, 0x0100014c,
         0x2658, 0xffffffff, 0x0100015c,
         0x2659, 0xffffffff, 0x01000174,
         0x265a, 0xffffffff, 0x01000164,
         0x265b, 0xffffffff, 0x01000164,
         0x265c, 0xffffffff, 0x0040015c,
         0x265d, 0xffffffff, 0x01000160,
         0x265e, 0xffffffff, 0x01000178,
         0x265f, 0xffffffff, 0x02c00148,
         0x2660, 0xffffffff, 0x02c00150,
         0x2661, 0xffffffff, 0x06c00154,
         0x2662, 0xffffffff, 0x06c00154,
         0x2663, 0xffffffff, 0x00000000,
         0x2664, 0xffffffff, 0x000000e8,
         0x2665, 0xffffffff, 0x000000e8,
         0x2666, 0xffffffff, 0x000000e8,
         0x2667, 0xffffffff, 0x000000e4,
         0x2668, 0xffffffff, 0x000000d0,
         0x2669, 0xffffffff, 0x000000d0,
         0x266a, 0xffffffff, 0x000000d0,
         0x266b, 0xffffffff, 0x00000000,
         0x266c, 0xffffffff, 0x000000ed,
         0x266d, 0xffffffff, 0x000000e9,
         0x266e, 0xffffffff, 0x000000e8,
         0x266f, 0xffffffff, 0x000000e4,
         0x2670, 0xffffffff, 0x000000d0,
         0x2671, 0xffffffff, 0x00000090,
         0x2672, 0xffffffff, 0x00000040,
         0x2673, 0xffffffff, 0x00000000,
      },
      .mmr_reg_count = 56,
      .dev = {
         .device_id = 0x699f,
         .chip_rev = 0x00,
         .external_rev = 0x64,
         .pci_rev = 0xc7,
         .family = AMDGPU_FAMILY_VI,
         .num_shader_engines = 2,
         .num_shader_arrays_per_engine = 1,
         .gpu_counter_freq = 25000,
         .max_engine_clock = 1183000llu,
         .max_memory_clock = 1750000llu,
         .cu_active_number = 8,
         .cu_ao_mask = 0x1e001e,
         .cu_bitmap = {
            { 0x1e, 0x0, 0x0, 0x0, },
            { 0x1e, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .enabled_rb_pipes_mask = 0xf,
         .num_rb_pipes = 4,
         .num_hw_gfx_contexts = 8,
         .pcie_gen = 3,
         .ids_flags = 0x0llu,
         .virtual_address_offset = 0x200000llu,
         .virtual_address_max = 0x3fffe00000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 32768,
         .vram_type = 5,
         .vram_bit_width = 128,
         .vce_harvest_config = 2,
         .gc_double_offchip_lds_buf = 1,
         .prim_buf_gpu_addr = 0llu,
         .pos_buf_gpu_addr = 0llu,
         .cntl_sb_buf_gpu_addr = 0llu,
         .param_buf_gpu_addr = 0llu,
         .prim_buf_size = 0,
         .pos_buf_size = 0,
         .cntl_sb_buf_size = 0,
         .param_buf_size = 0,
         .wave_front_size = 64,
         .num_shader_visible_vgprs = 256,
         .num_cu_per_sh = 5,
         .num_tcc_blocks = 4,
         .gs_vgt_table_depth = 0,
         .gs_prim_buffer_depth = 0,
         .max_gs_waves_per_vgt = 32,
         .pcie_num_lanes = 1,
         .cu_ao_bitmap = {
            { 0x1e, 0x0, 0x0, 0x0, },
            { 0x1e, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .high_va_offset = 0x0llu,
         .high_va_max = 0x0llu,
         .pa_sc_tile_steering_override = 0,
         .tcc_disabled_mask = 0llu,
         .min_engine_clock = 214000llu,
         .min_memory_clock = 300000llu,
         .tcp_cache_size = 0,
         .num_sqc_per_wgp = 0,
         .sqc_data_cache_size = 0,
         .sqc_inst_cache_size = 0,
         .gl1c_cache_size = 0,
         .gl2c_cache_size = 0,
         .mall_size = 0llu,
         .enabled_rb_pipes_mask_hi = 0,
      },
      .mem = {
         .vram = {
            .total_heap_size = 4294967296,
            .usable_heap_size = 4281139200,
            .heap_usage = 5963776,
            .max_allocation = 3210854400,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 4294967296,
            .usable_heap_size = 4281139200,
            .heap_usage = 5963776,
            .max_allocation = 3210854400,
         },
         .gtt = {
            .total_heap_size = 33254252544,
            .usable_heap_size = 33249120256,
            .heap_usage = 17903616,
            .max_allocation = 24936840192,
         },
      },
   },
   {
      .name = "gfx1100",
      .radeon_family = CHIP_GFX1100,
      .hw_ip_gfx = {
         .hw_ip_version_major = 11,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0xb0000,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 11,
         .hw_ip_version_minor = 0,
         .capabilities_flags = 0llu,
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0xb0000,
      },
      .fw_gfx_me = {
         .ver = 1486,
         .feature = 29,
      },
      .fw_gfx_pfp = {
         .ver = 1525,
         .feature = 29,
      },
      .fw_gfx_mec = {
         .ver = 494,
         .feature = 29,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x00000545,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x744c,
         .chip_rev = 0x00,
         .external_rev = 0x01,
         .pci_rev = 0xc8,
         .family = AMDGPU_FAMILY_GC_11_0_0,
         .num_shader_engines = 6,
         .num_shader_arrays_per_engine = 2,
         .gpu_counter_freq = 100000,
         .max_engine_clock = 2371000llu,
         .max_memory_clock = 1249000llu,
         .cu_active_number = 96,
         .cu_ao_mask = 0x0,
         .cu_bitmap = {
            { 0xff, 0xff, 0xff, 0xff, },
            { 0xff, 0xff, 0xff, 0xff, },
            { 0xff, 0xff, 0x0, 0x0, },
            { 0xff, 0xff, 0x0, 0x0, },
         },
         .enabled_rb_pipes_mask = 0xffffff,
         .num_rb_pipes = 24,
         .num_hw_gfx_contexts = 8,
         .pcie_gen = 4,
         .ids_flags = 0x0llu,
         .virtual_address_offset = 0x200000llu,
         .virtual_address_max = 0x800000000000llu,
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 0,
         .vram_type = 9,
         .vram_bit_width = 384,
         .vce_harvest_config = 0,
         .gc_double_offchip_lds_buf = 0,
         .prim_buf_gpu_addr = 0llu,
         .pos_buf_gpu_addr = 0llu,
         .cntl_sb_buf_gpu_addr = 0llu,
         .param_buf_gpu_addr = 0llu,
         .prim_buf_size = 0,
         .pos_buf_size = 0,
         .cntl_sb_buf_size = 0,
         .param_buf_size = 0,
         .wave_front_size = 32,
         .num_shader_visible_vgprs = 1536,
         .num_cu_per_sh = 8,
         .num_tcc_blocks = 24,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .pcie_num_lanes = 16,
         .cu_ao_bitmap = {
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .high_va_offset = 0xffff800000000000llu,
         .high_va_max = 0xffffffffffe00000llu,
         .pa_sc_tile_steering_override = 0,
         .tcc_disabled_mask = 0llu,
         .min_engine_clock = 500000llu,
         .min_memory_clock = 96000llu,
         .tcp_cache_size = 32,
         .num_sqc_per_wgp = 1,
         .sqc_data_cache_size = 16,
         .sqc_inst_cache_size = 32,
         .gl1c_cache_size = 256,
         .gl2c_cache_size = 6144,
         .mall_size = 100663296llu,
         .enabled_rb_pipes_mask_hi = 0,
      },
      .mem = {
         .vram = {
            .total_heap_size = 25753026560,
            .usable_heap_size = 25681096704,
            .heap_usage = 7515435008,
            .max_allocation = 19260822528,
         },
         .cpu_accessible_vram = {
            .total_heap_size = 25753026560,
            .usable_heap_size = 25681096704,
            .heap_usage = 7515435008,
            .max_allocation = 19260822528,
         },
         .gtt = {
            .total_heap_size = 33254252544,
            .usable_heap_size = 33240895488,
            .heap_usage = 142462976,
            .max_allocation = 24930671616,
         },
      },
   }
};

static void
amdgpu_select_device()
{
   const char *gpu_id = getenv("AMDGPU_GPU_ID");
   if (gpu_id) {
      for (uint32_t i = 0; i < ARRAY_SIZE(amdgpu_devices); i++) {
         const struct amdgpu_device *dev = &amdgpu_devices[i];
         if (!strcasecmp(dev->name, gpu_id)) {
            amdgpu_dev = &amdgpu_devices[i];
            break;
         }
      }
   } else {
      amdgpu_dev = &amdgpu_devices[0];
   }

   if (!amdgpu_dev) {
      mesa_loge("Failed to find amdgpu GPU named \"%s\"\n", gpu_id);
      abort();
   }
}

void
drm_shim_driver_init(void)
{
   amdgpu_select_device();

   shim_device.bus_type = DRM_BUS_PCI;
   shim_device.driver_name = "amdgpu";
   shim_device.driver_ioctls = amdgpu_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(amdgpu_ioctls);

   shim_device.version_major = 3;
   shim_device.version_minor = 49;
   shim_device.version_patchlevel = 0;

   /* make drmGetDevices2 and drmProcessPciDevice happy */
   static const char uevent_content[] =
      "DRIVER=amdgpu\n"
      "PCI_CLASS=30000\n"
      "PCI_ID=1002:15E7\n"
      "PCI_SUBSYS_ID=1028:1636\n"
      "PCI_SLOT_NAME=0000:04:00.0\n"
      "MODALIAS=pci:v00001002d000015E7sv00001002sd00001636bc03sc00i00\n";
   drm_shim_override_file(uevent_content, "/sys/dev/char/%d:%d/device/uevent", DRM_MAJOR,
                          render_node_minor);
   drm_shim_override_file("0xe9\n", "/sys/dev/char/%d:%d/device/revision", DRM_MAJOR,
                          render_node_minor);
   drm_shim_override_file("0x1002", "/sys/dev/char/%d:%d/device/vendor", DRM_MAJOR,
                          render_node_minor);
   drm_shim_override_file("0x15e7", "/sys/dev/char/%d:%d/device/device", DRM_MAJOR,
                          render_node_minor);
   drm_shim_override_file("0x1002", "/sys/dev/char/%d:%d/device/subsystem_vendor", DRM_MAJOR,
                          render_node_minor);
   drm_shim_override_file("0x1636", "/sys/dev/char/%d:%d/device/subsystem_device", DRM_MAJOR,
                          render_node_minor);
}

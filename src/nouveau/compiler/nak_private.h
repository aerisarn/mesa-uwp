/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef NAK_PRIVATE_H
#define NAK_PRIVATE_H

#include "nak.h"
#include "nir.h"
#include "nv_device_info.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nak_compiler {
   uint8_t sm;

   struct nir_shader_compiler_options nir_options;
};

struct nak_io_addr_offset {
   nir_scalar base;
   int32_t offset;
};

struct nak_io_addr_offset
nak_get_io_addr_offset(nir_def *addr, uint8_t imm_bits);

enum nak_nir_lod_mode {
   NAK_NIR_LOD_MODE_AUTO = 0,
   NAK_NIR_LOD_MODE_ZERO,
   NAK_NIR_LOD_MODE_BIAS,
   NAK_NIR_LOD_MODE_LOD,
   NAK_NIR_LOD_MODE_CLAMP,
   NAK_NIR_LOD_MODE_BIAS_CLAMP,
};

enum nak_nir_offset_mode {
   NAK_NIR_OFFSET_MODE_NONE = 0,
   NAK_NIR_OFFSET_MODE_AOFFI,
   NAK_NIR_OFFSET_MODE_PER_PX,
};

struct nak_nir_tex_flags {
   enum nak_nir_lod_mode lod_mode:3;
   enum nak_nir_offset_mode offset_mode:2;
   bool has_z_cmpr:1;
   uint32_t pad:26;
};

bool nak_nir_lower_tex(nir_shader *nir, const struct nak_compiler *nak);

#ifdef __cplusplus
}
#endif

#endif /* NAK_PRIVATE */

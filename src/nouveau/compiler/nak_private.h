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

#ifdef __cplusplus
}
#endif

#endif /* NAK_PRIVATE */

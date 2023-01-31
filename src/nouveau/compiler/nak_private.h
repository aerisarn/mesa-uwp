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

#ifdef __cplusplus
}
#endif

#endif /* NAK_PRIVATE */

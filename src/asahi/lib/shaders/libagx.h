/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBAGX_H
#define LIBAGX_H

/* Define stdint types compatible between the CPU and GPU for shared headers */
#ifndef __OPENCL_VERSION__
#include <stdint.h>
#else
typedef ulong uint64_t;
typedef uint uint32_t;
typedef ushort uint16_t;
typedef uint uint8_t;

typedef long int64_t;
typedef int int32_t;
typedef short int16_t;
typedef int int8_t;
#endif

#endif

/*
 * Copyright © 2009 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "glsl_types.h"
#include "util/glheader.h"

#define DECL_TYPE(NAME, ...)                                    \
const struct glsl_type glsl_type_builtin_##NAME = __VA_ARGS__;

#include "compiler/builtin_type_macros.h"
#undef DECL_TYPE

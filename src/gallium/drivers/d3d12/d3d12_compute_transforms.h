/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef D3D12_COMPUTE_TRANSFORMS_H
#define D3D12_COMPUTE_TRANSFORMS_H

#include "d3d12_context.h"
#include "d3d12_compiler.h"

enum class d3d12_compute_transform_type
{
   base_vertex,
   max,
};

struct d3d12_compute_transform_key
{
   d3d12_compute_transform_type type;

   struct {
      unsigned indexed:1;
      unsigned dynamic_count:1;
   } base_vertex;
};

d3d12_shader_selector *
d3d12_get_compute_transform(struct d3d12_context *ctx, const d3d12_compute_transform_key *key);

void
d3d12_compute_transform_cache_init(struct d3d12_context *ctx);

void
d3d12_compute_transform_cache_destroy(struct d3d12_context *ctx);

#endif

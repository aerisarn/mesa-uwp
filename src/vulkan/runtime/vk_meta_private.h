/*
 * Copyright © 2022 Collabora Ltd
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
#ifndef VK_META_PRIVATE_H
#define VK_META_PRIVATE_H

#include "vk_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const VkPipelineVertexInputStateCreateInfo vk_meta_draw_rects_vi_state;
extern const VkPipelineInputAssemblyStateCreateInfo vk_meta_draw_rects_ia_state;
extern const VkPipelineViewportStateCreateInfo vk_meta_draw_rects_vs_state;

struct nir_shader *vk_meta_draw_rects_vs_nir(struct vk_meta_device *device);

#ifdef __cplusplus
}
#endif

#endif /* VK_META_PRIVATE_H */

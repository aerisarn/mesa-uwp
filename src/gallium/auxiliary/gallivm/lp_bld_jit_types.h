/*
 * Copyright 2022 Red Hat.
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

#ifndef LP_BLD_JIT_TYPES_H
#define LP_BLD_JIT_TYPES_H

struct lp_jit_buffer
{
   union {
      const uint32_t *u;
      const float *f;
   };
   uint32_t num_elements;
};

enum {
   LP_JIT_BUFFER_BASE = 0,
   LP_JIT_BUFFER_NUM_ELEMENTS,
   LP_JIT_BUFFER_NUM_FIELDS,
};

LLVMTypeRef
lp_build_create_jit_buffer_type(struct gallivm_state *gallivm);

LLVMValueRef
lp_llvm_buffer_base(struct gallivm_state *gallivm,
                    LLVMValueRef buffers_ptr,
                    LLVMValueRef buffers_offset, unsigned buffers_limit);

LLVMValueRef
lp_llvm_buffer_num_elements(struct gallivm_state *gallivm,
                            LLVMValueRef buffers_ptr,
                            LLVMValueRef buffers_offset, unsigned buffers_limit);

struct lp_jit_texture
{
   const void *base;
   uint32_t width;        /* same as number of elements */
   uint32_t height;
   uint32_t depth;        /* doubles as array size */
   uint32_t num_samples;
   uint32_t sample_stride;
   uint32_t row_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint32_t img_stride[PIPE_MAX_TEXTURE_LEVELS];
   uint32_t first_level;
   uint32_t last_level;
   uint32_t mip_offsets[PIPE_MAX_TEXTURE_LEVELS];
};

enum {
   LP_JIT_TEXTURE_BASE = 0,
   LP_JIT_TEXTURE_WIDTH,
   LP_JIT_TEXTURE_HEIGHT,
   LP_JIT_TEXTURE_DEPTH,
   LP_JIT_TEXTURE_NUM_SAMPLES,
   LP_JIT_TEXTURE_SAMPLE_STRIDE,
   LP_JIT_TEXTURE_ROW_STRIDE,
   LP_JIT_TEXTURE_IMG_STRIDE,
   LP_JIT_TEXTURE_FIRST_LEVEL,
   LP_JIT_TEXTURE_LAST_LEVEL,
   LP_JIT_TEXTURE_MIP_OFFSETS,
   LP_JIT_TEXTURE_NUM_FIELDS  /* number of fields above */
};

LLVMTypeRef
lp_build_create_jit_texture_type(struct gallivm_state *gallivm);

struct lp_jit_sampler
{
   float min_lod;
   float max_lod;
   float lod_bias;
   float border_color[4];
   float max_aniso;
};

enum {
   LP_JIT_SAMPLER_MIN_LOD,
   LP_JIT_SAMPLER_MAX_LOD,
   LP_JIT_SAMPLER_LOD_BIAS,
   LP_JIT_SAMPLER_BORDER_COLOR,
   LP_JIT_SAMPLER_MAX_ANISO,
   LP_JIT_SAMPLER_NUM_FIELDS  /* number of fields above */
};

LLVMTypeRef
lp_build_create_jit_sampler_type(struct gallivm_state *gallivm);

struct lp_jit_image
{
   const void *base;
   uint32_t width;        /* same as number of elements */
   uint32_t height;
   uint32_t depth;
   uint32_t num_samples;
   uint32_t sample_stride;
   uint32_t row_stride;
   uint32_t img_stride;
};

enum {
   LP_JIT_IMAGE_BASE = 0,
   LP_JIT_IMAGE_WIDTH,
   LP_JIT_IMAGE_HEIGHT,
   LP_JIT_IMAGE_DEPTH,
   LP_JIT_IMAGE_NUM_SAMPLES,
   LP_JIT_IMAGE_SAMPLE_STRIDE,
   LP_JIT_IMAGE_ROW_STRIDE,
   LP_JIT_IMAGE_IMG_STRIDE,
   LP_JIT_IMAGE_NUM_FIELDS  /* number of fields above */
};

LLVMTypeRef
lp_build_create_jit_image_type(struct gallivm_state *gallivm);

#endif

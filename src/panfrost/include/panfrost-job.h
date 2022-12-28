/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
 * © Copyright 2017-2018 Connor Abbott
 * © Copyright 2017-2018 Lyude Paul
 * © Copyright2019 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PANFROST_JOB_H__
#define __PANFROST_JOB_H__

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t mali_ptr;

#define MALI_FORMAT_COMPRESSED (0 << 5)
#define MALI_EXTRACT_TYPE(fmt) ((fmt)&0xe0)
#define MALI_EXTRACT_INDEX(pixfmt) (((pixfmt) >> 12) & 0xFF)

/* Purposeful off-by-one in width, height fields. For example, a (64, 64)
 * texture is stored as (63, 63) in these fields. This adjusts for that.
 * There's an identical pattern in the framebuffer descriptor. Even vertex
 * count fields work this way, hence the generic name -- integral fields that
 * are strictly positive generally need this adjustment. */

#define MALI_POSITIVE(dim) (dim - 1)

/* Mali hardware can texture up to 65536 x 65536 x 65536 and render up to 16384
 * x 16384, but 8192 x 8192 should be enough for anyone.  The OpenGL game
 * "Cathedral" requires a texture of width 8192 to start.
 */
#define MAX_MIP_LEVELS (14)

/* Used for lod encoding. Thanks @urjaman for pointing out these routines can
 * be cleaned up a lot. */

#define DECODE_FIXED_16(x) ((float)(x / 256.0))

static inline int16_t
FIXED_16(float x, bool allow_negative)
{
   /* Clamp inputs, accounting for float error */
   float max_lod = (32.0 - (1.0 / 512.0));
   float min_lod = allow_negative ? -max_lod : 0.0;

   x = ((x > max_lod) ? max_lod : ((x < min_lod) ? min_lod : x));

   return (int)(x * 256.0);
}

#endif /* __PANFROST_JOB_H__ */

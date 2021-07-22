/*
 * Copyright (C) 2021 Collabora Ltd.
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
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#ifndef __VALHALL_COMPILER_H
#define __VALHALL_COMPILER_H

#include "compiler.h"
#include "valhall.h"

#ifdef __cplusplus
extern "C" {
#endif

void va_fuse_add_imm(bi_instr *I);
uint64_t va_pack_instr(const bi_instr *I, unsigned flow);

static inline unsigned
va_fau_page(enum bir_fau value)
{
   /* Uniform slots of FAU have a 7-bit index. The top 2-bits are the page; the
    * bottom 5-bits are specified in the source.
    */
   if (value & BIR_FAU_UNIFORM) {
      unsigned slot = value & ~BIR_FAU_UNIFORM;
      unsigned page = slot >> 5;

      assert(page <= 3);
      return page;
   }

   /* Special indices are also paginated */
   switch (value) {
   case BIR_FAU_TLS_PTR:
   case BIR_FAU_WLS_PTR:
      return 1;
   case BIR_FAU_LANE_ID:
   case BIR_FAU_CORE_ID:
   case BIR_FAU_PROGRAM_COUNTER:
      return 3;
   default:
      return 0;
   }
}

static inline unsigned
va_select_fau_page(const bi_instr *I)
{
   bi_foreach_src(I, s) {
      if (I->src[s].type == BI_INDEX_FAU)
         return va_fau_page((enum bir_fau) I->src[s].value);
   }

   return 0;
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif

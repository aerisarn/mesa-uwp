/*
 * Copyright (C) 2021 Collabora, Ltd.
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
 */

#include "compiler.h"
#include "bi_test.h"
#include "bi_builder.h"

#define CASE(instr, expected) do { \
   bi_builder *A = bit_builder(ralloc_ctx); \
   bi_builder *B = bit_builder(ralloc_ctx); \
   { \
      bi_builder *b = A; \
      instr; \
   } \
   { \
      bi_builder *b = B; \
      expected; \
   } \
   bi_opt_mod_prop_forward(A->shader); \
   bi_opt_mod_prop_backward(A->shader); \
   bi_opt_dead_code_eliminate(A->shader); \
   if (bit_shader_equal(A->shader, B->shader)) { \
      nr_pass++; \
   } else { \
      fprintf(stderr, "Got:\n"); \
      bi_print_shader(A->shader, stderr); \
      fprintf(stderr, "Expected:\n"); \
      bi_print_shader(B->shader, stderr); \
      fprintf(stderr, "\n"); \
      nr_fail++; \
   } \
} while(0)

#define NEGCASE(instr) CASE(instr, instr)

int
main(int argc, const char **argv)
{
   unsigned nr_fail = 0, nr_pass = 0;
   void *ralloc_ctx = ralloc_context(NULL);
   bi_index zero = bi_zero();
   bi_index reg = bi_register(0);
   bi_index x = bi_register(1);
   bi_index y = bi_register(2);
   bi_index negabsx = bi_neg(bi_abs(x));

   /* Check absneg is fused */

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, bi_abs(x)), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, bi_abs(x), y, BI_ROUND_NONE));

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, bi_neg(x)), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, bi_neg(x), y, BI_ROUND_NONE));

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, negabsx), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, negabsx, y, BI_ROUND_NONE));

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, x), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, x, y, BI_ROUND_NONE));

   /* Check absneg is fused on a variety of instructions */

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, negabsx), y, BI_ROUND_RTP),
        bi_fadd_f32_to(b, reg, negabsx, y, BI_ROUND_RTP));

   CASE(bi_fmin_f32_to(b, reg, bi_fabsneg_f32(b, negabsx), bi_neg(y)),
        bi_fmin_f32_to(b, reg, negabsx, bi_neg(y)));

   /* Check absneg is fused on fp16 */

   CASE(bi_fadd_v2f16_to(b, reg, bi_fabsneg_v2f16(b, negabsx), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, negabsx, y, BI_ROUND_RTP));

   CASE(bi_fmin_v2f16_to(b, reg, bi_fabsneg_v2f16(b, negabsx), bi_neg(y)),
        bi_fmin_v2f16_to(b, reg, negabsx, bi_neg(y)));

   /* Check that swizzles are composed for fp16 */

   CASE(bi_fadd_v2f16_to(b, reg, bi_fabsneg_v2f16(b, bi_swz_16(negabsx, true, false)), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, bi_swz_16(negabsx, true, false), y, BI_ROUND_RTP));

   CASE(bi_fadd_v2f16_to(b, reg, bi_swz_16(bi_fabsneg_v2f16(b, negabsx), true, false), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, bi_swz_16(negabsx, true, false), y, BI_ROUND_RTP));

   CASE(bi_fadd_v2f16_to(b, reg, bi_swz_16(bi_fabsneg_v2f16(b, bi_swz_16(negabsx, true, false)), true, false), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, negabsx, y, BI_ROUND_RTP));

   CASE(bi_fadd_v2f16_to(b, reg, bi_swz_16(bi_fabsneg_v2f16(b, bi_half(negabsx, false)), true, false), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, bi_half(negabsx, false), y, BI_ROUND_RTP));

   CASE(bi_fadd_v2f16_to(b, reg, bi_swz_16(bi_fabsneg_v2f16(b, bi_half(negabsx, true)), true, false), y, BI_ROUND_RTP),
        bi_fadd_v2f16_to(b, reg, bi_half(negabsx, true), y, BI_ROUND_RTP));

   /* Check that widens are passed through */

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, bi_half(negabsx, false)), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, bi_half(negabsx, false), y, BI_ROUND_NONE));

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, bi_half(negabsx, true)), y, BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, bi_half(negabsx, true), y, BI_ROUND_NONE));

   CASE(bi_fadd_f32_to(b, reg, bi_fabsneg_f32(b, bi_half(x, true)), bi_fabsneg_f32(b, bi_half(x, false)), BI_ROUND_NONE),
        bi_fadd_f32_to(b, reg, bi_half(x, true), bi_half(x, false), BI_ROUND_NONE));

   /* Refuse to mix sizes for fabsneg, that's wrong */

   NEGCASE(bi_fadd_f32_to(b, reg, bi_fabsneg_v2f16(b, negabsx), y, BI_ROUND_NONE));
   NEGCASE(bi_fadd_v2f16_to(b, reg, bi_fabsneg_f32(b, negabsx), y, BI_ROUND_NONE));

   /* It's tempting to use addition by 0.0 as the absneg primitive, but that
    * has footguns around signed zero and round modes. Check we don't
    * incorrectly fuse these rules. */

   NEGCASE(bi_fadd_f32_to(b, reg, bi_fadd_f32(b, bi_abs(x), zero, BI_ROUND_NONE), y, BI_ROUND_NONE));
   NEGCASE(bi_fadd_f32_to(b, reg, bi_fadd_f32(b, bi_neg(x), zero, BI_ROUND_NONE), y, BI_ROUND_NONE));
   NEGCASE(bi_fadd_f32_to(b, reg, bi_fadd_f32(b, bi_neg(bi_abs(x)), zero, BI_ROUND_NONE), y, BI_ROUND_NONE));
   NEGCASE(bi_fadd_f32_to(b, reg, bi_fadd_f32(b, x, zero, BI_ROUND_NONE), y, BI_ROUND_NONE));

   ralloc_free(ralloc_ctx);
   TEST_END(nr_pass, nr_fail);
}

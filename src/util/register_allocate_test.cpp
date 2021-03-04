/*
 * Copyright © 2021 Google LLC
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

#include <gtest/gtest.h>
#include "ralloc.h"
#include "register_allocate.h"
#include "register_allocate_internal.h"

class ra_test : public ::testing::Test {
public:
   void *mem_ctx;

protected:
   ra_test();
   ~ra_test();
};

ra_test::ra_test()
{
   mem_ctx = ralloc_context(NULL);
}

ra_test::~ra_test()
{
   ralloc_free(mem_ctx);
}

TEST_F(ra_test, thumb)
{
   struct ra_regs *regs = ra_alloc_reg_set(mem_ctx, 100, true);

   /* r0..15 are the real HW registers. */
   int next_vreg = 16;

   /* reg32low is any of the low 8 registers. */
   struct ra_class *reg32low = ra_alloc_reg_class(regs);
   for (int i = 0; i < 8; i++) {
      int vreg = next_vreg++;
      ra_class_add_reg(reg32low, vreg);
      ra_add_transitive_reg_conflict(regs, i, vreg);
   }

   /* reg64low is pairs of the low 8 registers (with wraparound!) */
   struct ra_class *reg64low = ra_alloc_reg_class(regs);
   for (int i = 0; i < 8; i++) {
      int vreg = next_vreg++;
      ra_class_add_reg(reg64low, vreg);
      ra_add_transitive_reg_conflict(regs, i, vreg);
      ra_add_transitive_reg_conflict(regs, (i + 1) % 8, vreg);
   }

   /* reg96 is one of either r[0..2] or r[1..3] */
   struct ra_class *reg96 = ra_alloc_reg_class(regs);
   for (int i = 0; i < 2; i++) {
      int vreg = next_vreg++;
      ra_class_add_reg(reg96, vreg);
      for (int j = 0; j < 3; j++)
         ra_add_transitive_reg_conflict(regs, i + j, vreg);
   }

   ra_set_finalize(regs, NULL);

   /* Table 4.1 */
   ASSERT_EQ(reg32low->p, 8);
   ASSERT_EQ(reg32low->q[reg32low->index], 1);
   ASSERT_EQ(reg32low->q[reg64low->index], 2);
   ASSERT_EQ(reg32low->q[reg96->index], 3);
   ASSERT_EQ(reg64low->p, 8);
   ASSERT_EQ(reg64low->q[reg32low->index], 2);
   ASSERT_EQ(reg64low->q[reg64low->index], 3);
   ASSERT_EQ(reg64low->q[reg96->index], 4);
   ASSERT_EQ(reg96->p, 2);
   ASSERT_EQ(reg96->q[reg96->index], 2);
   ASSERT_EQ(reg96->q[reg64low->index], 2);
   ASSERT_EQ(reg96->q[reg96->index], 2);
}

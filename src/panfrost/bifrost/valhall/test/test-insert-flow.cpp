/*
 * Copyright (C) 2022 Collabora, Ltd.
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

#include "bi_test.h"
#include "bi_builder.h"
#include "va_compiler.h"
#include "valhall_enums.h"

#include <gtest/gtest.h>

#define CASE(shader_stage, test, expected) do { \
   bi_builder *A = bit_builder(mem_ctx); \
   bi_builder *B = bit_builder(mem_ctx); \
   { \
      UNUSED bi_builder *b = A; \
      A->shader->stage = MESA_SHADER_ ## shader_stage; \
      test; \
   } \
   va_insert_flow_control_nops(A->shader); \
   { \
      UNUSED bi_builder *b = B; \
      B->shader->stage = MESA_SHADER_ ## shader_stage; \
      expected; \
   } \
   ASSERT_SHADER_EQUAL(A->shader, B->shader); \
} while(0)

#define flow(f) bi_nop(b)->flow = VA_FLOW_ ## f

class InsertFlow : public testing::Test {
protected:
   InsertFlow() {
      mem_ctx = ralloc_context(NULL);
   }

   ~InsertFlow() {
      ralloc_free(mem_ctx);
   }

   void *mem_ctx;
};

TEST_F(InsertFlow, PreserveEmptyShader) {
   CASE(FRAGMENT, {}, {});
}

TEST_F(InsertFlow, TilebufferWait7) {
   CASE(FRAGMENT, {
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        bi_blend_to(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), bi_register(7), bi_register(8),
                    BI_REGISTER_FORMAT_AUTO, 4, 4);
   },
   {
        flow(DISCARD);
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        flow(WAIT);
        bi_blend_to(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), bi_register(7), bi_register(8),
                    BI_REGISTER_FORMAT_AUTO, 4, 4);
        flow(END);
   });

   CASE(FRAGMENT, {
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        bi_st_tile(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), BI_REGISTER_FORMAT_AUTO, BI_VECSIZE_V4);
   },
   {
        flow(DISCARD);
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        flow(WAIT);
        bi_st_tile(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), BI_REGISTER_FORMAT_AUTO, BI_VECSIZE_V4);
        flow(END);
   });

   CASE(FRAGMENT, {
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        bi_ld_tile_to(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), BI_REGISTER_FORMAT_AUTO, BI_VECSIZE_V4);
   },
   {
        flow(DISCARD);
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        flow(WAIT);
        bi_ld_tile_to(b, bi_register(0), bi_register(4), bi_register(5),
                    bi_register(6), BI_REGISTER_FORMAT_AUTO, BI_VECSIZE_V4);
        flow(END);
   });
}

TEST_F(InsertFlow, AtestWait6) {
   CASE(FRAGMENT, {
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        bi_atest_to(b, bi_register(0), bi_register(4), bi_register(5));
   },
   {
        flow(DISCARD);
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        flow(WAIT0126);
        bi_atest_to(b, bi_register(0), bi_register(4), bi_register(5));
        flow(END);
   });
}

TEST_F(InsertFlow, ZSEmitWait6) {
   CASE(FRAGMENT, {
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        bi_zs_emit_to(b, bi_register(0), bi_register(4), bi_register(5),
                      bi_register(6), true, true);
   },
   {
        flow(DISCARD);
        bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
        flow(WAIT0126);
        bi_zs_emit_to(b, bi_register(0), bi_register(4), bi_register(5),
                      bi_register(6), true, true);
        flow(END);
   });
}

TEST_F(InsertFlow, LoadThenUnrelatedThenUse) {
   CASE(VERTEX, {
         bi_ld_attr_imm_to(b, bi_register(16), bi_register(60), bi_register(61),
                           BI_REGISTER_FORMAT_F32, BI_VECSIZE_V4, 1);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(19));
   },
   {
         bi_ld_attr_imm_to(b, bi_register(16), bi_register(60), bi_register(61),
                           BI_REGISTER_FORMAT_F32, BI_VECSIZE_V4, 1);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         flow(WAIT0);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(19));
         flow(END);
   });
}

TEST_F(InsertFlow, SingleLdVar) {
   CASE(FRAGMENT, {
         bi_ld_var_buf_imm_f16_to(b, bi_register(2), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_RETRIEVE, BI_VECSIZE_V4, 0);
   },
   {
         flow(DISCARD);
         bi_ld_var_buf_imm_f16_to(b, bi_register(2), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_RETRIEVE, BI_VECSIZE_V4, 0);
         flow(WAIT0);
         flow(END);
   });
}

TEST_F(InsertFlow, SerializeLdVars) {
   CASE(FRAGMENT, {
         bi_ld_var_buf_imm_f16_to(b, bi_register(16), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_STORE, BI_VECSIZE_V4, 0);

         bi_ld_var_buf_imm_f16_to(b, bi_register(2), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_RETRIEVE, BI_VECSIZE_V4, 0);

         bi_ld_var_buf_imm_f16_to(b, bi_register(8), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_STORE, BI_VECSIZE_V4, 1);
   },
   {
         flow(DISCARD);
         bi_ld_var_buf_imm_f16_to(b, bi_register(16), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_STORE, BI_VECSIZE_V4, 0);
         bi_ld_var_buf_imm_f16_to(b, bi_register(2), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_RETRIEVE, BI_VECSIZE_V4, 0);
         flow(WAIT0);
         bi_ld_var_buf_imm_f16_to(b, bi_register(8), bi_register(61),
                                 BI_REGISTER_FORMAT_F16, BI_SAMPLE_CENTER,
                                 BI_SOURCE_FORMAT_F16,
                                 BI_UPDATE_STORE, BI_VECSIZE_V4, 1);
         flow(WAIT0);
         flow(END);
   });
}

TEST_F(InsertFlow, Clper) {
   CASE(FRAGMENT, {
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_clper_i32_to(b, bi_register(0), bi_register(4), bi_register(8),
                         BI_INACTIVE_RESULT_ZERO, BI_LANE_OP_NONE,
                         BI_SUBGROUP_SUBGROUP4);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
   },
   {
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_clper_i32_to(b, bi_register(0), bi_register(4), bi_register(8),
                         BI_INACTIVE_RESULT_ZERO, BI_LANE_OP_NONE,
                         BI_SUBGROUP_SUBGROUP4);
         flow(DISCARD);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         flow(END);
   });
}

TEST_F(InsertFlow, TextureImplicit) {
   CASE(FRAGMENT, {
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_tex_single_to(b, bi_register(0), bi_register(4), bi_register(8),
                          bi_register(12), false, BI_DIMENSION_2D,
                          BI_REGISTER_FORMAT_F32, false, false,
                          BI_VA_LOD_MODE_COMPUTED_LOD, BI_WRITE_MASK_RGBA, 4);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
   },
   {
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_tex_single_to(b, bi_register(0), bi_register(4), bi_register(8),
                          bi_register(12), false, BI_DIMENSION_2D,
                          BI_REGISTER_FORMAT_F32, false, false,
                          BI_VA_LOD_MODE_COMPUTED_LOD, BI_WRITE_MASK_RGBA, 4);
         flow(DISCARD);
         flow(WAIT0);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         flow(END);
   });
}

TEST_F(InsertFlow, TextureExplicit) {
   CASE(FRAGMENT, {
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_tex_single_to(b, bi_register(0), bi_register(4), bi_register(8),
                          bi_register(12), false, BI_DIMENSION_2D,
                          BI_REGISTER_FORMAT_F32, false, false,
                          BI_VA_LOD_MODE_ZERO_LOD, BI_WRITE_MASK_RGBA, 4);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
   },
   {
         flow(DISCARD);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         bi_tex_single_to(b, bi_register(0), bi_register(4), bi_register(8),
                          bi_register(12), false, BI_DIMENSION_2D,
                          BI_REGISTER_FORMAT_F32, false, false,
                          BI_VA_LOD_MODE_ZERO_LOD, BI_WRITE_MASK_RGBA, 4);
         flow(WAIT0);
         bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
         flow(END);
   });
}

static bi_context *
build_diamond_cfg(void *mem_ctx, bool with_flow)
{
   bi_builder *b = bit_builder(mem_ctx);
   bi_context *ctx = b->shader;
   ctx->stage = MESA_SHADER_FRAGMENT;

   /*      A
    *     / \
    *    B   C
    *     \ /
    *      D
    */

   bi_block *A = bi_start_block(&ctx->blocks);
   bi_block *B = bit_block(ctx);
   bi_block *C = bit_block(ctx);
   bi_block *D = bit_block(ctx);

   bi_block_add_successor(A, B);
   bi_block_add_successor(A, C);

   bi_block_add_successor(B, D);
   bi_block_add_successor(C, D);

   /* B uses helper invocations, no other block does.
    *
    * That means B and C need to discard helpers.
    */
   b->cursor = bi_after_block(B);
   bi_clper_i32_to(b, bi_register(0), bi_register(4), bi_register(8),
                   BI_INACTIVE_RESULT_ZERO, BI_LANE_OP_NONE,
                   BI_SUBGROUP_SUBGROUP4);
   if (with_flow) flow(DISCARD);
   if (with_flow) flow(RECONVERGE);

   b->cursor = bi_after_block(C);
   if (with_flow) flow(DISCARD);
   bi_fadd_f32_to(b, bi_register(0), bi_register(0), bi_register(0));
   if (with_flow) flow(RECONVERGE);

   b->cursor = bi_after_block(D);
   if (with_flow) flow(END);

   return ctx;
}

TEST_F(InsertFlow, DiamondCFG) {
   bi_context *A = build_diamond_cfg(mem_ctx, false);
   va_insert_flow_control_nops(A);

   ASSERT_SHADER_EQUAL(A, build_diamond_cfg(mem_ctx, true));
}

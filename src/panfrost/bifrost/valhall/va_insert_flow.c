/*
 * Copyright (C) 2022 Collabora Ltd.
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

#include "va_compiler.h"
#include "valhall_enums.h"
#include "bi_builder.h"

/*
 * Insert flow control into a scheduled and register allocated shader.  This
 * pass runs after scheduling and register allocation. This pass only
 * inserts NOPs with the appropriate flow control modifiers. It should be
 * followed by a cleanup pass to merge flow control modifiers on adjacent
 * instructions, eliminating the NOPs. This decouples optimization from
 * correctness, simplifying both passes.
 */

/*
 * Insert a NOP instruction with given flow control.
 */
static void
bi_flow(bi_context *ctx, bi_cursor cursor, enum va_flow flow)
{
   bi_builder b = bi_init_builder(ctx, cursor);

   bi_nop(&b)->flow = flow;
}

/*
 * Determine if execution should terminate after a given block. Execution cannot
 * terminate within a basic block.
 */
static bool
va_should_end(bi_block *block)
{
   /* Don't return if we're succeeded by instructions */
   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      bi_block *succ = block->successors[i];

      if (succ)
         return false;
   }

   return true;
}

/*
 * Given a program with no flow control modifiers, insert NOPs signaling the
 * required flow control. Not much optimization happens here.
 */
void
va_insert_flow_control_nops(bi_context *ctx)
{
   bi_foreach_block(ctx, block) {
      bi_foreach_instr_in_block_safe(block, I) {
         switch (I->op) {
         /* Signal barriers immediately */
         case BI_OPCODE_BARRIER:
            bi_flow(ctx, bi_after_instr(I), VA_FLOW_WAIT);
            break;

         /* Insert waits for tilebuffer and depth/stencil instructions. These
          * only happen in regular fragment shaders, as the required waits are
          * assumed to already have happened in blend shaders.
          */
         case BI_OPCODE_BLEND:
         case BI_OPCODE_LD_TILE:
         case BI_OPCODE_ST_TILE:
            if (!ctx->inputs->is_blend)
               bi_flow(ctx, bi_before_instr(I), VA_FLOW_WAIT);
            break;
         case BI_OPCODE_ATEST:
         case BI_OPCODE_ZS_EMIT:
            if (!ctx->inputs->is_blend)
               bi_flow(ctx, bi_before_instr(I), VA_FLOW_WAIT0126);
            break;

         /* TODO: Optimize waits for asynchronous instructions */
         default:
            if (bi_opcode_props[I->op].message)
               bi_flow(ctx, bi_after_instr(I), VA_FLOW_WAIT0);
            break;
         }
      }

      /* End exeuction at the end of the block if needed, or reconverge if we
       * continue but we don't need to end execution.
       */
      if (va_should_end(block) || block->needs_nop) {
         /* Don't bother adding a NOP into an unreachable block */
         if (block == bi_start_block(&ctx->blocks) || bi_num_predecessors(block))
            bi_flow(ctx, bi_after_block(block), VA_FLOW_END);
      } else if (bi_reconverge_branches(block)) {
         /* TODO: Do we have ever need to reconverge from an empty block? */
         if (!list_is_empty(&block->instructions))
            bi_flow(ctx, bi_after_block(block), VA_FLOW_RECONVERGE);
      }
   }
}

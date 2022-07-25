/*
 * Copyright (C) 2020 Collabora, Ltd.
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright © 2014 Intel Corporation
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
#include "util/u_memory.h"
#include "util/list.h"
#include "util/set.h"

/* Liveness analysis is a backwards-may dataflow analysis pass. Within a block,
 * we compute live_out from live_in. The intrablock pass is linear-time. It
 * returns whether progress was made. */

void
bi_liveness_ins_update(uint8_t *live, bi_instr *ins, unsigned max)
{
        /* live_in[s] = GEN[s] + (live_out[s] - KILL[s]) */

        bi_foreach_dest(ins, d) {
                unsigned node = bi_get_node(ins->dest[d]);

                if (node < max)
                        live[node] &= ~bi_writemask(ins, d);
        }

        bi_foreach_src(ins, src) {
                unsigned count = bi_count_read_registers(ins, src);
                unsigned rmask = BITFIELD_MASK(count);
                uint8_t mask = (rmask << ins->src[src].offset);

                unsigned node = bi_get_node(ins->src[src]);
                if (node < max)
                        live[node] |= mask;
        }
}

static bool
liveness_block_update(bi_block *blk, unsigned temp_count)
{
        bool progress = false;

        /* live_out[s] = sum { p in succ[s] } ( live_in[p] ) */
        bi_foreach_successor(blk, succ) {
                for (unsigned i = 0; i < temp_count; ++i)
                        blk->live_out[i] |= succ->live_in[i];
        }

        uint8_t *live = ralloc_array(blk, uint8_t, temp_count);
        memcpy(live, blk->live_out, temp_count);

        bi_foreach_instr_in_block_rev(blk, ins)
                bi_liveness_ins_update(live, ins, temp_count);

        /* To figure out progress, diff live_in */

        for (unsigned i = 0; (i < temp_count) && !progress; ++i)
                progress |= (blk->live_in[i] != live[i]);

        ralloc_free(blk->live_in);
        blk->live_in = live;

        return progress;
}

/* Globally, liveness analysis uses a fixed-point algorithm based on a
 * worklist. We initialize a work list with the exit block. We iterate the work
 * list to compute live_in from live_out for each block on the work list,
 * adding the predecessors of the block to the work list if we made progress.
 */

void
bi_compute_liveness(bi_context *ctx)
{
        unsigned temp_count = bi_max_temp(ctx);

        u_worklist worklist;
        bi_worklist_init(ctx, &worklist);

        bi_foreach_block(ctx, block) {
                if (block->live_in)
                        ralloc_free(block->live_in);

                if (block->live_out)
                        ralloc_free(block->live_out);

                block->live_in = rzalloc_array(block, uint8_t, temp_count);
                block->live_out = rzalloc_array(block, uint8_t, temp_count);

                bi_worklist_push_tail(&worklist, block);
        }

        while (!u_worklist_is_empty(&worklist)) {
                /* Pop off in reverse order since liveness is backwards */
                bi_block *blk = bi_worklist_pop_tail(&worklist);

                /* Update liveness information. If we made progress, we need to
                 * reprocess the predecessors
                 */
                if (liveness_block_update(blk, temp_count)) {
                        bi_foreach_predecessor(blk, pred)
                                bi_worklist_push_head(&worklist, *pred);
                }
        }

        u_worklist_fini(&worklist);
}

void
bi_liveness_ins_update_ssa(BITSET_WORD *live, const bi_instr *I)
{
        bi_foreach_dest(I, d) {
                assert(I->dest[d].type == BI_INDEX_NORMAL);
                BITSET_CLEAR(live, I->dest[d].value);
        }

        bi_foreach_src(I, s) {
                if (I->src[s].type == BI_INDEX_NORMAL)
                        BITSET_SET(live, I->src[s].value);
        }
}

void
bi_compute_liveness_ssa(bi_context *ctx)
{
        u_worklist worklist;
        u_worklist_init(&worklist, ctx->num_blocks, NULL);

        /* Free any previous liveness, and allocate */
        unsigned words = BITSET_WORDS(ctx->ssa_alloc);

        bi_foreach_block(ctx, block) {
                if (block->ssa_live_in)
                        ralloc_free(block->ssa_live_in);

                if (block->ssa_live_out)
                        ralloc_free(block->ssa_live_out);

                block->ssa_live_in = rzalloc_array(block, BITSET_WORD, words);
                block->ssa_live_out = rzalloc_array(block, BITSET_WORD, words);

                bi_worklist_push_head(&worklist, block);
        }

        /* Iterate the work list */
        while(!u_worklist_is_empty(&worklist)) {
                /* Pop in reverse order since liveness is a backwards pass */
                bi_block *blk = bi_worklist_pop_head(&worklist);

                /* Update its liveness information */
                memcpy(blk->ssa_live_in, blk->ssa_live_out, words * sizeof(BITSET_WORD));

                bi_foreach_instr_in_block_rev(blk, I) {
                        /* Phi nodes are handled separately, so we skip them. As phi nodes are
                         * at the beginning and we're iterating backwards, we stop as soon as
                         * we hit a phi node.
                         */
                        if (I->op == BI_OPCODE_PHI)
                                break;

                        bi_liveness_ins_update_ssa(blk->ssa_live_in, I);
                }

                /* Propagate the live in of the successor (blk) to the live out of
                 * predecessors.
                 *
                 * Phi nodes are logically on the control flow edge and act in parallel.
                 * To handle when propagating, we kill writes from phis and make live the
                 * corresponding sources.
                 */
                bi_foreach_predecessor(blk, pred) {
                        BITSET_WORD *live = ralloc_array(blk, BITSET_WORD, words);
                        memcpy(live, blk->ssa_live_in, words * sizeof(BITSET_WORD));

                        /* Kill write */
                        bi_foreach_instr_in_block(blk, I) {
                                if (I->op != BI_OPCODE_PHI) break;

                                assert(I->dest[0].type == BI_INDEX_NORMAL);
                                BITSET_CLEAR(live, I->dest[0].value);
                        }

                        /* Make live the corresponding source */
                        bi_foreach_instr_in_block(blk, I) {
                                if (I->op != BI_OPCODE_PHI) break;

                                bi_index operand = I->src[bi_predecessor_index(blk, *pred)];
                                if (operand.type == BI_INDEX_NORMAL)
                                        BITSET_SET(live, operand.value);
                        }

                        BITSET_WORD progress = 0;

                        for (unsigned i = 0; i < words; ++i) {
                                progress |= live[i] & ~((*pred)->ssa_live_out[i]);
                                (*pred)->ssa_live_out[i] |= live[i];
                        }

                        if (progress != 0)
                                bi_worklist_push_tail(&worklist, *pred);
                }
        }

        u_worklist_fini(&worklist);
}

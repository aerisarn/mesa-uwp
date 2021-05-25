/*
 * Copyright (C) 2020 Collabora Ltd.
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

#include "compiler.h"
#include "bi_builder.h"
#include "util/u_memory.h"

struct lcra_state {
        unsigned node_count;
        uint64_t *affinity;

        /* Linear constraints imposed. Nested array sized upfront, organized as
         * linear[node_left][node_right]. That is, calculate indices as:
         *
         * Each element is itself a bit field denoting whether (c_j - c_i) bias
         * is present or not, including negative biases.
         *
         * Note for Midgard, there are 16 components so the bias is in range
         * [-15, 15] so encoded by 32-bit field. */

        uint32_t *linear;

        /* Before solving, forced registers; after solving, solutions. */
        unsigned *solutions;
};

/* This module is an implementation of "Linearly Constrained
 * Register Allocation". The paper is available in PDF form
 * (https://people.collabora.com/~alyssa/LCRA.pdf) as well as Markdown+LaTeX
 * (https://gitlab.freedesktop.org/alyssa/lcra/blob/master/LCRA.md)
 */

static struct lcra_state *
lcra_alloc_equations(unsigned node_count)
{
        struct lcra_state *l = calloc(1, sizeof(*l));

        l->node_count = node_count;

        l->linear = calloc(sizeof(l->linear[0]), node_count * node_count);
        l->solutions = calloc(sizeof(l->solutions[0]), node_count);
        l->affinity = calloc(sizeof(l->affinity[0]), node_count);

        memset(l->solutions, ~0, sizeof(l->solutions[0]) * node_count);

        return l;
}

static void
lcra_free(struct lcra_state *l)
{
        free(l->linear);
        free(l->affinity);
        free(l->solutions);
        free(l);
}

static void
lcra_add_node_interference(struct lcra_state *l, unsigned i, unsigned cmask_i, unsigned j, unsigned cmask_j)
{
        if (i == j)
                return;

        uint32_t constraint_fw = 0;
        uint32_t constraint_bw = 0;

        for (unsigned D = 0; D < 16; ++D) {
                if (cmask_i & (cmask_j << D)) {
                        constraint_bw |= (1 << (15 + D));
                        constraint_fw |= (1 << (15 - D));
                }

                if (cmask_i & (cmask_j >> D)) {
                        constraint_fw |= (1 << (15 + D));
                        constraint_bw |= (1 << (15 - D));
                }
        }

        l->linear[j * l->node_count + i] |= constraint_fw;
        l->linear[i * l->node_count + j] |= constraint_bw;
}

static bool
lcra_test_linear(struct lcra_state *l, unsigned *solutions, unsigned i)
{
        unsigned *row = &l->linear[i * l->node_count];
        signed constant = solutions[i];

        for (unsigned j = 0; j < l->node_count; ++j) {
                if (solutions[j] == ~0) continue;

                signed lhs = solutions[j] - constant;

                if (lhs < -15 || lhs > 15)
                        continue;

                if (row[j] & (1 << (lhs + 15)))
                        return false;
        }

        return true;
}

static bool
lcra_solve(struct lcra_state *l)
{
        for (unsigned step = 0; step < l->node_count; ++step) {
                if (l->solutions[step] != ~0) continue;
                if (l->affinity[step] == 0) continue;

                bool succ = false;

                u_foreach_bit64(r, l->affinity[step]) {
                        l->solutions[step] = r * 4;

                        if (lcra_test_linear(l, l->solutions, step)) {
                                succ = true;
                                break;
                        }
                }

                /* Out of registers - prepare to spill */
                if (!succ)
                        return false;
        }

        return true;
}

/* Register spilling is implemented with a cost-benefit system. Costs are set
 * by the user. Benefits are calculated from the constraints. */

static unsigned
lcra_count_constraints(struct lcra_state *l, unsigned i)
{
        unsigned count = 0;
        unsigned *constraints = &l->linear[i * l->node_count];

        for (unsigned j = 0; j < l->node_count; ++j)
                count += util_bitcount(constraints[j]);

        return count;
}

/* Construct an affinity mask such that the vector with `count` elements does
 * not intersect any of the registers in the bitset `clobber`. In other words,
 * an allocated register r needs to satisfy for each i < count: a + i != b.
 * Equivalently that's a != b - i, so we need a \ne { b - i : i < n }. For the
 * entire clobber set B, we need a \ne union b \in B { b - i : i < n }, where
 * that union is the desired clobber set. That may be written equivalently as
 * the union over i < n of (B - i), where subtraction is defined elementwise
 * and corresponds to a shift of the entire bitset.
 */

static uint64_t
bi_make_affinity(uint64_t clobber, unsigned count)
{
        uint64_t clobbered = 0;

        for (unsigned i = 0; i < count; ++i)
                clobbered |= (clobber >> i);

        /* We can use a register iff it's not clobberred */
        return ~clobbered;
}

static void
bi_mark_interference(bi_block *block, struct lcra_state *l, uint16_t *live, uint64_t preload_live, unsigned node_count, bool is_blend)
{
        bi_foreach_instr_in_block_rev(block, ins) {
                /* Mark all registers live after the instruction as
                 * interfering with the destination */

                bi_foreach_dest(ins, d) {
                        unsigned node = bi_get_node(ins->dest[d]);

                        if (node >= node_count)
                                continue;

                        /* Don't allocate to anything that's read later as a
                         * preloaded register. The affinity is the intersection
                         * of affinity masks for each write. Since writes have
                         * offsets, but the affinity is for the whole node, we
                         * need to offset the affinity opposite the write
                         * offset, so we shift right. */
                        unsigned count = bi_count_write_registers(ins, d);
                        unsigned offset = ins->dest[d].offset;
                        uint64_t affinity = bi_make_affinity(preload_live, count);
                        l->affinity[node] &= (affinity >> offset);

                        for (unsigned i = 0; i < node_count; ++i) {
                                if (live[i]) {
                                        lcra_add_node_interference(l, node,
                                                        bi_writemask(ins, d), i, live[i]);
                                }
                        }
                }

                if (!is_blend && ins->op == BI_OPCODE_BLEND) {
                        /* Blend shaders might clobber r0-r15, r48. */
                        uint64_t clobber = BITFIELD64_MASK(16) | BITFIELD64_BIT(48);

                        for (unsigned i = 0; i < node_count; ++i) {
                                if (live[i])
                                        l->affinity[i] &= ~clobber;
                        }
                }

                /* Update live_in */
                preload_live = bi_postra_liveness_ins(preload_live, ins);
                bi_liveness_ins_update(live, ins, node_count);
        }

        block->reg_live_in = preload_live;
}

static void
bi_compute_interference(bi_context *ctx, struct lcra_state *l)
{
        unsigned node_count = bi_max_temp(ctx);

        bi_compute_liveness(ctx);
        bi_postra_liveness(ctx);

        bi_foreach_block_rev(ctx, _blk) {
                bi_block *blk = (bi_block *) _blk;
                uint16_t *live = mem_dup(_blk->live_out, node_count * sizeof(uint16_t));

                bi_mark_interference(blk, l, live, blk->reg_live_out,
                                node_count, ctx->inputs->is_blend);

                free(live);
        }
}

static struct lcra_state *
bi_allocate_registers(bi_context *ctx, bool *success)
{
        unsigned node_count = bi_max_temp(ctx);
        struct lcra_state *l = lcra_alloc_equations(node_count);

        /* R0-R3 are reserved for the blend input in blend shaders, otherwise
         * we can use the whole register file */
        uint64_t default_affinity =
                ctx->inputs->is_blend ? BITFIELD64_MASK(16) : ~0ull;

        bi_foreach_instr_global(ctx, ins) {
                bi_foreach_dest(ins, d) {
                        unsigned dest = bi_get_node(ins->dest[d]);

                        /* Blend shaders expect the src colour to be in r0-r3 */
                        if (ins->op == BI_OPCODE_BLEND &&
                            !ctx->inputs->is_blend) {
                                unsigned node = bi_get_node(ins->src[0]);
                                assert(node < node_count);
                                l->solutions[node] = 0;
                        }

                        if (dest < node_count)
                                l->affinity[dest] = default_affinity;
                }

        }

        bi_compute_interference(ctx, l);

        *success = lcra_solve(l);

        return l;
}

static bi_index
bi_reg_from_index(bi_context *ctx, struct lcra_state *l, bi_index index)
{
        /* Offsets can only be applied when we register allocated an index, or
         * alternatively for FAU's encoding */

        ASSERTED bool is_offset = (index.offset > 0) &&
                (index.type != BI_INDEX_FAU);
        unsigned node_count = bi_max_temp(ctx);

        /* Did we run RA for this index at all */
        if (bi_get_node(index) >= node_count) {
                assert(!is_offset);
                return index;
        }

        /* LCRA didn't bother solving this index (how lazy!) */
        signed solution = l->solutions[bi_get_node(index)];
        if (solution < 0) {
                assert(!is_offset);
                return index;
        }

        assert((solution & 0x3) == 0);
        unsigned reg = solution / 4;
        reg += index.offset;

        /* todo: do we want to compose with the subword swizzle? */
        bi_index new_index = bi_register(reg);
        new_index.swizzle = index.swizzle;
        new_index.abs = index.abs;
        new_index.neg = index.neg;
        return new_index;
}

static void
bi_install_registers(bi_context *ctx, struct lcra_state *l)
{
        bi_foreach_instr_global(ctx, ins) {
                bi_foreach_dest(ins, d)
                        ins->dest[d] = bi_reg_from_index(ctx, l, ins->dest[d]);

                bi_foreach_src(ins, s)
                        ins->src[s] = bi_reg_from_index(ctx, l, ins->src[s]);
        }
}

static void
bi_rewrite_index_src_single(bi_instr *ins, bi_index old, bi_index new)
{
        bi_foreach_src(ins, i) {
                if (bi_is_equiv(ins->src[i], old)) {
                        ins->src[i].type = new.type;
                        ins->src[i].reg = new.reg;
                        ins->src[i].value = new.value;
                }
        }
}

/* If register allocation fails, find the best spill node */

static signed
bi_choose_spill_node(bi_context *ctx, struct lcra_state *l)
{
        /* Pick a node satisfying bi_spill_register's preconditions */
        BITSET_WORD *no_spill = calloc(sizeof(BITSET_WORD), BITSET_WORDS(l->node_count));

        bi_foreach_instr_global(ctx, ins) {
                bi_foreach_dest(ins, d) {
                        if (ins->no_spill || ins->dest[d].offset)
                                BITSET_SET(no_spill, bi_get_node(ins->dest[d]));
                }
        }

        /* If there are no constraints on a node, do not pick it to spill under
         * any circumstance, or else we would hang rather than fail RA */
        unsigned best_benefit = 0.0;
        signed best_node = -1;

        /* Skip register nodes (with bit 0 set) */
        for (unsigned i = 0; i < l->node_count; i += 2) {
                if (BITSET_TEST(no_spill, i)) continue;

                unsigned benefit = lcra_count_constraints(l, i);

                if (benefit > best_benefit) {
                        best_benefit = benefit;
                        best_node = i;
                }
        }

        free(no_spill);
        return best_node;
}

static void
bi_spill_dest(bi_builder *b, bi_index index, bi_index temp, uint32_t offset,
                bi_instr *instr, bi_block *block, unsigned channels)
{
        b->cursor = bi_after_instr(instr);
        bi_store(b, channels * 32, temp, bi_imm_u32(offset), bi_zero(),
                        BI_SEG_TL);

        b->shader->spills++;
}

static void
bi_fill_src(bi_builder *b, bi_index index, bi_index temp, uint32_t offset,
                bi_instr *instr, bi_block *block, unsigned channels)
{
        b->cursor = bi_before_instr(instr);
        bi_instr *ld = bi_load_to(b, channels * 32, temp,
                        bi_imm_u32(offset), bi_zero(), BI_SEG_TL);
        ld->no_spill = true;

        b->shader->fills++;
}

static unsigned
bi_instr_mark_spill(bi_context *ctx, bi_block *block,
                bi_instr *ins, bi_index index, bi_index *temp)
{
        unsigned channels = 0;

        bi_foreach_dest(ins, d) {
                if (!bi_is_equiv(ins->dest[d], index)) continue;
                if (bi_is_null(*temp)) *temp = bi_temp_reg(ctx);
                ins->no_spill = true;

                unsigned offset = ins->dest[d].offset;
                ins->dest[d] = bi_replace_index(ins->dest[d], *temp);
                ins->dest[d].offset = offset;

                unsigned newc = util_last_bit(bi_writemask(ins, d)) >> 2;
                channels = MAX2(channels, newc);
        }

        return channels;
}

static bool
bi_instr_mark_fill(bi_context *ctx, bi_block *block, bi_instr *ins,
                bi_index index, bi_index *temp)
{
        if (!bi_has_arg(ins, index)) return false;
        if (bi_is_null(*temp)) *temp = bi_temp_reg(ctx);
        bi_rewrite_index_src_single(ins, index, *temp);
        return true;
}

/* Once we've chosen a spill node, spill it. Precondition: node is a valid
 * SSA node in the non-optimized scheduled IR that was not already
 * spilled (enforced by bi_choose_spill_node). Returns bytes spilled */

static unsigned
bi_spill_register(bi_context *ctx, bi_index index, uint32_t offset)
{
        assert(!index.reg);

        bi_builder _b = { .shader = ctx };
        unsigned channels = 1;

        /* Spill after every store, fill before every load */
        bi_foreach_block(ctx, _block) {
                bi_block *block = (bi_block *) _block;
                bi_foreach_instr_in_block_safe(block, instr) {
                        bi_index tmp;
                        unsigned local_channels = bi_instr_mark_spill(ctx,
                                        block, instr, index, &tmp);

                        channels = MAX2(channels, local_channels);

                        if (local_channels) {
                                bi_spill_dest(&_b, index, tmp, offset,
                                                instr, block, channels);
                        }

                        /* For SSA form, if we write/spill, there was no prior
                         * contents to fill, so don't waste time reading
                         * garbage */

                        bool should_fill = !local_channels || index.reg;
                        should_fill &= bi_instr_mark_fill(ctx, block, instr,
                                        index, &tmp);

                        if (should_fill) {
                                bi_fill_src(&_b, index, tmp, offset, instr,
                                                block, channels);
                        }
                }
        }

        return (channels * 4);
}

void
bi_register_allocate(bi_context *ctx)
{
        struct lcra_state *l = NULL;
        bool success = false;

        unsigned iter_count = 1000; /* max iterations */

        /* Number of bytes of memory we've spilled into */
        unsigned spill_count = ctx->info->tls_size;

        do {
                if (l) {
                        signed spill_node = bi_choose_spill_node(ctx, l);
                        lcra_free(l);
                        l = NULL;

                        if (spill_node == -1)
                                unreachable("Failed to choose spill node\n");

                        spill_count += bi_spill_register(ctx,
                                        bi_node_to_index(spill_node, bi_max_temp(ctx)),
                                        spill_count);
                }

                bi_invalidate_liveness(ctx);
                l = bi_allocate_registers(ctx, &success);
        } while(!success && ((iter_count--) > 0));

        assert(success);

        ctx->info->tls_size = spill_count;
        bi_install_registers(ctx, l);

        lcra_free(l);
}

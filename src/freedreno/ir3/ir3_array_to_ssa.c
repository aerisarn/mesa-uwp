/*
 * Copyright (C) 2021 Valve Corporation
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

/* This pass lowers array accesses to SSA.
 *
 * After this pass, instructions writing arrays implicitly read the contents of
 * the array defined in instr->regs[0]->def (possibly a phi node), perform the
 * operation, and store to instr->regs[0].
 *
 * This makes arrays appear like "normal" SSA values, even if the false
 * dependencies mean that they always stay in CSSA form (i.e. able to removed
 * out-of-SSA with no copies.) While hopefully they shouldn't induce copies in
 * most cases, we can't make that guarantee while also splitting spilling from
 * RA and guaranteeing a certain number of registers are used, so we have to
 * insert the phi nodes to be able to know when copying should happen.
 *
 * The implementation is based on the idea in "Simple and Efficient Construction
 * of Static Single Assignment Form" of scanning backwards to find the
 * definition. However, since we're not doing this on-the-fly we can simplify
 * things a little by doing a pre-pass to get the last definition of each array
 * in each block. Then we optimize trivial phis in a separate pass, "on the fly"
 * so that we don't have to rewrite (and keep track of) users.
 */

#include "ir3.h"
#include <stdlib.h>

struct array_state {
	struct ir3_register *live_in_definition;
	struct ir3_register *live_out_definition;
	bool constructed;
	bool optimized;
};

struct array_ctx {
	struct array_state *states;
	struct ir3 *ir;
	unsigned array_count;
};

static struct array_state *
get_state(struct array_ctx *ctx, struct ir3_block *block, unsigned id)
{
	return &ctx->states[ctx->array_count * block->index + id];
}

static struct ir3_register *
read_value_beginning(struct array_ctx *ctx, struct ir3_block *block, struct ir3_array *arr);

static struct ir3_register *
read_value_end(struct array_ctx *ctx, struct ir3_block *block, struct ir3_array *arr)
{
	struct array_state *state = get_state(ctx, block, arr->id);
	if (state->live_out_definition)
		return state->live_out_definition;

	state->live_out_definition = read_value_beginning(ctx, block, arr);
	return state->live_out_definition;
}

/* Roughly equivalent to readValueRecursive from the paper: */
static struct ir3_register *
read_value_beginning(struct array_ctx *ctx, struct ir3_block *block, struct ir3_array *arr)
{
	struct array_state *state = get_state(ctx, block, arr->id);

	if (state->constructed)
		return state->live_in_definition;

	if (block->predecessors_count == 0) {
		state->constructed = true;
		return NULL;
	}

	if (block->predecessors_count == 1) {
		state->live_in_definition = read_value_end(ctx, block->predecessors[0], arr);
		state->constructed = true;
		return state->live_in_definition;
	}

	unsigned flags = IR3_REG_ARRAY | (arr->half ? IR3_REG_HALF : 0);
	struct ir3_instruction *phi =
		ir3_instr_create(block, OPC_META_PHI, block->predecessors_count + 1);
	list_del(&phi->node);
	list_add(&phi->node, &block->instr_list);

	struct ir3_register *dst = __ssa_dst(phi);
	dst->flags |= flags;
	dst->array.id = arr->id;
	dst->size = arr->length;

	state->live_in_definition = phi->regs[0];
	state->constructed = true;

	for (unsigned i = 0; i < block->predecessors_count; i++) {
		struct ir3_register *src = read_value_end(ctx, block->predecessors[i], arr);
		struct ir3_register *src_reg;
		if (src) {
			src_reg = __ssa_src(phi, src->instr, flags);
		} else {
			src_reg = ir3_reg_create(phi, INVALID_REG, flags | IR3_REG_SSA);
		}
		src_reg->array.id = arr->id;
		src_reg->size = arr->length;
	}
	return phi->regs[0];
}

static struct ir3_register *
remove_trivial_phi(struct ir3_instruction *phi)
{
	/* Break cycles */
	if (phi->data)
		return phi->data;
	
	phi->data = phi->regs[0];

	struct ir3_register *unique_def = NULL;
	bool unique = true;
	for (unsigned i = 0; i < phi->block->predecessors_count; i++) {
		struct ir3_register *src = phi->regs[i + 1];

		/* If there are any undef sources, then the remaining sources may not
		 * dominate the phi node, even if they are all equal. So we need to
		 * bail out in this case.
		 *
		 * This seems to be a bug in the original paper.
		 */
		if (!src->def) {
			unique = false;
			break;
		}

		struct ir3_instruction *src_instr = src->def->instr;
		
		/* phi sources which point to the phi itself don't count for
		 * figuring out if the phi is trivial
		 */
		if (src_instr == phi)
			continue;

		if (src_instr->opc == OPC_META_PHI) {
			src->def = remove_trivial_phi(src->def->instr);
		}

		if (unique_def) {
			if (unique_def != src->def) {
				unique = false;
				break;
			}
		} else {
			unique_def = src->def;
		}
	}

	if (unique) {
		phi->data = unique_def;
		return unique_def;
	} else {
		return phi->regs[0];
	}
}

static struct ir3_register *
lookup_value(struct ir3_register *reg)
{
	if (reg->instr->opc == OPC_META_PHI)
		return reg->instr->data;
	return reg;
}

static struct ir3_register *
lookup_live_in(struct array_ctx *ctx, struct ir3_block *block, unsigned id)
{
	struct array_state *state = get_state(ctx, block, id);
	if (state->live_in_definition)
		return lookup_value(state->live_in_definition);

	return NULL;
}

bool
ir3_array_to_ssa(struct ir3 *ir)
{
	struct array_ctx ctx = {};

	foreach_array (array, &ir->array_list) {
		ctx.array_count = MAX2(ctx.array_count, array->id + 1);
	}

	if (ctx.array_count == 0)
		return false;

	unsigned i = 0;
	foreach_block (block, &ir->block_list) {
		block->index = i++;
	}

	ctx.ir = ir;
	ctx.states = calloc(ctx.array_count * i, sizeof(struct array_state));

	foreach_block (block, &ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			for (unsigned i = 0; i < instr->regs_count; i++) {
				if ((instr->regs[i]->flags & IR3_REG_ARRAY) &&
					(instr->regs[i]->flags & IR3_REG_DEST)) {
					struct array_state *state =
						get_state(&ctx, block, instr->regs[i]->array.id);
					state->live_out_definition = instr->regs[i];
				}
			}
		}
	}

	foreach_block (block, &ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			if (instr->opc == OPC_META_PHI)
				continue;

			for (unsigned i = 0; i < instr->regs_count; i++) {
				struct ir3_register *reg = instr->regs[i];
				if ((reg->flags & IR3_REG_ARRAY) &&
					 (!reg->def || reg->def->instr->block != block)) {
					reg->def = NULL;
					struct ir3_array *arr = ir3_lookup_array(ir, reg->array.id);

					/* Construct any phi nodes necessary to read this value */
					read_value_beginning(&ctx, block, arr);
				}
			}
		}
	}

	foreach_block (block, &ir->block_list) {
		foreach_instr_safe (instr, &block->instr_list) {
			if (instr->opc == OPC_META_PHI)
				remove_trivial_phi(instr);
			else
				break;
		}
	}

	foreach_block (block, &ir->block_list) {
		foreach_instr_safe (instr, &block->instr_list) {
			if (instr->opc == OPC_META_PHI) {
				if (!(instr->flags & IR3_REG_ARRAY))
					continue;
				if (instr->data != instr->regs[0]) {
					list_del(&instr->node);
					continue;
				}
				for (unsigned i = 1; i < instr->regs_count; i++) {
					instr->regs[i] = lookup_value(instr->regs[i]);
				}
			} else {
				for (unsigned i = 0; i < instr->regs_count; i++) {
					struct ir3_register *reg = instr->regs[i];
					if ((reg->flags & IR3_REG_ARRAY)) {
						if (!reg->def) {
							/* It is assumed that before calling
							 * ir3_array_to_ssa(), reg->def was set to the
							 * previous writer of the array within the current
							 * block (if any).  The reg->def of the first write
							 * to an array within a block was cleared in the
							 * loop calling read_value_beginning() above.
							 */
							reg->def = lookup_live_in(&ctx, block, reg->array.id);
						}
						reg->flags |= IR3_REG_SSA;
					}
				}
			}
		}
	}

	free(ctx.states);
	return true;
}


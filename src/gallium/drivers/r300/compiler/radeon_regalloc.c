/*
 * Copyright (C) 2009 Nicolai Haehnle.
 * Copyright 2011 Tom Stellard <tstellar@gmail.com>
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "radeon_regalloc.h"
#include "radeon_list.h"

#define VERBOSE 0

#define DBG(...) do { if (VERBOSE) fprintf(stderr, __VA_ARGS__); } while(0)

const struct rc_class rc_class_list [] = {
	{RC_REG_CLASS_SINGLE, 3,
		{RC_MASK_X,
		 RC_MASK_Y,
		 RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_DOUBLE, 3,
		{RC_MASK_X | RC_MASK_Y,
		 RC_MASK_X | RC_MASK_Z,
		 RC_MASK_Y | RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_TRIPLE, 1,
		{RC_MASK_X | RC_MASK_Y | RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_ALPHA, 1,
		{RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_SINGLE_PLUS_ALPHA, 3,
		{RC_MASK_X | RC_MASK_W,
		 RC_MASK_Y | RC_MASK_W,
		 RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_DOUBLE_PLUS_ALPHA, 3,
		{RC_MASK_X | RC_MASK_Y | RC_MASK_W,
		 RC_MASK_X | RC_MASK_Z | RC_MASK_W,
		 RC_MASK_Y | RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_TRIPLE_PLUS_ALPHA, 1,
		{RC_MASK_X | RC_MASK_Y | RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_X, 1,
		{RC_MASK_X,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_Y, 1,
		{RC_MASK_Y,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_Z, 1,
		{RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_XY, 1,
		{RC_MASK_X | RC_MASK_Y,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_YZ, 1,
		{RC_MASK_Y | RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_XZ, 1,
		{RC_MASK_X | RC_MASK_Z,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_XW, 1,
		{RC_MASK_X | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_YW, 1,
		{RC_MASK_Y | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_ZW, 1,
		{RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_XYW, 1,
		{RC_MASK_X | RC_MASK_Y | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_YZW, 1,
		{RC_MASK_Y | RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}},
	{RC_REG_CLASS_XZW, 1,
		{RC_MASK_X | RC_MASK_Z | RC_MASK_W,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE,
		 RC_MASK_NONE}}
};

static void print_live_intervals(struct live_intervals * src)
{
	if (!src || !src->Used) {
		DBG("(null)");
		return;
	}

	DBG("(%i,%i)", src->Start, src->End);
}

static int overlap_live_intervals(struct live_intervals * a, struct live_intervals * b)
{
	if (VERBOSE) {
		DBG("overlap_live_intervals: ");
		print_live_intervals(a);
		DBG(" to ");
		print_live_intervals(b);
		DBG("\n");
	}

	if (!a->Used || !b->Used) {
		DBG("    unused interval\n");
		return 0;
	}

	if (a->Start > b->Start) {
		if (a->Start < b->End) {
			DBG("    overlap\n");
			return 1;
		}
	} else if (b->Start > a->Start) {
		if (b->Start < a->End) {
			DBG("    overlap\n");
			return 1;
		}
	} else { /* a->Start == b->Start */
		if (a->Start != a->End && b->Start != b->End) {
			DBG("    overlap\n");
			return 1;
		}
	}

	DBG("    no overlap\n");

	return 0;
}

int rc_find_class(
	const struct rc_class * classes,
	unsigned int writemask,
	unsigned int max_writemask_count)
{
	unsigned int i;
	for (i = 0; i < RC_REG_CLASS_COUNT; i++) {
		unsigned int j;
		if (classes[i].WritemaskCount > max_writemask_count) {
			continue;
		}
		for (j = 0; j < classes[i].WritemaskCount; j++) {
			if (classes[i].Writemasks[j] == writemask) {
				return i;
			}
		}
	}
	return -1;
}

unsigned int rc_overlap_live_intervals_array(
	struct live_intervals * a,
	struct live_intervals * b)
{
	unsigned int a_chan, b_chan;
	for (a_chan = 0; a_chan < 4; a_chan++) {
		for (b_chan = 0; b_chan < 4; b_chan++) {
			if (overlap_live_intervals(&a[a_chan], &b[b_chan])) {
					return 1;
			}
		}
	}
	return 0;
}

#if VERBOSE
static void print_reg(int reg)
{
	unsigned int index = reg_get_index(reg);
	unsigned int mask = reg_get_writemask(reg);
	fprintf(stderr, "Temp[%u].%c%c%c%c", index,
		mask & RC_MASK_X ? 'x' : '_',
		mask & RC_MASK_Y ? 'y' : '_',
		mask & RC_MASK_Z ? 'z' : '_',
		mask & RC_MASK_W ? 'w' : '_');
}
#endif

static void add_register_conflicts(
	struct ra_regs * regs,
	unsigned int max_temp_regs)
{
	unsigned int index, a_mask, b_mask;
	for (index = 0; index < max_temp_regs; index++) {
		for(a_mask = 1; a_mask <= RC_MASK_XYZW; a_mask++) {
			for (b_mask = a_mask + 1; b_mask <= RC_MASK_XYZW;
								b_mask++) {
				if (a_mask & b_mask) {
					ra_add_reg_conflict(regs,
						get_reg_id(index, a_mask),
						get_reg_id(index, b_mask));
				}
			}
		}
	}
}

void rc_build_interference_graph(
	struct ra_graph * graph,
	struct rc_list * variables)
{
	unsigned node_index;
	struct rc_list * var_ptr;

	/* Build the interference graph */
	for (var_ptr = variables, node_index = 0; var_ptr;
					var_ptr = var_ptr->Next, node_index++) {
		struct rc_list * a, * b;
		unsigned int b_index;

		for (a = var_ptr, b = var_ptr->Next, b_index = node_index + 1;
						b; b = b->Next, b_index++) {
			struct rc_variable * var_a = a->Item;
			while (var_a) {
				struct rc_variable * var_b = b->Item;
				while (var_b) {
					if (rc_overlap_live_intervals_array(var_a->Live, var_b->Live)) {
						ra_add_node_interference(graph,
							node_index, b_index);
					}
					var_b = var_b->Friend;
				}
				var_a = var_a->Friend;
			}
		}
	}
}

void rc_init_regalloc_state(struct rc_regalloc_state *s)
{
	unsigned i, j, index;
	unsigned **ra_q_values;

	/* Pre-computed q values.  This array describes the maximum number of
	 * a class's [row] registers that are in conflict with a single
	 * register from another class [column].
	 *
	 * For example:
	 * q_values[0][2] is 3, because a register from class 2
	 * (RC_REG_CLASS_TRIPLE) may conflict with at most 3 registers from
	 * class 0 (RC_REG_CLASS_SINGLE) e.g. T0.xyz conflicts with T0.x, T0.y,
	 * and T0.z.
	 *
	 * q_values[2][0] is 1, because a register from class 0
	 * (RC_REG_CLASS_SINGLE) may conflict with at most 1 register from
	 * class 2 (RC_REG_CLASS_TRIPLE) e.g. T0.x conflicts with T0.xyz
	 *
	 * The q values for each register class [row] will never be greater
	 * than the maximum number of writemask combinations for that class.
	 *
	 * For example:
	 *
	 * Class 2 (RC_REG_CLASS_TRIPLE) only has 1 writemask combination,
	 * so no value in q_values[2][0..RC_REG_CLASS_COUNT] will be greater
	 * than 1.
	 */
	const unsigned q_values[RC_REG_CLASS_COUNT][RC_REG_CLASS_COUNT] = {
	{1, 2, 3, 0, 1, 2, 3, 1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2},
	{2, 3, 3, 0, 2, 3, 3, 2, 2, 2, 3, 3, 3, 2, 2, 2, 3, 3, 3},
	{1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1},
	{1, 2, 3, 3, 3, 3, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 3},
	{2, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3},
	{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1},
	{1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
	{1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1},
	{1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1},
	{1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1},
	{1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}
	};

	/* Allocate the main ra data structure */
	s->regs = ra_alloc_reg_set(NULL, R500_PFS_NUM_TEMP_REGS * RC_MASK_XYZW,
                                   true);

	s->class_list = rc_class_list;

	/* Create the register classes */
	for (i = 0; i < RC_REG_CLASS_COUNT; i++) {
		const struct rc_class *class = &rc_class_list[i];
		s->classes[class->ID] = ra_alloc_reg_class(s->regs);

		/* Assign registers to the classes */
		for (index = 0; index < R500_PFS_NUM_TEMP_REGS; index++) {
			for (j = 0; j < class->WritemaskCount; j++) {
				int reg_id = get_reg_id(index,
						class->Writemasks[j]);
				ra_class_add_reg(s->classes[class->ID], reg_id);
			}
		}
	}

	/* Set the q values.  The q_values array is indexed based on
	 * the rc_reg_class ID (RC_REG_CLASS_*) which might be
	 * different than the ID assigned to that class by ra.
	 * This why we need to manually construct this list.
	 */
	ra_q_values = MALLOC(RC_REG_CLASS_COUNT * sizeof(unsigned *));

	for (i = 0; i < RC_REG_CLASS_COUNT; i++) {
		ra_q_values[i] = MALLOC(RC_REG_CLASS_COUNT * sizeof(unsigned));
		for (j = 0; j < RC_REG_CLASS_COUNT; j++) {
			ra_q_values[i][j] = q_values[i][j];
		}
	}

	/* Add register conflicts */
	add_register_conflicts(s->regs, R500_PFS_NUM_TEMP_REGS);

	ra_set_finalize(s->regs, ra_q_values);

	for (i = 0; i < RC_REG_CLASS_COUNT; i++) {
		FREE(ra_q_values[i]);
	}
	FREE(ra_q_values);
}

void rc_destroy_regalloc_state(struct rc_regalloc_state *s)
{
	ralloc_free(s->regs);
}

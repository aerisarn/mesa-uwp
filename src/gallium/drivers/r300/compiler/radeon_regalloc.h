/*
 * Copyright (C) 2009 Nicolai Haehnle.
 * Copyright 2011 Tom Stellard <tstellar@gmail.com>
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Tom Stellard <thomas.stellard@amd.com>
 */

#ifndef RADEON_REGALLOC_H
#define RADEON_REGALLOC_H

#include "util/register_allocate.h"
#include "util/u_memory.h"
#include "util/ralloc.h"

#include "radeon_variable.h"

struct ra_regs;

enum rc_reg_class {
	RC_REG_CLASS_SINGLE,
	RC_REG_CLASS_DOUBLE,
	RC_REG_CLASS_TRIPLE,
	RC_REG_CLASS_ALPHA,
	RC_REG_CLASS_SINGLE_PLUS_ALPHA,
	RC_REG_CLASS_DOUBLE_PLUS_ALPHA,
	RC_REG_CLASS_TRIPLE_PLUS_ALPHA,
	RC_REG_CLASS_X,
	RC_REG_CLASS_Y,
	RC_REG_CLASS_Z,
	RC_REG_CLASS_XY,
	RC_REG_CLASS_YZ,
	RC_REG_CLASS_XZ,
	RC_REG_CLASS_XW,
	RC_REG_CLASS_YW,
	RC_REG_CLASS_ZW,
	RC_REG_CLASS_XYW,
	RC_REG_CLASS_YZW,
	RC_REG_CLASS_XZW,
	RC_REG_CLASS_COUNT
};

struct rc_regalloc_state {
	struct ra_regs *regs;
	struct ra_class *classes[RC_REG_CLASS_COUNT];
	const struct rc_class *class_list;
};

struct register_info {
	struct live_intervals Live[4];

	unsigned int Used:1;
	unsigned int Allocated:1;
	unsigned int File:3;
	unsigned int Index:RC_REGISTER_INDEX_BITS;
	unsigned int Writemask;
};

struct regalloc_state {
	struct radeon_compiler * C;

	struct register_info * Input;
	unsigned int NumInputs;

	struct register_info * Temporary;
	unsigned int NumTemporaries;

	unsigned int Simple;
	int LoopEnd;
};

struct rc_class {
	enum rc_reg_class ID;

	unsigned int WritemaskCount;

	/** List of writemasks that belong to this class */
	unsigned int Writemasks[3];
};

int rc_find_class(
	const struct rc_class * classes,
	unsigned int writemask,
	unsigned int max_writemask_count);

unsigned int rc_overlap_live_intervals_array(
	struct live_intervals * a,
	struct live_intervals * b);

static inline unsigned int reg_get_index(int reg)
{
	return reg / RC_MASK_XYZW;
};

static inline unsigned int reg_get_writemask(int reg)
{
	return (reg % RC_MASK_XYZW) + 1;
};

static inline int get_reg_id(unsigned int index, unsigned int writemask)
{
       assert(writemask);
       if (writemask == 0) {
               return 0;
       }
       return (index * RC_MASK_XYZW) + (writemask - 1);
}

void rc_init_regalloc_state(struct rc_regalloc_state *s);
void rc_destroy_regalloc_state(struct rc_regalloc_state *s);

#endif /* RADEON_REGALLOC_H */

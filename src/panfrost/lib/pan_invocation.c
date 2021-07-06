/*
 * Copyright (C) 2019 Collabora, Ltd.
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
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *
 */

#include <assert.h>
#include "util/u_math.h"
#include "pan_encoder.h"

/* Compute shaders are invoked with a gl_NumWorkGroups X/Y/Z triplet. Vertex
 * shaders are invoked as (1, vertex_count, instance_count). Compute shaders
 * also have a gl_WorkGroupSize X/Y/Z triplet. These 6 values are packed
 * together in a dynamic bitfield, packed by this routine. */

void
panfrost_pack_work_groups_compute(
        struct mali_invocation_packed *out,
        unsigned num_x, unsigned num_y, unsigned num_z,
        unsigned size_x, unsigned size_y, unsigned size_z,
        bool quirk_graphics, bool indirect_dispatch)
{
        /* The values needing packing, in order, and the corresponding shifts.
         * Indicies into shift are off-by-one to make the logic easier */

        unsigned values[6] = { size_x, size_y, size_z, num_x, num_y, num_z };
        unsigned shifts[7] = { 0 };
        uint32_t packed = 0;

        for (unsigned i = 0; i < 6; ++i) {
                /* Must be positive, otherwise we underflow */
                assert(values[i] >= 1);

                /* OR it in, shifting as required */
                packed |= ((values[i] - 1) << shifts[i]);

                /* How many bits did we use? */
                unsigned bit_count = util_logbase2_ceil(values[i]);

                /* Set the next shift accordingly */
                shifts[i + 1] = shifts[i] + bit_count;
        }

        pan_pack(out, INVOCATION, cfg) {
                cfg.invocations = packed;
                cfg.size_y_shift = shifts[1];
                cfg.size_z_shift = shifts[2];
                cfg.workgroups_x_shift = shifts[3];

                if (!indirect_dispatch) {
                        /* Leave zero for the dispatch shader */
                        cfg.workgroups_y_shift = shifts[4];
                        cfg.workgroups_z_shift = shifts[5];
                }

                /* Quirk: for non-instanced graphics, the blob sets
                 * workgroups_z_shift = 32. This doesn't appear to matter to
                 * the hardware, but it's good to be bit-identical. */

                if (quirk_graphics && (num_z <= 1))
                        cfg.workgroups_z_shift = 32;

                /* For graphics, set to the minimum efficient value. For
                 * compute, must equal the workgroup X shift for barriers to
                 * function correctly */

                cfg.thread_group_split = quirk_graphics ?
                        MALI_SPLIT_MIN_EFFICIENT : cfg.workgroups_x_shift;
        }
}

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
 */

#include "pan_context.h"

void
panfrost_analyze_sysvals(struct panfrost_shader_state *ss)
{
        unsigned dirty = 0;
        unsigned dirty_shader =
                PAN_DIRTY_STAGE_RENDERER | PAN_DIRTY_STAGE_CONST;

        for (unsigned i = 0; i < ss->info.sysvals.sysval_count; ++i) {
                switch (PAN_SYSVAL_TYPE(ss->info.sysvals.sysvals[i])) {
                case PAN_SYSVAL_VIEWPORT_SCALE:
                case PAN_SYSVAL_VIEWPORT_OFFSET:
                        dirty |= PAN_DIRTY_VIEWPORT;
                        break;

                case PAN_SYSVAL_TEXTURE_SIZE:
                        dirty_shader |= PAN_DIRTY_STAGE_TEXTURE;
                        break;

                case PAN_SYSVAL_SSBO:
                        dirty_shader |= PAN_DIRTY_STAGE_SSBO;
                        break;

                case PAN_SYSVAL_SAMPLER:
                        dirty_shader |= PAN_DIRTY_STAGE_SAMPLER;
                        break;

                case PAN_SYSVAL_IMAGE_SIZE:
                        dirty_shader |= PAN_DIRTY_STAGE_IMAGE;
                        break;

                case PAN_SYSVAL_NUM_WORK_GROUPS:
                case PAN_SYSVAL_LOCAL_GROUP_SIZE:
                case PAN_SYSVAL_WORK_DIM:
                case PAN_SYSVAL_VERTEX_INSTANCE_OFFSETS:
                        dirty |= PAN_DIRTY_PARAMS;
                        break;

                case PAN_SYSVAL_DRAWID:
                        dirty |= PAN_DIRTY_DRAWID;
                        break;

                case PAN_SYSVAL_SAMPLE_POSITIONS:
                case PAN_SYSVAL_MULTISAMPLED:
                case PAN_SYSVAL_RT_CONVERSION:
                        /* Nothing beyond the batch itself */
                        break;
                default:
                        unreachable("Invalid sysval");
                }
        }

        ss->dirty_3d = dirty;
        ss->dirty_shader = dirty_shader;
}

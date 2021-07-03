/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2020 Collabora Ltd.
 * Copyright © 2017 Intel Corporation
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

#include "util/macros.h"
#include "util/u_prim.h"
#include "util/u_vbuf.h"
#include "util/u_helpers.h"

#include "panfrost-quirks.h"

#include "pan_pool.h"
#include "pan_bo.h"
#include "pan_cmdstream.h"
#include "pan_context.h"
#include "pan_job.h"
#include "pan_shader.h"
#include "pan_texture.h"

/* Statically assert that PIPE_* enums match the hardware enums.
 * (As long as they match, we don't need to translate them.)
 */
UNUSED static void
pan_pipe_asserts()
{
#define PIPE_ASSERT(x) STATIC_ASSERT((int)x)

        /* Compare functions are natural in both Gallium and Mali */
        PIPE_ASSERT(PIPE_FUNC_NEVER    == MALI_FUNC_NEVER);
        PIPE_ASSERT(PIPE_FUNC_LESS     == MALI_FUNC_LESS);
        PIPE_ASSERT(PIPE_FUNC_EQUAL    == MALI_FUNC_EQUAL);
        PIPE_ASSERT(PIPE_FUNC_LEQUAL   == MALI_FUNC_LEQUAL);
        PIPE_ASSERT(PIPE_FUNC_GREATER  == MALI_FUNC_GREATER);
        PIPE_ASSERT(PIPE_FUNC_NOTEQUAL == MALI_FUNC_NOT_EQUAL);
        PIPE_ASSERT(PIPE_FUNC_GEQUAL   == MALI_FUNC_GEQUAL);
        PIPE_ASSERT(PIPE_FUNC_ALWAYS   == MALI_FUNC_ALWAYS);
}

/* If a BO is accessed for a particular shader stage, will it be in the primary
 * batch (vertex/tiler) or the secondary batch (fragment)? Anything but
 * fragment will be primary, e.g. compute jobs will be considered
 * "vertex/tiler" by analogy */

static inline uint32_t
panfrost_bo_access_for_stage(enum pipe_shader_type stage)
{
        assert(stage == PIPE_SHADER_FRAGMENT ||
               stage == PIPE_SHADER_VERTEX ||
               stage == PIPE_SHADER_COMPUTE);

        return stage == PIPE_SHADER_FRAGMENT ?
               PAN_BO_ACCESS_FRAGMENT :
               PAN_BO_ACCESS_VERTEX_TILER;
}

/* Gets a GPU address for the associated index buffer. Only gauranteed to be
 * good for the duration of the draw (transient), could last longer. Also get
 * the bounds on the index buffer for the range accessed by the draw. We do
 * these operations together because there are natural optimizations which
 * require them to be together. */

mali_ptr
panfrost_get_index_buffer_bounded(struct panfrost_batch *batch,
                                  const struct pipe_draw_info *info,
                                  const struct pipe_draw_start_count_bias *draw,
                                  unsigned *min_index, unsigned *max_index)
{
        struct panfrost_resource *rsrc = pan_resource(info->index.resource);
        struct panfrost_context *ctx = batch->ctx;
        off_t offset = draw->start * info->index_size;
        bool needs_indices = true;
        mali_ptr out = 0;

        if (info->index_bounds_valid) {
                *min_index = info->min_index;
                *max_index = info->max_index;
                needs_indices = false;
        }

        if (!info->has_user_indices) {
                /* Only resources can be directly mapped */
                panfrost_batch_add_bo(batch, rsrc->image.data.bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      PAN_BO_ACCESS_VERTEX_TILER);
                out = rsrc->image.data.bo->ptr.gpu + offset;

                /* Check the cache */
                needs_indices = !panfrost_minmax_cache_get(rsrc->index_cache,
                                                           draw->start,
                                                           draw->count,
                                                           min_index,
                                                           max_index);
        } else {
                /* Otherwise, we need to upload to transient memory */
                const uint8_t *ibuf8 = (const uint8_t *) info->index.user;
                struct panfrost_ptr T =
                        panfrost_pool_alloc_aligned(&batch->pool,
                                draw->count * info->index_size,
                                info->index_size);

                memcpy(T.cpu, ibuf8 + offset, draw->count * info->index_size);
                out = T.gpu;
        }

        if (needs_indices) {
                /* Fallback */
                u_vbuf_get_minmax_index(&ctx->base, info, draw, min_index, max_index);

                if (!info->has_user_indices)
                        panfrost_minmax_cache_add(rsrc->index_cache,
                                                  draw->start, draw->count,
                                                  *min_index, *max_index);
        }

        return out;
}

static unsigned
translate_tex_wrap(enum pipe_tex_wrap w, bool supports_clamp, bool using_nearest)
{
        /* Bifrost doesn't support the GL_CLAMP wrap mode, so instead use
         * CLAMP_TO_EDGE and CLAMP_TO_BORDER. On Midgard, CLAMP is broken for
         * nearest filtering, so use CLAMP_TO_EDGE in that case. */

        switch (w) {
        case PIPE_TEX_WRAP_REPEAT: return MALI_WRAP_MODE_REPEAT;
        case PIPE_TEX_WRAP_CLAMP:
                return using_nearest ? MALI_WRAP_MODE_CLAMP_TO_EDGE :
                     (supports_clamp ? MALI_WRAP_MODE_CLAMP :
                                       MALI_WRAP_MODE_CLAMP_TO_BORDER);
        case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return MALI_WRAP_MODE_CLAMP_TO_EDGE;
        case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return MALI_WRAP_MODE_CLAMP_TO_BORDER;
        case PIPE_TEX_WRAP_MIRROR_REPEAT: return MALI_WRAP_MODE_MIRRORED_REPEAT;
        case PIPE_TEX_WRAP_MIRROR_CLAMP:
                return using_nearest ? MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE :
                     (supports_clamp ? MALI_WRAP_MODE_MIRRORED_CLAMP :
                                       MALI_WRAP_MODE_MIRRORED_CLAMP_TO_BORDER);
        case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE: return MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE;
        case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER: return MALI_WRAP_MODE_MIRRORED_CLAMP_TO_BORDER;
        default: unreachable("Invalid wrap");
        }
}

/* The hardware compares in the wrong order order, so we have to flip before
 * encoding. Yes, really. */

static enum mali_func
panfrost_sampler_compare_func(const struct pipe_sampler_state *cso)
{
        return !cso->compare_mode ? MALI_FUNC_NEVER :
                panfrost_flip_compare_func((enum mali_func) cso->compare_func);
}

static enum mali_mipmap_mode
pan_pipe_to_mipmode(enum pipe_tex_mipfilter f)
{
        switch (f) {
        case PIPE_TEX_MIPFILTER_NEAREST: return MALI_MIPMAP_MODE_NEAREST;
        case PIPE_TEX_MIPFILTER_LINEAR: return MALI_MIPMAP_MODE_TRILINEAR;
        case PIPE_TEX_MIPFILTER_NONE: return MALI_MIPMAP_MODE_NONE;
        default: unreachable("Invalid");
        }
}

void panfrost_sampler_desc_init(const struct pipe_sampler_state *cso,
                                struct mali_midgard_sampler_packed *hw)
{
        bool using_nearest = cso->min_img_filter == PIPE_TEX_MIPFILTER_NEAREST;

        pan_pack(hw, MIDGARD_SAMPLER, cfg) {
                cfg.magnify_nearest = cso->mag_img_filter == PIPE_TEX_FILTER_NEAREST;
                cfg.minify_nearest = cso->min_img_filter == PIPE_TEX_FILTER_NEAREST;
                cfg.mipmap_mode = (cso->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR) ?
                        MALI_MIPMAP_MODE_TRILINEAR : MALI_MIPMAP_MODE_NEAREST;
                cfg.normalized_coordinates = cso->normalized_coords;

                cfg.lod_bias = FIXED_16(cso->lod_bias, true);

                cfg.minimum_lod = FIXED_16(cso->min_lod, false);

                /* If necessary, we disable mipmapping in the sampler descriptor by
                 * clamping the LOD as tight as possible (from 0 to epsilon,
                 * essentially -- remember these are fixed point numbers, so
                 * epsilon=1/256) */

                cfg.maximum_lod = (cso->min_mip_filter == PIPE_TEX_MIPFILTER_NONE) ?
                        cfg.minimum_lod + 1 :
                        FIXED_16(cso->max_lod, false);

                cfg.wrap_mode_s = translate_tex_wrap(cso->wrap_s, true, using_nearest);
                cfg.wrap_mode_t = translate_tex_wrap(cso->wrap_t, true, using_nearest);
                cfg.wrap_mode_r = translate_tex_wrap(cso->wrap_r, true, using_nearest);

                cfg.compare_function = panfrost_sampler_compare_func(cso);
                cfg.seamless_cube_map = cso->seamless_cube_map;

                cfg.border_color_r = cso->border_color.ui[0];
                cfg.border_color_g = cso->border_color.ui[1];
                cfg.border_color_b = cso->border_color.ui[2];
                cfg.border_color_a = cso->border_color.ui[3];
        }
}

void panfrost_sampler_desc_init_bifrost(const struct pipe_sampler_state *cso,
                                        struct mali_bifrost_sampler_packed *hw)
{
        bool using_nearest = cso->min_img_filter == PIPE_TEX_MIPFILTER_NEAREST;

        pan_pack(hw, BIFROST_SAMPLER, cfg) {
                cfg.point_sample_magnify = cso->mag_img_filter == PIPE_TEX_FILTER_NEAREST;
                cfg.point_sample_minify = cso->min_img_filter == PIPE_TEX_FILTER_NEAREST;
                cfg.mipmap_mode = pan_pipe_to_mipmode(cso->min_mip_filter);
                cfg.normalized_coordinates = cso->normalized_coords;

                cfg.lod_bias = FIXED_16(cso->lod_bias, true);
                cfg.minimum_lod = FIXED_16(cso->min_lod, false);
                cfg.maximum_lod = FIXED_16(cso->max_lod, false);

                if (cso->max_anisotropy > 1) {
                        cfg.maximum_anisotropy = cso->max_anisotropy;
                        cfg.lod_algorithm = MALI_LOD_ALGORITHM_ANISOTROPIC;
                }

                cfg.wrap_mode_s = translate_tex_wrap(cso->wrap_s, false, using_nearest);
                cfg.wrap_mode_t = translate_tex_wrap(cso->wrap_t, false, using_nearest);
                cfg.wrap_mode_r = translate_tex_wrap(cso->wrap_r, false, using_nearest);

                cfg.compare_function = panfrost_sampler_compare_func(cso);
                cfg.seamless_cube_map = cso->seamless_cube_map;

                cfg.border_color_r = cso->border_color.ui[0];
                cfg.border_color_g = cso->border_color.ui[1];
                cfg.border_color_b = cso->border_color.ui[2];
                cfg.border_color_a = cso->border_color.ui[3];
        }
}

static bool
panfrost_fs_required(
                struct panfrost_shader_state *fs,
                struct panfrost_blend_state *blend,
                struct pipe_framebuffer_state *state)
{
        /* If we generally have side effects. This inclues use of discard,
         * which can affect the results of an occlusion query. */
        if (fs->info.fs.sidefx)
                return true;

        /* If colour is written we need to execute */
        for (unsigned i = 0; i < state->nr_cbufs; ++i) {
                if (state->cbufs[i] && !blend->info[i].no_colour)
                        return true;
        }

        /* If depth is written and not implied we need to execute.
         * TODO: Predicate on Z/S writes being enabled */
        return (fs->info.fs.writes_depth || fs->info.fs.writes_stencil);
}

static void
panfrost_emit_bifrost_blend(struct panfrost_batch *batch,
                            mali_ptr *blend_shaders, void *rts)
{
        unsigned rt_count = batch->key.nr_cbufs;
        struct panfrost_context *ctx = batch->ctx;
        const struct panfrost_blend_state *so = ctx->blend;
        const struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_shader_state *fs = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        /* Always have at least one render target for depth-only passes */
        for (unsigned i = 0; i < MAX2(rt_count, 1); ++i) {
                /* Disable blending for unbacked render targets */
                if (rt_count == 0 || !batch->key.cbufs[i] || so->info[i].no_colour) {
                        pan_pack(rts + i * MALI_BLEND_LENGTH, BLEND, cfg) {
                                cfg.enable = false;
                                cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_OFF;
                        }

                        continue;
                }

                struct pan_blend_info info = so->info[i];
                enum pipe_format format = batch->key.cbufs[i]->format;
                const struct util_format_description *format_desc;
                unsigned chan_size = 0;

                format_desc = util_format_description(format);

                for (unsigned i = 0; i < format_desc->nr_channels; i++)
                        chan_size = MAX2(format_desc->channel[0].size, chan_size);

                /* Fixed point constant */
                float constant_f = pan_blend_get_constant(
                                info.constant_mask,
                                ctx->blend_color.color);

                u16 constant = constant_f * ((1 << chan_size) - 1);
                constant <<= 16 - chan_size;

                struct mali_blend_packed *packed = rts + (i * MALI_BLEND_LENGTH);

                /* Word 0: Flags and constant */
                pan_pack(packed, BLEND, cfg) {
                        cfg.srgb = util_format_is_srgb(batch->key.cbufs[i]->format);
                        cfg.load_destination = info.load_dest;
                        cfg.round_to_fb_precision = !ctx->blend->base.dither;
                        cfg.alpha_to_one = ctx->blend->base.alpha_to_one;
                        cfg.bifrost.constant = constant;
                }

                if (!blend_shaders[i]) {
                        /* Word 1: Blend Equation */
                        STATIC_ASSERT(MALI_BLEND_EQUATION_LENGTH == 4);
                        packed->opaque[1] = so->equation[i].opaque[0];
                }

                /* Words 2 and 3: Internal blend */
                if (blend_shaders[i]) {
                        /* The blend shader's address needs to be at
                         * the same top 32 bit as the fragment shader.
                         * TODO: Ensure that's always the case.
                         */
                        assert(!fs->bin.bo ||
                                        (blend_shaders[i] & (0xffffffffull << 32)) ==
                                        (fs->bin.gpu & (0xffffffffull << 32)));

                        unsigned ret_offset = fs->info.bifrost.blend[i].return_offset;
                        assert(!(ret_offset & 0x7));

                        pan_pack(&packed->opaque[2], BIFROST_INTERNAL_BLEND, cfg) {
                                cfg.mode = MALI_BIFROST_BLEND_MODE_SHADER;
                                cfg.shader.pc = (u32) blend_shaders[i];
                                cfg.shader.return_value = ret_offset ?
                                        fs->bin.gpu + ret_offset : 0;
                        }
                } else {
                        pan_pack(&packed->opaque[2], BIFROST_INTERNAL_BLEND, cfg) {
                                cfg.mode = info.opaque ?
                                        MALI_BIFROST_BLEND_MODE_OPAQUE :
                                        MALI_BIFROST_BLEND_MODE_FIXED_FUNCTION;

                                /* If we want the conversion to work properly,
                                 * num_comps must be set to 4
                                 */
                                cfg.fixed_function.num_comps = 4;
                                cfg.fixed_function.conversion.memory_format =
                                        panfrost_format_to_bifrost_blend(dev, format);
                                cfg.fixed_function.conversion.register_format =
                                        fs->info.bifrost.blend[i].format;
                                cfg.fixed_function.rt = i;
                        }
                }
        }
}

static void
panfrost_emit_midgard_blend(struct panfrost_batch *batch,
                            mali_ptr *blend_shaders, void *rts)
{
        unsigned rt_count = batch->key.nr_cbufs;
        struct panfrost_context *ctx = batch->ctx;
        const struct panfrost_blend_state *so = ctx->blend;

        /* Always have at least one render target for depth-only passes */
        for (unsigned i = 0; i < MAX2(rt_count, 1); ++i) {
                struct mali_blend_packed *packed = rts + (i * MALI_BLEND_LENGTH);

                /* Disable blending for unbacked render targets */
                if (rt_count == 0 || !batch->key.cbufs[i] || so->info[i].no_colour) {
                        pan_pack(packed, BLEND, cfg) {
                                cfg.enable = false;
                        }

                        continue;
                }

                pan_pack(packed, BLEND, cfg) {
                        struct pan_blend_info info = so->info[i];

                        cfg.srgb = util_format_is_srgb(batch->key.cbufs[i]->format);
                        cfg.load_destination = info.load_dest;
                        cfg.round_to_fb_precision = !ctx->blend->base.dither;
                        cfg.alpha_to_one = ctx->blend->base.alpha_to_one;
                        cfg.midgard.blend_shader = (blend_shaders[i] != 0);
                        if (blend_shaders[i]) {
                                cfg.midgard.shader_pc = blend_shaders[i];
                        } else {
                                cfg.midgard.constant = pan_blend_get_constant(
                                                info.constant_mask,
                                                ctx->blend_color.color);
                        }
                }

                if (!blend_shaders[i]) {
                        /* Word 2: Blend Equation */
                        STATIC_ASSERT(MALI_BLEND_EQUATION_LENGTH == 4);
                        packed->opaque[2] = so->equation[i].opaque[0];
                }
        }
}

static void
panfrost_emit_blend(struct panfrost_batch *batch, void *rts, mali_ptr *blend_shaders)
{
        const struct panfrost_device *dev = pan_device(batch->ctx->base.screen);
        struct panfrost_blend_state *so = batch->ctx->blend;

        if (pan_is_bifrost(dev))
                panfrost_emit_bifrost_blend(batch, blend_shaders, rts);
        else
                panfrost_emit_midgard_blend(batch, blend_shaders, rts);

        for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
                if (!so->info[i].no_colour && batch->key.cbufs[i]) {
                        batch->draws |= (PIPE_CLEAR_COLOR0 << i);
                        batch->resolve |= (PIPE_CLEAR_COLOR0 << i);
                }
        }
}

/* Construct a partial RSD corresponding to no executed fragment shader, and
 * merge with the existing partial RSD. This depends only on the architecture,
 * so packing separately allows the packs to be constant folded away. */

static void
pan_merge_empty_fs(struct mali_renderer_state_packed *rsd, bool is_bifrost)
{
        struct mali_renderer_state_packed empty_rsd;

        if (is_bifrost) {
                pan_pack(&empty_rsd, RENDERER_STATE, cfg) {
                        cfg.properties.bifrost.shader_modifies_coverage = true;
                        cfg.properties.bifrost.allow_forward_pixel_to_kill = true;
                        cfg.properties.bifrost.allow_forward_pixel_to_be_killed = true;
                        cfg.properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
                }
        } else {
                pan_pack(&empty_rsd, RENDERER_STATE, cfg) {
                        cfg.shader.shader = 0x1;
                        cfg.properties.midgard.work_register_count = 1;
                        cfg.properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
                        cfg.properties.midgard.force_early_z = true;
                }
        }

        pan_merge((*rsd), empty_rsd, RENDERER_STATE);
}

/* Get the last blend shader, for an erratum workaround */

static mali_ptr
panfrost_last_nonnull(mali_ptr *ptrs, unsigned count)
{
        for (signed i = ((signed) count - 1); i >= 0; --i) {
                if (ptrs[i])
                        return ptrs[i];
        }

        return 0;
}

static void
panfrost_prepare_fs_state(struct panfrost_context *ctx,
                          mali_ptr *blend_shaders,
                          struct mali_renderer_state_packed *rsd)
{
        const struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct pipe_rasterizer_state *rast = &ctx->rasterizer->base;
        const struct panfrost_zsa_state *zsa = ctx->depth_stencil;
        struct panfrost_shader_state *fs = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);
        struct panfrost_blend_state *so = ctx->blend;
        bool alpha_to_coverage = ctx->blend->base.alpha_to_coverage;
        bool msaa = rast->multisample;

        pan_pack(rsd, RENDERER_STATE, cfg) {
                if (pan_is_bifrost(dev) && panfrost_fs_required(fs, so, &ctx->pipe_framebuffer)) {
                        /* Track if any colour buffer is reused across draws, either
                         * from reading it directly, or from failing to write it */
                        unsigned rt_mask = ctx->fb_rt_mask;
                        bool blend_reads_dest = (so->load_dest_mask & rt_mask);

                        cfg.properties.bifrost.allow_forward_pixel_to_kill =
                                fs->info.fs.can_fpk &&
                                !(rt_mask & ~fs->info.outputs_written) &&
                                !alpha_to_coverage &&
                                !blend_reads_dest;
                } else if (!pan_is_bifrost(dev)) {
                        unsigned rt_count = ctx->pipe_framebuffer.nr_cbufs;

                        if (panfrost_fs_required(fs, ctx->blend, &ctx->pipe_framebuffer)) {
                                cfg.properties.midgard.force_early_z =
                                        fs->info.fs.can_early_z && !alpha_to_coverage &&
                                        ((enum mali_func) zsa->base.alpha_func == MALI_FUNC_ALWAYS);

                                bool has_blend_shader = false;

                                for (unsigned c = 0; c < rt_count; ++c)
                                        has_blend_shader |= (blend_shaders[c] != 0);

                                /* TODO: Reduce this limit? */
                                if (has_blend_shader)
                                        cfg.properties.midgard.work_register_count = MAX2(fs->info.work_reg_count, 8);
                                else
                                        cfg.properties.midgard.work_register_count = fs->info.work_reg_count;

                                /* Hardware quirks around early-zs forcing
                                 * without a depth buffer. Note this breaks
                                 * occlusion queries. */
                                bool has_oq = ctx->occlusion_query && ctx->active_queries;
                                bool force_ez_with_discard = !zsa->enabled && !has_oq;

                                cfg.properties.midgard.shader_reads_tilebuffer =
                                        force_ez_with_discard && fs->info.fs.can_discard;
                                cfg.properties.midgard.shader_contains_discard =
                                        !force_ez_with_discard && fs->info.fs.can_discard;
                        }

                        if (dev->quirks & MIDGARD_SFBD && rt_count > 0) {
                                cfg.multisample_misc.sfbd_load_destination = so->info[0].load_dest;
                                cfg.multisample_misc.sfbd_blend_shader = (blend_shaders[0] != 0);
                                cfg.stencil_mask_misc.sfbd_write_enable = !so->info[0].no_colour;
                                cfg.stencil_mask_misc.sfbd_srgb = util_format_is_srgb(ctx->pipe_framebuffer.cbufs[0]->format);
                                cfg.stencil_mask_misc.sfbd_dither_disable = !so->base.dither;
                                cfg.stencil_mask_misc.sfbd_alpha_to_one = so->base.alpha_to_one;

                                if (blend_shaders[0]) {
                                        cfg.sfbd_blend_shader = blend_shaders[0];
                                } else {
                                        cfg.sfbd_blend_constant = pan_blend_get_constant(
                                                        so->info[0].constant_mask,
                                                        ctx->blend_color.color);
                                }
                        } else if (dev->quirks & MIDGARD_SFBD) {
                                /* If there is no colour buffer, leaving fields default is
                                 * fine, except for blending which is nonnullable */
                                cfg.sfbd_blend_equation.color_mask = 0xf;
                                cfg.sfbd_blend_equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
                                cfg.sfbd_blend_equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
                                cfg.sfbd_blend_equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
                                cfg.sfbd_blend_equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
                                cfg.sfbd_blend_equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
                                cfg.sfbd_blend_equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
                        } else {
                                /* Workaround on v5 */
                                cfg.sfbd_blend_shader = panfrost_last_nonnull(blend_shaders, rt_count);
                        }
                }

                cfg.multisample_misc.sample_mask = msaa ? ctx->sample_mask : 0xFFFF;

                cfg.multisample_misc.evaluate_per_sample =
                        msaa && (ctx->min_samples > 1);

                cfg.stencil_mask_misc.alpha_to_coverage = alpha_to_coverage;
                cfg.depth_units = rast->offset_units * 2.0f;
                cfg.depth_factor = rast->offset_scale;

                bool back_enab = zsa->base.stencil[1].enabled;
                cfg.stencil_front.reference_value = ctx->stencil_ref.ref_value[0];
                cfg.stencil_back.reference_value = ctx->stencil_ref.ref_value[back_enab ? 1 : 0];

                /* v6+ fits register preload here, no alpha testing */
                if (dev->arch <= 5)
                        cfg.alpha_reference = zsa->base.alpha_ref_value;
        }
}

static void
panfrost_emit_frag_shader(struct panfrost_context *ctx,
                          struct mali_renderer_state_packed *fragmeta,
                          mali_ptr *blend_shaders)
{
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        const struct panfrost_zsa_state *zsa = ctx->depth_stencil;
        const struct panfrost_rasterizer *rast = ctx->rasterizer;
        struct panfrost_shader_state *fs =
                panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        /* We need to merge several several partial renderer state descriptors,
         * so stage to temporary storage rather than reading back write-combine
         * memory, which will trash performance. */
        struct mali_renderer_state_packed rsd;
        panfrost_prepare_fs_state(ctx, blend_shaders, &rsd);

        if ((dev->quirks & MIDGARD_SFBD)
                        && ctx->pipe_framebuffer.nr_cbufs > 0
                        && !blend_shaders[0]) {

                /* Word 14: SFBD Blend Equation */
                STATIC_ASSERT(MALI_BLEND_EQUATION_LENGTH == 4);
                rsd.opaque[14] = ctx->blend->equation[0].opaque[0];
        }

        /* Merge with CSO state and upload */
        if (panfrost_fs_required(fs, ctx->blend, &ctx->pipe_framebuffer))
                pan_merge(rsd, fs->partial_rsd, RENDERER_STATE);
        else
                pan_merge_empty_fs(&rsd, pan_is_bifrost(dev));

        /* Word 8, 9 Misc state */
        rsd.opaque[8] |= zsa->rsd_depth.opaque[0]
                       | rast->multisample.opaque[0];

        rsd.opaque[9] |= zsa->rsd_stencil.opaque[0]
                       | rast->stencil_misc.opaque[0];

        /* Word 10, 11 Stencil Front and Back */
        rsd.opaque[10] |= zsa->stencil_front.opaque[0];
        rsd.opaque[11] |= zsa->stencil_back.opaque[0];

        memcpy(fragmeta, &rsd, sizeof(rsd));
}

mali_ptr
panfrost_emit_compute_shader_meta(struct panfrost_batch *batch, enum pipe_shader_type stage)
{
        struct panfrost_shader_state *ss = panfrost_get_shader_state(batch->ctx, stage);

        panfrost_batch_add_bo(batch, ss->bin.bo,
                              PAN_BO_ACCESS_SHARED |
                              PAN_BO_ACCESS_READ |
                              PAN_BO_ACCESS_VERTEX_TILER);

        panfrost_batch_add_bo(batch, ss->state.bo,
                              PAN_BO_ACCESS_SHARED |
                              PAN_BO_ACCESS_READ |
                              PAN_BO_ACCESS_VERTEX_TILER);

        return ss->state.gpu;
}

mali_ptr
panfrost_emit_frag_shader_meta(struct panfrost_batch *batch)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_state *ss = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        /* Add the shader BO to the batch. */
        panfrost_batch_add_bo(batch, ss->bin.bo,
                              PAN_BO_ACCESS_SHARED |
                              PAN_BO_ACCESS_READ |
                              PAN_BO_ACCESS_FRAGMENT);

        struct panfrost_device *dev = pan_device(ctx->base.screen);
        unsigned rt_count = MAX2(ctx->pipe_framebuffer.nr_cbufs, 1);
        struct panfrost_ptr xfer;

        if (dev->quirks & MIDGARD_SFBD) {
                xfer = panfrost_pool_alloc_desc(&batch->pool, RENDERER_STATE);
        } else {
                xfer = panfrost_pool_alloc_desc_aggregate(&batch->pool,
                                                          PAN_DESC(RENDERER_STATE),
                                                          PAN_DESC_ARRAY(rt_count, BLEND));
        }

        mali_ptr blend_shaders[PIPE_MAX_COLOR_BUFS];
        unsigned shader_offset = 0;
        struct panfrost_bo *shader_bo = NULL;

        for (unsigned c = 0; c < ctx->pipe_framebuffer.nr_cbufs; ++c) {
                if (ctx->pipe_framebuffer.cbufs[c]) {
                        blend_shaders[c] = panfrost_get_blend(batch,
                                        c, &shader_bo, &shader_offset);
                }
        }

        panfrost_emit_frag_shader(ctx, (struct mali_renderer_state_packed *) xfer.cpu, blend_shaders);

        if (!(dev->quirks & MIDGARD_SFBD))
                panfrost_emit_blend(batch, xfer.cpu + MALI_RENDERER_STATE_LENGTH, blend_shaders);
        else {
                batch->draws |= PIPE_CLEAR_COLOR0;
                batch->resolve |= PIPE_CLEAR_COLOR0;
        }

        if (ctx->depth_stencil->base.depth_enabled)
                batch->read |= PIPE_CLEAR_DEPTH;

        if (ctx->depth_stencil->base.stencil[0].enabled)
                batch->read |= PIPE_CLEAR_STENCIL;

        return xfer.gpu;
}

mali_ptr
panfrost_emit_viewport(struct panfrost_batch *batch)
{
        struct panfrost_context *ctx = batch->ctx;
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;
        const struct pipe_scissor_state *ss = &ctx->scissor;
        const struct pipe_rasterizer_state *rast = &ctx->rasterizer->base;

        /* Derive min/max from translate/scale. Note since |x| >= 0 by
         * definition, we have that -|x| <= |x| hence translate - |scale| <=
         * translate + |scale|, so the ordering is correct here. */
        float vp_minx = vp->translate[0] - fabsf(vp->scale[0]);
        float vp_maxx = vp->translate[0] + fabsf(vp->scale[0]);
        float vp_miny = vp->translate[1] - fabsf(vp->scale[1]);
        float vp_maxy = vp->translate[1] + fabsf(vp->scale[1]);
        float minz = (vp->translate[2] - fabsf(vp->scale[2]));
        float maxz = (vp->translate[2] + fabsf(vp->scale[2]));

        /* Scissor to the intersection of viewport and to the scissor, clamped
         * to the framebuffer */

        unsigned minx = MIN2(batch->key.width, MAX2((int) vp_minx, 0));
        unsigned maxx = MIN2(batch->key.width, MAX2((int) vp_maxx, 0));
        unsigned miny = MIN2(batch->key.height, MAX2((int) vp_miny, 0));
        unsigned maxy = MIN2(batch->key.height, MAX2((int) vp_maxy, 0));

        if (ss && rast->scissor) {
                minx = MAX2(ss->minx, minx);
                miny = MAX2(ss->miny, miny);
                maxx = MIN2(ss->maxx, maxx);
                maxy = MIN2(ss->maxy, maxy);
        }

        /* Set the range to [1, 1) so max values don't wrap round */
        if (maxx == 0 || maxy == 0)
                maxx = maxy = minx = miny = 1;

        struct panfrost_ptr T = panfrost_pool_alloc_desc(&batch->pool, VIEWPORT);

        pan_pack(T.cpu, VIEWPORT, cfg) {
                /* [minx, maxx) and [miny, maxy) are exclusive ranges, but
                 * these are inclusive */
                cfg.scissor_minimum_x = minx;
                cfg.scissor_minimum_y = miny;
                cfg.scissor_maximum_x = maxx - 1;
                cfg.scissor_maximum_y = maxy - 1;

                cfg.minimum_z = rast->depth_clip_near ? minz : -INFINITY;
                cfg.maximum_z = rast->depth_clip_far ? maxz : INFINITY;
        }

        panfrost_batch_union_scissor(batch, minx, miny, maxx, maxy);
        batch->scissor_culls_everything = (minx >= maxx || miny >= maxy);

        return T.gpu;
}

static mali_ptr
panfrost_map_constant_buffer_gpu(struct panfrost_batch *batch,
                                 enum pipe_shader_type st,
                                 struct panfrost_constant_buffer *buf,
                                 unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc) {
                panfrost_batch_add_bo(batch, rsrc->image.data.bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      panfrost_bo_access_for_stage(st));

                /* Alignment gauranteed by
                 * PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT */
                return rsrc->image.data.bo->ptr.gpu + cb->buffer_offset;
        } else if (cb->user_buffer) {
                return panfrost_pool_upload_aligned(&batch->pool,
                                                 cb->user_buffer +
                                                 cb->buffer_offset,
                                                 cb->buffer_size, 16);
        } else {
                unreachable("No constant buffer");
        }
}

struct sysval_uniform {
        union {
                float f[4];
                int32_t i[4];
                uint32_t u[4];
                uint64_t du[2];
        };
};

static void
panfrost_upload_viewport_scale_sysval(struct panfrost_batch *batch,
                                      struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->scale[0];
        uniform->f[1] = vp->scale[1];
        uniform->f[2] = vp->scale[2];
}

static void
panfrost_upload_viewport_offset_sysval(struct panfrost_batch *batch,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->translate[0];
        uniform->f[1] = vp->translate[1];
        uniform->f[2] = vp->translate[2];
}

static void panfrost_upload_txs_sysval(struct panfrost_batch *batch,
                                       enum pipe_shader_type st,
                                       unsigned int sysvalid,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        unsigned texidx = PAN_SYSVAL_ID_TO_TXS_TEX_IDX(sysvalid);
        unsigned dim = PAN_SYSVAL_ID_TO_TXS_DIM(sysvalid);
        bool is_array = PAN_SYSVAL_ID_TO_TXS_IS_ARRAY(sysvalid);
        struct pipe_sampler_view *tex = &ctx->sampler_views[st][texidx]->base;

        assert(dim);

        if (tex->target == PIPE_BUFFER) {
                assert(dim == 1);
                uniform->i[0] =
                        tex->u.buf.size / util_format_get_blocksize(tex->format);
                return;
        }

        uniform->i[0] = u_minify(tex->texture->width0, tex->u.tex.first_level);

        if (dim > 1)
                uniform->i[1] = u_minify(tex->texture->height0,
                                         tex->u.tex.first_level);

        if (dim > 2)
                uniform->i[2] = u_minify(tex->texture->depth0,
                                         tex->u.tex.first_level);

        if (is_array)
                uniform->i[dim] = tex->texture->array_size;
}

static void panfrost_upload_image_size_sysval(struct panfrost_batch *batch,
                                              enum pipe_shader_type st,
                                              unsigned int sysvalid,
                                              struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        unsigned idx = PAN_SYSVAL_ID_TO_TXS_TEX_IDX(sysvalid);
        unsigned dim = PAN_SYSVAL_ID_TO_TXS_DIM(sysvalid);
        unsigned is_array = PAN_SYSVAL_ID_TO_TXS_IS_ARRAY(sysvalid);

        assert(dim && dim < 4);

        struct pipe_image_view *image = &ctx->images[st][idx];

        if (image->resource->target == PIPE_BUFFER) {
                unsigned blocksize = util_format_get_blocksize(image->format);
                uniform->i[0] = image->resource->width0 / blocksize;
                return;
        }

        uniform->i[0] = u_minify(image->resource->width0,
                                 image->u.tex.level);

        if (dim > 1)
                uniform->i[1] = u_minify(image->resource->height0,
                                         image->u.tex.level);

        if (dim > 2)
                uniform->i[2] = u_minify(image->resource->depth0,
                                         image->u.tex.level);

        if (is_array)
                uniform->i[dim] = image->resource->array_size;
}

static void
panfrost_upload_ssbo_sysval(struct panfrost_batch *batch,
                            enum pipe_shader_type st,
                            unsigned ssbo_id,
                            struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        assert(ctx->ssbo_mask[st] & (1 << ssbo_id));
        struct pipe_shader_buffer sb = ctx->ssbo[st][ssbo_id];

        /* Compute address */
        struct panfrost_resource *rsrc = pan_resource(sb.buffer);
        struct panfrost_bo *bo = rsrc->image.data.bo;

        panfrost_batch_add_bo(batch, bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_RW |
                              panfrost_bo_access_for_stage(st));

        util_range_add(&rsrc->base, &rsrc->valid_buffer_range,
                        sb.buffer_offset, sb.buffer_size);

        /* Upload address and size as sysval */
        uniform->du[0] = bo->ptr.gpu + sb.buffer_offset;
        uniform->u[2] = sb.buffer_size;
}

static void
panfrost_upload_sampler_sysval(struct panfrost_batch *batch,
                               enum pipe_shader_type st,
                               unsigned samp_idx,
                               struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_sampler_state *sampl = &ctx->samplers[st][samp_idx]->base;

        uniform->f[0] = sampl->min_lod;
        uniform->f[1] = sampl->max_lod;
        uniform->f[2] = sampl->lod_bias;

        /* Even without any errata, Midgard represents "no mipmapping" as
         * fixing the LOD with the clamps; keep behaviour consistent. c.f.
         * panfrost_create_sampler_state which also explains our choice of
         * epsilon value (again to keep behaviour consistent) */

        if (sampl->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
                uniform->f[1] = uniform->f[0] + (1.0/256.0);
}

static void
panfrost_upload_num_work_groups_sysval(struct panfrost_batch *batch,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        uniform->u[0] = ctx->compute_grid->grid[0];
        uniform->u[1] = ctx->compute_grid->grid[1];
        uniform->u[2] = ctx->compute_grid->grid[2];
}

static void
panfrost_upload_local_group_size_sysval(struct panfrost_batch *batch,
                                        struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        uniform->u[0] = ctx->compute_grid->block[0];
        uniform->u[1] = ctx->compute_grid->block[1];
        uniform->u[2] = ctx->compute_grid->block[2];
}

static void
panfrost_upload_work_dim_sysval(struct panfrost_batch *batch,
                                struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        uniform->u[0] = ctx->compute_grid->work_dim;
}

/* Sample positions are pushed in a Bifrost specific format on Bifrost. On
 * Midgard, we emulate the Bifrost path with some extra arithmetic in the
 * shader, to keep the code as unified as possible. */

static void
panfrost_upload_sample_positions_sysval(struct panfrost_batch *batch,
                                struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);

        unsigned samples = util_framebuffer_get_num_samples(&batch->key);
        uniform->du[0] = panfrost_sample_positions(dev, panfrost_sample_pattern(samples));
}

static void
panfrost_upload_multisampled_sysval(struct panfrost_batch *batch,
                                struct sysval_uniform *uniform)
{
        unsigned samples = util_framebuffer_get_num_samples(&batch->key);
        uniform->u[0] = samples > 1;
}

static void
panfrost_upload_rt_conversion_sysval(struct panfrost_batch *batch,
                unsigned size_and_rt, struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        unsigned rt = size_and_rt & 0xF;
        unsigned size = size_and_rt >> 4;

        if (rt < batch->key.nr_cbufs && batch->key.cbufs[rt]) {
                enum pipe_format format = batch->key.cbufs[rt]->format;
                uniform->u[0] =
                        pan_blend_get_bifrost_desc(dev, format, rt, size) >> 32;
        } else {
                pan_pack(&uniform->u[0], BIFROST_INTERNAL_CONVERSION, cfg)
                        cfg.memory_format = dev->formats[PIPE_FORMAT_NONE].hw;
        }
}

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

static void
panfrost_upload_sysvals(struct panfrost_batch *batch,
                        const struct panfrost_ptr *ptr,
                        struct panfrost_shader_state *ss,
                        enum pipe_shader_type st)
{
        struct sysval_uniform *uniforms = ptr->cpu;

        for (unsigned i = 0; i < ss->info.sysvals.sysval_count; ++i) {
                int sysval = ss->info.sysvals.sysvals[i];

                switch (PAN_SYSVAL_TYPE(sysval)) {
                case PAN_SYSVAL_VIEWPORT_SCALE:
                        panfrost_upload_viewport_scale_sysval(batch,
                                                              &uniforms[i]);
                        break;
                case PAN_SYSVAL_VIEWPORT_OFFSET:
                        panfrost_upload_viewport_offset_sysval(batch,
                                                               &uniforms[i]);
                        break;
                case PAN_SYSVAL_TEXTURE_SIZE:
                        panfrost_upload_txs_sysval(batch, st,
                                                   PAN_SYSVAL_ID(sysval),
                                                   &uniforms[i]);
                        break;
                case PAN_SYSVAL_SSBO:
                        panfrost_upload_ssbo_sysval(batch, st,
                                                    PAN_SYSVAL_ID(sysval),
                                                    &uniforms[i]);
                        break;
                case PAN_SYSVAL_NUM_WORK_GROUPS:
                        for (unsigned j = 0; j < 3; j++) {
                                batch->num_wg_sysval[j] =
                                        ptr->gpu + (i * sizeof(*uniforms)) + (j * 4);
                        }
                        panfrost_upload_num_work_groups_sysval(batch,
                                                               &uniforms[i]);
                        break;
                case PAN_SYSVAL_LOCAL_GROUP_SIZE:
                        panfrost_upload_local_group_size_sysval(batch,
                                                                &uniforms[i]);
                        break;
                case PAN_SYSVAL_WORK_DIM:
                        panfrost_upload_work_dim_sysval(batch,
                                                        &uniforms[i]);
                        break;
                case PAN_SYSVAL_SAMPLER:
                        panfrost_upload_sampler_sysval(batch, st,
                                                       PAN_SYSVAL_ID(sysval),
                                                       &uniforms[i]);
                        break;
                case PAN_SYSVAL_IMAGE_SIZE:
                        panfrost_upload_image_size_sysval(batch, st,
                                                          PAN_SYSVAL_ID(sysval),
                                                          &uniforms[i]);
                        break;
                case PAN_SYSVAL_SAMPLE_POSITIONS:
                        panfrost_upload_sample_positions_sysval(batch,
                                                        &uniforms[i]);
                        break;
                case PAN_SYSVAL_MULTISAMPLED:
                        panfrost_upload_multisampled_sysval(batch,
                                                               &uniforms[i]);
                        break;
                case PAN_SYSVAL_RT_CONVERSION:
                        panfrost_upload_rt_conversion_sysval(batch,
                                        PAN_SYSVAL_ID(sysval), &uniforms[i]);
                        break;
                case PAN_SYSVAL_VERTEX_INSTANCE_OFFSETS:
                        batch->ctx->first_vertex_sysval_ptr =
                                ptr->gpu + (i * sizeof(*uniforms));
                        batch->ctx->base_vertex_sysval_ptr =
                                batch->ctx->first_vertex_sysval_ptr + 4;
                        batch->ctx->base_instance_sysval_ptr =
                                batch->ctx->first_vertex_sysval_ptr + 8;

                        uniforms[i].u[0] = batch->ctx->offset_start;
                        uniforms[i].u[1] = batch->ctx->base_vertex;
                        uniforms[i].u[2] = batch->ctx->base_instance;
                        break;
                case PAN_SYSVAL_DRAWID:
                        uniforms[i].u[0] = batch->ctx->drawid;
                        break;
                default:
                        assert(0);
                }
        }
}

static const void *
panfrost_map_constant_buffer_cpu(struct panfrost_context *ctx,
                                 struct panfrost_constant_buffer *buf,
                                 unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc) {
                panfrost_bo_mmap(rsrc->image.data.bo);
                panfrost_flush_batches_accessing_bo(ctx, rsrc->image.data.bo, false);
                panfrost_bo_wait(rsrc->image.data.bo, INT64_MAX, false);

                return rsrc->image.data.bo->ptr.cpu + cb->buffer_offset;
        } else if (cb->user_buffer) {
                return cb->user_buffer + cb->buffer_offset;
        } else
                unreachable("No constant buffer");
}

mali_ptr
panfrost_emit_const_buf(struct panfrost_batch *batch,
                        enum pipe_shader_type stage,
                        mali_ptr *push_constants)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_variants *all = ctx->shader[stage];

        if (!all)
                return 0;

        struct panfrost_constant_buffer *buf = &ctx->constant_buffer[stage];
        struct panfrost_shader_state *ss = &all->variants[all->active_variant];

        /* Allocate room for the sysval and the uniforms */
        size_t sys_size = sizeof(float) * 4 * ss->info.sysvals.sysval_count;
        struct panfrost_ptr transfer =
                panfrost_pool_alloc_aligned(&batch->pool, sys_size, 16);

        /* Upload sysvals requested by the shader */
        panfrost_upload_sysvals(batch, &transfer, ss, stage);

        /* Next up, attach UBOs. UBO count includes gaps but no sysval UBO */
        struct panfrost_shader_state *shader = panfrost_get_shader_state(ctx, stage);
        unsigned ubo_count = shader->info.ubo_count - (sys_size ? 1 : 0);
        unsigned sysval_ubo = sys_size ? ubo_count : ~0;

        struct panfrost_ptr ubos =
                panfrost_pool_alloc_desc_array(&batch->pool, ubo_count + 1,
                                               UNIFORM_BUFFER);

        uint64_t *ubo_ptr = (uint64_t *) ubos.cpu;

        /* Upload sysval as a final UBO */

        if (sys_size) {
                pan_pack(ubo_ptr + ubo_count, UNIFORM_BUFFER, cfg) {
                        cfg.entries = DIV_ROUND_UP(sys_size, 16);
                        cfg.pointer = transfer.gpu;
                }
        }

        /* The rest are honest-to-goodness UBOs */

        for (unsigned ubo = 0; ubo < ubo_count; ++ubo) {
                size_t usz = buf->cb[ubo].buffer_size;
                bool enabled = buf->enabled_mask & (1 << ubo);
                bool empty = usz == 0;

                if (!enabled || empty) {
                        ubo_ptr[ubo] = 0;
                        continue;
                }

                /* Issue (57) for the ARB_uniform_buffer_object spec says that
                 * the buffer can be larger than the uniform data inside it,
                 * so clamp ubo size to what hardware supports. */

                pan_pack(ubo_ptr + ubo, UNIFORM_BUFFER, cfg) {
                        cfg.entries = MIN2(DIV_ROUND_UP(usz, 16), 1 << 12);
                        cfg.pointer = panfrost_map_constant_buffer_gpu(batch,
                                        stage, buf, ubo);
                }
        }

        if (ss->info.push.count == 0)
                return ubos.gpu;

        /* Copy push constants required by the shader */
        struct panfrost_ptr push_transfer =
                panfrost_pool_alloc_aligned(&batch->pool,
                                            ss->info.push.count * 4, 16);

        uint32_t *push_cpu = (uint32_t *) push_transfer.cpu;
        *push_constants = push_transfer.gpu;

        for (unsigned i = 0; i < ss->info.push.count; ++i) {
                struct panfrost_ubo_word src = ss->info.push.words[i];

                if (src.ubo == sysval_ubo) {
                        unsigned sysval_idx = src.offset / 16;
                        unsigned sysval_comp = (src.offset % 16) / 4;
                        unsigned sysval_type = PAN_SYSVAL_TYPE(ss->info.sysvals.sysvals[sysval_idx]);
                        mali_ptr ptr = push_transfer.gpu + (4 * i);

                        switch (sysval_type) {
                        case PAN_SYSVAL_VERTEX_INSTANCE_OFFSETS:
                                switch (sysval_comp) {
                                case 0:
                                        batch->ctx->first_vertex_sysval_ptr = ptr;
                                        break;
                                case 1:
                                        batch->ctx->base_vertex_sysval_ptr = ptr;
                                        break;
                                case 2:
                                        batch->ctx->base_instance_sysval_ptr = ptr;
                                        break;
                                case 3:
                                        /* Spurious (Midgard doesn't pack) */
                                        break;
                                default:
                                        unreachable("Invalid vertex/instance offset component\n");
                                }
                                break;

                        case PAN_SYSVAL_NUM_WORK_GROUPS:
                                batch->num_wg_sysval[sysval_comp] = ptr;
                                break;

                        default:
                                break;
                        }
                }
                /* Map the UBO, this should be cheap. However this is reading
                 * from write-combine memory which is _very_ slow. It might pay
                 * off to upload sysvals to a staging buffer on the CPU on the
                 * assumption sysvals will get pushed (TODO) */

                const void *mapped_ubo = (src.ubo == sysval_ubo) ? transfer.cpu :
                        panfrost_map_constant_buffer_cpu(ctx, buf, src.ubo);

                /* TODO: Is there any benefit to combining ranges */
                memcpy(push_cpu + i, (uint8_t *) mapped_ubo + src.offset, 4);
        }

        return ubos.gpu;
}

mali_ptr
panfrost_emit_shared_memory(struct panfrost_batch *batch,
                            const struct pipe_grid_info *info)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_shader_variants *all = ctx->shader[PIPE_SHADER_COMPUTE];
        struct panfrost_shader_state *ss = &all->variants[all->active_variant];
        struct panfrost_ptr t =
                panfrost_pool_alloc_desc(&batch->pool, LOCAL_STORAGE);

        pan_pack(t.cpu, LOCAL_STORAGE, ls) {
                unsigned wls_single_size =
                        util_next_power_of_two(MAX2(ss->info.wls_size, 128));

                if (ss->info.wls_size) {
                        ls.wls_instances =
                                util_next_power_of_two(info->grid[0]) *
                                util_next_power_of_two(info->grid[1]) *
                                util_next_power_of_two(info->grid[2]);

                        ls.wls_size_scale = util_logbase2(wls_single_size) + 1;

                        unsigned wls_size = wls_single_size * ls.wls_instances * dev->core_count;

                        ls.wls_base_pointer =
                                (panfrost_batch_get_shared_memory(batch,
                                                                  wls_size,
                                                                  1))->ptr.gpu;
                } else {
                        ls.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
                }

                if (ss->info.tls_size) {
                        unsigned shift =
                                panfrost_get_stack_shift(ss->info.tls_size);
                        struct panfrost_bo *bo =
                                panfrost_batch_get_scratchpad(batch,
                                                              ss->info.tls_size,
                                                              dev->thread_tls_alloc,
                                                              dev->core_count);

                        ls.tls_size = shift;
                        ls.tls_base_pointer = bo->ptr.gpu;
                }
        };

        return t.gpu;
}

static mali_ptr
panfrost_get_tex_desc(struct panfrost_batch *batch,
                      enum pipe_shader_type st,
                      struct panfrost_sampler_view *view)
{
        if (!view)
                return (mali_ptr) 0;

        struct pipe_sampler_view *pview = &view->base;
        struct panfrost_resource *rsrc = pan_resource(pview->texture);

        /* Add the BO to the job so it's retained until the job is done. */

        panfrost_batch_add_bo(batch, rsrc->image.data.bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                              panfrost_bo_access_for_stage(st));

        panfrost_batch_add_bo(batch, view->state.bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                              panfrost_bo_access_for_stage(st));

        return view->state.gpu;
}

static void
panfrost_update_sampler_view(struct panfrost_sampler_view *view,
                             struct pipe_context *pctx)
{
        struct panfrost_resource *rsrc = pan_resource(view->base.texture);
        if (view->texture_bo != rsrc->image.data.bo->ptr.gpu ||
            view->modifier != rsrc->image.layout.modifier) {
                panfrost_bo_unreference(view->state.bo);
                panfrost_create_sampler_view_bo(view, pctx, &rsrc->base);
        }
}

mali_ptr
panfrost_emit_texture_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *device = pan_device(ctx->base.screen);

        if (!ctx->sampler_view_count[stage])
                return 0;

        if (pan_is_bifrost(device)) {
                struct panfrost_ptr T =
                        panfrost_pool_alloc_desc_array(&batch->pool,
                                                       ctx->sampler_view_count[stage],
                                                       BIFROST_TEXTURE);
                struct mali_bifrost_texture_packed *out =
                        (struct mali_bifrost_texture_packed *) T.cpu;

                for (int i = 0; i < ctx->sampler_view_count[stage]; ++i) {
                        struct panfrost_sampler_view *view = ctx->sampler_views[stage][i];
                        struct pipe_sampler_view *pview = &view->base;
                        struct panfrost_resource *rsrc = pan_resource(pview->texture);

                        panfrost_update_sampler_view(view, &ctx->base);
                        out[i] = view->bifrost_descriptor;

                        /* Add the BOs to the job so they are retained until the job is done. */

                        panfrost_batch_add_bo(batch, rsrc->image.data.bo,
                                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                                              panfrost_bo_access_for_stage(stage));

                        panfrost_batch_add_bo(batch, view->state.bo,
                                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                                              panfrost_bo_access_for_stage(stage));
                }

                return T.gpu;
        } else {
                uint64_t trampolines[PIPE_MAX_SHADER_SAMPLER_VIEWS];

                for (int i = 0; i < ctx->sampler_view_count[stage]; ++i) {
                        struct panfrost_sampler_view *view = ctx->sampler_views[stage][i];

                        panfrost_update_sampler_view(view, &ctx->base);

                        trampolines[i] = panfrost_get_tex_desc(batch, stage, view);
                }

                return panfrost_pool_upload_aligned(&batch->pool, trampolines,
                                sizeof(uint64_t) *
                                ctx->sampler_view_count[stage],
                                sizeof(uint64_t));
        }
}

mali_ptr
panfrost_emit_sampler_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage)
{
        struct panfrost_context *ctx = batch->ctx;

        if (!ctx->sampler_count[stage])
                return 0;

        assert(MALI_BIFROST_SAMPLER_LENGTH == MALI_MIDGARD_SAMPLER_LENGTH);
        assert(MALI_BIFROST_SAMPLER_ALIGN == MALI_MIDGARD_SAMPLER_ALIGN);

        struct panfrost_ptr T =
                panfrost_pool_alloc_desc_array(&batch->pool, ctx->sampler_count[stage],
                                               MIDGARD_SAMPLER);
        struct mali_midgard_sampler_packed *out = (struct mali_midgard_sampler_packed *) T.cpu;

        for (unsigned i = 0; i < ctx->sampler_count[stage]; ++i)
                out[i] = ctx->samplers[stage][i]->hw;

        return T.gpu;
}

/* Packs all image attribute descs and attribute buffer descs.
 * `first_image_buf_index` must be the index of the first image attribute buffer descriptor.
 */
static void
emit_image_attribs(struct panfrost_context *ctx, enum pipe_shader_type shader,
                   struct mali_attribute_packed *attribs, unsigned first_buf)
{
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        unsigned last_bit = util_last_bit(ctx->image_mask[shader]);

        for (unsigned i = 0; i < last_bit; ++i) {
                enum pipe_format format = ctx->images[shader][i].format;

                pan_pack(attribs + i, ATTRIBUTE, cfg) {
                        /* Continuation record means 2 buffers per image */
                        cfg.buffer_index = first_buf + (i * 2);
                        cfg.offset_enable = !pan_is_bifrost(dev);
                        cfg.format = dev->formats[format].hw;
                }
        }
}

static enum mali_attribute_type
pan_modifier_to_attr_type(uint64_t modifier)
{
        switch (modifier) {
        case DRM_FORMAT_MOD_LINEAR:
                return MALI_ATTRIBUTE_TYPE_3D_LINEAR;
        case DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED:
                return MALI_ATTRIBUTE_TYPE_3D_INTERLEAVED;
        default:
                unreachable("Invalid modifier for attribute record");
        }
}

static void
emit_image_bufs(struct panfrost_batch *batch, enum pipe_shader_type shader,
                struct mali_attribute_buffer_packed *bufs,
                unsigned first_image_buf_index)
{
        struct panfrost_context *ctx = batch->ctx;
        unsigned last_bit = util_last_bit(ctx->image_mask[shader]);

        for (unsigned i = 0; i < last_bit; ++i) {
                struct pipe_image_view *image = &ctx->images[shader][i];

                /* TODO: understand how v3d/freedreno does it */
                if (!(ctx->image_mask[shader] & (1 << i)) ||
                    !(image->shader_access & PIPE_IMAGE_ACCESS_READ_WRITE)) {
                        /* Unused image bindings */
                        pan_pack(bufs + (i * 2), ATTRIBUTE_BUFFER, cfg);
                        pan_pack(bufs + (i * 2) + 1, ATTRIBUTE_BUFFER, cfg);
                        continue;
                }

                struct panfrost_resource *rsrc = pan_resource(image->resource);

                /* TODO: MSAA */
                assert(image->resource->nr_samples <= 1 && "MSAA'd images not supported");

                bool is_3d = rsrc->base.target == PIPE_TEXTURE_3D;
                bool is_buffer = rsrc->base.target == PIPE_BUFFER;

                unsigned offset = is_buffer ? image->u.buf.offset :
                        panfrost_texture_offset(&rsrc->image.layout,
                                                image->u.tex.level,
                                                is_3d ? 0 : image->u.tex.first_layer,
                                                is_3d ? image->u.tex.first_layer : 0);

                /* Add a dependency of the batch on the shader image buffer */
                uint32_t flags = PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_VERTEX_TILER;
                if (image->shader_access & PIPE_IMAGE_ACCESS_READ)
                        flags |= PAN_BO_ACCESS_READ;
                if (image->shader_access & PIPE_IMAGE_ACCESS_WRITE) {
                        flags |= PAN_BO_ACCESS_WRITE;
                        unsigned level = is_buffer ? 0 : image->u.tex.level;
                        BITSET_SET(rsrc->valid.data, level);

                        if (is_buffer) {
                                util_range_add(&rsrc->base, &rsrc->valid_buffer_range,
                                                0, rsrc->base.width0);
                        }
                }
                panfrost_batch_add_bo(batch, rsrc->image.data.bo, flags);

                pan_pack(bufs + (i * 2), ATTRIBUTE_BUFFER, cfg) {
                        cfg.type = pan_modifier_to_attr_type(rsrc->image.layout.modifier);
                        cfg.pointer = rsrc->image.data.bo->ptr.gpu + offset;
                        cfg.stride = util_format_get_blocksize(image->format);
                        cfg.size = rsrc->image.data.bo->size - offset;
                }

                if (is_buffer) {
                        pan_pack(bufs + (i * 2) + 1, ATTRIBUTE_BUFFER_CONTINUATION_3D, cfg) {
                                cfg.s_dimension = rsrc->base.width0 /
                                        util_format_get_blocksize(image->format);
                                cfg.t_dimension = cfg.r_dimension = 1;
                        }

                        continue;
                }

                pan_pack(bufs + (i * 2) + 1, ATTRIBUTE_BUFFER_CONTINUATION_3D, cfg) {
                        unsigned level = image->u.tex.level;

                        cfg.s_dimension = u_minify(rsrc->base.width0, level);
                        cfg.t_dimension = u_minify(rsrc->base.height0, level);
                        cfg.r_dimension = is_3d ?
                                u_minify(rsrc->base.depth0, level) :
                                image->u.tex.last_layer - image->u.tex.first_layer + 1;

                        cfg.row_stride =
                                rsrc->image.layout.slices[level].row_stride;

                        if (rsrc->base.target != PIPE_TEXTURE_2D) {
                                cfg.slice_stride =
                                        panfrost_get_layer_stride(&rsrc->image.layout,
                                                                  level);
                        }
                }
        }
}

mali_ptr
panfrost_emit_image_attribs(struct panfrost_batch *batch,
                            mali_ptr *buffers,
                            enum pipe_shader_type type)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_state *shader = panfrost_get_shader_state(ctx, type);

        if (!shader->info.attribute_count) {
                *buffers = 0;
                return 0;
        }

        struct panfrost_device *dev = pan_device(ctx->base.screen);

        /* Images always need a MALI_ATTRIBUTE_BUFFER_CONTINUATION_3D */
        unsigned attr_count = shader->info.attribute_count;
        unsigned buf_count = (attr_count * 2) + (pan_is_bifrost(dev) ? 1 : 0);

        struct panfrost_ptr bufs =
                panfrost_pool_alloc_desc_array(&batch->pool, buf_count, ATTRIBUTE_BUFFER);

        struct panfrost_ptr attribs =
                panfrost_pool_alloc_desc_array(&batch->pool, attr_count, ATTRIBUTE);

        emit_image_attribs(ctx, type, attribs.cpu, 0);
        emit_image_bufs(batch, type, bufs.cpu, 0);

        /* We need an empty attrib buf to stop the prefetching on Bifrost */
        if (pan_is_bifrost(dev)) {
                pan_pack(bufs.cpu +
                         ((buf_count - 1) * MALI_ATTRIBUTE_BUFFER_LENGTH),
                         ATTRIBUTE_BUFFER, cfg);
        }

        *buffers = bufs.gpu;
        return attribs.gpu;
}

mali_ptr
panfrost_emit_vertex_data(struct panfrost_batch *batch,
                          mali_ptr *buffers)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_vertex_state *so = ctx->vertex;
        struct panfrost_shader_state *vs = panfrost_get_shader_state(ctx, PIPE_SHADER_VERTEX);
        bool instanced = ctx->indirect_draw || ctx->instance_count > 1;
        uint32_t image_mask = ctx->image_mask[PIPE_SHADER_VERTEX];
        unsigned nr_images = util_last_bit(image_mask);

        /* Worst case: everything is NPOT, which is only possible if instancing
         * is enabled. Otherwise single record is gauranteed.
         * Also, we allocate more memory than what's needed here if either instancing
         * is enabled or images are present, this can be improved. */
        unsigned bufs_per_attrib = (instanced || nr_images > 0) ? 2 : 1;
        unsigned nr_bufs = ((so->nr_bufs + nr_images) * bufs_per_attrib) +
                           (pan_is_bifrost(dev) ? 1 : 0);

        /* Midgard needs vertexid/instanceid handled specially */
        bool special_vbufs = dev->arch < 6 && vs->info.attribute_count >= PAN_VERTEX_ID;

        if (special_vbufs)
                nr_bufs += 2;

        if (!nr_bufs) {
                *buffers = 0;
                return 0;
        }

        struct panfrost_ptr S =
                panfrost_pool_alloc_desc_array(&batch->pool, nr_bufs,
                                               ATTRIBUTE_BUFFER);
        struct panfrost_ptr T =
                panfrost_pool_alloc_desc_array(&batch->pool,
                                               vs->info.attribute_count,
                                               ATTRIBUTE);

        struct mali_attribute_buffer_packed *bufs =
                (struct mali_attribute_buffer_packed *) S.cpu;

        struct mali_attribute_packed *out =
                (struct mali_attribute_packed *) T.cpu;

        unsigned attrib_to_buffer[PIPE_MAX_ATTRIBS] = { 0 };
        unsigned k = 0;

        for (unsigned i = 0; i < so->nr_bufs; ++i) {
                unsigned vbi = so->buffers[i].vbi;
                unsigned divisor = so->buffers[i].divisor;
                attrib_to_buffer[i] = k;

                if (!(ctx->vb_mask & (1 << vbi)))
                        continue;

                struct pipe_vertex_buffer *buf = &ctx->vertex_buffers[vbi];
                struct panfrost_resource *rsrc;

                rsrc = pan_resource(buf->buffer.resource);
                if (!rsrc)
                        continue;

                /* Add a dependency of the batch on the vertex buffer */
                panfrost_batch_add_bo(batch, rsrc->image.data.bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      PAN_BO_ACCESS_VERTEX_TILER);

                /* Mask off lower bits, see offset fixup below */
                mali_ptr raw_addr = rsrc->image.data.bo->ptr.gpu + buf->buffer_offset;
                mali_ptr addr = raw_addr & ~63;

                /* Since we advanced the base pointer, we shrink the buffer
                 * size, but add the offset we subtracted */
                unsigned size = rsrc->base.width0 + (raw_addr - addr)
                        - buf->buffer_offset;

                /* When there is a divisor, the hardware-level divisor is
                 * the product of the instance divisor and the padded count */
                unsigned stride = buf->stride;

                if (ctx->indirect_draw) {
                        /* We allocated 2 records for each attribute buffer */
                        assert((k & 1) == 0);

                        /* With indirect draws we can't guess the vertex_count.
                         * Pre-set the address, stride and size fields, the
                         * compute shader do the rest.
                         */
                        pan_pack(bufs + k, ATTRIBUTE_BUFFER, cfg) {
                                cfg.type = MALI_ATTRIBUTE_TYPE_1D;
                                cfg.pointer = addr;
                                cfg.stride = stride;
                                cfg.size = size;
                        }

                        /* We store the unmodified divisor in the continuation
                         * slot so the compute shader can retrieve it.
                         */
                        pan_pack(bufs + k + 1, ATTRIBUTE_BUFFER_CONTINUATION_NPOT, cfg) {
                                cfg.divisor = divisor;
                        }

                        k += 2;
                        continue;
                }

                unsigned hw_divisor = ctx->padded_count * divisor;

                if (ctx->instance_count <= 1) {
                        /* Per-instance would be every attribute equal */
                        if (divisor)
                                stride = 0;

                        pan_pack(bufs + k, ATTRIBUTE_BUFFER, cfg) {
                                cfg.pointer = addr;
                                cfg.stride = stride;
                                cfg.size = size;
                        }
                } else if (!divisor) {
                        pan_pack(bufs + k, ATTRIBUTE_BUFFER, cfg) {
                                cfg.type = MALI_ATTRIBUTE_TYPE_1D_MODULUS;
                                cfg.pointer = addr;
                                cfg.stride = stride;
                                cfg.size = size;
                                cfg.divisor = ctx->padded_count;
                        }
                } else if (util_is_power_of_two_or_zero(hw_divisor)) {
                        pan_pack(bufs + k, ATTRIBUTE_BUFFER, cfg) {
                                cfg.type = MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR;
                                cfg.pointer = addr;
                                cfg.stride = stride;
                                cfg.size = size;
                                cfg.divisor_r = __builtin_ctz(hw_divisor);
                        }

                } else {
                        unsigned shift = 0, extra_flags = 0;

                        unsigned magic_divisor =
                                panfrost_compute_magic_divisor(hw_divisor, &shift, &extra_flags);

                        /* Records with continuations must be aligned */
                        k = ALIGN_POT(k, 2);
                        attrib_to_buffer[i] = k;

                        pan_pack(bufs + k, ATTRIBUTE_BUFFER, cfg) {
                                cfg.type = MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR;
                                cfg.pointer = addr;
                                cfg.stride = stride;
                                cfg.size = size;

                                cfg.divisor_r = shift;
                                cfg.divisor_e = extra_flags;
                        }

                        pan_pack(bufs + k + 1, ATTRIBUTE_BUFFER_CONTINUATION_NPOT, cfg) {
                                cfg.divisor_numerator = magic_divisor;
                                cfg.divisor = divisor;
                        }

                        ++k;
                }

                ++k;
        }

        /* Add special gl_VertexID/gl_InstanceID buffers */
        if (unlikely(special_vbufs)) {
                panfrost_vertex_id(ctx->padded_count, &bufs[k], ctx->instance_count > 1);

                pan_pack(out + PAN_VERTEX_ID, ATTRIBUTE, cfg) {
                        cfg.buffer_index = k++;
                        cfg.format = so->formats[PAN_VERTEX_ID];
                }

                panfrost_instance_id(ctx->padded_count, &bufs[k], ctx->instance_count > 1);

                pan_pack(out + PAN_INSTANCE_ID, ATTRIBUTE, cfg) {
                        cfg.buffer_index = k++;
                        cfg.format = so->formats[PAN_INSTANCE_ID];
                }
        }

        k = ALIGN_POT(k, 2);
        emit_image_attribs(ctx, PIPE_SHADER_VERTEX, out + so->num_elements, k);
        emit_image_bufs(batch, PIPE_SHADER_VERTEX, bufs + k, k);
        k += (util_last_bit(ctx->image_mask[PIPE_SHADER_VERTEX]) * 2);

        /* We need an empty attrib buf to stop the prefetching on Bifrost */
        if (pan_is_bifrost(dev))
                pan_pack(&bufs[k], ATTRIBUTE_BUFFER, cfg);

        /* Attribute addresses require 64-byte alignment, so let:
         *
         *      base' = base & ~63 = base - (base & 63)
         *      offset' = offset + (base & 63)
         *
         * Since base' + offset' = base + offset, these are equivalent
         * addressing modes and now base is 64 aligned.
         */

        for (unsigned i = 0; i < so->num_elements; ++i) {
                unsigned vbi = so->pipe[i].vertex_buffer_index;
                struct pipe_vertex_buffer *buf = &ctx->vertex_buffers[vbi];

                /* BOs are aligned; just fixup for buffer_offset */
                signed src_offset = so->pipe[i].src_offset;
                src_offset += (buf->buffer_offset & 63);

                /* Base instance offset */
                if (ctx->base_instance && so->pipe[i].instance_divisor) {
                        src_offset += (ctx->base_instance * buf->stride) /
                                      so->pipe[i].instance_divisor;
                }

                /* Also, somewhat obscurely per-instance data needs to be
                 * offset in response to a delayed start in an indexed draw */

                if (so->pipe[i].instance_divisor && ctx->instance_count > 1)
                        src_offset -= buf->stride * ctx->offset_start;

                pan_pack(out + i, ATTRIBUTE, cfg) {
                        cfg.buffer_index = attrib_to_buffer[so->element_buffer[i]];
                        cfg.format = so->formats[i];
                        cfg.offset = src_offset;
                }
        }

        *buffers = S.gpu;
        return T.gpu;
}

static mali_ptr
panfrost_emit_varyings(struct panfrost_batch *batch,
                struct mali_attribute_buffer_packed *slot,
                unsigned stride, unsigned count)
{
        unsigned size = stride * count;
        mali_ptr ptr =
                batch->ctx->indirect_draw ? 0 :
                panfrost_pool_alloc_aligned(&batch->invisible_pool, size, 64).gpu;

        pan_pack(slot, ATTRIBUTE_BUFFER, cfg) {
                cfg.stride = stride;
                cfg.size = size;
                cfg.pointer = ptr;
        }

        return ptr;
}

static unsigned
panfrost_xfb_offset(unsigned stride, struct pipe_stream_output_target *target)
{
        return target->buffer_offset + (pan_so_target(target)->offset * stride);
}

static void
panfrost_emit_streamout(struct panfrost_batch *batch,
                        struct mali_attribute_buffer_packed *slot,
                        unsigned stride, unsigned count,
                        struct pipe_stream_output_target *target)
{
        unsigned max_size = target->buffer_size;
        unsigned expected_size = stride * count;

        /* Grab the BO and bind it to the batch */
        struct panfrost_resource *rsrc = pan_resource(target->buffer);
        struct panfrost_bo *bo = rsrc->image.data.bo;

        /* Varyings are WRITE from the perspective of the VERTEX but READ from
         * the perspective of the TILER and FRAGMENT.
         */
        panfrost_batch_add_bo(batch, bo,
                              PAN_BO_ACCESS_SHARED |
                              PAN_BO_ACCESS_RW |
                              PAN_BO_ACCESS_VERTEX_TILER |
                              PAN_BO_ACCESS_FRAGMENT);

        unsigned offset = panfrost_xfb_offset(stride, target);

        pan_pack(slot, ATTRIBUTE_BUFFER, cfg) {
                cfg.pointer = bo->ptr.gpu + (offset & ~63);
                cfg.stride = stride;
                cfg.size = MIN2(max_size, expected_size) + (offset & 63);

                util_range_add(&rsrc->base, &rsrc->valid_buffer_range,
                                offset, cfg.size);
        }
}

/* Helpers for manipulating stream out information so we can pack varyings
 * accordingly. Compute the src_offset for a given captured varying */

static struct pipe_stream_output *
pan_get_so(struct pipe_stream_output_info *info, gl_varying_slot loc)
{
        for (unsigned i = 0; i < info->num_outputs; ++i) {
                if (info->output[i].register_index == loc)
                        return &info->output[i];
        }

        unreachable("Varying not captured");
}

/* Given a varying, figure out which index it corresponds to */

static inline unsigned
pan_varying_index(unsigned present, enum pan_special_varying v)
{
        return util_bitcount(present & BITFIELD_MASK(v));
}

/* Get the base offset for XFB buffers, which by convention come after
 * everything else. Wrapper function for semantic reasons; by construction this
 * is just popcount. */

static inline unsigned
pan_xfb_base(unsigned present)
{
        return util_bitcount(present);
}

/* Determines which varying buffers are required */

static inline unsigned
pan_varying_present(const struct panfrost_device *dev,
                    struct pan_shader_info *producer,
                    struct pan_shader_info *consumer,
                    uint16_t point_coord_mask)
{
        /* At the moment we always emit general and position buffers. Not
         * strictly necessary but usually harmless */

        unsigned present = BITFIELD_BIT(PAN_VARY_GENERAL) | BITFIELD_BIT(PAN_VARY_POSITION);

        /* Enable special buffers by the shader info */

        if (producer->vs.writes_point_size)
                present |= BITFIELD_BIT(PAN_VARY_PSIZ);

        /* On Bifrost, special fragment varyings are replaced by LD_VAR_SPECIAL */
        if (pan_is_bifrost(dev))
                return present;

        /* On Midgard, these exist as real varyings */
        if (consumer->fs.reads_point_coord)
                present |= BITFIELD_BIT(PAN_VARY_PNTCOORD);

        if (consumer->fs.reads_face)
                present |= BITFIELD_BIT(PAN_VARY_FACE);

        if (consumer->fs.reads_frag_coord)
                present |= BITFIELD_BIT(PAN_VARY_FRAGCOORD);

        /* Also, if we have a point sprite, we need a point coord buffer */

        for (unsigned i = 0; i < consumer->varyings.input_count; i++)  {
                gl_varying_slot loc = consumer->varyings.input[i].location;

                if (util_varying_is_point_coord(loc, point_coord_mask))
                        present |= BITFIELD_BIT(PAN_VARY_PNTCOORD);
        }

        return present;
}

/* Emitters for varying records */

static void
pan_emit_vary(const struct panfrost_device *dev,
              struct mali_attribute_packed *out,
              unsigned buffer_index,
              mali_pixel_format format, unsigned offset)
{
        pan_pack(out, ATTRIBUTE, cfg) {
                cfg.buffer_index = buffer_index;
                cfg.offset_enable = !pan_is_bifrost(dev);
                cfg.format = format;
                cfg.offset = offset;
        }
}

/* Special records */

static const struct {
       unsigned components;
       enum mali_format format;
} pan_varying_formats[PAN_VARY_MAX] = {
        [PAN_VARY_POSITION]     = { 4, MALI_SNAP_4 },
        [PAN_VARY_PSIZ]         = { 1, MALI_R16F },
        [PAN_VARY_PNTCOORD]     = { 1, MALI_R16F },
        [PAN_VARY_FACE]         = { 1, MALI_R32I },
        [PAN_VARY_FRAGCOORD]    = { 4, MALI_RGBA32F },
};

static mali_pixel_format
pan_special_format(const struct panfrost_device *dev,
                enum pan_special_varying buf)
{
        assert(buf < PAN_VARY_MAX);
        mali_pixel_format format = (pan_varying_formats[buf].format << 12);

        if (dev->quirks & HAS_SWIZZLES) {
                unsigned nr = pan_varying_formats[buf].components;
                format |= panfrost_get_default_swizzle(nr);
        }

        return format;
}

static void
pan_emit_vary_special(const struct panfrost_device *dev,
                      struct mali_attribute_packed *out,
                      unsigned present, enum pan_special_varying buf)
{
        pan_emit_vary(dev, out, pan_varying_index(present, buf),
                        pan_special_format(dev, buf), 0);
}

/* Negative indicates a varying is not found */

static signed
pan_find_vary(const struct pan_shader_varying *vary,
                unsigned vary_count, unsigned loc)
{
        for (unsigned i = 0; i < vary_count; ++i) {
                if (vary[i].location == loc)
                        return i;
        }

        return -1;
}

/* Assign varying locations for the general buffer. Returns the calculated
 * per-vertex stride, and outputs offsets into the passed array. Negative
 * offset indicates a varying is not used. */

static unsigned
pan_assign_varyings(const struct panfrost_device *dev,
                    struct pan_shader_info *producer,
                    struct pan_shader_info *consumer,
                    signed *offsets)
{
        unsigned producer_count = producer->varyings.output_count;
        unsigned consumer_count = consumer->varyings.input_count;

        const struct pan_shader_varying *producer_vars = producer->varyings.output;
        const struct pan_shader_varying *consumer_vars = consumer->varyings.input;

        unsigned stride = 0;

        for (unsigned i = 0; i < producer_count; ++i) {
                signed loc = pan_find_vary(consumer_vars, consumer_count,
                                producer_vars[i].location);

                if (loc >= 0) {
                        offsets[i] = stride;

                        enum pipe_format format = consumer_vars[loc].format;
                        stride += util_format_get_blocksize(format);
                } else {
                        offsets[i] = -1;
                }
        }

        return stride;
}

/* Emitter for a single varying (attribute) descriptor */

static void
panfrost_emit_varying(const struct panfrost_device *dev,
                      struct mali_attribute_packed *out,
                      const struct pan_shader_varying varying,
                      enum pipe_format pipe_format,
                      unsigned present,
                      uint16_t point_sprite_mask,
                      struct pipe_stream_output_info *xfb,
                      uint64_t xfb_loc_mask,
                      unsigned max_xfb,
                      unsigned *xfb_offsets,
                      signed offset,
                      enum pan_special_varying pos_varying)
{
        /* Note: varying.format != pipe_format in some obscure cases due to a
         * limitation of the NIR linker. This should be fixed in the future to
         * eliminate the additional lookups. See:
         * dEQP-GLES3.functional.shaders.conditionals.if.sequence_statements_vertex
         */
        gl_varying_slot loc = varying.location;
        mali_pixel_format format = dev->formats[pipe_format].hw;

        struct pipe_stream_output *o = (xfb_loc_mask & BITFIELD64_BIT(loc)) ?
                pan_get_so(xfb, loc) : NULL;

        if (util_varying_is_point_coord(loc, point_sprite_mask)) {
                pan_emit_vary_special(dev, out, present, PAN_VARY_PNTCOORD);
        } else if (o && o->output_buffer < max_xfb) {
                unsigned fixup_offset = xfb_offsets[o->output_buffer] & 63;

                pan_emit_vary(dev, out,
                                pan_xfb_base(present) + o->output_buffer,
                                format, (o->dst_offset * 4) + fixup_offset);
        } else if (loc == VARYING_SLOT_POS) {
                pan_emit_vary_special(dev, out, present, pos_varying);
        } else if (loc == VARYING_SLOT_PSIZ) {
                pan_emit_vary_special(dev, out, present, PAN_VARY_PSIZ);
        } else if (loc == VARYING_SLOT_FACE) {
                pan_emit_vary_special(dev, out, present, PAN_VARY_FACE);
        } else if (offset < 0) {
                pan_emit_vary(dev, out, 0, (MALI_CONSTANT << 12), 0);
        } else {
                STATIC_ASSERT(PAN_VARY_GENERAL == 0);
                pan_emit_vary(dev, out, 0, format, offset);
        }
}

/* Links varyings and uploads ATTRIBUTE descriptors. Can execute at link time,
 * rather than draw time (under good conditions). */

static void
panfrost_emit_varying_descs(
                struct pan_pool *pool,
                struct panfrost_shader_state *producer,
                struct panfrost_shader_state *consumer,
                struct panfrost_streamout *xfb,
                uint16_t point_coord_mask,
                struct pan_linkage *out)
{
        struct panfrost_device *dev = pool->dev;
        struct pipe_stream_output_info *xfb_info = &producer->stream_output;
        unsigned producer_count = producer->info.varyings.output_count;
        unsigned consumer_count = consumer->info.varyings.input_count;

        /* Offsets within the general varying buffer, indexed by location */
        signed offsets[PIPE_MAX_ATTRIBS];
        assert(producer_count < ARRAY_SIZE(offsets));
        assert(consumer_count < ARRAY_SIZE(offsets));

        /* Allocate enough descriptors for both shader stages */
        struct panfrost_ptr T = panfrost_pool_alloc_desc_array(pool,
                        producer_count + consumer_count, ATTRIBUTE);

        /* Take a reference if we're being put on the CSO */
        if (!pool->owned) {
                out->bo = pool->transient_bo;
                panfrost_bo_reference(out->bo);
        }

        struct mali_attribute_packed *descs = T.cpu;
        out->producer = producer_count ? T.gpu : 0;
        out->consumer = consumer_count ? T.gpu +
                (MALI_ATTRIBUTE_LENGTH * producer_count) : 0;

        /* Lay out the varyings. Must use producer to lay out, in order to
         * respect transform feedback precisions. */
        out->present = pan_varying_present(dev, &producer->info,
                        &consumer->info, point_coord_mask);

        out->stride = pan_assign_varyings(dev, &producer->info,
                        &consumer->info, offsets);

        unsigned xfb_offsets[PIPE_MAX_SO_BUFFERS];

        for (unsigned i = 0; i < xfb->num_targets; ++i) {
                xfb_offsets[i] = panfrost_xfb_offset(xfb_info->stride[i] * 4,
                                xfb->targets[i]);
        }

        for (unsigned i = 0; i < producer_count; ++i) {
                signed j = pan_find_vary(consumer->info.varyings.input,
                                consumer->info.varyings.input_count,
                                producer->info.varyings.output[i].location);

                enum pipe_format format = (j >= 0) ?
                        consumer->info.varyings.input[j].format :
                        producer->info.varyings.output[i].format;

                panfrost_emit_varying(dev, descs + i,
                                producer->info.varyings.output[i], format,
                                out->present, 0, &producer->stream_output,
                                producer->so_mask, xfb->num_targets,
                                xfb_offsets, offsets[i], PAN_VARY_POSITION);
        }

        for (unsigned i = 0; i < consumer_count; ++i) {
                signed j = pan_find_vary(producer->info.varyings.output,
                                producer->info.varyings.output_count,
                                consumer->info.varyings.input[i].location);

                signed offset = (j >= 0) ? offsets[j] : -1;

                panfrost_emit_varying(dev, descs + producer_count + i,
                                consumer->info.varyings.input[i],
                                consumer->info.varyings.input[i].format,
                                out->present, point_coord_mask,
                                &producer->stream_output, producer->so_mask,
                                xfb->num_targets, xfb_offsets, offset,
                                PAN_VARY_FRAGCOORD);
        }
}

static void
pan_emit_special_input(struct mali_attribute_buffer_packed *out,
                unsigned present,
                enum pan_special_varying v,
                unsigned special)
{
        if (present & BITFIELD_BIT(v)) {
                unsigned idx = pan_varying_index(present, v);

                pan_pack(out + idx, ATTRIBUTE_BUFFER, cfg) {
                        cfg.special = special;
                        cfg.type = 0;
                }
        }
}

void
panfrost_emit_varying_descriptor(struct panfrost_batch *batch,
                                 unsigned vertex_count,
                                 mali_ptr *vs_attribs,
                                 mali_ptr *fs_attribs,
                                 mali_ptr *buffers,
                                 unsigned *buffer_count,
                                 mali_ptr *position,
                                 mali_ptr *psiz,
                                 bool point_coord_replace)
{
        /* Load the shaders */
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct panfrost_shader_state *vs, *fs;

        vs = panfrost_get_shader_state(ctx, PIPE_SHADER_VERTEX);
        fs = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        uint16_t point_coord_mask = ctx->rasterizer->base.sprite_coord_enable;

        /* TODO: point sprites need lowering on Bifrost */
        if (!point_coord_replace || pan_is_bifrost(dev))
                point_coord_mask =  0;

        /* In good conditions, we only need to link varyings once */
        bool prelink =
                (point_coord_mask == 0) &&
                (ctx->streamout.num_targets == 0) &&
                !vs->info.separable &&
                !fs->info.separable;

        /* Try to reduce copies */
        struct pan_linkage _linkage;
        struct pan_linkage *linkage = prelink ? &vs->linkage : &_linkage;

        /* Emit ATTRIBUTE descriptors if needed */
        if (!prelink || vs->linkage.bo == NULL) {
                struct pan_pool *pool =
                        prelink ? &ctx->descs : &batch->pool;

                panfrost_emit_varying_descs(pool, vs, fs, &ctx->streamout, point_coord_mask, linkage);
        }

        struct pipe_stream_output_info *so = &vs->stream_output;
        unsigned present = linkage->present, stride = linkage->stride;
        unsigned xfb_base = pan_xfb_base(present);
        struct panfrost_ptr T =
                panfrost_pool_alloc_desc_array(&batch->pool,
                                               xfb_base +
                                               ctx->streamout.num_targets + 1,
                                               ATTRIBUTE_BUFFER);
        struct mali_attribute_buffer_packed *varyings =
                (struct mali_attribute_buffer_packed *) T.cpu;

        if (buffer_count)
                *buffer_count = xfb_base + ctx->streamout.num_targets;

        /* Suppress prefetch on Bifrost */
        memset(varyings + (xfb_base * ctx->streamout.num_targets), 0, sizeof(*varyings));

        /* Emit the stream out buffers. We need enough room for all the
         * vertices we emit across all instances */

        unsigned out_count = ctx->instance_count *
                u_stream_outputs_for_vertices(ctx->active_prim, ctx->vertex_count);

        for (unsigned i = 0; i < ctx->streamout.num_targets; ++i) {
                panfrost_emit_streamout(batch, &varyings[xfb_base + i],
                                        so->stride[i] * 4,
                                        out_count,
                                        ctx->streamout.targets[i]);
        }

        if (stride) {
                panfrost_emit_varyings(batch,
                                &varyings[pan_varying_index(present, PAN_VARY_GENERAL)],
                                stride, vertex_count);
        }

        /* fp32 vec4 gl_Position */
        *position = panfrost_emit_varyings(batch,
                        &varyings[pan_varying_index(present, PAN_VARY_POSITION)],
                        sizeof(float) * 4, vertex_count);

        if (present & BITFIELD_BIT(PAN_VARY_PSIZ)) {
                *psiz = panfrost_emit_varyings(batch,
                                &varyings[pan_varying_index(present, PAN_VARY_PSIZ)],
                                2, vertex_count);
        }

        pan_emit_special_input(varyings, present,
                        PAN_VARY_PNTCOORD, MALI_ATTRIBUTE_SPECIAL_POINT_COORD);
        pan_emit_special_input(varyings, present, PAN_VARY_FACE,
                        MALI_ATTRIBUTE_SPECIAL_FRONT_FACING);
        pan_emit_special_input(varyings, present, PAN_VARY_FRAGCOORD,
                        MALI_ATTRIBUTE_SPECIAL_FRAG_COORD);

        *buffers = T.gpu;
        *vs_attribs = linkage->producer;
        *fs_attribs = linkage->consumer;
}

void
panfrost_emit_vertex_tiler_jobs(struct panfrost_batch *batch,
                                const struct panfrost_ptr *vertex_job,
                                const struct panfrost_ptr *tiler_job)
{
        struct panfrost_context *ctx = batch->ctx;

        /* If rasterizer discard is enable, only submit the vertex. XXX - set
         * job_barrier in case buffers get ping-ponged and we need to enforce
         * ordering, this has a perf hit! See
         * KHR-GLES31.core.vertex_attrib_binding.advanced-iterations */

        unsigned vertex = panfrost_add_job(&batch->pool, &batch->scoreboard,
                                           MALI_JOB_TYPE_VERTEX, true, false,
                                           ctx->indirect_draw ?
                                           batch->indirect_draw_job_id : 0,
                                           0, vertex_job, false);

        if (ctx->rasterizer->base.rasterizer_discard || batch->scissor_culls_everything)
                return;

        panfrost_add_job(&batch->pool, &batch->scoreboard,
                         MALI_JOB_TYPE_TILER, false, false,
                         vertex, 0, tiler_job, false);
}

void
panfrost_emit_tls(struct panfrost_batch *batch)
{
        struct panfrost_device *dev = pan_device(batch->ctx->base.screen);

        /* Emitted with the FB descriptor on Midgard. */
        if (!pan_is_bifrost(dev) && batch->framebuffer.gpu)
                return;

        struct panfrost_bo *tls_bo =
                batch->stack_size ?
                panfrost_batch_get_scratchpad(batch,
                                              batch->stack_size,
                                              dev->thread_tls_alloc,
                                              dev->core_count):
                NULL;
        struct pan_tls_info tls = {
                .tls = {
                        .ptr = tls_bo ? tls_bo->ptr.gpu : 0,
                        .size = batch->stack_size,
                },
        };

        assert(batch->tls.cpu);
        pan_emit_tls(dev, &tls, batch->tls.cpu);
}

void
panfrost_emit_fbd(struct panfrost_batch *batch,
                  const struct pan_fb_info *fb)
{
        struct panfrost_device *dev = pan_device(batch->ctx->base.screen);
        struct panfrost_bo *tls_bo =
                batch->stack_size ?
                panfrost_batch_get_scratchpad(batch,
                                              batch->stack_size,
                                              dev->thread_tls_alloc,
                                              dev->core_count):
                NULL;
        struct pan_tls_info tls = {
                .tls = {
                        .ptr = tls_bo ? tls_bo->ptr.gpu : 0,
                        .size = batch->stack_size,
                },
        };

        batch->framebuffer.gpu |=
                pan_emit_fbd(dev, fb, &tls, &batch->tiler_ctx,
                             batch->framebuffer.cpu);
}

/* Mark a surface as written */

static void
panfrost_initialize_surface(struct panfrost_batch *batch,
                            struct pipe_surface *surf)
{
        if (surf) {
                struct panfrost_resource *rsrc = pan_resource(surf->texture);
                BITSET_SET(rsrc->valid.data, surf->u.tex.level);
        }
}

void
panfrost_emit_tile_map(struct panfrost_batch *batch, struct pan_fb_info *fb)
{
        if (batch->key.nr_cbufs < 1 || !batch->key.cbufs[0])
                return;

        struct pipe_surface *surf = batch->key.cbufs[0];
        struct panfrost_resource *pres = surf ? pan_resource(surf->texture) : NULL;

        if (pres && pres->damage.tile_map.enable) {
                fb->tile_map.base =
                        panfrost_pool_upload_aligned(&batch->pool,
                                                     pres->damage.tile_map.data,
                                                     pres->damage.tile_map.size,
                                                     64);
                fb->tile_map.stride = pres->damage.tile_map.stride;
        }
}

/* Generate a fragment job. This should be called once per frame. (According to
 * presentations, this is supposed to correspond to eglSwapBuffers) */

mali_ptr
panfrost_emit_fragment_job(struct panfrost_batch *batch,
                           const struct pan_fb_info *pfb)
{
        struct panfrost_device *dev = pan_device(batch->ctx->base.screen);

        /* Mark the affected buffers as initialized, since we're writing to it.
         * Also, add the surfaces we're writing to to the batch */

        struct pipe_framebuffer_state *fb = &batch->key;

        for (unsigned i = 0; i < fb->nr_cbufs; ++i)
                panfrost_initialize_surface(batch, fb->cbufs[i]);

        panfrost_initialize_surface(batch, fb->zsbuf);

        /* The passed tile coords can be out of range in some cases, so we need
         * to clamp them to the framebuffer size to avoid a TILE_RANGE_FAULT.
         * Theoretically we also need to clamp the coordinates positive, but we
         * avoid that edge case as all four values are unsigned. Also,
         * theoretically we could clamp the minima, but if that has to happen
         * the asserts would fail anyway (since the maxima would get clamped
         * and then be smaller than the minima). An edge case of sorts occurs
         * when no scissors are added to draw, so by default min=~0 and max=0.
         * But that can't happen if any actual drawing occurs (beyond a
         * wallpaper reload), so this is again irrelevant in practice. */

        batch->maxx = MIN2(batch->maxx, fb->width);
        batch->maxy = MIN2(batch->maxy, fb->height);

        /* Rendering region must be at least 1x1; otherwise, there is nothing
         * to do and the whole job chain should have been discarded. */

        assert(batch->maxx > batch->minx);
        assert(batch->maxy > batch->miny);

        struct panfrost_ptr transfer =
                panfrost_pool_alloc_desc(&batch->pool, FRAGMENT_JOB);

        pan_emit_fragment_job(dev, pfb, batch->framebuffer.gpu,
                              transfer.cpu);

        return transfer.gpu;
}

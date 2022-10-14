/*
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2019 Red Hat Inc.
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
 *
 * Authors (Collabora):
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *
 */

#include "pan_context.h"
#include "pan_bo.h"
#include "pan_shader.h"
#include "util/u_memory.h"
#include "nir/tgsi_to_nir.h"
#include "nir_serialize.h"

static void
panfrost_build_key(struct panfrost_context *ctx,
                   struct panfrost_shader_key *key,
                   nir_shader *nir)
{
        /* We don't currently have vertex shader variants */
        if (nir->info.stage != MESA_SHADER_FRAGMENT)
               return;

        struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct pipe_framebuffer_state *fb = &ctx->pipe_framebuffer;
        struct pipe_rasterizer_state *rast = (void *) ctx->rasterizer;
        struct panfrost_shader_variants *vs = ctx->shader[MESA_SHADER_VERTEX];

        key->fs.nr_cbufs = fb->nr_cbufs;

        /* Point sprite lowering needed on Bifrost and newer */
        if (dev->arch >= 6 && rast && ctx->active_prim == PIPE_PRIM_POINTS) {
                key->fs.sprite_coord_enable = rast->sprite_coord_enable;
        }

        /* User clip plane lowering needed everywhere */
        if (rast) {
                key->fs.clip_plane_enable = rast->clip_plane_enable;
        }

        if (dev->arch <= 5) {
                u_foreach_bit(i, (nir->info.outputs_read >> FRAG_RESULT_DATA0)) {
                        enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

                        if ((fb->nr_cbufs > i) && fb->cbufs[i])
                                fmt = fb->cbufs[i]->format;

                        if (panfrost_blendable_formats_v6[fmt].internal)
                                fmt = PIPE_FORMAT_NONE;

                        key->fs.rt_formats[i] = fmt;
                }
        }

        /* Funny desktop GL varying lowering on Valhall */
        if (dev->arch >= 9) {
                assert(vs != NULL && "too early");
                key->fixed_varying_mask = vs->fixed_varying_mask;
        }
}

/**
 * Fix an uncompiled shader's stream output info, and produce a bitmask
 * of which VARYING_SLOT_* are captured for stream output.
 *
 * Core Gallium stores output->register_index as a "slot" number, where
 * slots are assigned consecutively to all outputs in info->outputs_written.
 * This naive packing of outputs doesn't work for us - we too have slots,
 * but the layout is defined by the VUE map, which we won't have until we
 * compile a specific shader variant.  So, we remap these and simply store
 * VARYING_SLOT_* in our copy's output->register_index fields.
 *
 * We then produce a bitmask of outputs which are used for SO.
 *
 * Implementation from iris.
 */

static uint64_t
update_so_info(struct pipe_stream_output_info *so_info,
               uint64_t outputs_written)
{
	uint64_t so_outputs = 0;
	uint8_t reverse_map[64] = {0};
	unsigned slot = 0;

	while (outputs_written)
		reverse_map[slot++] = u_bit_scan64(&outputs_written);

	for (unsigned i = 0; i < so_info->num_outputs; i++) {
		struct pipe_stream_output *output = &so_info->output[i];

		/* Map Gallium's condensed "slots" back to real VARYING_SLOT_* enums */
		output->register_index = reverse_map[output->register_index];

		so_outputs |= 1ull << output->register_index;
	}

	return so_outputs;
}

static unsigned
panfrost_new_variant_locked(
        struct panfrost_context *ctx,
        struct panfrost_shader_variants *variants,
        struct panfrost_shader_key *key)
{
        unsigned variant = variants->variant_count++;

        if (variants->variant_count > variants->variant_space) {
                unsigned old_space = variants->variant_space;

                variants->variant_space *= 2;
                if (variants->variant_space == 0)
                        variants->variant_space = 1;

                unsigned msize = sizeof(struct panfrost_shader_state);
                variants->variants = realloc(variants->variants,
                                             variants->variant_space * msize);

                memset(&variants->variants[old_space], 0,
                       (variants->variant_space - old_space) * msize);
        }

        variants->variants[variant].key = *key;

        struct panfrost_shader_state *shader_state = &variants->variants[variant];

        /* We finally have a variant, so compile it */
        panfrost_shader_compile(ctx->base.screen,
                                &ctx->shaders, &ctx->descs,
                                variants->nir, &ctx->base.debug, shader_state, 0);

        /* Fixup the stream out information */
        shader_state->stream_output = variants->stream_output;
        shader_state->so_mask =
                update_so_info(&shader_state->stream_output,
                               shader_state->info.outputs_written);

        shader_state->earlyzs = pan_earlyzs_analyze(&shader_state->info);

        return variant;
}

static void
panfrost_bind_shader_state(
        struct pipe_context *pctx,
        void *hwcso,
        enum pipe_shader_type type)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->shader[type] = hwcso;

        ctx->dirty |= PAN_DIRTY_TLS_SIZE;
        ctx->dirty_shader[type] |= PAN_DIRTY_STAGE_SHADER;

        if (hwcso)
                panfrost_update_shader_variant(ctx, type);
}

void
panfrost_update_shader_variant(struct panfrost_context *ctx,
                               enum pipe_shader_type type)
{
        /* No shader variants for compute */
        if (type == PIPE_SHADER_COMPUTE)
                return;

        /* We need linking information, defer this */
        if (type == PIPE_SHADER_FRAGMENT && !ctx->shader[PIPE_SHADER_VERTEX])
                return;

        /* Also defer, happens with GALLIUM_HUD */
        if (!ctx->shader[type])
                return;

        /* Match the appropriate variant */
        signed variant = -1;
        struct panfrost_shader_variants *variants = ctx->shader[type];

        simple_mtx_lock(&variants->lock);

        struct panfrost_shader_key key = {
                .fixed_varying_mask = variants->fixed_varying_mask
        };

        panfrost_build_key(ctx, &key, variants->nir);

        for (unsigned i = 0; i < variants->variant_count; ++i) {
                if (memcmp(&key, &variants->variants[i].key, sizeof(key)) == 0) {
                        variant = i;
                        break;
                }
        }

        if (variant == -1)
                variant = panfrost_new_variant_locked(ctx, variants, &key);

        variants->active_variant = variant;

        /* TODO: it would be more efficient to release the lock before
         * compiling instead of after, but that can race if thread A compiles a
         * variant while thread B searches for that same variant */
        simple_mtx_unlock(&variants->lock);
}

static void
panfrost_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_VERTEX);

        /* Fragment shaders are linked with vertex shaders */
        struct panfrost_context *ctx = pan_context(pctx);
        panfrost_update_shader_variant(ctx, PIPE_SHADER_FRAGMENT);
}

static void
panfrost_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_FRAGMENT);
}

static void *
panfrost_create_shader_state(
        struct pipe_context *pctx,
        const struct pipe_shader_state *cso)
{
        struct panfrost_shader_variants *so = CALLOC_STRUCT(panfrost_shader_variants);
        struct panfrost_device *dev = pan_device(pctx->screen);

        simple_mtx_init(&so->lock, mtx_plain);

        so->stream_output = cso->stream_output;

        if (cso->type == PIPE_SHADER_IR_TGSI)
                so->nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
        else
                so->nir = cso->ir.nir;

        /* Fix linkage early */
        if (so->nir->info.stage == MESA_SHADER_VERTEX) {
                so->fixed_varying_mask =
                        (so->nir->info.outputs_written & BITFIELD_MASK(VARYING_SLOT_VAR0)) &
                        ~VARYING_BIT_POS & ~VARYING_BIT_PSIZ;
        }

        /* Precompile for shader-db if we need to */
        if (unlikely(dev->debug & PAN_DBG_PRECOMPILE)) {
                struct panfrost_context *ctx = pan_context(pctx);

                struct panfrost_shader_state state = { 0 };

                panfrost_shader_compile(pctx->screen,
                                        &ctx->shaders, &ctx->descs,
                                        so->nir, &ctx->base.debug, &state, 0);
        }

        return so;
}

static void
panfrost_delete_shader_state(
        struct pipe_context *pctx,
        void *so)
{
        struct panfrost_shader_variants *cso = (struct panfrost_shader_variants *) so;

        ralloc_free(cso->nir);

        for (unsigned i = 0; i < cso->variant_count; ++i) {
                struct panfrost_shader_state *shader_state = &cso->variants[i];
                panfrost_bo_unreference(shader_state->bin.bo);
                panfrost_bo_unreference(shader_state->state.bo);
                panfrost_bo_unreference(shader_state->linkage.bo);

                if (shader_state->xfb) {
                        panfrost_bo_unreference(shader_state->xfb->bin.bo);
                        panfrost_bo_unreference(shader_state->xfb->state.bo);
                        panfrost_bo_unreference(shader_state->xfb->linkage.bo);
                        free(shader_state->xfb);
                }
        }

        simple_mtx_destroy(&cso->lock);

        free(cso->variants);
        free(so);
}

/* Compute CSOs are tracked like graphics shader CSOs, but are
 * considerably simpler. We do not implement multiple
 * variants/keying. So the CSO create function just goes ahead and
 * compiles the thing. */

static void *
panfrost_create_compute_state(
        struct pipe_context *pctx,
        const struct pipe_compute_state *cso)
{
        struct panfrost_context *ctx = pan_context(pctx);

        struct panfrost_shader_variants *so = CALLOC_STRUCT(panfrost_shader_variants);
        so->req_input_mem = cso->req_input_mem;

        struct panfrost_shader_state *v = calloc(1, sizeof(*v));
        so->variants = v;

        so->variant_count = 1;
        so->active_variant = 0;

        assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");

        panfrost_shader_compile(pctx->screen, &ctx->shaders, &ctx->descs,
                                cso->prog, &ctx->base.debug, v,
                                cso->req_local_mem);

        return so;
}

static void
panfrost_bind_compute_state(struct pipe_context *pipe, void *cso)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->shader[PIPE_SHADER_COMPUTE] = cso;
}

static void
panfrost_delete_compute_state(struct pipe_context *pipe, void *cso)
{
        struct panfrost_shader_variants *so =
                (struct panfrost_shader_variants *)cso;

        free(so->variants);
        free(cso);
}

void
panfrost_shader_context_init(struct pipe_context *pctx)
{
        pctx->create_vs_state = panfrost_create_shader_state;
        pctx->delete_vs_state = panfrost_delete_shader_state;
        pctx->bind_vs_state = panfrost_bind_vs_state;

        pctx->create_fs_state = panfrost_create_shader_state;
        pctx->delete_fs_state = panfrost_delete_shader_state;
        pctx->bind_fs_state = panfrost_bind_fs_state;

        pctx->create_compute_state = panfrost_create_compute_state;
        pctx->bind_compute_state = panfrost_bind_compute_state;
        pctx->delete_compute_state = panfrost_delete_compute_state;
}

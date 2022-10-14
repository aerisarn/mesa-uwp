/*
 * Copyright (c) 2022 Amazon.com, Inc. or its affiliates.
 * Copyright (C) 2019-2022 Collabora, Ltd.
 * Copyright (C) 2019 Red Hat Inc.
 * Copyright (C) 2018 Alyssa Rosenzweig
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

static struct panfrost_uncompiled_shader *
panfrost_alloc_shader(void)
{
        struct panfrost_uncompiled_shader *so = CALLOC_STRUCT(panfrost_uncompiled_shader);

        simple_mtx_init(&so->lock, mtx_plain);
        util_dynarray_init(&so->variants, NULL);

        return so;
}

static struct panfrost_compiled_shader *
panfrost_alloc_variant(struct panfrost_uncompiled_shader *so)
{
        return util_dynarray_grow(&so->variants, struct panfrost_compiled_shader, 1);
}

static void
panfrost_shader_compile(struct pipe_screen *pscreen,
                        struct panfrost_pool *shader_pool,
                        struct panfrost_pool *desc_pool,
                        const nir_shader *ir,
                        struct util_debug_callback *dbg,
                        struct panfrost_compiled_shader *state,
                        unsigned req_local_mem,
                        unsigned fixed_varying_mask)
{
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);

        nir_shader *s = nir_shader_clone(NULL, ir);

        struct panfrost_compile_inputs inputs = {
                .debug = dbg,
                .gpu_id = dev->gpu_id,
                .fixed_sysval_ubo = -1,
        };

        /* Lower this early so the backends don't have to worry about it */
        if (s->info.stage == MESA_SHADER_FRAGMENT) {
                inputs.fixed_varying_mask = state->key.fs.fixed_varying_mask;

                NIR_PASS_V(s, nir_lower_fragcolor, state->key.fs.nr_cbufs);

                if (state->key.fs.sprite_coord_enable) {
                        NIR_PASS_V(s, nir_lower_texcoord_replace,
                                   state->key.fs.sprite_coord_enable,
                                   true /* point coord is sysval */,
                                   false /* Y-invert */);
                }

                if (state->key.fs.clip_plane_enable) {
                        NIR_PASS_V(s, nir_lower_clip_fs,
                                   state->key.fs.clip_plane_enable,
                                   false);
                }

                memcpy(inputs.rt_formats, state->key.fs.rt_formats, sizeof(inputs.rt_formats));
        } else if (s->info.stage == MESA_SHADER_VERTEX) {
                inputs.fixed_varying_mask = fixed_varying_mask;

                /* No IDVS for internal XFB shaders */
                inputs.no_idvs = s->info.has_transform_feedback_varyings;
        }

        struct util_dynarray binary;

        util_dynarray_init(&binary, NULL);
        screen->vtbl.compile_shader(s, &inputs, &binary, &state->info);

        assert(req_local_mem >= state->info.wls_size);
        state->info.wls_size = req_local_mem;

        if (binary.size) {
                state->bin = panfrost_pool_take_ref(shader_pool,
                        pan_pool_upload_aligned(&shader_pool->base,
                                binary.data, binary.size, 128));
        }


        /* Don't upload RSD for fragment shaders since they need draw-time
         * merging for e.g. depth/stencil/alpha. RSDs are replaced by simpler
         * shader program descriptors on Valhall, which can be preuploaded even
         * for fragment shaders. */
        bool upload = !(s->info.stage == MESA_SHADER_FRAGMENT && dev->arch <= 7);
        screen->vtbl.prepare_shader(state, desc_pool, upload);

        panfrost_analyze_sysvals(state);

        util_dynarray_fini(&binary);

        /* In both clone and tgsi_to_nir paths, the shader is ralloc'd against
         * a NULL context */
        ralloc_free(s);
}

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
        struct panfrost_uncompiled_shader *vs = ctx->uncompiled[MESA_SHADER_VERTEX];

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
                key->fs.fixed_varying_mask = vs->fixed_varying_mask;
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

static struct panfrost_compiled_shader *
panfrost_new_variant_locked(
        struct panfrost_context *ctx,
        struct panfrost_uncompiled_shader *uncompiled,
        struct panfrost_shader_key *key)
{
        struct panfrost_compiled_shader *prog = panfrost_alloc_variant(uncompiled);

        *prog = (struct panfrost_compiled_shader) {
                .key = *key,
                .stream_output = uncompiled->stream_output,
        };

        panfrost_shader_compile(ctx->base.screen,
                                &ctx->shaders, &ctx->descs, uncompiled->nir,
                                &ctx->base.debug, prog, 0,
                                uncompiled->fixed_varying_mask);

        /* Fixup the stream out information */
        prog->so_mask =
                update_so_info(&prog->stream_output,
                               prog->info.outputs_written);

        prog->earlyzs = pan_earlyzs_analyze(&prog->info);

        return prog;
}

static void
panfrost_bind_shader_state(
        struct pipe_context *pctx,
        void *hwcso,
        enum pipe_shader_type type)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->uncompiled[type] = hwcso;
        ctx->prog[type] = NULL;

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
        if (type == PIPE_SHADER_FRAGMENT && !ctx->uncompiled[PIPE_SHADER_VERTEX])
                return;

        /* Also defer, happens with GALLIUM_HUD */
        if (!ctx->uncompiled[type])
                return;

        /* Match the appropriate variant */
        struct panfrost_uncompiled_shader *uncompiled = ctx->uncompiled[type];
        struct panfrost_compiled_shader *compiled = NULL;

        simple_mtx_lock(&uncompiled->lock);

        struct panfrost_shader_key key = { 0 };
        panfrost_build_key(ctx, &key, uncompiled->nir);

        util_dynarray_foreach(&uncompiled->variants, struct panfrost_compiled_shader, so) {
                if (memcmp(&key, &so->key, sizeof(key)) == 0) {
                        compiled = so;
                        break;
                }
        }

        if (compiled == NULL)
                compiled = panfrost_new_variant_locked(ctx, uncompiled, &key);

        ctx->prog[type] = compiled;

        /* TODO: it would be more efficient to release the lock before
         * compiling instead of after, but that can race if thread A compiles a
         * variant while thread B searches for that same variant */
        simple_mtx_unlock(&uncompiled->lock);
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
        struct panfrost_uncompiled_shader *so = panfrost_alloc_shader();
        struct panfrost_device *dev = pan_device(pctx->screen);

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

        /* If this shader uses transform feedback, compile the transform
         * feedback program. This is a special shader variant.
         */
        struct panfrost_context *ctx = pan_context(pctx);
        struct util_debug_callback *dbg = &ctx->base.debug;

        if (so->nir->xfb_info) {
                nir_shader *xfb = nir_shader_clone(NULL, so->nir);
                xfb->info.name = ralloc_asprintf(xfb, "%s@xfb", xfb->info.name);
                xfb->info.internal = true;

                so->xfb = calloc(1, sizeof(struct panfrost_compiled_shader));
                panfrost_shader_compile(pctx->screen, &ctx->shaders,
                                        &ctx->descs, xfb, dbg, so->xfb, 0,
                                        so->fixed_varying_mask);

                /* Since transform feedback is handled via the transform
                 * feedback program, the original program no longer uses XFB
                 */
                so->nir->info.has_transform_feedback_varyings = false;
        }

        /* Precompile for shader-db if we need to */
        if (unlikely(dev->debug & PAN_DBG_PRECOMPILE)) {
                struct panfrost_compiled_shader state = { 0 };

                panfrost_shader_compile(pctx->screen,
                                        &ctx->shaders, &ctx->descs,
                                        so->nir, dbg, &state, 0,
                                        so->fixed_varying_mask);
        }

        return so;
}

static void
panfrost_delete_shader_state(
        struct pipe_context *pctx,
        void *so)
{
        struct panfrost_uncompiled_shader *cso = (struct panfrost_uncompiled_shader *) so;

        ralloc_free(cso->nir);

        util_dynarray_foreach(&cso->variants, struct panfrost_compiled_shader, so) {
                panfrost_bo_unreference(so->bin.bo);
                panfrost_bo_unreference(so->state.bo);
                panfrost_bo_unreference(so->linkage.bo);
        }

        if (cso->xfb) {
                panfrost_bo_unreference(cso->xfb->bin.bo);
                panfrost_bo_unreference(cso->xfb->state.bo);
                panfrost_bo_unreference(cso->xfb->linkage.bo);
                free(cso->xfb);
        }

        simple_mtx_destroy(&cso->lock);

        util_dynarray_fini(&cso->variants);
        free(so);
}

/*
 * Create a compute CSO. As compute kernels do not require variants, they are
 * precompiled, creating both the uncompiled and compiled shaders now.
 */
static void *
panfrost_create_compute_state(
        struct pipe_context *pctx,
        const struct pipe_compute_state *cso)
{
        struct panfrost_context *ctx = pan_context(pctx);
        struct panfrost_uncompiled_shader *so = panfrost_alloc_shader();
        struct panfrost_compiled_shader *v = panfrost_alloc_variant(so);
        memset(v, 0, sizeof *v);

        assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");

        panfrost_shader_compile(pctx->screen, &ctx->shaders, &ctx->descs,
                                cso->prog, &ctx->base.debug, v,
                                cso->req_local_mem, 0);

        return so;
}

static void
panfrost_bind_compute_state(struct pipe_context *pipe, void *cso)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_uncompiled_shader *uncompiled = cso;

        ctx->uncompiled[PIPE_SHADER_COMPUTE] = uncompiled;

        ctx->prog[PIPE_SHADER_COMPUTE] =
                uncompiled ? util_dynarray_begin(&uncompiled->variants) : NULL;
}

static void
panfrost_delete_compute_state(struct pipe_context *pipe, void *cso)
{
        struct panfrost_uncompiled_shader *so =
                (struct panfrost_uncompiled_shader *)cso;

        util_dynarray_fini(&so->variants);
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

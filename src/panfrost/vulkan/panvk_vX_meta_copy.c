/*
 * Copyright © 2021 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "gen_macros.h"

#include "nir/nir_builder.h"
#include "pan_encoder.h"
#include "pan_shader.h"

#include "panvk_private.h"

static mali_ptr
panvk_meta_copy_img_emit_texture(struct panfrost_device *pdev,
                                 struct pan_pool *desc_pool,
                                 const struct pan_image_view *view)
{
#if PAN_ARCH >= 6
   struct panfrost_ptr texture =
      pan_pool_alloc_desc(desc_pool, TEXTURE);
   size_t payload_size =
      GENX(panfrost_estimate_texture_payload_size)(view);
   struct panfrost_ptr surfaces =
      pan_pool_alloc_aligned(desc_pool, payload_size,
                             pan_alignment(SURFACE_WITH_STRIDE));

   GENX(panfrost_new_texture)(pdev, view, texture.cpu, &surfaces);

   return texture.gpu;
#else
   size_t sz = pan_size(TEXTURE) +
               GENX(panfrost_estimate_texture_payload_size)(view);
   struct panfrost_ptr texture =
      pan_pool_alloc_aligned(desc_pool, sz, pan_alignment(TEXTURE));
   struct panfrost_ptr surfaces = {
      .cpu = texture.cpu + pan_size(TEXTURE),
      .gpu = texture.gpu + pan_size(TEXTURE),
   };

   GENX(panfrost_new_texture)(pdev, view, texture.cpu, &surfaces);

   return pan_pool_upload_aligned(desc_pool, &texture.gpu,
                                  sizeof(mali_ptr),
                                  sizeof(mali_ptr));
#endif
}

static mali_ptr
panvk_meta_copy_img_emit_sampler(struct panfrost_device *pdev,
                                 struct pan_pool *desc_pool)
{
   struct panfrost_ptr sampler =
      pan_pool_alloc_desc(desc_pool, SAMPLER);

   pan_pack(sampler.cpu, SAMPLER, cfg) {
#if PAN_ARCH >= 6
      cfg.seamless_cube_map = false;
#endif
      cfg.normalized_coordinates = false;
      cfg.minify_nearest = true;
      cfg.magnify_nearest = true;
   }

   return sampler.gpu;
}

static void
panvk_meta_copy_emit_varying(struct pan_pool *pool,
                             mali_ptr coordinates,
                             mali_ptr *varying_bufs,
                             mali_ptr *varyings)
{
   /* Bifrost needs an empty desc to mark end of prefetching */
   bool padding_buffer = PAN_ARCH >= 6;

   struct panfrost_ptr varying =
      pan_pool_alloc_desc(pool, ATTRIBUTE);
   struct panfrost_ptr varying_buffer =
      pan_pool_alloc_desc_array(pool, (padding_buffer ? 2 : 1),
                                     ATTRIBUTE_BUFFER);

   pan_pack(varying_buffer.cpu, ATTRIBUTE_BUFFER, cfg) {
      cfg.pointer = coordinates;
      cfg.stride = 4 * sizeof(uint32_t);
      cfg.size = cfg.stride * 4;
   }

   if (padding_buffer) {
      pan_pack(varying_buffer.cpu + pan_size(ATTRIBUTE_BUFFER),
               ATTRIBUTE_BUFFER, cfg);
   }

   pan_pack(varying.cpu, ATTRIBUTE, cfg) {
      cfg.buffer_index = 0;
      cfg.offset_enable = PAN_ARCH <= 5;
      cfg.format = pool->dev->formats[PIPE_FORMAT_R32G32B32_FLOAT].hw;
   }

   *varyings = varying.gpu;
   *varying_bufs = varying_buffer.gpu;
}

static void
panvk_meta_copy_emit_dcd(struct pan_pool *pool,
                         mali_ptr src_coords, mali_ptr dst_coords,
                         mali_ptr texture, mali_ptr sampler,
                         mali_ptr vpd, mali_ptr tsd, mali_ptr rsd,
                         mali_ptr ubos, mali_ptr push_constants,
                         void *out)
{
   pan_pack(out, DRAW, cfg) {
      cfg.four_components_per_vertex = true;
      cfg.draw_descriptor_is_64b = true;
      cfg.thread_storage = tsd;
      cfg.state = rsd;
      cfg.uniform_buffers = ubos;
      cfg.push_uniforms = push_constants;
      cfg.position = dst_coords;
      if (src_coords) {
              panvk_meta_copy_emit_varying(pool, src_coords,
                                           &cfg.varying_buffers,
                                           &cfg.varyings);
      }
      cfg.viewport = vpd;
      cfg.texture_descriptor_is_64b = PAN_ARCH <= 5;
      cfg.textures = texture;
      cfg.samplers = sampler;
   }
}

static struct panfrost_ptr
panvk_meta_copy_emit_tiler_job(struct pan_pool *desc_pool,
                               struct pan_scoreboard *scoreboard,
                               mali_ptr src_coords, mali_ptr dst_coords,
                               mali_ptr texture, mali_ptr sampler,
                               mali_ptr ubo, mali_ptr push_constants,
                               mali_ptr vpd, mali_ptr rsd,
                               mali_ptr tsd, mali_ptr tiler)
{
   struct panfrost_ptr job =
      pan_pool_alloc_desc(desc_pool, TILER_JOB);

   panvk_meta_copy_emit_dcd(desc_pool, src_coords, dst_coords,
                            texture, sampler, vpd, tsd, rsd, ubo, push_constants,
                            pan_section_ptr(job.cpu, TILER_JOB, DRAW));

   pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE, cfg) {
      cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
      cfg.index_count = 4;
      cfg.job_task_split = 6;
   }

   pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE_SIZE, cfg) {
      cfg.constant = 1.0f;
   }

   void *invoc = pan_section_ptr(job.cpu,
                                 TILER_JOB,
                                 INVOCATION);
   panfrost_pack_work_groups_compute(invoc, 1, 4,
                                     1, 1, 1, 1, true, false);

#if PAN_ARCH >= 6
   pan_section_pack(job.cpu, TILER_JOB, PADDING, cfg);
   pan_section_pack(job.cpu, TILER_JOB, TILER, cfg) {
      cfg.address = tiler;
   }
#endif

   panfrost_add_job(desc_pool, scoreboard, MALI_JOB_TYPE_TILER,
                    false, false, 0, 0, &job, false);
   return job;
}

#if PAN_ARCH >= 6
static uint32_t
panvk_meta_copy_img_bifrost_raw_format(unsigned texelsize)
{
   switch (texelsize) {
   case 6: return MALI_RGB16UI << 12;
   case 8: return MALI_RG32UI << 12;
   case 12: return MALI_RGB32UI << 12;
   case 16: return MALI_RGBA32UI << 12;
   default: unreachable("Invalid texel size\n");
   }
}
#endif

static mali_ptr
panvk_meta_copy_to_img_emit_rsd(struct panfrost_device *pdev,
                                struct pan_pool *desc_pool,
                                mali_ptr shader,
                                const struct pan_shader_info *shader_info,
                                enum pipe_format fmt, unsigned wrmask,
                                bool from_img)
{
   struct panfrost_ptr rsd_ptr =
      pan_pool_alloc_desc_aggregate(desc_pool,
                                    PAN_DESC(RENDERER_STATE),
                                    PAN_DESC_ARRAY(1, BLEND));

   bool raw = util_format_get_blocksize(fmt) > 4;
   unsigned fullmask = (1 << util_format_get_nr_components(fmt)) - 1;
   bool partialwrite = fullmask != wrmask && !raw;
   bool readstb = fullmask != wrmask && raw;

   pan_pack(rsd_ptr.cpu, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(shader_info, shader, &cfg);
      if (from_img) {
         cfg.shader.varying_count = 1;
         cfg.shader.texture_count = 1;
         cfg.shader.sampler_count = 1;
      }
      cfg.properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
      cfg.multisample_misc.sample_mask = UINT16_MAX;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;

#if PAN_ARCH >= 6
      cfg.properties.bifrost.allow_forward_pixel_to_be_killed = true;
      cfg.properties.bifrost.allow_forward_pixel_to_kill =
         !partialwrite && !readstb;
      cfg.properties.bifrost.zs_update_operation =
         MALI_PIXEL_KILL_STRONG_EARLY;
      cfg.properties.bifrost.pixel_kill_operation =
         MALI_PIXEL_KILL_FORCE_EARLY;
#else
      cfg.properties.midgard.shader_reads_tilebuffer = readstb;
      cfg.properties.midgard.work_register_count = shader_info->work_reg_count;
      cfg.properties.midgard.force_early_z = true;
      cfg.stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
#endif
   }

   pan_pack(rsd_ptr.cpu + pan_size(RENDERER_STATE), BLEND, cfg) {
      cfg.round_to_fb_precision = true;
      cfg.load_destination = partialwrite;
#if PAN_ARCH >= 6
      cfg.bifrost.internal.mode =
         partialwrite ?
         MALI_BIFROST_BLEND_MODE_FIXED_FUNCTION :
         MALI_BIFROST_BLEND_MODE_OPAQUE;
      cfg.bifrost.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.bifrost.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.bifrost.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.bifrost.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.bifrost.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.bifrost.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.bifrost.equation.color_mask = partialwrite ? wrmask : 0xf;
      cfg.bifrost.internal.fixed_function.num_comps = 4;
      if (!raw) {
         cfg.bifrost.internal.fixed_function.conversion.memory_format =
            panfrost_format_to_bifrost_blend(pdev, fmt, false);
         cfg.bifrost.internal.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_F32;
      } else {
         unsigned imgtexelsz = util_format_get_blocksize(fmt);

         cfg.bifrost.internal.fixed_function.conversion.memory_format =
            panvk_meta_copy_img_bifrost_raw_format(imgtexelsz);
         cfg.bifrost.internal.fixed_function.conversion.register_format =
            (imgtexelsz & 2) ?
            MALI_BIFROST_REGISTER_FILE_FORMAT_U16 :
            MALI_BIFROST_REGISTER_FILE_FORMAT_U32;
      }
#else
      cfg.midgard.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.midgard.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.midgard.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.midgard.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.midgard.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.midgard.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.midgard.equation.color_mask = wrmask;
#endif
   }

   return rsd_ptr.gpu;
}

static mali_ptr
panvk_meta_copy_img2img_shader(struct panfrost_device *pdev,
                               struct pan_pool *bin_pool,
                               enum pipe_format srcfmt,
                               enum pipe_format dstfmt, unsigned dstmask,
                               unsigned texdim, unsigned texisarray,
                               struct pan_shader_info *shader_info)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                     GENX(pan_shader_get_compiler_options)(),
                                     "panvk_meta_copy_img2img(srcfmt=%s,dstfmt=%s)",
                                     util_format_name(srcfmt), util_format_name(dstfmt));

   b.shader->info.internal = true;

   nir_variable *coord_var =
      nir_variable_create(b.shader, nir_var_shader_in,
                          glsl_vector_type(GLSL_TYPE_FLOAT, texdim + texisarray),
                          "coord");
   coord_var->data.location = VARYING_SLOT_TEX0;
   nir_ssa_def *coord = nir_f2u32(&b, nir_load_var(&b, coord_var));

   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_txf;
   tex->texture_index = 0;
   tex->is_array = texisarray;
   tex->dest_type = util_format_is_unorm(srcfmt) ?
                    nir_type_float32 : nir_type_uint32;

   switch (texdim) {
   case 1: tex->sampler_dim = GLSL_SAMPLER_DIM_1D; break;
   case 2: tex->sampler_dim = GLSL_SAMPLER_DIM_2D; break;
   case 3: tex->sampler_dim = GLSL_SAMPLER_DIM_3D; break;
   default: unreachable("Invalid texture dimension");
   }

   tex->src[0].src_type = nir_tex_src_coord;
   tex->src[0].src = nir_src_for_ssa(coord);
   tex->coord_components = texdim + texisarray;
   nir_ssa_dest_init(&tex->instr, &tex->dest, 4,
                     nir_alu_type_get_type_size(tex->dest_type), NULL);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_ssa_def *texel = &tex->dest.ssa;

   unsigned dstcompsz =
      util_format_get_component_bits(dstfmt, UTIL_FORMAT_COLORSPACE_RGB, 0);
   unsigned ndstcomps = util_format_get_nr_components(dstfmt);
   const struct glsl_type *outtype = NULL;

   if (srcfmt == PIPE_FORMAT_R5G6B5_UNORM && dstfmt == PIPE_FORMAT_R8G8_UNORM) {
      nir_ssa_def *rgb =
         nir_f2u32(&b, nir_fmul(&b, texel,
                                nir_vec3(&b,
                                         nir_imm_float(&b, 31),
                                         nir_imm_float(&b, 63),
                                         nir_imm_float(&b, 31))));
      nir_ssa_def *rg =
         nir_vec2(&b,
                  nir_ior(&b, nir_channel(&b, rgb, 0),
                          nir_ishl(&b, nir_channel(&b, rgb, 1),
                                   nir_imm_int(&b, 5))),
                  nir_ior(&b,
                          nir_ushr_imm(&b, nir_channel(&b, rgb, 1), 3),
                          nir_ishl(&b, nir_channel(&b, rgb, 2),
                                   nir_imm_int(&b, 3))));
      rg = nir_iand_imm(&b, rg, 255);
      texel = nir_fmul_imm(&b, nir_u2f32(&b, rg), 1.0 / 255);
      outtype = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
   } else if (srcfmt == PIPE_FORMAT_R8G8_UNORM && dstfmt == PIPE_FORMAT_R5G6B5_UNORM) {
      nir_ssa_def *rg = nir_f2u32(&b, nir_fmul_imm(&b, texel, 255));
      nir_ssa_def *rgb =
         nir_vec3(&b,
                  nir_channel(&b, rg, 0),
                  nir_ior(&b,
                          nir_ushr_imm(&b, nir_channel(&b, rg, 0), 5),
                          nir_ishl(&b, nir_channel(&b, rg, 1),
                                   nir_imm_int(&b, 3))),
                  nir_ushr_imm(&b, nir_channel(&b, rg, 1), 3));
      rgb = nir_iand(&b, rgb,
                     nir_vec3(&b,
                              nir_imm_int(&b, 31),
                              nir_imm_int(&b, 63),
                              nir_imm_int(&b, 31)));
      texel = nir_fmul(&b, nir_u2f32(&b, rgb),
                       nir_vec3(&b,
                                nir_imm_float(&b, 1.0 / 31),
                                nir_imm_float(&b, 1.0 / 63),
                                nir_imm_float(&b, 1.0 / 31)));
      outtype = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   } else {
      assert(srcfmt == dstfmt);
      enum glsl_base_type basetype;
      if (util_format_is_unorm(dstfmt)) {
         basetype = GLSL_TYPE_FLOAT;
      } else if (dstcompsz == 16) {
         basetype = GLSL_TYPE_UINT16;
      } else {
         assert(dstcompsz == 32);
         basetype = GLSL_TYPE_UINT;
      }

      if (dstcompsz == 16)
         texel = nir_u2u16(&b, texel);

      texel = nir_channels(&b, texel, (1 << ndstcomps) - 1);
      outtype = glsl_vector_type(basetype, ndstcomps);
   }

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, outtype, "out");
   out->data.location = FRAG_RESULT_DATA0;

   unsigned fullmask = (1 << ndstcomps) - 1;
   if (dstcompsz > 8 && dstmask != fullmask) {
      nir_ssa_def *oldtexel = nir_load_var(&b, out);
      nir_ssa_def *dstcomps[4];

      for (unsigned i = 0; i < ndstcomps; i++) {
         if (dstmask & BITFIELD_BIT(i))
            dstcomps[i] = nir_channel(&b, texel, i);
         else
            dstcomps[i] = nir_channel(&b, oldtexel, i);
      }

      texel = nir_vec(&b, dstcomps, ndstcomps);
   }

   nir_store_var(&b, out, texel, 0xff);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = pdev->gpu_id,
      .is_blit = true,
   };

#if PAN_ARCH >= 6
   pan_pack(&inputs.bifrost.rt_conv[0], BIFROST_INTERNAL_CONVERSION, cfg) {
      cfg.memory_format = (dstcompsz == 2 ? MALI_RG16UI : MALI_RG32UI) << 12;
      cfg.register_format = dstcompsz == 2 ?
                            MALI_BIFROST_REGISTER_FILE_FORMAT_U16 :
                            MALI_BIFROST_REGISTER_FILE_FORMAT_U32;
   }
   inputs.bifrost.static_rt_conv = true;
#endif

   struct util_dynarray binary;

   util_dynarray_init(&binary, NULL);
   GENX(pan_shader_compile)(b.shader, &inputs, &binary, shader_info);

   mali_ptr shader =
      pan_pool_upload_aligned(bin_pool, binary.data, binary.size,
                              PAN_ARCH >= 6 ? 128 : 64);

   util_dynarray_fini(&binary);
   ralloc_free(b.shader);

   return shader;
}

static enum pipe_format
panvk_meta_copy_img_format(enum pipe_format fmt)
{
   /* We can't use a non-compressed format when handling a tiled/AFBC
    * compressed format because the tile size differ (4x4 blocks for
    * compressed formats and 16x16 texels for non-compressed ones).
    */
   assert(!util_format_is_compressed(fmt));

   /* Pick blendable formats when we can, otherwise pick the UINT variant
    * matching the texel size.
    */
   switch (util_format_get_blocksize(fmt)) {
   case 16: return PIPE_FORMAT_R32G32B32A32_UINT;
   case 12: return PIPE_FORMAT_R32G32B32_UINT;
   case 8: return PIPE_FORMAT_R32G32_UINT;
   case 6: return PIPE_FORMAT_R16G16B16_UINT;
   case 4: return PIPE_FORMAT_R8G8B8A8_UNORM;
   case 2: return (fmt == PIPE_FORMAT_R5G6B5_UNORM ||
                   fmt == PIPE_FORMAT_B5G6R5_UNORM) ?
                  PIPE_FORMAT_R5G6B5_UNORM : PIPE_FORMAT_R8G8_UNORM;
   case 1: return PIPE_FORMAT_R8_UNORM;
   default: unreachable("Unsupported format\n");
   }
}

struct panvk_meta_copy_img2img_format_info {
   enum pipe_format srcfmt;
   enum pipe_format dstfmt;
   unsigned dstmask;
};

static const struct panvk_meta_copy_img2img_format_info panvk_meta_copy_img2img_fmts[] = {
   { PIPE_FORMAT_R8_UNORM, PIPE_FORMAT_R8_UNORM, 0x1},
   { PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_R5G6B5_UNORM, 0x7},
   { PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_R8G8_UNORM, 0x3},
   { PIPE_FORMAT_R8G8_UNORM, PIPE_FORMAT_R5G6B5_UNORM, 0x7},
   { PIPE_FORMAT_R8G8_UNORM, PIPE_FORMAT_R8G8_UNORM, 0x3},
   /* Z24S8(depth) */
   { PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM, 0x7 },
   /* Z24S8(stencil) */
   { PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM, 0x8 },
   { PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_UNORM, 0xf },
   { PIPE_FORMAT_R16G16B16_UINT, PIPE_FORMAT_R16G16B16_UINT, 0x7 },
   { PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32_UINT, 0x3 },
   /* Z32S8X24(depth) */
   { PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32_UINT, 0x1 },
   /* Z32S8X24(stencil) */
   { PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32_UINT, 0x2 },
   { PIPE_FORMAT_R32G32B32_UINT, PIPE_FORMAT_R32G32B32_UINT, 0x7 },
   { PIPE_FORMAT_R32G32B32A32_UINT, PIPE_FORMAT_R32G32B32A32_UINT, 0xf },
};

static unsigned
panvk_meta_copy_img2img_format_idx(struct panvk_meta_copy_img2img_format_info key)
{
   STATIC_ASSERT(ARRAY_SIZE(panvk_meta_copy_img2img_fmts) == PANVK_META_COPY_IMG2IMG_NUM_FORMATS);

   for (unsigned i = 0; i < ARRAY_SIZE(panvk_meta_copy_img2img_fmts); i++) {
      if (!memcmp(&key, &panvk_meta_copy_img2img_fmts[i], sizeof(key)))
         return i;
   }

   unreachable("Invalid image format\n");
}

static unsigned
panvk_meta_copy_img_mask(enum pipe_format imgfmt, VkImageAspectFlags aspectMask)
{
   if (aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT &&
       aspectMask != VK_IMAGE_ASPECT_STENCIL_BIT) {
      enum pipe_format outfmt = panvk_meta_copy_img_format(imgfmt);

      return (1 << util_format_get_nr_components(outfmt)) - 1;
   }

   switch (imgfmt) {
   case PIPE_FORMAT_S8_UINT:
      return 1;
   case PIPE_FORMAT_Z16_UNORM:
      return 3;
   case PIPE_FORMAT_Z16_UNORM_S8_UINT:
      return aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT ? 3 : 8;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      return aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT ? 7 : 8;
   case PIPE_FORMAT_Z24X8_UNORM:
      assert(aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
      return 7;
   case PIPE_FORMAT_Z32_FLOAT:
      return 0xf;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      return aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT ? 1 : 2;
   default:
      unreachable("Invalid depth format\n");
   }
}

static void
panvk_meta_copy_img2img(struct panvk_cmd_buffer *cmdbuf,
                        const struct panvk_image *src,
                        const struct panvk_image *dst,
                        const VkImageCopy *region)
{
   struct panfrost_device *pdev = &cmdbuf->device->physical_device->pdev;
   struct pan_fb_info *fbinfo = &cmdbuf->state.fb.info;
   struct panvk_meta_copy_img2img_format_info key = {
      .srcfmt = panvk_meta_copy_img_format(src->pimage.layout.format),
      .dstfmt = panvk_meta_copy_img_format(dst->pimage.layout.format),
      .dstmask = panvk_meta_copy_img_mask(dst->pimage.layout.format,
                                          region->dstSubresource.aspectMask),
   };

   unsigned texdimidx =
      panvk_meta_copy_tex_type(src->pimage.layout.dim,
                               src->pimage.layout.array_size > 1);
   unsigned fmtidx =
      panvk_meta_copy_img2img_format_idx(key);

   mali_ptr rsd =
      cmdbuf->device->physical_device->meta.copy.img2img[texdimidx][fmtidx].rsd;

   struct pan_image_view srcview = {
      .format = key.srcfmt,
      .dim = src->pimage.layout.dim == MALI_TEXTURE_DIMENSION_CUBE ?
             MALI_TEXTURE_DIMENSION_2D : src->pimage.layout.dim,
      .image = &src->pimage,
      .nr_samples = src->pimage.layout.nr_samples,
      .first_level = region->srcSubresource.mipLevel,
      .last_level = region->srcSubresource.mipLevel,
      .first_layer = region->srcSubresource.baseArrayLayer,
      .last_layer = region->srcSubresource.baseArrayLayer + region->srcSubresource.layerCount - 1,
      .swizzle = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W },
   };

   struct pan_image_view dstview = {
      .format = key.dstfmt,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .image = &dst->pimage,
      .nr_samples = dst->pimage.layout.nr_samples,
      .first_level = region->dstSubresource.mipLevel,
      .last_level = region->dstSubresource.mipLevel,
      .swizzle = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W },
   };

   unsigned minx = MAX2(region->dstOffset.x, 0);
   unsigned miny = MAX2(region->dstOffset.y, 0);
   unsigned maxx = MAX2(region->dstOffset.x + region->extent.width - 1, 0);
   unsigned maxy = MAX2(region->dstOffset.y + region->extent.height - 1, 0);

   mali_ptr vpd =
      panvk_per_arch(meta_emit_viewport)(&cmdbuf->desc_pool.base,
                                         minx, miny, maxx, maxy);

   float dst_rect[] = {
      minx, miny, 0.0, 1.0,
      maxx + 1, miny, 0.0, 1.0,
      minx, maxy + 1, 0.0, 1.0,
      maxx + 1, maxy + 1, 0.0, 1.0,
   };

   mali_ptr dst_coords =
      pan_pool_upload_aligned(&cmdbuf->desc_pool.base, dst_rect,
                              sizeof(dst_rect), 64);

   /* TODO: don't force preloads of dst resources if unneeded */

   unsigned width = u_minify(dst->pimage.layout.width, region->dstSubresource.mipLevel);
   unsigned height = u_minify(dst->pimage.layout.height, region->dstSubresource.mipLevel);
   cmdbuf->state.fb.crc_valid[0] = false;
   *fbinfo = (struct pan_fb_info){
      .width = width,
      .height = height,
      .extent.minx = minx & ~31,
      .extent.miny = miny & ~31,
      .extent.maxx = MIN2(ALIGN_POT(maxx + 1, 32), width) - 1,
      .extent.maxy = MIN2(ALIGN_POT(maxy + 1, 32), height) - 1,
      .nr_samples = 1,
      .rt_count = 1,
      .rts[0].view = &dstview,
      .rts[0].preload = true,
      .rts[0].crc_valid = &cmdbuf->state.fb.crc_valid[0],
   };

   mali_ptr texture =
      panvk_meta_copy_img_emit_texture(pdev, &cmdbuf->desc_pool.base, &srcview);
   mali_ptr sampler =
      panvk_meta_copy_img_emit_sampler(pdev, &cmdbuf->desc_pool.base);

   if (cmdbuf->state.batch)
      panvk_per_arch(cmd_close_batch)(cmdbuf);

   minx = MAX2(region->srcOffset.x, 0);
   miny = MAX2(region->srcOffset.y, 0);
   maxx = MAX2(region->srcOffset.x + region->extent.width - 1, 0);
   maxy = MAX2(region->srcOffset.y + region->extent.height - 1, 0);
   assert(region->dstOffset.z >= 0);

   unsigned first_src_layer = MAX2(0, region->srcOffset.z);
   unsigned first_dst_layer = MAX2(region->dstSubresource.baseArrayLayer, region->dstOffset.z);
   unsigned nlayers = MAX2(region->dstSubresource.layerCount, region->extent.depth);
   for (unsigned l = 0; l < nlayers; l++) {
      unsigned src_l = l + first_src_layer;
      float src_rect[] = {
         minx, miny, src_l, 1.0,
         maxx + 1, miny, src_l, 1.0,
         minx, maxy + 1, src_l, 1.0,
         maxx + 1, maxy + 1, src_l, 1.0,
      };

      mali_ptr src_coords =
         pan_pool_upload_aligned(&cmdbuf->desc_pool.base, src_rect,
                                 sizeof(src_rect), 64);

      panvk_cmd_open_batch(cmdbuf);

      struct panvk_batch *batch = cmdbuf->state.batch;

      dstview.first_layer = dstview.last_layer = l + first_dst_layer;
      batch->blit.src = src->pimage.data.bo;
      batch->blit.dst = dst->pimage.data.bo;
      panvk_per_arch(cmd_alloc_tls_desc)(cmdbuf, true);
      panvk_per_arch(cmd_alloc_fb_desc)(cmdbuf);
      panvk_per_arch(cmd_prepare_tiler_context)(cmdbuf);

      mali_ptr tsd, tiler;

#if PAN_ARCH >= 6
      tsd = batch->tls.gpu;
      tiler = batch->tiler.descs.gpu;
#else
      tsd = batch->fb.desc.gpu;
      tiler = 0;
#endif

      struct panfrost_ptr job;

      job = panvk_meta_copy_emit_tiler_job(&cmdbuf->desc_pool.base,
                                           &batch->scoreboard,
                                           src_coords, dst_coords,
                                           texture, sampler, 0, 0,
                                           vpd, rsd, tsd, tiler);

      util_dynarray_append(&batch->jobs, void *, job.cpu);
      panvk_per_arch(cmd_close_batch)(cmdbuf);
   }
}

static void
panvk_meta_copy_img2img_init(struct panvk_physical_device *dev)
{
   STATIC_ASSERT(ARRAY_SIZE(panvk_meta_copy_img2img_fmts) == PANVK_META_COPY_IMG2IMG_NUM_FORMATS);

   for (unsigned i = 0; i < ARRAY_SIZE(panvk_meta_copy_img2img_fmts); i++) {
      for (unsigned texdim = 1; texdim <= 3; texdim++) {
         unsigned texdimidx = panvk_meta_copy_tex_type(texdim, false);
         assert(texdimidx < ARRAY_SIZE(dev->meta.copy.img2img));

         struct pan_shader_info shader_info;
         mali_ptr shader =
            panvk_meta_copy_img2img_shader(&dev->pdev, &dev->meta.bin_pool.base,
                                           panvk_meta_copy_img2img_fmts[i].srcfmt,
                                           panvk_meta_copy_img2img_fmts[i].dstfmt,
                                           panvk_meta_copy_img2img_fmts[i].dstmask,
                                           texdim, false, &shader_info);
         dev->meta.copy.img2img[texdimidx][i].rsd =
            panvk_meta_copy_to_img_emit_rsd(&dev->pdev, &dev->meta.desc_pool.base,
                                            shader, &shader_info,
                                            panvk_meta_copy_img2img_fmts[i].dstfmt,
                                            panvk_meta_copy_img2img_fmts[i].dstmask,
                                            true);
         if (texdim == 3)
            continue;

	 memset(&shader_info, 0, sizeof(shader_info));
         texdimidx = panvk_meta_copy_tex_type(texdim, true);
         assert(texdimidx < ARRAY_SIZE(dev->meta.copy.img2img));
         shader =
            panvk_meta_copy_img2img_shader(&dev->pdev, &dev->meta.bin_pool.base,
                                           panvk_meta_copy_img2img_fmts[i].srcfmt,
                                           panvk_meta_copy_img2img_fmts[i].dstfmt,
                                           panvk_meta_copy_img2img_fmts[i].dstmask,
                                           texdim, true, &shader_info);
         dev->meta.copy.img2img[texdimidx][i].rsd =
            panvk_meta_copy_to_img_emit_rsd(&dev->pdev, &dev->meta.desc_pool.base,
                                            shader, &shader_info,
                                            panvk_meta_copy_img2img_fmts[i].dstfmt,
                                            panvk_meta_copy_img2img_fmts[i].dstmask,
                                            true);
      }
   }
}

void
panvk_per_arch(CmdCopyImage)(VkCommandBuffer commandBuffer,
                             VkImage srcImage,
                             VkImageLayout srcImageLayout,
                             VkImage destImage,
                             VkImageLayout destImageLayout,
                             uint32_t regionCount,
                             const VkImageCopy *pRegions)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_image, dst, destImage);
   VK_FROM_HANDLE(panvk_image, src, srcImage);

   for (unsigned i = 0; i < regionCount; i++) {
      panvk_meta_copy_img2img(cmdbuf, src, dst, &pRegions[i]);
   }
}

void
panvk_per_arch(CmdCopyBufferToImage)(VkCommandBuffer commandBuffer,
                                     VkBuffer srcBuffer,
                                     VkImage destImage,
                                     VkImageLayout destImageLayout,
                                     uint32_t regionCount,
                                     const VkBufferImageCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdCopyImageToBuffer)(VkCommandBuffer commandBuffer,
                                     VkImage srcImage,
                                     VkImageLayout srcImageLayout,
                                     VkBuffer destBuffer,
                                     uint32_t regionCount,
                                     const VkBufferImageCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdCopyBuffer)(VkCommandBuffer commandBuffer,
                              VkBuffer srcBuffer,
                              VkBuffer destBuffer,
                              uint32_t regionCount,
                              const VkBufferCopy *pRegions)
{
   panvk_stub();
}

void
panvk_per_arch(CmdFillBuffer)(VkCommandBuffer commandBuffer,
                              VkBuffer dstBuffer,
                              VkDeviceSize dstOffset,
                              VkDeviceSize fillSize,
                              uint32_t data)
{
   panvk_stub();
}

void
panvk_per_arch(CmdUpdateBuffer)(VkCommandBuffer commandBuffer,
                                VkBuffer dstBuffer,
                                VkDeviceSize dstOffset,
                                VkDeviceSize dataSize,
                                const void *pData)
{
   panvk_stub();
}

void
panvk_per_arch(meta_copy_init)(struct panvk_physical_device *dev)
{
   panvk_meta_copy_img2img_init(dev);
}

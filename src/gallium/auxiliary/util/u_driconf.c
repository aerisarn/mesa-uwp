/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "u_driconf.h"

void
u_driconf_fill_st_options(struct st_config_options *options,
                          const struct driOptionCache *optionCache)
{
   options->disable_blend_func_extended =
      driQueryOptionb(optionCache, "disable_blend_func_extended");
   options->disable_arb_gpu_shader5 =
      driQueryOptionb(optionCache, "disable_arb_gpu_shader5");
   options->disable_glsl_line_continuations =
      driQueryOptionb(optionCache, "disable_glsl_line_continuations");
   options->force_glsl_extensions_warn =
      driQueryOptionb(optionCache, "force_glsl_extensions_warn");
   options->force_glsl_version =
      driQueryOptioni(optionCache, "force_glsl_version");
   options->allow_extra_pp_tokens =
      driQueryOptionb(optionCache, "allow_extra_pp_tokens");
   options->allow_glsl_extension_directive_midshader =
      driQueryOptionb(optionCache, "allow_glsl_extension_directive_midshader");
   options->allow_glsl_120_subset_in_110 =
      driQueryOptionb(optionCache, "allow_glsl_120_subset_in_110");
   options->allow_glsl_builtin_const_expression =
      driQueryOptionb(optionCache, "allow_glsl_builtin_const_expression");
   options->allow_glsl_relaxed_es =
      driQueryOptionb(optionCache, "allow_glsl_relaxed_es");
   options->allow_glsl_builtin_variable_redeclaration =
      driQueryOptionb(optionCache, "allow_glsl_builtin_variable_redeclaration");
   options->allow_higher_compat_version =
      driQueryOptionb(optionCache, "allow_higher_compat_version");
   options->glsl_ignore_write_to_readonly_var =
      driQueryOptionb(optionCache, "glsl_ignore_write_to_readonly_var");
   options->glsl_zero_init = driQueryOptionb(optionCache, "glsl_zero_init");
   options->force_integer_tex_nearest =
      driQueryOptionb(optionCache, "force_integer_tex_nearest");
   options->vs_position_always_invariant =
      driQueryOptionb(optionCache, "vs_position_always_invariant");
   options->force_glsl_abs_sqrt =
      driQueryOptionb(optionCache, "force_glsl_abs_sqrt");
   options->allow_glsl_cross_stage_interpolation_mismatch =
      driQueryOptionb(optionCache, "allow_glsl_cross_stage_interpolation_mismatch");
   options->allow_draw_out_of_order =
      driQueryOptionb(optionCache, "allow_draw_out_of_order");
   options->allow_incorrect_primitive_id =
      driQueryOptionb(optionCache, "allow_incorrect_primitive_id");
   options->ignore_map_unsynchronized =
      driQueryOptionb(optionCache, "ignore_map_unsynchronized");
   options->force_gl_names_reuse =
      driQueryOptionb(optionCache, "force_gl_names_reuse");
   options->transcode_etc =
      driQueryOptionb(optionCache, "transcode_etc");
   options->transcode_astc =
      driQueryOptionb(optionCache, "transcode_astc");

   char *vendor_str = driQueryOptionstr(optionCache, "force_gl_vendor");
   /* not an empty string */
   if (*vendor_str)
      options->force_gl_vendor = strdup(vendor_str);

   char *renderer_str = driQueryOptionstr(optionCache, "force_gl_renderer");
   if (*renderer_str)
      options->force_gl_renderer = strdup(renderer_str);

   driComputeOptionsSha1(optionCache, options->config_options_sha1);
}

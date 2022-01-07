/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * \file shader_types.h
 * All the GL shader/program types.
 */

#ifndef SHADER_TYPES_H
#define SHADER_TYPES_H

#include "main/config.h" /* for MAX_FEEDBACK_BUFFERS */
#include "main/glheader.h"

/**
 * Shader information needed by both gl_shader and gl_linked shader.
 */
struct gl_shader_info
{
   /**
    * Tessellation Control shader state from layout qualifiers.
    */
   struct {
      /**
       * 0 - vertices not declared in shader, or
       * 1 .. GL_MAX_PATCH_VERTICES
       */
      GLint VerticesOut;
   } TessCtrl;

   /**
    * Tessellation Evaluation shader state from layout qualifiers.
    */
   struct {
      enum tess_primitive_mode _PrimitiveMode;

      enum gl_tess_spacing Spacing;

      /**
       * GL_CW, GL_CCW, or 0 if it's not set in this shader.
       */
      GLenum16 VertexOrder;
      /**
       * 1, 0, or -1 if it's not set in this shader.
       */
      int PointMode;
   } TessEval;

   /**
    * Geometry shader state from GLSL 1.50 layout qualifiers.
    */
   struct {
      GLint VerticesOut;
      /**
       * 0 - Invocations count not declared in shader, or
       * 1 .. Const.MaxGeometryShaderInvocations
       */
      GLint Invocations;
      /**
       * GL_POINTS, GL_LINES, GL_LINES_ADJACENCY, GL_TRIANGLES, or
       * GL_TRIANGLES_ADJACENCY, or PRIM_UNKNOWN if it's not set in this
       * shader.
       */
      enum shader_prim InputType;
       /**
        * GL_POINTS, GL_LINE_STRIP or GL_TRIANGLE_STRIP, or PRIM_UNKNOWN if
        * it's not set in this shader.
        */
      enum shader_prim OutputType;
   } Geom;

   /**
    * Compute shader state from ARB_compute_shader and
    * ARB_compute_variable_group_size layout qualifiers.
    */
   struct {
      /**
       * Size specified using local_size_{x,y,z}, or all 0's to indicate that
       * it's not set in this shader.
       */
      unsigned LocalSize[3];

      /**
       * Whether a variable work group size has been specified as defined by
       * ARB_compute_variable_group_size.
       */
      bool LocalSizeVariable;

      /*
       * Arrangement of invocations used to calculate derivatives in a compute
       * shader.  From NV_compute_shader_derivatives.
       */
      enum gl_derivative_group DerivativeGroup;
   } Comp;
};

/**
 * Compile status enum. COMPILE_SKIPPED is used to indicate the compile
 * was skipped due to the shader matching one that's been seen before by
 * the on-disk cache.
 */
enum gl_compile_status
{
   COMPILE_FAILURE = 0,
   COMPILE_SUCCESS,
   COMPILE_SKIPPED
};

/**
 * A GLSL shader object.
 */
struct gl_shader
{
   /** GL_FRAGMENT_SHADER || GL_VERTEX_SHADER || GL_GEOMETRY_SHADER_ARB ||
    *  GL_TESS_CONTROL_SHADER || GL_TESS_EVALUATION_SHADER.
    * Must be the first field.
    */
   GLenum16 Type;
   gl_shader_stage Stage;
   GLuint Name;  /**< AKA the handle */
   GLint RefCount;  /**< Reference count */
   GLchar *Label;   /**< GL_KHR_debug */
   GLboolean DeletePending;
   bool IsES;              /**< True if this shader uses GLSL ES */

   enum gl_compile_status CompileStatus;

   /** SHA1 of the pre-processed source used by the disk cache. */
   uint8_t disk_cache_sha1[SHA1_DIGEST_LENGTH];
   /** SHA1 of the original source before replacement, set by glShaderSource. */
   uint8_t source_sha1[SHA1_DIGEST_LENGTH];
   /** SHA1 of FallbackSource (a copy of some original source before replacement). */
   uint8_t fallback_source_sha1[SHA1_DIGEST_LENGTH];
   /** SHA1 of the current compiled source, set by successful glCompileShader. */
   uint8_t compiled_source_sha1[SHA1_DIGEST_LENGTH];

   const GLchar *Source;  /**< Source code string */
   const GLchar *FallbackSource;  /**< Fallback string used by on-disk cache*/

   GLchar *InfoLog;

   unsigned Version;       /**< GLSL version used for linking */

   /**
    * A bitmask of gl_advanced_blend_mode values
    */
   GLbitfield BlendSupport;

   struct exec_list *ir;
   struct glsl_symbol_table *symbols;

   /**
    * Whether early fragment tests are enabled as defined by
    * ARB_shader_image_load_store.
    */
   bool EarlyFragmentTests;

   bool ARB_fragment_coord_conventions_enable;

   bool redeclares_gl_fragcoord;
   bool uses_gl_fragcoord;

   bool PostDepthCoverage;
   bool PixelInterlockOrdered;
   bool PixelInterlockUnordered;
   bool SampleInterlockOrdered;
   bool SampleInterlockUnordered;
   bool InnerCoverage;

   /**
    * Fragment shader state from GLSL 1.50 layout qualifiers.
    */
   bool origin_upper_left;
   bool pixel_center_integer;

   /**
    * Whether bindless_sampler/bindless_image, and respectively
    * bound_sampler/bound_image are declared at global scope as defined by
    * ARB_bindless_texture.
    */
   bool bindless_sampler;
   bool bindless_image;
   bool bound_sampler;
   bool bound_image;

   /**
    * Whether layer output is viewport-relative.
    */
   bool redeclares_gl_layer;
   bool layer_viewport_relative;

   /** Global xfb_stride out qualifier if any */
   GLuint TransformFeedbackBufferStride[MAX_FEEDBACK_BUFFERS];

   struct gl_shader_info info;

   /* ARB_gl_spirv related data */
   struct gl_shader_spirv_data *spirv_data;
};

#endif

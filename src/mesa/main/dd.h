/**
 * \file dd.h
 * Device driver interfaces.
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2006  Brian Paul   All Rights Reserved.
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


#ifndef DD_INCLUDED
#define DD_INCLUDED

#include "glheader.h"
#include "formats.h"
#include "menums.h"
#include "compiler/shader_enums.h"

/* Windows winnt.h defines MemoryBarrier as a macro on some platforms,
 * including as a function-like macro in some cases. That either causes
 * the table entry below to have a weird name, or fail to compile.
 */
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

struct gl_bitmap_atlas;
struct gl_buffer_object;
struct gl_context;
struct gl_display_list;
struct gl_framebuffer;
struct gl_image_unit;
struct gl_pixelstore_attrib;
struct gl_program;
struct gl_renderbuffer;
struct gl_renderbuffer_attachment;
struct gl_shader;
struct gl_shader_program;
struct gl_texture_image;
struct gl_texture_object;
struct gl_memory_info;
struct gl_memory_object;
struct gl_query_object;
struct gl_sampler_object;
struct gl_transform_feedback_object;
struct gl_vertex_array_object;
struct ati_fragment_shader;
struct util_queue_monitoring;
struct _mesa_prim;
struct _mesa_index_buffer;
struct pipe_draw_info;
struct pipe_draw_start_count_bias;
struct pipe_vertex_state;
struct pipe_draw_vertex_state_info;
struct pipe_vertex_buffer;
struct pipe_vertex_element;

/* GL_ARB_vertex_buffer_object */
/* Modifies GL_MAP_UNSYNCHRONIZED_BIT to allow driver to fail (return
 * NULL) if buffer is unavailable for immediate mapping.
 *
 * Does GL_MAP_INVALIDATE_RANGE_BIT do this?  It seems so, but it
 * would require more book-keeping in the driver than seems necessary
 * at this point.
 *
 * Does GL_MAP_INVALDIATE_BUFFER_BIT do this?  Not really -- we don't
 * want to provoke the driver to throw away the old storage, we will
 * respect the contents of already referenced data.
 */
#define MESA_MAP_NOWAIT_BIT       0x4000

/* Mapping a buffer is allowed from any thread. */
#define MESA_MAP_THREAD_SAFE_BIT  0x8000

/* This buffer will only be mapped/unmapped once */
#define MESA_MAP_ONCE            0x10000

/* This BufferStorage flag indicates that the buffer will be used
 * by pipe_vertex_state, which doesn't track buffer busyness and doesn't
 * support invalidations.
 */
#define MESA_GALLIUM_VERTEX_STATE_STORAGE 0x20000


/**
 * Device driver function table.
 * Core Mesa uses these function pointers to call into device drivers.
 * Most of these functions directly correspond to OpenGL state commands.
 * Core Mesa will call these functions after error checking has been done
 * so that the drivers don't have to worry about error testing.
 *
 * Vertex transformation/clipping/lighting is patched into the T&L module.
 * Rasterization functions are patched into the swrast module.
 *
 * Note: when new functions are added here, the drivers/common/driverfuncs.c
 * file should be updated too!!!
 */
struct dd_function_table {
   /**
    * \name Vertex/fragment program functions
    */
   /** Allocate a new program */
   struct gl_program * (*NewProgram)(struct gl_context *ctx,
                                     gl_shader_stage stage,
                                     GLuint id, bool is_arb_asm);
   /**
    * \name Draw functions.
    */
   /*@{*/
   /**
    * For indirect array drawing:
    *
    *    typedef struct {
    *       GLuint count;
    *       GLuint primCount;
    *       GLuint first;
    *       GLuint baseInstance; // in GL 4.2 and later, must be zero otherwise
    *    } DrawArraysIndirectCommand;
    *
    * For indirect indexed drawing:
    *
    *    typedef struct {
    *       GLuint count;
    *       GLuint primCount;
    *       GLuint firstIndex;
    *       GLint  baseVertex;
    *       GLuint baseInstance; // in GL 4.2 and later, must be zero otherwise
    *    } DrawElementsIndirectCommand;
    */

   /**
    * Draw a number of primitives.
    * \param prims  array [nr_prims] describing what to draw (prim type,
    *               vertex count, first index, instance count, etc).
    * \param ib  index buffer for indexed drawing, NULL for array drawing
    * \param index_bounds_valid  are min_index and max_index valid?
    * \param min_index  lowest vertex index used
    * \param max_index  highest vertex index used
    * \param num_instances  instance count from ARB_draw_instanced
    * \param base_instance  base instance from ARB_base_instance
    */
   void (*Draw)(struct gl_context *ctx,
                const struct _mesa_prim *prims, unsigned nr_prims,
                const struct _mesa_index_buffer *ib,
                bool index_bounds_valid,
                bool primitive_restart,
                unsigned restart_index,
                unsigned min_index, unsigned max_index,
                unsigned num_instances, unsigned base_instance);

   /**
    * Optimal Gallium version of Draw() that doesn't require translation
    * of draw info in the state tracker.
    *
    * The interface is identical to pipe_context::draw_vbo
    * with indirect == NULL.
    *
    * "info" is not const and the following fields can be changed by
    * the callee, so callers should be aware:
    * - info->index_bounds_valid (if false)
    * - info->min_index (if index_bounds_valid is false)
    * - info->max_index (if index_bounds_valid is false)
    * - info->drawid (if increment_draw_id is true)
    * - info->index.gl_bo (if index_size && !has_user_indices)
    */
   void (*DrawGallium)(struct gl_context *ctx,
                       struct pipe_draw_info *info,
                       unsigned drawid_offset,
                       const struct pipe_draw_start_count_bias *draws,
                       unsigned num_draws);

   /**
    * Same as DrawGallium, but mode can also change between draws.
    *
    * "info" is not const and the following fields can be changed by
    * the callee in addition to the fields listed by DrawGallium:
    * - info->mode
    *
    * This function exists to decrease complexity of DrawGallium.
    */
   void (*DrawGalliumMultiMode)(struct gl_context *ctx,
                                struct pipe_draw_info *info,
                                const struct pipe_draw_start_count_bias *draws,
                                const unsigned char *mode,
                                unsigned num_draws);

   /**
    * Draw a primitive, getting the vertex count, instance count, start
    * vertex, etc. from a buffer object.
    * \param mode  GL_POINTS, GL_LINES, GL_TRIANGLE_STRIP, etc.
    * \param indirect_data  buffer to get "DrawArrays/ElementsIndirectCommand"
    *                       data
    * \param indirect_offset  offset of first primitive in indrect_data buffer
    * \param draw_count  number of primitives to draw
    * \param stride  stride, in bytes, between
    *                "DrawArrays/ElementsIndirectCommand" objects
    * \param indirect_draw_count_buffer  if non-NULL specifies a buffer to get
    *                                    the real draw_count value.  Used for
    *                                    GL_ARB_indirect_parameters.
    * \param indirect_draw_count_offset  offset to the draw_count value in
    *                                    indirect_draw_count_buffer
    * \param ib  index buffer for indexed drawing, NULL otherwise.
    */
   void (*DrawIndirect)(struct gl_context *ctx, GLuint mode,
                        struct gl_buffer_object *indirect_data,
                        GLsizeiptr indirect_offset, unsigned draw_count,
                        unsigned stride,
                        struct gl_buffer_object *indirect_draw_count_buffer,
                        GLsizeiptr indirect_draw_count_offset,
                        const struct _mesa_index_buffer *ib,
                        bool primitive_restart,
                        unsigned restart_index);

   /**
    * Driver implementation of glDrawTransformFeedback.
    *
    * \param mode    Primitive type
    * \param num_instances  instance count from ARB_draw_instanced
    * \param stream  If called via DrawTransformFeedbackStream, specifies
    *                the vertex stream buffer from which to get the vertex
    *                count.
    * \param tfb_vertcount  if non-null, indicates which transform feedback
    *                       object has the vertex count.
    */
   void (*DrawTransformFeedback)(struct gl_context *ctx, GLenum mode,
                                 unsigned num_instances, unsigned stream,
                                 struct gl_transform_feedback_object *tfb_vertcount);

   void (*DrawGalliumVertexState)(struct gl_context *ctx,
                                  struct pipe_vertex_state *state,
                                  struct pipe_draw_vertex_state_info info,
                                  const struct pipe_draw_start_count_bias *draws,
                                  const uint8_t *mode,
                                  unsigned num_draws,
                                  bool per_vertex_edgeflags);
   /*@}*/

   struct pipe_vertex_state *
      (*CreateGalliumVertexState)(struct gl_context *ctx,
                                  const struct gl_vertex_array_object *vao,
                                  struct gl_buffer_object *indexbuf,
                                  uint32_t enabled_attribs);

   /**
    * \name Vertex/pixel buffer object functions
    */
   /*@{*/
   void (*InvalidateBufferSubData)( struct gl_context *ctx,
                                    struct gl_buffer_object *obj,
                                    GLintptr offset,
                                    GLsizeiptr length );

   /*@}*/

   /**
    * \name Functions for GL_ARB_sample_locations
    */
   void (*GetProgrammableSampleCaps)(struct gl_context *ctx,
                                     const struct gl_framebuffer *fb,
                                     GLuint *bits, GLuint *width, GLuint *height);

   /*@}*/

   /**
    * \name GREMEDY debug/marker functions
    */
   /*@{*/
   void (*EmitStringMarker)(struct gl_context *ctx, const GLchar *string, GLsizei len);
   /*@}*/

   /**
    * \name Support for multiple T&L engines
    */
   /*@{*/

   /**
    * Set by the driver-supplied T&L engine.  
    *
    * Set to PRIM_OUTSIDE_BEGIN_END when outside glBegin()/glEnd().
    */
   GLuint CurrentExecPrimitive;

   /**
    * Current glBegin state of an in-progress compilation.  May be
    * GL_POINTS, GL_TRIANGLE_STRIP, etc. or PRIM_OUTSIDE_BEGIN_END
    * or PRIM_UNKNOWN.
    */
   GLuint CurrentSavePrimitive;


#define FLUSH_STORED_VERTICES 0x1
#define FLUSH_UPDATE_CURRENT  0x2
   /**
    * Set by the driver-supplied T&L engine whenever vertices are buffered
    * between glBegin()/glEnd() objects or __struct gl_contextRec::Current
    * is not updated.  A bitmask of the FLUSH_x values above.
    *
    * The dd_function_table::FlushVertices call below may be used to resolve
    * these conditions.
    */
   GLbitfield NeedFlush;

   /** Need to call vbo_save_SaveFlushVertices() upon state change? */
   GLboolean SaveNeedFlush;

   /**@}*/

   /**
    * \name GL_OES_draw_texture interface
    */
   /*@{*/
   void (*DrawTex)(struct gl_context *ctx, GLfloat x, GLfloat y, GLfloat z,
                   GLfloat width, GLfloat height);
   /*@}*/

   /**
    * \name GL_OES_EGL_image interface
    */
   void (*EGLImageTargetTexture2D)(struct gl_context *ctx, GLenum target,
				   struct gl_texture_object *texObj,
				   struct gl_texture_image *texImage,
				   GLeglImageOES image_handle);
   void (*EGLImageTargetRenderbufferStorage)(struct gl_context *ctx,
					     struct gl_renderbuffer *rb,
					     void *image_handle);

   /**
    * \name GL_EXT_EGL_image_storage interface
    */
   void (*EGLImageTargetTexStorage)(struct gl_context *ctx, GLenum target,
                                    struct gl_texture_object *texObj,
                                    struct gl_texture_image *texImage,
                                    GLeglImageOES image_handle);

   /**
    * \name GL_ARB_texture_multisample
    */
   void (*GetSamplePosition)(struct gl_context *ctx,
                             struct gl_framebuffer *fb,
                             GLuint index,
                             GLfloat *outValue);

   /**
    * \name NV_vdpau_interop interface
    */
   void (*VDPAUMapSurface)(struct gl_context *ctx, GLenum target,
                           GLenum access, GLboolean output,
                           struct gl_texture_object *texObj,
                           struct gl_texture_image *texImage,
                           const GLvoid *vdpSurface, GLuint index);
   void (*VDPAUUnmapSurface)(struct gl_context *ctx, GLenum target,
                             GLenum access, GLboolean output,
                             struct gl_texture_object *texObj,
                             struct gl_texture_image *texImage,
                             const GLvoid *vdpSurface, GLuint index);

   /**
    * Query reset status for GL_ARB_robustness
    *
    * Per \c glGetGraphicsResetStatusARB, this function should return a
    * non-zero value once after a reset.  If a reset is non-atomic, the
    * non-zero status should be returned for the duration of the reset.
    */
   GLenum (*GetGraphicsResetStatus)(struct gl_context *ctx);

   /**
    * \name GL_ARB_compute_shader interface
    */
   /*@{*/
   void (*DispatchCompute)(struct gl_context *ctx, const GLuint *num_groups);
   void (*DispatchComputeIndirect)(struct gl_context *ctx, GLintptr indirect);
   /*@}*/

   /**
    * \name GL_ARB_compute_variable_group_size interface
    */
   /*@{*/
   void (*DispatchComputeGroupSize)(struct gl_context *ctx,
                                    const GLuint *num_groups,
                                    const GLuint *group_size);
   /*@}*/

   /**
    * \name GL_ARB_get_program_binary
    */
   /*@{*/
   /**
    * Calls to retrieve/store a binary serialized copy of the current program.
    */
   void (*GetProgramBinaryDriverSHA1)(struct gl_context *ctx, uint8_t *sha1);

   void (*ProgramBinarySerializeDriverBlob)(struct gl_context *ctx,
                                            struct gl_shader_program *shProg,
                                            struct gl_program *prog);

   void (*ProgramBinaryDeserializeDriverBlob)(struct gl_context *ctx,
                                              struct gl_shader_program *shProg,
                                              struct gl_program *prog);
   /*@}*/

   /**
    * \name Disk shader cache functions
    */
   /*@{*/
   /**
    * Called to initialize gl_program::driver_cache_blob (and size) with a
    * ralloc allocated buffer.
    *
    * This buffer will be saved and restored as part of the gl_program
    * serialization and deserialization.
    */
   void (*ShaderCacheSerializeDriverBlob)(struct gl_context *ctx,
                                          struct gl_program *prog);
   /*@}*/

   void (*PinDriverToL3Cache)(struct gl_context *ctx, unsigned L3_cache);

   GLboolean (*ValidateEGLImage)(struct gl_context *ctx, GLeglImageOES image_handle);
};


/**
 * Per-vertex functions.
 *
 * These are the functions which can appear between glBegin and glEnd.
 * Depending on whether we're inside or outside a glBegin/End pair
 * and whether we're in immediate mode or building a display list, these
 * functions behave differently.  This structure allows us to switch
 * between those modes more easily.
 *
 * Generally, these pointers point to functions in the VBO module.
 */
typedef struct {
   void (GLAPIENTRYP ArrayElement)( GLint );
   void (GLAPIENTRYP Color3f)( GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP Color3fv)( const GLfloat * );
   void (GLAPIENTRYP Color4f)( GLfloat, GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP Color4fv)( const GLfloat * );
   void (GLAPIENTRYP EdgeFlag)( GLboolean );
   void (GLAPIENTRYP EvalCoord1f)( GLfloat );
   void (GLAPIENTRYP EvalCoord1fv)( const GLfloat * );
   void (GLAPIENTRYP EvalCoord2f)( GLfloat, GLfloat );
   void (GLAPIENTRYP EvalCoord2fv)( const GLfloat * );
   void (GLAPIENTRYP EvalPoint1)( GLint );
   void (GLAPIENTRYP EvalPoint2)( GLint, GLint );
   void (GLAPIENTRYP FogCoordfEXT)( GLfloat );
   void (GLAPIENTRYP FogCoordfvEXT)( const GLfloat * );
   void (GLAPIENTRYP Indexf)( GLfloat );
   void (GLAPIENTRYP Indexfv)( const GLfloat * );
   void (GLAPIENTRYP Materialfv)( GLenum face, GLenum pname, const GLfloat * );
   void (GLAPIENTRYP MultiTexCoord1fARB)( GLenum, GLfloat );
   void (GLAPIENTRYP MultiTexCoord1fvARB)( GLenum, const GLfloat * );
   void (GLAPIENTRYP MultiTexCoord2fARB)( GLenum, GLfloat, GLfloat );
   void (GLAPIENTRYP MultiTexCoord2fvARB)( GLenum, const GLfloat * );
   void (GLAPIENTRYP MultiTexCoord3fARB)( GLenum, GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP MultiTexCoord3fvARB)( GLenum, const GLfloat * );
   void (GLAPIENTRYP MultiTexCoord4fARB)( GLenum, GLfloat, GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP MultiTexCoord4fvARB)( GLenum, const GLfloat * );
   void (GLAPIENTRYP Normal3f)( GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP Normal3fv)( const GLfloat * );
   void (GLAPIENTRYP SecondaryColor3fEXT)( GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP SecondaryColor3fvEXT)( const GLfloat * );
   void (GLAPIENTRYP TexCoord1f)( GLfloat );
   void (GLAPIENTRYP TexCoord1fv)( const GLfloat * );
   void (GLAPIENTRYP TexCoord2f)( GLfloat, GLfloat );
   void (GLAPIENTRYP TexCoord2fv)( const GLfloat * );
   void (GLAPIENTRYP TexCoord3f)( GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP TexCoord3fv)( const GLfloat * );
   void (GLAPIENTRYP TexCoord4f)( GLfloat, GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP TexCoord4fv)( const GLfloat * );
   void (GLAPIENTRYP Vertex2f)( GLfloat, GLfloat );
   void (GLAPIENTRYP Vertex2fv)( const GLfloat * );
   void (GLAPIENTRYP Vertex3f)( GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP Vertex3fv)( const GLfloat * );
   void (GLAPIENTRYP Vertex4f)( GLfloat, GLfloat, GLfloat, GLfloat );
   void (GLAPIENTRYP Vertex4fv)( const GLfloat * );
   void (GLAPIENTRYP CallList)( GLuint );
   void (GLAPIENTRYP CallLists)( GLsizei, GLenum, const GLvoid * );
   void (GLAPIENTRYP Begin)( GLenum );
   void (GLAPIENTRYP End)( void );
   void (GLAPIENTRYP PrimitiveRestartNV)( void );
   /* Originally for GL_NV_vertex_program, now used only dlist.c and friends */
   void (GLAPIENTRYP VertexAttrib1fNV)( GLuint index, GLfloat x );
   void (GLAPIENTRYP VertexAttrib1fvNV)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib2fNV)( GLuint index, GLfloat x, GLfloat y );
   void (GLAPIENTRYP VertexAttrib2fvNV)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib3fNV)( GLuint index, GLfloat x, GLfloat y, GLfloat z );
   void (GLAPIENTRYP VertexAttrib3fvNV)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib4fNV)( GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w );
   void (GLAPIENTRYP VertexAttrib4fvNV)( GLuint index, const GLfloat *v );
   /* GL_ARB_vertex_program */
   void (GLAPIENTRYP VertexAttrib1fARB)( GLuint index, GLfloat x );
   void (GLAPIENTRYP VertexAttrib1fvARB)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib2fARB)( GLuint index, GLfloat x, GLfloat y );
   void (GLAPIENTRYP VertexAttrib2fvARB)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib3fARB)( GLuint index, GLfloat x, GLfloat y, GLfloat z );
   void (GLAPIENTRYP VertexAttrib3fvARB)( GLuint index, const GLfloat *v );
   void (GLAPIENTRYP VertexAttrib4fARB)( GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w );
   void (GLAPIENTRYP VertexAttrib4fvARB)( GLuint index, const GLfloat *v );

   /* GL_EXT_gpu_shader4 / GL 3.0 */
   void (GLAPIENTRYP VertexAttribI1i)( GLuint index, GLint x);
   void (GLAPIENTRYP VertexAttribI2i)( GLuint index, GLint x, GLint y);
   void (GLAPIENTRYP VertexAttribI3i)( GLuint index, GLint x, GLint y, GLint z);
   void (GLAPIENTRYP VertexAttribI4i)( GLuint index, GLint x, GLint y, GLint z, GLint w);
   void (GLAPIENTRYP VertexAttribI2iv)( GLuint index, const GLint *v);
   void (GLAPIENTRYP VertexAttribI3iv)( GLuint index, const GLint *v);
   void (GLAPIENTRYP VertexAttribI4iv)( GLuint index, const GLint *v);

   void (GLAPIENTRYP VertexAttribI1ui)( GLuint index, GLuint x);
   void (GLAPIENTRYP VertexAttribI2ui)( GLuint index, GLuint x, GLuint y);
   void (GLAPIENTRYP VertexAttribI3ui)( GLuint index, GLuint x, GLuint y, GLuint z);
   void (GLAPIENTRYP VertexAttribI4ui)( GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
   void (GLAPIENTRYP VertexAttribI2uiv)( GLuint index, const GLuint *v);
   void (GLAPIENTRYP VertexAttribI3uiv)( GLuint index, const GLuint *v);
   void (GLAPIENTRYP VertexAttribI4uiv)( GLuint index, const GLuint *v);

   /* GL_ARB_vertex_type_10_10_10_2_rev / GL3.3 */
   void (GLAPIENTRYP VertexP2ui)( GLenum type, GLuint value );
   void (GLAPIENTRYP VertexP2uiv)( GLenum type, const GLuint *value);

   void (GLAPIENTRYP VertexP3ui)( GLenum type, GLuint value );
   void (GLAPIENTRYP VertexP3uiv)( GLenum type, const GLuint *value);

   void (GLAPIENTRYP VertexP4ui)( GLenum type, GLuint value );
   void (GLAPIENTRYP VertexP4uiv)( GLenum type, const GLuint *value);

   void (GLAPIENTRYP TexCoordP1ui)( GLenum type, GLuint coords );
   void (GLAPIENTRYP TexCoordP1uiv)( GLenum type, const GLuint *coords );

   void (GLAPIENTRYP TexCoordP2ui)( GLenum type, GLuint coords );
   void (GLAPIENTRYP TexCoordP2uiv)( GLenum type, const GLuint *coords );

   void (GLAPIENTRYP TexCoordP3ui)( GLenum type, GLuint coords );
   void (GLAPIENTRYP TexCoordP3uiv)( GLenum type, const GLuint *coords );

   void (GLAPIENTRYP TexCoordP4ui)( GLenum type, GLuint coords );
   void (GLAPIENTRYP TexCoordP4uiv)( GLenum type, const GLuint *coords );

   void (GLAPIENTRYP MultiTexCoordP1ui)( GLenum texture, GLenum type, GLuint coords );
   void (GLAPIENTRYP MultiTexCoordP1uiv)( GLenum texture, GLenum type, const GLuint *coords );
   void (GLAPIENTRYP MultiTexCoordP2ui)( GLenum texture, GLenum type, GLuint coords );
   void (GLAPIENTRYP MultiTexCoordP2uiv)( GLenum texture, GLenum type, const GLuint *coords );
   void (GLAPIENTRYP MultiTexCoordP3ui)( GLenum texture, GLenum type, GLuint coords );
   void (GLAPIENTRYP MultiTexCoordP3uiv)( GLenum texture, GLenum type, const GLuint *coords );
   void (GLAPIENTRYP MultiTexCoordP4ui)( GLenum texture, GLenum type, GLuint coords );
   void (GLAPIENTRYP MultiTexCoordP4uiv)( GLenum texture, GLenum type, const GLuint *coords );

   void (GLAPIENTRYP NormalP3ui)( GLenum type, GLuint coords );
   void (GLAPIENTRYP NormalP3uiv)( GLenum type, const GLuint *coords );

   void (GLAPIENTRYP ColorP3ui)( GLenum type, GLuint color );
   void (GLAPIENTRYP ColorP3uiv)( GLenum type, const GLuint *color );

   void (GLAPIENTRYP ColorP4ui)( GLenum type, GLuint color );
   void (GLAPIENTRYP ColorP4uiv)( GLenum type, const GLuint *color );

   void (GLAPIENTRYP SecondaryColorP3ui)( GLenum type, GLuint color );
   void (GLAPIENTRYP SecondaryColorP3uiv)( GLenum type, const GLuint *color );

   void (GLAPIENTRYP VertexAttribP1ui)( GLuint index, GLenum type,
					GLboolean normalized, GLuint value);
   void (GLAPIENTRYP VertexAttribP2ui)( GLuint index, GLenum type,
					GLboolean normalized, GLuint value);
   void (GLAPIENTRYP VertexAttribP3ui)( GLuint index, GLenum type,
					GLboolean normalized, GLuint value);
   void (GLAPIENTRYP VertexAttribP4ui)( GLuint index, GLenum type,
					GLboolean normalized, GLuint value);
   void (GLAPIENTRYP VertexAttribP1uiv)( GLuint index, GLenum type,
					GLboolean normalized,
					 const GLuint *value);
   void (GLAPIENTRYP VertexAttribP2uiv)( GLuint index, GLenum type,
					GLboolean normalized,
					 const GLuint *value);
   void (GLAPIENTRYP VertexAttribP3uiv)( GLuint index, GLenum type,
					GLboolean normalized,
					 const GLuint *value);
   void (GLAPIENTRYP VertexAttribP4uiv)( GLuint index, GLenum type,
					 GLboolean normalized,
					 const GLuint *value);

   /* GL_ARB_vertex_attrib_64bit / GL 4.1 */
   void (GLAPIENTRYP VertexAttribL1d)( GLuint index, GLdouble x);
   void (GLAPIENTRYP VertexAttribL2d)( GLuint index, GLdouble x, GLdouble y);
   void (GLAPIENTRYP VertexAttribL3d)( GLuint index, GLdouble x, GLdouble y, GLdouble z);
   void (GLAPIENTRYP VertexAttribL4d)( GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);


   void (GLAPIENTRYP VertexAttribL1dv)( GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribL2dv)( GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribL3dv)( GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribL4dv)( GLuint index, const GLdouble *v);

   void (GLAPIENTRYP VertexAttribL1ui64ARB)( GLuint index, GLuint64EXT x);
   void (GLAPIENTRYP VertexAttribL1ui64vARB)( GLuint index, const GLuint64EXT *v);

   /* GL_NV_half_float */
   void (GLAPIENTRYP Vertex2hNV)( GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Vertex2hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP Vertex3hNV)( GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Vertex3hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP Vertex4hNV)( GLhalfNV, GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Vertex4hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP Normal3hNV)( GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Normal3hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP Color3hNV)( GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Color3hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP Color4hNV)( GLhalfNV, GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP Color4hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP TexCoord1hNV)( GLhalfNV );
   void (GLAPIENTRYP TexCoord1hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP TexCoord2hNV)( GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP TexCoord2hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP TexCoord3hNV)( GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP TexCoord3hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP TexCoord4hNV)( GLhalfNV, GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP TexCoord4hvNV)( const GLhalfNV * );
   void (GLAPIENTRYP MultiTexCoord1hNV)( GLenum, GLhalfNV );
   void (GLAPIENTRYP MultiTexCoord1hvNV)( GLenum, const GLhalfNV * );
   void (GLAPIENTRYP MultiTexCoord2hNV)( GLenum, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP MultiTexCoord2hvNV)( GLenum, const GLhalfNV * );
   void (GLAPIENTRYP MultiTexCoord3hNV)( GLenum, GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP MultiTexCoord3hvNV)( GLenum, const GLhalfNV * );
   void (GLAPIENTRYP MultiTexCoord4hNV)( GLenum, GLhalfNV, GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP MultiTexCoord4hvNV)( GLenum, const GLhalfNV * );
   void (GLAPIENTRYP VertexAttrib1hNV)( GLuint index, GLhalfNV x );
   void (GLAPIENTRYP VertexAttrib1hvNV)( GLuint index, const GLhalfNV *v );
   void (GLAPIENTRYP VertexAttrib2hNV)( GLuint index, GLhalfNV x, GLhalfNV y );
   void (GLAPIENTRYP VertexAttrib2hvNV)( GLuint index, const GLhalfNV *v );
   void (GLAPIENTRYP VertexAttrib3hNV)( GLuint index, GLhalfNV x, GLhalfNV y, GLhalfNV z );
   void (GLAPIENTRYP VertexAttrib3hvNV)( GLuint index, const GLhalfNV *v );
   void (GLAPIENTRYP VertexAttrib4hNV)( GLuint index, GLhalfNV x, GLhalfNV y, GLhalfNV z, GLhalfNV w );
   void (GLAPIENTRYP VertexAttrib4hvNV)( GLuint index, const GLhalfNV *v );
   void (GLAPIENTRYP VertexAttribs1hvNV)(GLuint index, GLsizei n, const GLhalfNV *v);
   void (GLAPIENTRYP VertexAttribs2hvNV)(GLuint index, GLsizei n, const GLhalfNV *v);
   void (GLAPIENTRYP VertexAttribs3hvNV)(GLuint index, GLsizei n, const GLhalfNV *v);
   void (GLAPIENTRYP VertexAttribs4hvNV)(GLuint index, GLsizei n, const GLhalfNV *v);
   void (GLAPIENTRYP FogCoordhNV)( GLhalfNV );
   void (GLAPIENTRYP FogCoordhvNV)( const GLhalfNV * );
   void (GLAPIENTRYP SecondaryColor3hNV)( GLhalfNV, GLhalfNV, GLhalfNV );
   void (GLAPIENTRYP SecondaryColor3hvNV)( const GLhalfNV * );

   void (GLAPIENTRYP Color3b)( GLbyte red, GLbyte green, GLbyte blue );
   void (GLAPIENTRYP Color3d)( GLdouble red, GLdouble green, GLdouble blue );
   void (GLAPIENTRYP Color3i)( GLint red, GLint green, GLint blue );
   void (GLAPIENTRYP Color3s)( GLshort red, GLshort green, GLshort blue );
   void (GLAPIENTRYP Color3ui)( GLuint red, GLuint green, GLuint blue );
   void (GLAPIENTRYP Color3us)( GLushort red, GLushort green, GLushort blue );
   void (GLAPIENTRYP Color3ub)( GLubyte red, GLubyte green, GLubyte blue );
   void (GLAPIENTRYP Color3bv)( const GLbyte *v );
   void (GLAPIENTRYP Color3dv)( const GLdouble *v );
   void (GLAPIENTRYP Color3iv)( const GLint *v );
   void (GLAPIENTRYP Color3sv)( const GLshort *v );
   void (GLAPIENTRYP Color3uiv)( const GLuint *v );
   void (GLAPIENTRYP Color3usv)( const GLushort *v );
   void (GLAPIENTRYP Color3ubv)( const GLubyte *v );
   void (GLAPIENTRYP Color4b)( GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha );
   void (GLAPIENTRYP Color4d)( GLdouble red, GLdouble green, GLdouble blue,
                       GLdouble alpha );
   void (GLAPIENTRYP Color4i)( GLint red, GLint green, GLint blue, GLint alpha );
   void (GLAPIENTRYP Color4s)( GLshort red, GLshort green, GLshort blue,
                       GLshort alpha );
   void (GLAPIENTRYP Color4ui)( GLuint red, GLuint green, GLuint blue, GLuint alpha );
   void (GLAPIENTRYP Color4us)( GLushort red, GLushort green, GLushort blue,
                        GLushort alpha );
   void (GLAPIENTRYP Color4ub)( GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha );
   void (GLAPIENTRYP Color4iv)( const GLint *v );
   void (GLAPIENTRYP Color4bv)( const GLbyte *v );
   void (GLAPIENTRYP Color4dv)( const GLdouble *v );
   void (GLAPIENTRYP Color4sv)( const GLshort *v);
   void (GLAPIENTRYP Color4uiv)( const GLuint *v);
   void (GLAPIENTRYP Color4usv)( const GLushort *v);
   void (GLAPIENTRYP Color4ubv)( const GLubyte *v);
   void (GLAPIENTRYP FogCoordd)( GLdouble d );
   void (GLAPIENTRYP FogCoorddv)( const GLdouble *v );
   void (GLAPIENTRYP Indexd)( GLdouble c );
   void (GLAPIENTRYP Indexi)( GLint c );
   void (GLAPIENTRYP Indexs)( GLshort c );
   void (GLAPIENTRYP Indexub)( GLubyte c );
   void (GLAPIENTRYP Indexdv)( const GLdouble *c );
   void (GLAPIENTRYP Indexiv)( const GLint *c );
   void (GLAPIENTRYP Indexsv)( const GLshort *c );
   void (GLAPIENTRYP Indexubv)( const GLubyte *c );
   void (GLAPIENTRYP EdgeFlagv)(const GLboolean *flag);
   void (GLAPIENTRYP Normal3b)( GLbyte nx, GLbyte ny, GLbyte nz );
   void (GLAPIENTRYP Normal3d)( GLdouble nx, GLdouble ny, GLdouble nz );
   void (GLAPIENTRYP Normal3i)( GLint nx, GLint ny, GLint nz );
   void (GLAPIENTRYP Normal3s)( GLshort nx, GLshort ny, GLshort nz );
   void (GLAPIENTRYP Normal3bv)( const GLbyte *v );
   void (GLAPIENTRYP Normal3dv)( const GLdouble *v );
   void (GLAPIENTRYP Normal3iv)( const GLint *v );
   void (GLAPIENTRYP Normal3sv)( const GLshort *v );
   void (GLAPIENTRYP TexCoord1d)( GLdouble s );
   void (GLAPIENTRYP TexCoord1i)( GLint s );
   void (GLAPIENTRYP TexCoord1s)( GLshort s );
   void (GLAPIENTRYP TexCoord2d)( GLdouble s, GLdouble t );
   void (GLAPIENTRYP TexCoord2s)( GLshort s, GLshort t );
   void (GLAPIENTRYP TexCoord2i)( GLint s, GLint t );
   void (GLAPIENTRYP TexCoord3d)( GLdouble s, GLdouble t, GLdouble r );
   void (GLAPIENTRYP TexCoord3i)( GLint s, GLint t, GLint r );
   void (GLAPIENTRYP TexCoord3s)( GLshort s, GLshort t, GLshort r );
   void (GLAPIENTRYP TexCoord4d)( GLdouble s, GLdouble t, GLdouble r, GLdouble q );
   void (GLAPIENTRYP TexCoord4i)( GLint s, GLint t, GLint r, GLint q );
   void (GLAPIENTRYP TexCoord4s)( GLshort s, GLshort t, GLshort r, GLshort q );
   void (GLAPIENTRYP TexCoord1dv)( const GLdouble *v );
   void (GLAPIENTRYP TexCoord1iv)( const GLint *v );
   void (GLAPIENTRYP TexCoord1sv)( const GLshort *v );
   void (GLAPIENTRYP TexCoord2dv)( const GLdouble *v );
   void (GLAPIENTRYP TexCoord2iv)( const GLint *v );
   void (GLAPIENTRYP TexCoord2sv)( const GLshort *v );
   void (GLAPIENTRYP TexCoord3dv)( const GLdouble *v );
   void (GLAPIENTRYP TexCoord3iv)( const GLint *v );
   void (GLAPIENTRYP TexCoord3sv)( const GLshort *v );
   void (GLAPIENTRYP TexCoord4dv)( const GLdouble *v );
   void (GLAPIENTRYP TexCoord4iv)( const GLint *v );
   void (GLAPIENTRYP TexCoord4sv)( const GLshort *v );
   void (GLAPIENTRYP Vertex2d)( GLdouble x, GLdouble y );
   void (GLAPIENTRYP Vertex2i)( GLint x, GLint y );
   void (GLAPIENTRYP Vertex2s)( GLshort x, GLshort y );
   void (GLAPIENTRYP Vertex3d)( GLdouble x, GLdouble y, GLdouble z );
   void (GLAPIENTRYP Vertex3i)( GLint x, GLint y, GLint z );
   void (GLAPIENTRYP Vertex3s)( GLshort x, GLshort y, GLshort z );
   void (GLAPIENTRYP Vertex4d)( GLdouble x, GLdouble y, GLdouble z, GLdouble w );
   void (GLAPIENTRYP Vertex4i)( GLint x, GLint y, GLint z, GLint w );
   void (GLAPIENTRYP Vertex4s)( GLshort x, GLshort y, GLshort z, GLshort w );
   void (GLAPIENTRYP Vertex2dv)( const GLdouble *v );
   void (GLAPIENTRYP Vertex2iv)( const GLint *v );
   void (GLAPIENTRYP Vertex2sv)( const GLshort *v );
   void (GLAPIENTRYP Vertex3dv)( const GLdouble *v );
   void (GLAPIENTRYP Vertex3iv)( const GLint *v );
   void (GLAPIENTRYP Vertex3sv)( const GLshort *v );
   void (GLAPIENTRYP Vertex4dv)( const GLdouble *v );
   void (GLAPIENTRYP Vertex4iv)( const GLint *v );
   void (GLAPIENTRYP Vertex4sv)( const GLshort *v );
   void (GLAPIENTRYP MultiTexCoord1d)(GLenum target, GLdouble s);
   void (GLAPIENTRYP MultiTexCoord1dv)(GLenum target, const GLdouble *v);
   void (GLAPIENTRYP MultiTexCoord1i)(GLenum target, GLint s);
   void (GLAPIENTRYP MultiTexCoord1iv)(GLenum target, const GLint *v);
   void (GLAPIENTRYP MultiTexCoord1s)(GLenum target, GLshort s);
   void (GLAPIENTRYP MultiTexCoord1sv)(GLenum target, const GLshort *v);
   void (GLAPIENTRYP MultiTexCoord2d)(GLenum target, GLdouble s, GLdouble t);
   void (GLAPIENTRYP MultiTexCoord2dv)(GLenum target, const GLdouble *v);
   void (GLAPIENTRYP MultiTexCoord2i)(GLenum target, GLint s, GLint t);
   void (GLAPIENTRYP MultiTexCoord2iv)(GLenum target, const GLint *v);
   void (GLAPIENTRYP MultiTexCoord2s)(GLenum target, GLshort s, GLshort t);
   void (GLAPIENTRYP MultiTexCoord2sv)(GLenum target, const GLshort *v);
   void (GLAPIENTRYP MultiTexCoord3d)(GLenum target, GLdouble s, GLdouble t, GLdouble r);
   void (GLAPIENTRYP MultiTexCoord3dv)(GLenum target, const GLdouble *v);
   void (GLAPIENTRYP MultiTexCoord3i)(GLenum target, GLint s, GLint t, GLint r);
   void (GLAPIENTRYP MultiTexCoord3iv)(GLenum target, const GLint *v);
   void (GLAPIENTRYP MultiTexCoord3s)(GLenum target, GLshort s, GLshort t, GLshort r);
   void (GLAPIENTRYP MultiTexCoord3sv)(GLenum target, const GLshort *v);
   void (GLAPIENTRYP MultiTexCoord4d)(GLenum target, GLdouble s, GLdouble t, GLdouble r,
                               GLdouble q);
   void (GLAPIENTRYP MultiTexCoord4dv)(GLenum target, const GLdouble *v);
   void (GLAPIENTRYP MultiTexCoord4i)(GLenum target, GLint s, GLint t, GLint r, GLint q);
   void (GLAPIENTRYP MultiTexCoord4iv)(GLenum target, const GLint *v);
   void (GLAPIENTRYP MultiTexCoord4s)(GLenum target, GLshort s, GLshort t, GLshort r,
                               GLshort q);
   void (GLAPIENTRYP MultiTexCoord4sv)(GLenum target, const GLshort *v);
   void (GLAPIENTRYP EvalCoord2dv)( const GLdouble *u );
   void (GLAPIENTRYP EvalCoord2d)( GLdouble u, GLdouble v );
   void (GLAPIENTRYP EvalCoord1dv)( const GLdouble *u );
   void (GLAPIENTRYP EvalCoord1d)( GLdouble u );
   void (GLAPIENTRYP Materialf)( GLenum face, GLenum pname, GLfloat param );
   void (GLAPIENTRYP Materiali)(GLenum face, GLenum pname, GLint param );
   void (GLAPIENTRYP Materialiv)(GLenum face, GLenum pname, const GLint *params );
   void (GLAPIENTRYP SecondaryColor3b)( GLbyte red, GLbyte green, GLbyte blue );
   void (GLAPIENTRYP SecondaryColor3d)( GLdouble red, GLdouble green, GLdouble blue );
   void (GLAPIENTRYP SecondaryColor3i)( GLint red, GLint green, GLint blue );
   void (GLAPIENTRYP SecondaryColor3s)( GLshort red, GLshort green, GLshort blue );
   void (GLAPIENTRYP SecondaryColor3ui)( GLuint red, GLuint green, GLuint blue );
   void (GLAPIENTRYP SecondaryColor3us)( GLushort red, GLushort green, GLushort blue );
   void (GLAPIENTRYP SecondaryColor3ub)( GLubyte red, GLubyte green, GLubyte blue );
   void (GLAPIENTRYP SecondaryColor3bv)( const GLbyte *v );
   void (GLAPIENTRYP SecondaryColor3dv)( const GLdouble *v );
   void (GLAPIENTRYP SecondaryColor3iv)( const GLint *v );
   void (GLAPIENTRYP SecondaryColor3sv)( const GLshort *v );
   void (GLAPIENTRYP SecondaryColor3uiv)( const GLuint *v );
   void (GLAPIENTRYP SecondaryColor3usv)( const GLushort *v );
   void (GLAPIENTRYP SecondaryColor3ubv)( const GLubyte *v );
   void (GLAPIENTRYP VertexAttrib1sNV)(GLuint index, GLshort x);
   void (GLAPIENTRYP VertexAttrib1dNV)(GLuint index, GLdouble x);
   void (GLAPIENTRYP VertexAttrib2sNV)(GLuint index, GLshort x, GLshort y);
   void (GLAPIENTRYP VertexAttrib2dNV)(GLuint index, GLdouble x, GLdouble y);
   void (GLAPIENTRYP VertexAttrib3sNV)(GLuint index, GLshort x, GLshort y, GLshort z);
   void (GLAPIENTRYP VertexAttrib3dNV)(GLuint index, GLdouble x, GLdouble y, GLdouble z);
   void (GLAPIENTRYP VertexAttrib4sNV)(GLuint index, GLshort x, GLshort y, GLshort z,
                             GLshort w);
   void (GLAPIENTRYP VertexAttrib4dNV)(GLuint index, GLdouble x, GLdouble y, GLdouble z,
                             GLdouble w);
   void (GLAPIENTRYP VertexAttrib4ubNV)(GLuint index, GLubyte x, GLubyte y, GLubyte z,
                              GLubyte w);
   void (GLAPIENTRYP VertexAttrib1svNV)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib1dvNV)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib2svNV)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib2dvNV)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib3svNV)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib3dvNV)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib4svNV)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib4dvNV)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib4ubvNV)(GLuint index, const GLubyte *v);
   void (GLAPIENTRYP VertexAttribs1svNV)(GLuint index, GLsizei n, const GLshort *v);
   void (GLAPIENTRYP VertexAttribs1fvNV)(GLuint index, GLsizei n, const GLfloat *v);
   void (GLAPIENTRYP VertexAttribs1dvNV)(GLuint index, GLsizei n, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribs2svNV)(GLuint index, GLsizei n, const GLshort *v);
   void (GLAPIENTRYP VertexAttribs2fvNV)(GLuint index, GLsizei n, const GLfloat *v);
   void (GLAPIENTRYP VertexAttribs2dvNV)(GLuint index, GLsizei n, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribs3svNV)(GLuint index, GLsizei n, const GLshort *v);
   void (GLAPIENTRYP VertexAttribs3fvNV)(GLuint index, GLsizei n, const GLfloat *v);
   void (GLAPIENTRYP VertexAttribs3dvNV)(GLuint index, GLsizei n, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribs4svNV)(GLuint index, GLsizei n, const GLshort *v);
   void (GLAPIENTRYP VertexAttribs4fvNV)(GLuint index, GLsizei n, const GLfloat *v);
   void (GLAPIENTRYP VertexAttribs4dvNV)(GLuint index, GLsizei n, const GLdouble *v);
   void (GLAPIENTRYP VertexAttribs4ubvNV)(GLuint index, GLsizei n, const GLubyte *v);
   void (GLAPIENTRYP VertexAttrib1s)(GLuint index, GLshort x);
   void (GLAPIENTRYP VertexAttrib1d)(GLuint index, GLdouble x);
   void (GLAPIENTRYP VertexAttrib2s)(GLuint index, GLshort x, GLshort y);
   void (GLAPIENTRYP VertexAttrib2d)(GLuint index, GLdouble x, GLdouble y);
   void (GLAPIENTRYP VertexAttrib3s)(GLuint index, GLshort x, GLshort y, GLshort z);
   void (GLAPIENTRYP VertexAttrib3d)(GLuint index, GLdouble x, GLdouble y, GLdouble z);
   void (GLAPIENTRYP VertexAttrib4s)(GLuint index, GLshort x, GLshort y, GLshort z,
                              GLshort w);
   void (GLAPIENTRYP VertexAttrib4d)(GLuint index, GLdouble x, GLdouble y, GLdouble z,
                              GLdouble w);
   void (GLAPIENTRYP VertexAttrib1sv)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib1dv)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib2sv)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib2dv)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib3sv)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib3dv)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib4sv)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttrib4dv)(GLuint index, const GLdouble *v);
   void (GLAPIENTRYP VertexAttrib4bv)(GLuint index, const GLbyte * v);
   void (GLAPIENTRYP VertexAttrib4iv)(GLuint index, const GLint * v);
   void (GLAPIENTRYP VertexAttrib4ubv)(GLuint index, const GLubyte * v);
   void (GLAPIENTRYP VertexAttrib4usv)(GLuint index, const GLushort * v);
   void (GLAPIENTRYP VertexAttrib4uiv)(GLuint index, const GLuint * v);
   void (GLAPIENTRYP VertexAttrib4Nbv)(GLuint index, const GLbyte * v);
   void (GLAPIENTRYP VertexAttrib4Nsv)(GLuint index, const GLshort * v);
   void (GLAPIENTRYP VertexAttrib4Niv)(GLuint index, const GLint * v);
   void (GLAPIENTRYP VertexAttrib4Nub)(GLuint index, GLubyte x, GLubyte y, GLubyte z,
                                GLubyte w);
   void (GLAPIENTRYP VertexAttrib4Nubv)(GLuint index, const GLubyte * v);
   void (GLAPIENTRYP VertexAttrib4Nusv)(GLuint index, const GLushort * v);
   void (GLAPIENTRYP VertexAttrib4Nuiv)(GLuint index, const GLuint * v);
   void (GLAPIENTRYP VertexAttribI1iv)(GLuint index, const GLint *v);
   void (GLAPIENTRYP VertexAttribI1uiv)(GLuint index, const GLuint *v);
   void (GLAPIENTRYP VertexAttribI4bv)(GLuint index, const GLbyte *v);
   void (GLAPIENTRYP VertexAttribI4sv)(GLuint index, const GLshort *v);
   void (GLAPIENTRYP VertexAttribI4ubv)(GLuint index, const GLubyte *v);
   void (GLAPIENTRYP VertexAttribI4usv)(GLuint index, const GLushort *v);
} GLvertexformat;


#endif /* DD_INCLUDED */

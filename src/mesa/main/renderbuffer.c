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


#include "glheader.h"

#include "context.h"
#include "bufferobj.h"
#include "fbobject.h"
#include "formats.h"
#include "glformats.h"
#include "mtypes.h"
#include "renderbuffer.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "state_tracker/st_format.h"
#include "state_tracker/st_cb_fbo.h"
#include "state_tracker/st_context.h"

/**
 * Delete a gl_framebuffer.
 * This is the default function for renderbuffer->Delete().
 * Drivers which subclass gl_renderbuffer should probably implement their
 * own delete function.  But the driver might also call this function to
 * free the object in the end.
 */
static void
delete_renderbuffer(struct gl_context *ctx, struct gl_renderbuffer *rb)
{
   if (ctx) {
      pipe_surface_release(ctx->pipe, &rb->surface_srgb);
      pipe_surface_release(ctx->pipe, &rb->surface_linear);
   } else {
      pipe_surface_release_no_context(&rb->surface_srgb);
      pipe_surface_release_no_context(&rb->surface_linear);
   }
   rb->surface = NULL;
   pipe_resource_reference(&rb->texture, NULL);
   free(rb->data);
   free(rb->Label);
   free(rb);
}

static GLboolean
renderbuffer_alloc_sw_storage(struct gl_context *ctx,
                              struct gl_renderbuffer *rb,
                              GLenum internalFormat,
                              GLuint width, GLuint height)
{
   struct st_context *st = st_context(ctx);
   enum pipe_format format;
   size_t size;

   free(rb->data);
   rb->data = NULL;

   if (internalFormat == GL_RGBA16_SNORM) {
      /* Special case for software accum buffers.  Otherwise, if the
       * call to st_choose_renderbuffer_format() fails (because the
       * driver doesn't support signed 16-bit/channel colors) we'd
       * just return without allocating the software accum buffer.
       */
      format = PIPE_FORMAT_R16G16B16A16_SNORM;
   }
   else {
      format = st_choose_renderbuffer_format(st, internalFormat, 0, 0);

      /* Not setting gl_renderbuffer::Format here will cause
       * FRAMEBUFFER_UNSUPPORTED and ValidateFramebuffer will not be called.
       */
      if (format == PIPE_FORMAT_NONE) {
         return GL_TRUE;
      }
   }

   rb->Format = st_pipe_format_to_mesa_format(format);

   size = _mesa_format_image_size(rb->Format, width, height, 1);
   rb->data = malloc(size);
   return rb->data != NULL;
}


/**
 * gl_renderbuffer::AllocStorage()
 * This is called to allocate the original drawing surface, and
 * during window resize.
 */
static GLboolean
renderbuffer_alloc_storage(struct gl_context * ctx,
                           struct gl_renderbuffer *rb,
                           GLenum internalFormat,
                           GLuint width, GLuint height)
{
   struct st_context *st = st_context(ctx);
   struct pipe_screen *screen = ctx->screen;
   enum pipe_format format = PIPE_FORMAT_NONE;
   struct pipe_resource templ;

   /* init renderbuffer fields */
   rb->Width  = width;
   rb->Height = height;
   rb->_BaseFormat = _mesa_base_fbo_format(ctx, internalFormat);
   rb->defined = GL_FALSE;  /* undefined contents now */

   if (rb->software) {
      return renderbuffer_alloc_sw_storage(ctx, rb, internalFormat,
                                           width, height);
   }

   /* Free the old surface and texture
    */
   pipe_surface_reference(&rb->surface_srgb, NULL);
   pipe_surface_reference(&rb->surface_linear, NULL);
   rb->surface = NULL;
   pipe_resource_reference(&rb->texture, NULL);

   /* If an sRGB framebuffer is unsupported, sRGB formats behave like linear
    * formats.
    */
   if (!ctx->Extensions.EXT_sRGB) {
      internalFormat = _mesa_get_linear_internalformat(internalFormat);
   }

   /* Handle multisample renderbuffers first.
    *
    * From ARB_framebuffer_object:
    *   If <samples> is zero, then RENDERBUFFER_SAMPLES is set to zero.
    *   Otherwise <samples> represents a request for a desired minimum
    *   number of samples. Since different implementations may support
    *   different sample counts for multisampled rendering, the actual
    *   number of samples allocated for the renderbuffer image is
    *   implementation dependent.  However, the resulting value for
    *   RENDERBUFFER_SAMPLES is guaranteed to be greater than or equal
    *   to <samples> and no more than the next larger sample count supported
    *   by the implementation.
    *
    * Find the supported number of samples >= rb->NumSamples
    */
   if (rb->NumSamples > 0) {
      unsigned start, start_storage;

      if (ctx->Const.MaxSamples > 1 &&  rb->NumSamples == 1) {
         /* don't try num_samples = 1 with drivers that support real msaa */
         start = 2;
         start_storage = 2;
      } else {
         start = rb->NumSamples;
         start_storage = rb->NumStorageSamples;
      }

      if (ctx->Extensions.AMD_framebuffer_multisample_advanced) {
         if (rb->_BaseFormat == GL_DEPTH_COMPONENT ||
             rb->_BaseFormat == GL_DEPTH_STENCIL ||
             rb->_BaseFormat == GL_STENCIL_INDEX) {
            /* Find a supported depth-stencil format. */
            for (unsigned samples = start;
                 samples <= ctx->Const.MaxDepthStencilFramebufferSamples;
                 samples++) {
               format = st_choose_renderbuffer_format(st, internalFormat,
                                                      samples, samples);

               if (format != PIPE_FORMAT_NONE) {
                  rb->NumSamples = samples;
                  rb->NumStorageSamples = samples;
                  break;
               }
            }
         } else {
            /* Find a supported color format, samples >= storage_samples. */
            for (unsigned storage_samples = start_storage;
                 storage_samples <= ctx->Const.MaxColorFramebufferStorageSamples;
                 storage_samples++) {
               for (unsigned samples = MAX2(start, storage_samples);
                    samples <= ctx->Const.MaxColorFramebufferSamples;
                    samples++) {
                  format = st_choose_renderbuffer_format(st, internalFormat,
                                                         samples,
                                                         storage_samples);

                  if (format != PIPE_FORMAT_NONE) {
                     rb->NumSamples = samples;
                     rb->NumStorageSamples = storage_samples;
                     goto found;
                  }
               }
            }
            found:;
         }
      } else {
         for (unsigned samples = start; samples <= ctx->Const.MaxSamples;
              samples++) {
            format = st_choose_renderbuffer_format(st, internalFormat,
                                                   samples, samples);

            if (format != PIPE_FORMAT_NONE) {
               rb->NumSamples = samples;
               rb->NumStorageSamples = samples;
               break;
            }
         }
      }
   } else {
      format = st_choose_renderbuffer_format(st, internalFormat, 0, 0);
   }

   /* Not setting gl_renderbuffer::Format here will cause
    * FRAMEBUFFER_UNSUPPORTED and ValidateFramebuffer will not be called.
    */
   if (format == PIPE_FORMAT_NONE) {
      return GL_TRUE;
   }

   rb->Format = st_pipe_format_to_mesa_format(format);

   if (width == 0 || height == 0) {
      /* if size is zero, nothing to allocate */
      return GL_TRUE;
   }

   /* Setup new texture template.
    */
   memset(&templ, 0, sizeof(templ));
   templ.target = st->internal_target;
   templ.format = format;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.nr_samples = rb->NumSamples;
   templ.nr_storage_samples = rb->NumStorageSamples;

   if (util_format_is_depth_or_stencil(format)) {
      templ.bind = PIPE_BIND_DEPTH_STENCIL;
   }
   else if (rb->Name != 0) {
      /* this is a user-created renderbuffer */
      templ.bind = PIPE_BIND_RENDER_TARGET;
   }
   else {
      /* this is a window-system buffer */
      templ.bind = (PIPE_BIND_DISPLAY_TARGET |
                    PIPE_BIND_RENDER_TARGET);
   }

   rb->texture = screen->resource_create(screen, &templ);

   if (!rb->texture)
      return FALSE;

   st_update_renderbuffer_surface(st, rb);
   return rb->surface != NULL;
}

/**
 * Initialize the fields of a gl_renderbuffer to default values.
 */
void
_mesa_init_renderbuffer(struct gl_renderbuffer *rb, GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);

   rb->ClassID = 0;
   rb->Name = name;
   rb->RefCount = 1;
   rb->Delete = delete_renderbuffer;

   /* The rest of these should be set later by the caller of this function or
    * the AllocStorage method:
    */
   rb->AllocStorage = NULL;

   rb->Width = 0;
   rb->Height = 0;
   rb->Depth = 0;

   /* In GL 3, the initial format is GL_RGBA according to Table 6.26
    * on page 302 of the GL 3.3 spec.
    *
    * In GLES 3, the initial format is GL_RGBA4 according to Table 6.15
    * on page 258 of the GLES 3.0.4 spec.
    *
    * If the context is current, set the initial format based on the
    * specs. If the context is not current, we cannot determine the
    * API, so default to GL_RGBA.
    */
   if (ctx && _mesa_is_gles(ctx)) {
      rb->InternalFormat = GL_RGBA4;
   } else {
      rb->InternalFormat = GL_RGBA;
   }

   rb->Format = MESA_FORMAT_NONE;

   rb->AllocStorage = renderbuffer_alloc_storage;
}

static void
validate_and_init_renderbuffer_attachment(struct gl_framebuffer *fb,
                                          gl_buffer_index bufferName,
                                          struct gl_renderbuffer *rb)
{
   assert(fb);
   assert(rb);
   assert(bufferName < BUFFER_COUNT);

   /* There should be no previous renderbuffer on this attachment point,
    * with the exception of depth/stencil since the same renderbuffer may
    * be used for both.
    */
   assert(bufferName == BUFFER_DEPTH ||
          bufferName == BUFFER_STENCIL ||
          fb->Attachment[bufferName].Renderbuffer == NULL);

   /* winsys vs. user-created buffer cross check */
   if (_mesa_is_user_fbo(fb)) {
      assert(rb->Name);
   }
   else {
      assert(!rb->Name);
   }

   fb->Attachment[bufferName].Type = GL_RENDERBUFFER_EXT;
   fb->Attachment[bufferName].Complete = GL_TRUE;
}


/**
 * Attach a renderbuffer to a framebuffer.
 * \param bufferName  one of the BUFFER_x tokens
 *
 * This function avoids adding a reference and is therefore intended to be
 * used with a freshly created renderbuffer.
 */
void
_mesa_attach_and_own_rb(struct gl_framebuffer *fb,
                        gl_buffer_index bufferName,
                        struct gl_renderbuffer *rb)
{
   assert(rb->RefCount == 1);

   validate_and_init_renderbuffer_attachment(fb, bufferName, rb);

   _mesa_reference_renderbuffer(&fb->Attachment[bufferName].Renderbuffer,
                                NULL);
   fb->Attachment[bufferName].Renderbuffer = rb;
}

/**
 * Attach a renderbuffer to a framebuffer.
 * \param bufferName  one of the BUFFER_x tokens
 */
void
_mesa_attach_and_reference_rb(struct gl_framebuffer *fb,
                              gl_buffer_index bufferName,
                              struct gl_renderbuffer *rb)
{
   validate_and_init_renderbuffer_attachment(fb, bufferName, rb);
   _mesa_reference_renderbuffer(&fb->Attachment[bufferName].Renderbuffer, rb);
}


/**
 * Remove the named renderbuffer from the given framebuffer.
 * \param bufferName  one of the BUFFER_x tokens
 */
void
_mesa_remove_renderbuffer(struct gl_framebuffer *fb,
                          gl_buffer_index bufferName)
{
   assert(bufferName < BUFFER_COUNT);
   _mesa_reference_renderbuffer(&fb->Attachment[bufferName].Renderbuffer,
                                NULL);
}


/**
 * Set *ptr to point to rb.  If *ptr points to another renderbuffer,
 * dereference that buffer first.  The new renderbuffer's refcount will
 * be incremented.  The old renderbuffer's refcount will be decremented.
 * This is normally only called from the _mesa_reference_renderbuffer() macro
 * when there's a real pointer change.
 */
void
_mesa_reference_renderbuffer_(struct gl_renderbuffer **ptr,
                              struct gl_renderbuffer *rb)
{
   if (*ptr) {
      /* Unreference the old renderbuffer */
      struct gl_renderbuffer *oldRb = *ptr;

      assert(oldRb->RefCount > 0);

      if (p_atomic_dec_zero(&oldRb->RefCount)) {
         GET_CURRENT_CONTEXT(ctx);
         oldRb->Delete(ctx, oldRb);
      }
   }

   if (rb) {
      /* reference new renderbuffer */
      p_atomic_inc(&rb->RefCount);
   }

   *ptr = rb;
}

void
_mesa_map_renderbuffer(struct gl_context *ctx,
                       struct gl_renderbuffer *rb,
                       GLuint x, GLuint y, GLuint w, GLuint h,
                       GLbitfield mode,
                       GLubyte **mapOut, GLint *rowStrideOut,
                       bool flip_y)
{
   struct pipe_context *pipe = ctx->pipe;
   const GLboolean invert = flip_y;
   GLuint y2;
   GLubyte *map;

   if (rb->software) {
      /* software-allocated renderbuffer (probably an accum buffer) */
      if (rb->data) {
         GLint bpp = _mesa_get_format_bytes(rb->Format);
         GLint stride = _mesa_format_row_stride(rb->Format,
                                                rb->Width);
         *mapOut = (GLubyte *) rb->data + y * stride + x * bpp;
         *rowStrideOut = stride;
      }
      else {
         *mapOut = NULL;
         *rowStrideOut = 0;
      }
      return;
   }

   /* Check for unexpected flags */
   assert((mode & ~(GL_MAP_READ_BIT |
                    GL_MAP_WRITE_BIT |
                    GL_MAP_INVALIDATE_RANGE_BIT)) == 0);

   const enum pipe_map_flags transfer_flags =
      _mesa_access_flags_to_transfer_flags(mode, false);

   /* Note: y=0=bottom of buffer while y2=0=top of buffer.
    * 'invert' will be true for window-system buffers and false for
    * user-allocated renderbuffers and textures.
    */
   if (invert)
      y2 = rb->Height - y - h;
   else
      y2 = y;

    map = pipe_texture_map(pipe,
                            rb->texture,
                            rb->surface->u.tex.level,
                            rb->surface->u.tex.first_layer,
                            transfer_flags, x, y2, w, h, &rb->transfer);
   if (map) {
      if (invert) {
         *rowStrideOut = -(int) rb->transfer->stride;
         map += (h - 1) * rb->transfer->stride;
      }
      else {
         *rowStrideOut = rb->transfer->stride;
      }
      *mapOut = map;
   }
   else {
      *mapOut = NULL;
      *rowStrideOut = 0;
   }
}

void
_mesa_unmap_renderbuffer(struct gl_context *ctx,
                         struct gl_renderbuffer *rb)
{
   struct pipe_context *pipe = ctx->pipe;

   if (rb->software) {
      /* software-allocated renderbuffer (probably an accum buffer) */
      return;
   }

   pipe_texture_unmap(pipe, rb->transfer);
   rb->transfer = NULL;
}

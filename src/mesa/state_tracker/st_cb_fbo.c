/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


/**
 * Framebuffer/renderbuffer functions.
 *
 * \author Brian Paul
 */



#include "main/context.h"
#include "main/bufferobj.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/glformats.h"
#include "main/macros.h"
#include "main/renderbuffer.h"
#include "main/state.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "st_atom.h"
#include "st_context.h"
#include "st_cb_fbo.h"
#include "st_cb_flush.h"
#include "st_cb_texture.h"
#include "st_format.h"
#include "st_texture.h"
#include "st_util.h"
#include "st_manager.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"
#include "util/u_memory.h"

/**
 * Allocate a renderbuffer for an on-screen window (not a user-created
 * renderbuffer).  The window system code determines the format.
 */
struct gl_renderbuffer *
st_new_renderbuffer_fb(enum pipe_format format, unsigned samples, boolean sw)
{
   struct gl_renderbuffer *rb;

   rb = CALLOC_STRUCT(gl_renderbuffer);
   if (!rb) {
      _mesa_error(NULL, GL_OUT_OF_MEMORY, "creating renderbuffer");
      return NULL;
   }

   _mesa_init_renderbuffer(rb, 0);
   rb->ClassID = 0x4242; /* just a unique value */
   rb->NumSamples = samples;
   rb->NumStorageSamples = samples;
   rb->Format = st_pipe_format_to_mesa_format(format);
   rb->_BaseFormat = _mesa_get_format_base_format(rb->Format);
   rb->software = sw;

   switch (format) {
   case PIPE_FORMAT_B10G10R10A2_UNORM:
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      rb->InternalFormat = GL_RGB10_A2;
      break;
   case PIPE_FORMAT_R10G10B10X2_UNORM:
   case PIPE_FORMAT_B10G10R10X2_UNORM:
      rb->InternalFormat = GL_RGB10;
      break;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_A8R8G8B8_UNORM:
      rb->InternalFormat = GL_RGBA8;
      break;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_X8R8G8B8_UNORM:
   case PIPE_FORMAT_R8G8B8_UNORM:
      rb->InternalFormat = GL_RGB8;
      break;
   case PIPE_FORMAT_R8G8B8A8_SRGB:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
   case PIPE_FORMAT_A8R8G8B8_SRGB:
      rb->InternalFormat = GL_SRGB8_ALPHA8;
      break;
   case PIPE_FORMAT_R8G8B8X8_SRGB:
   case PIPE_FORMAT_B8G8R8X8_SRGB:
   case PIPE_FORMAT_X8R8G8B8_SRGB:
      rb->InternalFormat = GL_SRGB8;
      break;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      rb->InternalFormat = GL_RGB5_A1;
      break;
   case PIPE_FORMAT_B4G4R4A4_UNORM:
      rb->InternalFormat = GL_RGBA4;
      break;
   case PIPE_FORMAT_B5G6R5_UNORM:
      rb->InternalFormat = GL_RGB565;
      break;
   case PIPE_FORMAT_Z16_UNORM:
      rb->InternalFormat = GL_DEPTH_COMPONENT16;
      break;
   case PIPE_FORMAT_Z32_UNORM:
      rb->InternalFormat = GL_DEPTH_COMPONENT32;
      break;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      rb->InternalFormat = GL_DEPTH24_STENCIL8_EXT;
      break;
   case PIPE_FORMAT_Z24X8_UNORM:
   case PIPE_FORMAT_X8Z24_UNORM:
      rb->InternalFormat = GL_DEPTH_COMPONENT24;
      break;
   case PIPE_FORMAT_S8_UINT:
      rb->InternalFormat = GL_STENCIL_INDEX8_EXT;
      break;
   case PIPE_FORMAT_R16G16B16A16_SNORM:
      /* accum buffer */
      rb->InternalFormat = GL_RGBA16_SNORM;
      break;
   case PIPE_FORMAT_R16G16B16A16_UNORM:
      rb->InternalFormat = GL_RGBA16;
      break;
   case PIPE_FORMAT_R16G16B16_UNORM:
      rb->InternalFormat = GL_RGB16;
      break;
   case PIPE_FORMAT_R8_UNORM:
      rb->InternalFormat = GL_R8;
      break;
   case PIPE_FORMAT_R8G8_UNORM:
      rb->InternalFormat = GL_RG8;
      break;
   case PIPE_FORMAT_R16_UNORM:
      rb->InternalFormat = GL_R16;
      break;
   case PIPE_FORMAT_R16G16_UNORM:
      rb->InternalFormat = GL_RG16;
      break;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      rb->InternalFormat = GL_RGBA32F;
      break;
   case PIPE_FORMAT_R32G32B32X32_FLOAT:
   case PIPE_FORMAT_R32G32B32_FLOAT:
      rb->InternalFormat = GL_RGB32F;
      break;
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      rb->InternalFormat = GL_RGBA16F;
      break;
   case PIPE_FORMAT_R16G16B16X16_FLOAT:
      rb->InternalFormat = GL_RGB16F;
      break;
   default:
      _mesa_problem(NULL,
                    "Unexpected format %s in st_new_renderbuffer_fb",
                    util_format_name(format));
      FREE(rb);
      return NULL;
   }

   rb->surface = NULL;

   return rb;
}

void
st_regen_renderbuffer_surface(struct st_context *st,
                              struct gl_renderbuffer *rb)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_resource *resource = rb->texture;

   struct pipe_surface **psurf =
      rb->surface_srgb ? &rb->surface_srgb : &rb->surface_linear;
   struct pipe_surface *surf = *psurf;
   /* create a new pipe_surface */
   struct pipe_surface surf_tmpl;
   memset(&surf_tmpl, 0, sizeof(surf_tmpl));
   surf_tmpl.format = surf->format;
   surf_tmpl.nr_samples = rb->rtt_nr_samples;
   surf_tmpl.u.tex.level = surf->u.tex.level;
   surf_tmpl.u.tex.first_layer = surf->u.tex.first_layer;
   surf_tmpl.u.tex.last_layer = surf->u.tex.last_layer;

   /* create -> destroy to avoid blowing up cached surfaces */
   surf = pipe->create_surface(pipe, resource, &surf_tmpl);
   pipe_surface_release(pipe, psurf);
   *psurf = surf;

   rb->surface = *psurf;
}

/**
 * Create or update the pipe_surface of a FBO renderbuffer.
 * This is usually called after st_finalize_texture.
 */
void
st_update_renderbuffer_surface(struct st_context *st,
                               struct gl_renderbuffer *rb)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_resource *resource = rb->texture;
   const struct gl_texture_object *stTexObj = NULL;
   unsigned rtt_width = rb->Width;
   unsigned rtt_height = rb->Height;
   unsigned rtt_depth = rb->Depth;

   /*
    * For winsys fbo, it is possible that the renderbuffer is sRGB-capable but
    * the format of rb->texture is linear (because we have no control over
    * the format).  Check rb->Format instead of rb->texture->format
    * to determine if the rb is sRGB-capable.
    */
   boolean enable_srgb = st->ctx->Color.sRGBEnabled &&
      _mesa_is_format_srgb(rb->Format);
   enum pipe_format format = resource->format;

   if (rb->is_rtt) {
      stTexObj = rb->TexImage->TexObject;
      if (stTexObj->surface_based)
         format = stTexObj->surface_format;
   }

   format = enable_srgb ? util_format_srgb(format) : util_format_linear(format);

   if (resource->target == PIPE_TEXTURE_1D_ARRAY) {
      rtt_depth = rtt_height;
      rtt_height = 1;
   }

   /* find matching mipmap level size */
   unsigned level;
   for (level = 0; level <= resource->last_level; level++) {
      if (u_minify(resource->width0, level) == rtt_width &&
          u_minify(resource->height0, level) == rtt_height &&
          (resource->target != PIPE_TEXTURE_3D ||
           u_minify(resource->depth0, level) == rtt_depth)) {
         break;
      }
   }
   assert(level <= resource->last_level);

   /* determine the layer bounds */
   unsigned first_layer, last_layer;
   if (rb->rtt_layered) {
      first_layer = 0;
      last_layer = util_max_layer(rb->texture, level);
   }
   else {
      first_layer =
      last_layer = rb->rtt_face + rb->rtt_slice;
   }

   /* Adjust for texture views */
   if (rb->is_rtt && resource->array_size > 1 &&
       stTexObj->Immutable) {
      const struct gl_texture_object *tex = stTexObj;
      first_layer += tex->Attrib.MinLayer;
      if (!rb->rtt_layered)
         last_layer += tex->Attrib.MinLayer;
      else
         last_layer = MIN2(first_layer + tex->Attrib.NumLayers - 1,
                           last_layer);
   }

   struct pipe_surface **psurf =
      enable_srgb ? &rb->surface_srgb : &rb->surface_linear;
   struct pipe_surface *surf = *psurf;

   if (!surf ||
       surf->texture->nr_samples != rb->NumSamples ||
       surf->texture->nr_storage_samples != rb->NumStorageSamples ||
       surf->format != format ||
       surf->texture != resource ||
       surf->width != rtt_width ||
       surf->height != rtt_height ||
       surf->nr_samples != rb->rtt_nr_samples ||
       surf->u.tex.level != level ||
       surf->u.tex.first_layer != first_layer ||
       surf->u.tex.last_layer != last_layer) {
      /* create a new pipe_surface */
      struct pipe_surface surf_tmpl;
      memset(&surf_tmpl, 0, sizeof(surf_tmpl));
      surf_tmpl.format = format;
      surf_tmpl.nr_samples = rb->rtt_nr_samples;
      surf_tmpl.u.tex.level = level;
      surf_tmpl.u.tex.first_layer = first_layer;
      surf_tmpl.u.tex.last_layer = last_layer;

      /* create -> destroy to avoid blowing up cached surfaces */
      struct pipe_surface *surf = pipe->create_surface(pipe, resource, &surf_tmpl);
      pipe_surface_release(pipe, psurf);
      *psurf = surf;
   }
   rb->surface = *psurf;
}

/**
 * Called via ctx->Driver.MapRenderbuffer.
 */
void
st_MapRenderbuffer(struct gl_context *ctx,
                   struct gl_renderbuffer *rb,
                   GLuint x, GLuint y, GLuint w, GLuint h,
                   GLbitfield mode,
                   GLubyte **mapOut, GLint *rowStrideOut,
                   bool flip_y)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;
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


/**
 * Called via ctx->Driver.UnmapRenderbuffer.
 */
void
st_UnmapRenderbuffer(struct gl_context *ctx,
                     struct gl_renderbuffer *rb)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;

   if (rb->software) {
      /* software-allocated renderbuffer (probably an accum buffer) */
      return;
   }

   pipe_texture_unmap(pipe, rb->transfer);
   rb->transfer = NULL;
}

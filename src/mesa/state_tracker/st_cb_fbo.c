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

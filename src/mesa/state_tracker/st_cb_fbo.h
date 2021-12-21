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


#ifndef ST_CB_FBO_H
#define ST_CB_FBO_H

#include "main/glheader.h"
#include "main/mtypes.h"
#include "main/fbobject.h"

#include "pipe/p_compiler.h"
#include "pipe/p_format.h"

struct dd_function_table;
struct pipe_context;

static inline struct pipe_resource *
st_get_renderbuffer_resource(struct gl_renderbuffer *rb)
{
   return rb->texture;
}

/**
 * Cast wrapper to convert a struct gl_framebuffer to an gl_framebuffer.
 * Return NULL if the struct gl_framebuffer is a user-created framebuffer.
 * We'll only return non-null for window system framebuffers.
 * Note that this function may fail.
 */
static inline struct gl_framebuffer *
st_ws_framebuffer(struct gl_framebuffer *fb)
{
   /* FBO cannot be casted.  See st_new_framebuffer */
   if (fb && _mesa_is_winsys_fbo(fb) &&
       fb != _mesa_get_incomplete_framebuffer())
      return fb;
   return NULL;
}


extern struct gl_renderbuffer *
st_new_renderbuffer_fb(enum pipe_format format, unsigned samples, boolean sw);

extern void
st_update_renderbuffer_surface(struct st_context *st,
                               struct gl_renderbuffer *strb);

extern void
st_regen_renderbuffer_surface(struct st_context *st,
                              struct gl_renderbuffer *strb);

GLboolean
st_renderbuffer_alloc_storage(struct gl_context * ctx,
                              struct gl_renderbuffer *rb,
                              GLenum internalFormat,
                              GLuint width, GLuint height);
void st_DrawBufferAllocate(struct gl_context *ctx);
void st_ReadBuffer(struct gl_context *ctx, GLenum buffer);

void st_MapRenderbuffer(struct gl_context *ctx,
                        struct gl_renderbuffer *rb,
                        GLuint x, GLuint y, GLuint w, GLuint h,
                        GLbitfield mode,
                        GLubyte **mapOut, GLint *rowStrideOut,
                        bool flip_y);
void st_UnmapRenderbuffer(struct gl_context *ctx,
                          struct gl_renderbuffer *rb);
#endif /* ST_CB_FBO_H */

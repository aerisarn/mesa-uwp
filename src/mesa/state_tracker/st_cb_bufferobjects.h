/**************************************************************************
 * 
 * Copyright 2005 VMware, Inc.
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

#ifndef ST_CB_BUFFEROBJECTS_H
#define ST_CB_BUFFEROBJECTS_H

#include "main/mtypes.h"

struct dd_function_table;
struct pipe_resource;
struct pipe_screen;
struct st_context;

enum pipe_map_flags
st_access_flags_to_transfer_flags(GLbitfield access, bool wholeBuffer);


extern void
st_init_bufferobject_functions(struct pipe_screen *screen,
                               struct dd_function_table *functions);

static inline struct pipe_resource *
st_get_buffer_reference(struct gl_context *ctx, struct gl_buffer_object *obj)
{
   if (unlikely(!obj))
      return NULL;

   struct pipe_resource *buffer = obj->buffer;

   if (unlikely(!buffer))
      return NULL;

   /* Only one context is using the fast path. All other contexts must use
    * the slow path.
    */
   if (unlikely(obj->private_refcount_ctx != ctx)) {
      p_atomic_inc(&buffer->reference.count);
      return buffer;
   }

   if (unlikely(obj->private_refcount <= 0)) {
      assert(obj->private_refcount == 0);

      /* This is the number of atomic increments we will skip. */
      obj->private_refcount = 100000000;
      p_atomic_add(&buffer->reference.count, obj->private_refcount);
   }

   /* Return a buffer reference while decrementing the private refcount. */
   obj->private_refcount--;
   return buffer;
}

struct gl_buffer_object *st_bufferobj_alloc(struct gl_context *ctx, GLuint name);
void st_bufferobj_free(struct gl_context *ctx, struct gl_buffer_object *obj);
void st_bufferobj_subdata(struct gl_context *ctx,
                          GLintptrARB offset,
                          GLsizeiptrARB size,
                          const void * data, struct gl_buffer_object *obj);
void st_bufferobj_get_subdata(struct gl_context *ctx,
                              GLintptrARB offset,
                              GLsizeiptrARB size,
                              void * data, struct gl_buffer_object *obj);
GLboolean st_bufferobj_data(struct gl_context *ctx,
                            GLenum target,
                            GLsizeiptrARB size,
                            const void *data,
                            GLenum usage,
                            GLbitfield storageFlags,
                            struct gl_buffer_object *obj);
GLboolean st_bufferobj_data_mem(struct gl_context *ctx,
                                GLenum target,
                                GLsizeiptrARB size,
                                struct gl_memory_object *memObj,
                                GLuint64 offset,
                                GLenum usage,
                                struct gl_buffer_object *bufObj);
void *st_bufferobj_map_range(struct gl_context *ctx,
                             GLintptr offset, GLsizeiptr length,
                             GLbitfield access,
                             struct gl_buffer_object *obj,
                             gl_map_buffer_index index);

void st_bufferobj_flush_mapped_range(struct gl_context *ctx,
                                     GLintptr offset, GLsizeiptr length,
                                     struct gl_buffer_object *obj,
                                     gl_map_buffer_index index);
GLboolean st_bufferobj_unmap(struct gl_context *ctx, struct gl_buffer_object *obj,
                             gl_map_buffer_index index);
void st_copy_buffer_subdata(struct gl_context *ctx,
                            struct gl_buffer_object *src,
                            struct gl_buffer_object *dst,
                            GLintptr readOffset, GLintptr writeOffset,
                            GLsizeiptr size);
void st_clear_buffer_subdata(struct gl_context *ctx,
                             GLintptr offset, GLsizeiptr size,
                             const void *clearValue,
                             GLsizeiptr clearValueSize,
                             struct gl_buffer_object *bufObj);
void st_bufferobj_page_commitment(struct gl_context *ctx,
                                  struct gl_buffer_object *bufferObj,
                                  GLintptr offset, GLsizeiptr size,
                                  GLboolean commit);

#endif

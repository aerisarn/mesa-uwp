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
 * Functions for pixel buffer objects and vertex/element buffer objects.
 */


#include <inttypes.h>  /* for PRId64 macro */

#include "main/errors.h"

#include "main/mtypes.h"
#include "main/arrayobj.h"
#include "main/bufferobj.h"

#include "st_context.h"
#include "st_cb_bufferobjects.h"
#include "st_cb_memoryobjects.h"
#include "st_debug.h"
#include "st_util.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"


/**
 * Called via glInvalidateBuffer(Sub)Data.
 */
static void
st_bufferobj_invalidate(struct gl_context *ctx,
                        struct gl_buffer_object *obj,
                        GLintptr offset,
                        GLsizeiptr size)
{
   struct pipe_context *pipe = ctx->pipe;

   /* We ignore partial invalidates. */
   if (offset != 0 || size != obj->Size)
      return;

   /* If the buffer is mapped, we can't invalidate it. */
   if (!obj->buffer || _mesa_bufferobj_mapped(obj, MAP_USER))
      return;

   pipe->invalidate_resource(pipe, obj->buffer);
}

void
st_init_bufferobject_functions(struct pipe_screen *screen,
                               struct dd_function_table *functions)
{
   if (screen->get_param(screen, PIPE_CAP_INVALIDATE_BUFFER))
      functions->InvalidateBufferSubData = st_bufferobj_invalidate;
}

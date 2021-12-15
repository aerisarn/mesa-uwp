/**************************************************************************
 * 
 * Copyright 2010 VMware, Inc.
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
 * IN NO EVENT SHALL THE AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/


/**
 * Transform feedback functions.
 *
 * \author Brian Paul
 *         Marek Olšák
 */


#include "main/bufferobj.h"
#include "main/context.h"
#include "main/transformfeedback.h"
#include "util/u_memory.h"

#include "st_cb_xformfb.h"
#include "st_context.h"

#include "pipe/p_context.h"
#include "util/u_draw.h"
#include "util/u_inlines.h"
#include "cso_cache/cso_context.h"



/* XXX Do we really need the mode? */
void
st_begin_transform_feedback(struct gl_context *ctx, GLenum mode,
                            struct gl_transform_feedback_object *obj)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = ctx->pipe;
   unsigned i, max_num_targets;
   unsigned offsets[PIPE_MAX_SO_BUFFERS] = {0};

   max_num_targets = MIN2(ARRAY_SIZE(obj->Buffers),
                          ARRAY_SIZE(obj->targets));

   /* Convert the transform feedback state into the gallium representation. */
   for (i = 0; i < max_num_targets; i++) {
      struct gl_buffer_object *bo = obj->Buffers[i];

      if (bo && bo->buffer) {
         unsigned stream = obj->program->sh.LinkedTransformFeedback->
            Buffers[i].Stream;

         /* Check whether we need to recreate the target. */
         if (!obj->targets[i] ||
             obj->targets[i] == obj->draw_count[stream] ||
             obj->targets[i]->buffer != bo->buffer ||
             obj->targets[i]->buffer_offset != obj->Offset[i] ||
             obj->targets[i]->buffer_size != obj->Size[i]) {
            /* Create a new target. */
            struct pipe_stream_output_target *so_target =
                  pipe->create_stream_output_target(pipe, bo->buffer,
                                                    obj->Offset[i],
                                                    obj->Size[i]);

            pipe_so_target_reference(&obj->targets[i], NULL);
            obj->targets[i] = so_target;
         }

         obj->num_targets = i+1;
      } else {
         pipe_so_target_reference(&obj->targets[i], NULL);
      }
   }

   /* Start writing at the beginning of each target. */
   cso_set_stream_outputs(st->cso_context, obj->num_targets,
                          obj->targets, offsets);
}


void
st_pause_transform_feedback(struct gl_context *ctx,
                            struct gl_transform_feedback_object *obj)
{
   struct st_context *st = st_context(ctx);
   cso_set_stream_outputs(st->cso_context, 0, NULL, NULL);
}


void
st_resume_transform_feedback(struct gl_context *ctx,
                             struct gl_transform_feedback_object *obj)
{
   struct st_context *st = st_context(ctx);
   unsigned offsets[PIPE_MAX_SO_BUFFERS];
   unsigned i;

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; i++)
      offsets[i] = (unsigned)-1;

   cso_set_stream_outputs(st->cso_context, obj->num_targets,
                          obj->targets, offsets);
}

void
st_end_transform_feedback(struct gl_context *ctx,
                          struct gl_transform_feedback_object *obj)
{
   struct st_context *st = st_context(ctx);
   unsigned i;

   cso_set_stream_outputs(st->cso_context, 0, NULL, NULL);

   /* The next call to glDrawTransformFeedbackStream should use the vertex
    * count from the last call to glEndTransformFeedback.
    * Therefore, save the targets for each stream.
    *
    * NULL means the vertex counter is 0 (initial state).
    */
   for (i = 0; i < ARRAY_SIZE(obj->draw_count); i++)
      pipe_so_target_reference(&obj->draw_count[i], NULL);

   for (i = 0; i < ARRAY_SIZE(obj->targets); i++) {
      unsigned stream = obj->program->sh.LinkedTransformFeedback->
         Buffers[i].Stream;

      /* Is it not bound or already set for this stream? */
      if (!obj->targets[i] || obj->draw_count[stream])
         continue;

      pipe_so_target_reference(&obj->draw_count[stream], obj->targets[i]);
   }
}


bool
st_transform_feedback_draw_init(struct gl_transform_feedback_object *obj,
                                unsigned stream,
                                struct pipe_draw_indirect_info *out)
{
   out->count_from_stream_output = obj->draw_count[stream];
   return out->count_from_stream_output != NULL;
}

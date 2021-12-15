/*
 * Copyright (C) 2013 Christoph Bumiller
 * Copyright (C) 2015 Samuel Pitoiset
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * Performance monitoring counters interface to gallium.
 */

#include "st_debug.h"
#include "st_context.h"
#include "st_cb_bitmap.h"
#include "st_cb_perfmon.h"
#include "st_util.h"

#include "util/bitset.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_memory.h"

#include "main/performance_monitor.h"

static bool
init_perf_monitor(struct gl_context *ctx, struct gl_perf_monitor_object *m)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;
   unsigned *batch = NULL;
   unsigned num_active_counters = 0;
   unsigned max_batch_counters = 0;
   unsigned num_batch_counters = 0;
   int gid, cid;

   st_flush_bitmap_cache(st);

   /* Determine the number of active counters. */
   for (gid = 0; gid < ctx->PerfMonitor.NumGroups; gid++) {
      const struct gl_perf_monitor_group *g = &ctx->PerfMonitor.Groups[gid];

      if (m->ActiveGroups[gid] > g->MaxActiveCounters) {
         /* Maximum number of counters reached. Cannot start the session. */
         if (ST_DEBUG & DEBUG_MESA) {
            debug_printf("Maximum number of counters reached. "
                         "Cannot start the session!\n");
         }
         return false;
      }

      num_active_counters += m->ActiveGroups[gid];
      if (g->has_batch)
         max_batch_counters += m->ActiveGroups[gid];
   }

   if (!num_active_counters)
      return true;

   m->active_counters = CALLOC(num_active_counters,
                                 sizeof(*m->active_counters));
   if (!m->active_counters)
      return false;

   if (max_batch_counters) {
      batch = CALLOC(max_batch_counters, sizeof(*batch));
      if (!batch)
         return false;
   }

   /* Create a query for each active counter. */
   for (gid = 0; gid < ctx->PerfMonitor.NumGroups; gid++) {
      const struct gl_perf_monitor_group *g = &ctx->PerfMonitor.Groups[gid];

      BITSET_FOREACH_SET(cid, m->ActiveCounters[gid], g->NumCounters) {
         const struct gl_perf_monitor_counter *c = &g->Counters[cid];
         struct gl_perf_counter_object *cntr =
            &m->active_counters[m->num_active_counters];

         cntr->id       = cid;
         cntr->group_id = gid;
         if (c->flags & PIPE_DRIVER_QUERY_FLAG_BATCH) {
            cntr->batch_index = num_batch_counters;
            batch[num_batch_counters++] = c->query_type;
         } else {
            cntr->query = pipe->create_query(pipe, c->query_type, 0);
            if (!cntr->query)
               goto fail;
         }
         ++m->num_active_counters;
      }
   }

   /* Create the batch query. */
   if (num_batch_counters) {
      m->batch_query = pipe->create_batch_query(pipe, num_batch_counters,
                                                  batch);
      m->batch_result = CALLOC(num_batch_counters, sizeof(m->batch_result->batch[0]));
      if (!m->batch_query || !m->batch_result)
         goto fail;
   }

   FREE(batch);
   return true;

fail:
   FREE(batch);
   return false;
}

static void
reset_perf_monitor(struct gl_perf_monitor_object *m,
                   struct pipe_context *pipe)
{
   unsigned i;

   for (i = 0; i < m->num_active_counters; ++i) {
      struct pipe_query *query = m->active_counters[i].query;
      if (query)
         pipe->destroy_query(pipe, query);
   }
   FREE(m->active_counters);
   m->active_counters = NULL;
   m->num_active_counters = 0;

   if (m->batch_query) {
      pipe->destroy_query(pipe, m->batch_query);
      m->batch_query = NULL;
   }
   FREE(m->batch_result);
   m->batch_result = NULL;
}

void
st_DeletePerfMonitor(struct gl_context *ctx, struct gl_perf_monitor_object *m)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;

   reset_perf_monitor(m, pipe);
   FREE(m);
}

GLboolean
st_BeginPerfMonitor(struct gl_context *ctx, struct gl_perf_monitor_object *m)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;
   unsigned i;

   if (!m->num_active_counters) {
      /* Create a query for each active counter before starting
       * a new monitoring session. */
      if (!init_perf_monitor(ctx, m))
         goto fail;
   }

   /* Start the query for each active counter. */
   for (i = 0; i < m->num_active_counters; ++i) {
      struct pipe_query *query = m->active_counters[i].query;
      if (query && !pipe->begin_query(pipe, query))
          goto fail;
   }

   if (m->batch_query && !pipe->begin_query(pipe, m->batch_query))
      goto fail;

   return true;

fail:
   /* Failed to start the monitoring session. */
   reset_perf_monitor(m, pipe);
   return false;
}

void
st_EndPerfMonitor(struct gl_context *ctx, struct gl_perf_monitor_object *m)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;
   unsigned i;

   /* Stop the query for each active counter. */
   for (i = 0; i < m->num_active_counters; ++i) {
      struct pipe_query *query = m->active_counters[i].query;
      if (query)
         pipe->end_query(pipe, query);
   }

   if (m->batch_query)
      pipe->end_query(pipe, m->batch_query);
}

void
st_ResetPerfMonitor(struct gl_context *ctx, struct gl_perf_monitor_object *m)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;

   if (!m->Ended)
      st_EndPerfMonitor(ctx, m);

   reset_perf_monitor(m, pipe);

   if (m->Active)
      st_BeginPerfMonitor(ctx, m);
}

GLboolean
st_IsPerfMonitorResultAvailable(struct gl_context *ctx,
                                struct gl_perf_monitor_object *m)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;
   unsigned i;

   if (!m->num_active_counters)
      return false;

   /* The result of a monitoring session is only available if the query of
    * each active counter is idle. */
   for (i = 0; i < m->num_active_counters; ++i) {
      struct pipe_query *query = m->active_counters[i].query;
      union pipe_query_result result;
      if (query && !pipe->get_query_result(pipe, query, FALSE, &result)) {
         /* The query is busy. */
         return false;
      }
   }

   if (m->batch_query &&
       !pipe->get_query_result(pipe, m->batch_query, FALSE, m->batch_result))
      return false;

   return true;
}

void
st_GetPerfMonitorResult(struct gl_context *ctx,
                        struct gl_perf_monitor_object *m,
                        GLsizei dataSize,
                        GLuint *data,
                        GLint *bytesWritten)
{
   struct pipe_context *pipe = st_context(ctx)->pipe;
   unsigned i;

   /* Copy data to the supplied array (data).
    *
    * The output data format is: <group ID, counter ID, value> for each
    * active counter. The API allows counters to appear in any order.
    */
   GLsizei offset = 0;
   bool have_batch_query = false;

   if (m->batch_query)
      have_batch_query = pipe->get_query_result(pipe, m->batch_query, TRUE,
                                                m->batch_result);

   /* Read query results for each active counter. */
   for (i = 0; i < m->num_active_counters; ++i) {
      struct gl_perf_counter_object *cntr = &m->active_counters[i];
      union pipe_query_result result = { 0 };
      int gid, cid;
      GLenum type;

      cid  = cntr->id;
      gid  = cntr->group_id;
      type = ctx->PerfMonitor.Groups[gid].Counters[cid].Type;

      if (cntr->query) {
         if (!pipe->get_query_result(pipe, cntr->query, TRUE, &result))
            continue;
      } else {
         if (!have_batch_query)
            continue;
         result.batch[0] = m->batch_result->batch[cntr->batch_index];
      }

      data[offset++] = gid;
      data[offset++] = cid;
      switch (type) {
      case GL_UNSIGNED_INT64_AMD:
         memcpy(&data[offset], &result.u64, sizeof(uint64_t));
         offset += sizeof(uint64_t) / sizeof(GLuint);
         break;
      case GL_UNSIGNED_INT:
         memcpy(&data[offset], &result.u32, sizeof(uint32_t));
         offset += sizeof(uint32_t) / sizeof(GLuint);
         break;
      case GL_FLOAT:
      case GL_PERCENTAGE_AMD:
         memcpy(&data[offset], &result.f, sizeof(GLfloat));
         offset += sizeof(GLfloat) / sizeof(GLuint);
         break;
      }
   }

   if (bytesWritten)
      *bytesWritten = offset * sizeof(GLuint);
}


bool
st_have_perfmon(struct st_context *st)
{
   struct pipe_screen *screen = st->screen;

   if (!screen->get_driver_query_info || !screen->get_driver_query_group_info)
      return false;

   return screen->get_driver_query_group_info(screen, 0, NULL) != 0;
}

void
st_destroy_perfmon(struct st_context *st)
{
   _mesa_free_perfomance_monitor_groups(st->ctx);
}

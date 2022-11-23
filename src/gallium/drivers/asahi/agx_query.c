/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_state.h"

static struct pipe_query *
agx_create_query(struct pipe_context *ctx, unsigned query_type, unsigned index)
{
   struct agx_query *query = calloc(1, sizeof(struct agx_query));

   return (struct pipe_query *)query;
}

static void
agx_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
   free(query);
}

static bool
agx_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
agx_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
agx_get_query_result(struct pipe_context *ctx,
                     struct pipe_query *query,
                     bool wait,
                     union pipe_query_result *vresult)
{
   uint64_t *result = (uint64_t*)vresult;

   *result = 0;
   return true;
}

static void
agx_set_active_query_state(struct pipe_context *pipe, bool enable)
{
}

void
agx_init_query_functions(struct pipe_context *pctx)
{
   pctx->create_query = agx_create_query;
   pctx->destroy_query = agx_destroy_query;
   pctx->begin_query = agx_begin_query;
   pctx->end_query = agx_end_query;
   pctx->get_query_result = agx_get_query_result;
   pctx->set_active_query_state = agx_set_active_query_state;
}

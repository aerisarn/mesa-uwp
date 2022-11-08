/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_state.h"

void
agx_flush_readers(struct agx_context *ctx, struct agx_resource *rsrc, const char *reason)
{
   /* TODO: Turn into loop when we support multiple batches */
   if (ctx->batch) {
      struct agx_batch *batch = ctx->batch;

      if (agx_batch_uses_bo(batch, rsrc->bo))
         agx_flush_batch(ctx, batch);
   }
}

void
agx_flush_writer(struct agx_context *ctx, struct agx_resource *rsrc, const char *reason)
{
   struct hash_entry *ent = _mesa_hash_table_search(ctx->writer, rsrc);

   if (ent)
      agx_flush_batch(ctx, ent->data);
}

void
agx_batch_reads(struct agx_batch *batch, struct agx_resource *rsrc)
{
   agx_batch_add_bo(batch, rsrc->bo);

   if (rsrc->separate_stencil)
      agx_batch_add_bo(batch, rsrc->separate_stencil->bo);
}

void
agx_batch_writes(struct agx_batch *batch, struct agx_resource *rsrc)
{
   struct agx_context *ctx = batch->ctx;
   struct hash_entry *ent = _mesa_hash_table_search(ctx->writer, rsrc);

   /* Nothing to do if we're already writing */
   if (ent && ent->data == batch)
      return;

   /* Flush the old writer if there is one */
   agx_flush_writer(ctx, rsrc, "Multiple writers");

   /* Write is strictly stronger than a read */
   agx_batch_reads(batch, rsrc);

   /* We are now the new writer */
   assert(!_mesa_hash_table_search(ctx->writer, rsrc));
   _mesa_hash_table_insert(ctx->writer, rsrc, batch);
}

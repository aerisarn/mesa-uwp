#ifndef ZINK_INLINES_H
#define ZINK_INLINES_H

/* these go here to avoid include hell */
static inline void
zink_select_draw_vbo(struct zink_context *ctx)
{
   ctx->base.draw_vbo = ctx->draw_vbo[ctx->multidraw];
   assert(ctx->base.draw_vbo);
}

#endif

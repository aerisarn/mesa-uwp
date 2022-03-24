#define pipe_resource_reference __pipe_resource_reference_wraped
#define util_format_pack_rgba __util_format_pack_rgba
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#undef pipe_resource_reference
#undef util_format_pack_rgba

void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src);
void util_format_pack_rgba(enum pipe_format format, void *dst, const void *src, unsigned w);

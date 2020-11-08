#define pipe_resource_reference __pipe_resource_reference_wraped
#include "util/u_inlines.h"
#undef pipe_resource_reference

void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src);

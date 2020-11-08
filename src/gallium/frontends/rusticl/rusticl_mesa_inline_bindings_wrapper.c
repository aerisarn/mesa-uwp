#include "rusticl_mesa_inline_bindings_wrapper.h"

void
pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src)
{
   __pipe_resource_reference_wraped(dst, src);
}

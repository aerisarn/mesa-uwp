
#ifndef U_TRANSFER_H
#define U_TRANSFER_H

/* Fallback implementations for inline read/writes which just go back
 * to the regular transfer behaviour.
 */
#include "pipe/p_state.h"

struct pipe_context;
struct winsys_handle;

#ifdef __cplusplus
extern "C" {
#endif

void u_default_buffer_subdata(struct pipe_context *pipe,
                              struct pipe_resource *resource,
                              unsigned usage, unsigned offset,
                              unsigned size, const void *data);

void u_default_texture_subdata(struct pipe_context *pipe,
                               struct pipe_resource *resource,
                               unsigned level,
                               unsigned usage,
                               const struct pipe_box *box,
                               const void *data,
                               unsigned stride,
                               unsigned layer_stride);

void u_default_transfer_flush_region( struct pipe_context *pipe,
                                      struct pipe_transfer *transfer,
                                      const struct pipe_box *box);

/* Useful helper to allow >1 implementation of resource functionality
 * to exist in a single driver.  This is intended to be transitionary!
 */
struct u_resource_vtbl {
};


struct u_resource {
   struct pipe_resource b;
   const struct u_resource_vtbl *vtbl;
};

#ifdef __cplusplus
} // extern "C" {
#endif

#endif

#ifndef NIL_FORMAT_H
#define NIL_FORMAT_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"

struct nouveau_ws_device;

/* We don't have our own format enum; we use PIPE_FORMAT for everything */

bool nil_format_supports_color_targets(struct nouveau_ws_device *dev,
                                       enum pipe_format format);

uint8_t nil_format_to_color_target(enum pipe_format format);

uint8_t nil_format_to_depth_stencil(enum pipe_format format);

struct nil_tic_format {
   unsigned comp_sizes:8;
   unsigned type_r:3;
   unsigned type_g:3;
   unsigned type_b:3;
   unsigned type_a:3;
   unsigned src_x:3;
   unsigned src_y:3;
   unsigned src_z:3;
   unsigned src_w:3;
};

const struct nil_tic_format *
nil_tic_format_for_pipe(enum pipe_format format);

#endif /* NIL_FORMAT_H */

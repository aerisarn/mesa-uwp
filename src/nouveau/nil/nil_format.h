#ifndef NIL_FORMAT_H
#define NIL_FORMAT_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"

/* We don't have our own format enum; we use PIPE_FORMAT for everything */

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

extern const struct nil_tic_format nil_tic_formats[];

static inline const struct nil_tic_format *
nil_tic_format_for_pipe(enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_tic_format *fmt = &nil_tic_formats[format];
   return fmt->comp_sizes == 0 ? NULL : fmt;
}

#endif /* NIL_FORMAT_H */

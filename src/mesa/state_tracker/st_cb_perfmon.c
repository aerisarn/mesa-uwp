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

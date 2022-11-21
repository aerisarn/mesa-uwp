/**************************************************************************
 * 
 * Copyright 2003 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/


#include <stdio.h>
#include "main/arrayobj.h"
#include "util/glheader.h"
#include "main/context.h"

#include "pipe/p_defines.h"
#include "st_context.h"
#include "st_atom.h"
#include "st_program.h"
#include "st_manager.h"
#include "st_util.h"

#include "util/u_cpu_detect.h"


/* The list state update functions. */
st_update_func_t st_update_functions[ST_NUM_ATOMS];

static void
init_atoms_once(void)
{
#define ST_STATE(FLAG, st_update) st_update_functions[FLAG##_INDEX] = st_update;
#include "st_atom_list.h"
#undef ST_STATE

   if (util_get_cpu_caps()->has_popcnt)
      st_update_functions[ST_NEW_VERTEX_ARRAYS_INDEX] = st_update_array_with_popcnt;
}

void st_init_atoms( struct st_context *st )
{
   STATIC_ASSERT(ARRAY_SIZE(st_update_functions) <= 64);

   static once_flag flag = ONCE_FLAG_INIT;
   call_once(&flag, init_atoms_once);
}

void st_destroy_atoms( struct st_context *st )
{
   /* no-op */
}

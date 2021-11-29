/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2004  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keithw@vmware.com> Gareth Hughes
 */

#include "glheader.h"
#include "api_arrayelt.h"
#include "context.h"

#include "mtypes.h"
#include "vtxfmt.h"
#include "eval.h"
#include "dlist.h"
#include "main/dispatch.h"
#include "vbo/vbo.h"


/**
 * Copy the functions found in the GLvertexformat object into the
 * dispatch table.
 */
static void
install_vtxfmt(struct gl_context *ctx, struct _glapi_table *tab,
               const GLvertexformat *vfmt)
{
   assert(ctx->Version > 0);

#include "api_vtxfmt_init.h"
}


/**
 * Install per-vertex functions into the API dispatch table for execution.
 */
void
_mesa_install_exec_vtxfmt(struct gl_context *ctx, const GLvertexformat *vfmt)
{
   install_vtxfmt(ctx, ctx->Exec, vfmt);
   if (ctx->BeginEnd)
      install_vtxfmt(ctx, ctx->BeginEnd, vfmt);
}


/**
 * Install per-vertex functions into the API dispatch table for display
 * list compilation.
 */
void
_mesa_install_save_vtxfmt(struct gl_context *ctx, const GLvertexformat *vfmt)
{
   if (_mesa_is_desktop_gl(ctx))
      install_vtxfmt(ctx, ctx->Save, vfmt);
}

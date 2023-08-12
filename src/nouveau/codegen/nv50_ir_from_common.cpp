/*
 * Copyright 2011 Christoph Bumiller
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

#include "nv50_ir_from_common.h"

namespace nv50_ir {

ConverterCommon::ConverterCommon(Program *prog, nv50_ir_prog_info *info,
                                 nv50_ir_prog_info_out *info_out)
   :  BuildUtil(prog),
      info(info),
      info_out(info_out) {}

uint8_t
ConverterCommon::translateInterpMode(const struct nv50_ir_varying *var, operation& op)
{
   uint8_t mode = NV50_IR_INTERP_PERSPECTIVE;

   if (var->flat)
      mode = NV50_IR_INTERP_FLAT;
   else
   if (var->linear)
      mode = NV50_IR_INTERP_LINEAR;
   else
   if (var->sc)
      mode = NV50_IR_INTERP_SC;

   op = (mode == NV50_IR_INTERP_PERSPECTIVE || mode == NV50_IR_INTERP_SC)
      ? OP_PINTERP : OP_LINTERP;

   if (var->centroid)
      mode |= NV50_IR_INTERP_CENTROID;

   return mode;
}

} // namespace nv50_ir

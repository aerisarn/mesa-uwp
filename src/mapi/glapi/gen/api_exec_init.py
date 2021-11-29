
# Copyright (C) 2012 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

# This script generates the file api_exec_init.c, which contains
# _mesa_initialize_exec_table().  It is responsible for populating all
# entries in the "exec" dispatch table that aren't dynamic.

import argparse
import collections
import license
import gl_XML
import sys
import apiexec


exec_flavor_map = {
    'vtxfmt': None,
    'dlist': '_mesa_',
    'mesa': '_mesa_',
    'skip': None,
    }


header = """/**
 * \\file api_exec_init.c
 * Initialize dispatch table.
 */


#include "main/accum.h"
#include "main/api_exec.h"
#include "main/arbprogram.h"
#include "main/atifragshader.h"
#include "main/attrib.h"
#include "main/blend.h"
#include "main/blit.h"
#include "main/bufferobj.h"
#include "main/arrayobj.h"
#include "main/bbox.h"
#include "main/buffers.h"
#include "main/clear.h"
#include "main/clip.h"
#include "main/colortab.h"
#include "main/compute.h"
#include "main/condrender.h"
#include "main/conservativeraster.h"
#include "main/context.h"
#include "main/convolve.h"
#include "main/copyimage.h"
#include "main/depth.h"
#include "main/debug_output.h"
#include "main/dlist.h"
#include "main/draw.h"
#include "main/drawpix.h"
#include "main/drawtex.h"
#include "main/rastpos.h"
#include "main/enable.h"
#include "main/errors.h"
#include "main/es1_conversion.h"
#include "main/eval.h"
#include "main/externalobjects.h"
#include "main/get.h"
#include "main/glspirv.h"
#include "main/feedback.h"
#include "main/fog.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/genmipmap.h"
#include "main/hint.h"
#include "main/histogram.h"
#include "main/light.h"
#include "main/lines.h"
#include "main/matrix.h"
#include "main/multisample.h"
#include "main/objectlabel.h"
#include "main/objectpurge.h"
#include "main/performance_monitor.h"
#include "main/performance_query.h"
#include "main/pipelineobj.h"
#include "main/pixel.h"
#include "main/pixelstore.h"
#include "main/points.h"
#include "main/polygon.h"
#include "main/program_resource.h"
#include "main/querymatrix.h"
#include "main/queryobj.h"
#include "main/readpix.h"
#include "main/samplerobj.h"
#include "main/scissor.h"
#include "main/stencil.h"
#include "main/texenv.h"
#include "main/texgetimage.h"
#include "main/teximage.h"
#include "main/texgen.h"
#include "main/texobj.h"
#include "main/texparam.h"
#include "main/texstate.h"
#include "main/texstorage.h"
#include "main/barrier.h"
#include "main/texturebindless.h"
#include "main/textureview.h"
#include "main/transformfeedback.h"
#include "main/mtypes.h"
#include "main/varray.h"
#include "main/viewport.h"
#include "main/shaderapi.h"
#include "main/shaderimage.h"
#include "main/uniforms.h"
#include "main/syncobj.h"
#include "main/formatquery.h"
#include "main/dispatch.h"
#include "main/vdpau.h"


/**
 * Initialize a context's exec table with pointers to Mesa's supported
 * GL functions.
 *
 * This function depends on ctx->Version.
 *
 * \param ctx  GL context to which \c exec belongs.
 */
void
_mesa_initialize_exec_table(struct gl_context *ctx)
{
   struct _glapi_table *exec;

   exec = ctx->Exec;
   assert(exec != NULL);

   assert(ctx->Version > 0);
"""


footer = """
}
"""


class PrintCode(gl_XML.gl_print_base):

    def __init__(self):
        gl_XML.gl_print_base.__init__(self)

        self.name = 'api_exec_init.py'
        self.license = license.bsd_license_template % (
            'Copyright (C) 2012 Intel Corporation',
            'Intel Corporation')

    def printRealHeader(self):
        print(header)

    def printRealFooter(self):
        print(footer)

    def printBody(self, api):
        # Collect SET_* calls by the condition under which they should
        # be called.
        settings_by_condition = collections.defaultdict(lambda: [])
        for f in api.functionIterateAll():
            if f.exec_flavor not in exec_flavor_map:
                raise Exception(
                    'Unrecognized exec flavor {0!r}'.format(f.exec_flavor))
            condition = apiexec.get_api_condition(f)
            if not condition:
                continue
            prefix = exec_flavor_map[f.exec_flavor]
            if prefix is None:
                # This function is not implemented, or is dispatched
                # via vtxfmt.
                continue
            if f.has_no_error_variant:
                no_error_condition = '_mesa_is_no_error_enabled(ctx) && ({0})'.format(condition)
                error_condition = '!_mesa_is_no_error_enabled(ctx) && ({0})'.format(condition)
                settings_by_condition[no_error_condition].append(
                    'SET_{0}(exec, {1}{0}_no_error);'.format(f.name, prefix, f.name))
                settings_by_condition[error_condition].append(
                    'SET_{0}(exec, {1}{0});'.format(f.name, prefix, f.name))
            else:
                settings_by_condition[condition].append(
                    'SET_{0}(exec, {1}{0});'.format(f.name, prefix, f.name))
        # Print out an if statement for each unique condition, with
        # the SET_* calls nested inside it.
        for condition in sorted(settings_by_condition.keys()):
            print('   if ({0}) {{'.format(condition))
            for setting in sorted(settings_by_condition[condition]):
                print('      {0}'.format(setting))
            print('   }')


if __name__ == '__main__':
    apiexec.print_glapi_file(PrintCode())

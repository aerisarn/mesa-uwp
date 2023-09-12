# Copyright © 2013 Intel Corporation
# SPDX-License-Identifier: MIT

import sys

from builtin_types import BUILTIN_TYPES
from mako.template import Template

template = """\
/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/* This is an automatically generated file. */

#include "glsl_types.h"
#include "util/glheader.h"

%for t in BUILTIN_TYPES:
const struct glsl_type glsl_type_builtin_${t["name"]} = {
   %for k, v in t.items():
       %if v is None:
          <% continue %>
       %elif k == "name":
          .${k} = "${v}",
       %else:
          .${k} = ${v},
       %endif
   %endfor
};

%endfor"""

if len(sys.argv) < 2:
    print('Missing output argument', file=sys.stderr)
    sys.exit(1)

output = sys.argv[1]

with open(output, 'w') as f:
    f.write(Template(template).render(BUILTIN_TYPES=BUILTIN_TYPES))

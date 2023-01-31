# Copyright © 2022 Collabora, Ltd.

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

mme_isa_depend_files = [
  'mme_tu104.xml',
  isaspec_py_deps
]

mme_encode_h = custom_target(
  'mme-tu104-encode.h',
  input: ['mme_tu104.xml'],
  output: 'mme_tu104_encode.h',
  command: [
    prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'
  ],
  depend_files: mme_isa_depend_files,
)

libnouveau_mme_files = files(
  'mme_builder.h',
  'mme_tu104.c',
  'mme_tu104.h',
  'mme_tu104_builder.c',
  'mme_tu104_sim.c',
  'mme_tu104_sim.h',
)

_libnouveau_mme = static_library(
  'nouveau_mme',
  [libnouveau_mme_files, mme_encode_h],
  include_directories : [inc_include, inc_src],
  gnu_symbol_visibility : 'hidden',
  dependencies : [
    idep_mesautil,
    idep_nvidia_headers,
  ],
)

idep_nouveau_mme = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnouveau_mme,
)

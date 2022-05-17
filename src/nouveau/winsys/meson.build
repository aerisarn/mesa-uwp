libnouveau_ws = static_library(
  'nouveau_ws',
  [
    'nouveau_device.h',
    'nouveau_device.c',
    'nouveau_private.h',
  ],
  include_directories : [
    inc_include,
    inc_src,
  ],
  c_args: [ '-Wno-gnu-variable-sized-type-not-at-end' ],
  dependencies : [
    dep_libdrm,
    dep_libdrm_nouveau,
  ],
  gnu_symbol_visibility : 'hidden',
)

idep_nouveau_ws = declare_dependency(
  link_with: libnouveau_ws,
  include_directories : include_directories('.')
)

libnak_c_files = files(
  'nak.h',
)

libnak_deps = [
  idep_mesautil,
  idep_nir_headers,
]

_libnak = static_library(
  'nak',
  libnak_c_files,
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium],
  dependencies : libnak_deps,
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
)

idep_nak = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnak,
)

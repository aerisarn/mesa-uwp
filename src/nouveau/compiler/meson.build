add_languages('rust', required: true)
rust = import('unstable-rust')

libnak_c_files = files(
  'nak.h',
  'nak_nir.c',
)

libnak_rs_files = files(
  'nak.rs',
)

libnak_deps = [
  idep_mesautil,
  idep_nir_headers,
]

nak_bindings_rs = rust.bindgen(
  input : ['nak.h'],
  output : 'nak_bindings.rs',
  args : [
    '--allowlist-type', 'nak_.*',
    '--allowlist-function', 'nak_.*',
    '--allowlist-type', 'nir_shader',
  ],
)

libnak_bindings_gen = static_library(
  'nak_bindings',
  nak_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
)

_libnak_rs = static_library(
  'nak_rs',
  [libnak_rs_files],
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'staticlib',
  rust_args : [
    # we error on all clippy warnings unless they are disabled
    '-Dclippy::all',
    # we want to add asserts in control flow
    '-Aclippy::assertions_on_constants',
    # dunno, kind of looks nicier being explicit
    '-Aclippy::redundant_field_names',
    '-Aclippy::too_many_arguments',
    '-Aclippy::type_complexity',
    '-Anon_snake_case',
  ],
  link_with: [libnak_bindings_gen],
)

_libnak = static_library(
  'nak',
  libnak_c_files,
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium],
  dependencies : libnak_deps,
  link_with : [_libnak_rs],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
)

idep_nak = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnak,
)

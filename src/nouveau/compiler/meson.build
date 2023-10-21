# Copyright © 2022 Collabora, Ltd.
# SPDX-License-Identifier: MIT
if meson.version().version_compare('< 1.3.0')
  error('NVK requires meson 1.3.0 or newer')
endif

add_languages('rust', required: true)
rust = import('rust')

rc = meson.get_compiler('rust')
if rc.version().version_compare('< 1.73.0')
  error('NAK requires Rust 1.73.0')
endif

dep_syn = dependency('syn',
  version : '>= 2.0.15',
  fallback : ['syn', 'dep_syn'],
  required : true,
)

libnak_c_files = files(
  'nak.h',
  'nak_nir.c',
  'nak_nir_add_barriers.c',
  'nak_nir_lower_tex.c',
  'nak_nir_lower_vtg_io.c',
  'nak_nir_lower_gs_intrinsics.c',
)

libnak_rs_files = files(
  'nak.rs',
)

libnak_deps = [
  idep_mesautil,
  idep_nir_headers,
  idep_nvidia_headers,
]

nak_bindings_rs = rust.bindgen(
  input : ['nak_bindings.h'],
  output : 'nak_bindings.rs',
  c_args : [
    pre_args,
  ],
  args : [
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--allowlist-type', 'exec_list',
    '--allowlist-type', 'exec_node',
    '--allowlist-type', 'gl_access_qualifier',
    '--allowlist-type', 'gl_frag_result',
    '--allowlist-type', 'gl_interp_mode',
    '--allowlist-type', 'gl_shader_stage',
    '--allowlist-type', 'gl_system_value',
    '--allowlist-type', 'gl_tess_spacing',
    '--allowlist-type', 'gl_varying_slot',
    '--allowlist-type', 'gl_vert_attrib',
    '--allowlist-type', 'nak_.*',
    '--allowlist-type', 'nir_.*',
    '--allowlist-type', 'mesa_scope',
    '--allowlist-type', 'mesa_prim',
    '--allowlist-type', 'tess_primitive_mode',
    '--allowlist-var', 'nir_.*_infos',
    '--allowlist-function', '_mesa_shader_stage_to_string',
    '--allowlist-function', 'nak_.*',
    '--allowlist-function', 'nir_.*',
    '--allowlist-function', 'glsl_.*',
    '--no-prepend-enum-name',
  ],
  dependencies : libnak_deps,
)

libnak_bindings_gen = static_library(
  'nak_bindings',
  nak_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
)

libnak_ir_proc = rust.proc_macro(
  'nak_ir_proc',
  files('nak_ir_proc.rs'),
  dependencies : [dep_syn],
)

_libnak_rs = static_library(
  'nak_rs',
  [libnak_rs_files],
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'c',
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
  link_with: [libnak_bindings_gen, libnak_ir_proc],
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

if with_tools.contains('nouveau')
  executable(
    'nvfuzz',
    files('nvfuzz.rs'),
    rust_crate_type : 'bin',
    install : true
  )
endif

idep_nak = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnak,
)

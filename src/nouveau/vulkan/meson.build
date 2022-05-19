nvk_files = files(
  'nvk_device.c',
  'nvk_device.h',
  'nvk_instance.c',
  'nvk_instance.h',
  'nvk_physical_device.c',
  'nvk_physical_device.h',
  'nvk_private.h',
)

nouveau_icd = custom_target(
  'nouveau_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'nouveau_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.3', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_nouveau.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

nvk_entrypoints = custom_target(
  'nvk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['nvk_entrypoints.h', 'nvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'nvk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

nvk_deps = [
  dep_libdrm,
  idep_nouveau_ws,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
]

libvulkan_nouveau = shared_library(
  'vulkan_nouveau',
  [
    nvk_entrypoints,
    nvk_files,
  ],
  include_directories : [
    inc_include,
    inc_src,
  ],
  dependencies : nvk_deps,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

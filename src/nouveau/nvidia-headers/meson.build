idep_nvidia_headers = declare_dependency(
  include_directories : include_directories('.', 'classes')
)

nvk_classes = [
  'cl902d',
  'cl906f',
  'cl9097',
  'cl90b5',
  'cla0b5',
  'cla097',
  'clb097',
  'clb197',
  'clc097',
  'clc1b5',
  'cla0c0',
  'clc0c0',
  'clc397',
  'clc3c0',
  'clc597',
]

nvk_cl_header_depend_files = [
  files('class_parser.py')
]

cl_generated = []
foreach cl : nvk_classes
  cl_generated += custom_target(
    cl + '.h',
    input : ['class_parser.py', 'classes/'+cl+'.h'],
    output : 'nvk_'+cl+'.h',
    command : [prog_python, '@INPUT0@', '--in_h', '@INPUT1@', '--out_h', '@OUTPUT0@'],
    depend_files: nvk_cl_header_depend_files,
  )
endforeach

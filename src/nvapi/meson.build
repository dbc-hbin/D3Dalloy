nvapi_src = [
  'nvapi.cpp',
]

nvapi_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
nvapi_link_depends = []

nvapi_name = cpu_family == 'x86' ? 'nvapi' : 'nvapi64'
nvapi_dll = shared_library(nvapi_name, nvapi_src,
  name_prefix         : '',
  dependencies        : [ dxgi_dep, util_dep, winemetal_dep ],
  include_directories : [ d3dalloy_include_path, include_directories('../../external/nvapi') ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : cpu_family == 'x86' ? 'nvapi.def' : 'nvapi64.def',
  link_args           : nvapi_ld_args,
  link_depends        : [ nvapi_link_depends ],
  install_tag         : 'nvext',
)

if wine_builtin_dll
custom_target('postprocess_lib' + nvapi_name,
  input : nvapi_dll,
  output: nvapi_name + '.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : nvapi_dll,
  build_by_default : true
)
endif

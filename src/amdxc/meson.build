# Anti-Lag 2 SDK discovers this already-loaded driver facade by basename.
# It shares the D3D12 scheduler; it is not an AMD rendering driver.
amdxc_dll = shared_library('amdxc64', 'amdxc.cpp',
  name_prefix         : '',
  dependencies        : [ util_dep ],
  include_directories : [ d3dalloy_include_path, include_directories('../d3d12') ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'amdxc.def',
)

if wine_builtin_dll
custom_target('postprocess_libamdxc64',
  input : amdxc_dll,
  output: 'amdxc64.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : amdxc_dll,
  build_by_default : true
)
endif

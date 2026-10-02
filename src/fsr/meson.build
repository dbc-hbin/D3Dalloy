fsr_dll = shared_library('amd_fidelityfx_upscaler_dx12', 'fsr.cpp',
  name_prefix         : '',
  dependencies        : [ util_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_builtin_install_dir,
  vs_module_defs      : 'fsr.def',
  install_tag         : 'nvext',
)

custom_target('postprocess_libfsr',
  input : fsr_dll,
  output: 'amd_fidelityfx_upscaler_dx12.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : fsr_dll,
  build_by_default : true
)

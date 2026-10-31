fsrrr_dll = shared_library('amd_fidelityfx_denoiser_dx12', 'fsrrr.c', 'fsrrr_backend.cpp',
  name_prefix         : '',
  dependencies        : [ util_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_builtin_install_dir,
  vs_module_defs      : 'fsrrr.def',
  install_tag         : 'nvext',
)

custom_target('postprocess_libfsrrr',
  input : fsrrr_dll,
  output: 'amd_fidelityfx_denoiser_dx12.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : fsrrr_dll,
  build_by_default : true
)

fsrfg_dll = shared_library('amd_fidelityfx_framegeneration_dx12',
  'fsrfg.cpp',
  'sdk/sdk_backend.cpp',
  'sdk/FrameInterpolationSwapchainDX12.cpp',
  'sdk/FrameInterpolationSwapchainDX12_Helpers.cpp',
  'sdk/FrameInterpolationSwapchainDX12_UiComposition.cpp',
  'sdk/FrameInterpolationSwapchainDX12_DebugPacing.cpp',
  name_prefix         : '',
  dependencies        : [ util_dep, cpp.find_library('winmm'), cpp.find_library('dwmapi'), cpp.find_library('dxguid') ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_builtin_install_dir,
  vs_module_defs      : 'fsrfg.def',
  install_tag         : 'nvext',
)

custom_target('postprocess_libfsrfg',
  input : fsrfg_dll,
  output: 'amd_fidelityfx_framegeneration_dx12.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : fsrfg_dll,
  build_by_default : true
)

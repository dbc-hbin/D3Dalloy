nvngx_src = [
  'nvngx.cpp',
  'nvngx_d3d12.cpp',
]

nvngx_ld_args      = []
nvngx_link_depends = []

if dxmt_crossbuild
  # NVSDK_NGX_Parameter is a C++ interface consumed by MSVC-built applications.
  # Keep its vftable in a tiny MSVC-ABI object while the storage/frontend stays
  # in the normal DXMT MinGW translation units.
  msvc_bridge_compiler = find_program('x86_64-w64-mingw32-g++')
  nvngx_d3d12_msvc_obj = custom_target('nvngx_d3d12_msvc_obj',
    input            : 'nvngx_d3d12_msvc.cpp',
    output           : 'nvngx_d3d12_msvc.obj',
    command          : [
      msvc_bridge_compiler, '--target=x86_64-pc-windows-msvc', '-std=c++20',
      '-fno-exceptions', '-fno-rtti', '-Wall', '-Wextra', '-Werror',
      '-c', '@INPUT@', '-o', '@OUTPUT@',
    ],
    build_by_default : true,
  )
  nvngx_ld_args += [ nvngx_d3d12_msvc_obj.full_path() ]
  nvngx_link_depends += [ nvngx_d3d12_msvc_obj ]
endif

nvngx_dll = shared_library('nvngx', nvngx_src,
  name_prefix         : '',
  dependencies        : [ dxgi_dep, util_dep, winemetal_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_builtin_install_dir,
  link_args           : nvngx_ld_args,
  link_depends        : [ nvngx_link_depends ],
  install_tag         : 'nvext',
)

# always build as a wine-builtin dll
custom_target('postprocess_libnvngx',
  input : nvngx_dll,
  output: 'nvngx.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : nvngx_dll,
  build_by_default : true
)

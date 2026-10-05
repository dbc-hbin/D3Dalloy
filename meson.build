project('dxmt', ['c', 'cpp'], version : 'v0.1', meson_version: '>=1.3.0', default_options : [ 
  'cpp_std=c++20', 
  'warning_level=2', 
  'build.cpp_std=c++20',
  'b_ndebug=if-release',
])

fs = import('fs')

dxmt_crossbuild = meson.is_cross_build()

cpp = meson.get_compiler('cpp')
cc = meson.get_compiler('c')
clang = meson.get_compiler('c', native: true)
clangpp = meson.get_compiler('cpp', native: true)

cpu_family = host_machine.cpu_family()
native_compiler_arch = get_option('native_arch')
native_cpu_family = native_compiler_arch == 'arm64' ? 'aarch64' : 'x86_64'
native_bridge_only = get_option('native_bridge_only')
build_native_components = get_option('build_native_components')
if build_native_components
  add_languages('objcpp', native: true)
  add_project_arguments(['-std=c++20', '-arch', native_compiler_arch], language: 'objcpp', native: true)
  add_project_link_arguments(['-arch', native_compiler_arch], language: 'objcpp', native: true)
endif
if not build_native_components and not dxmt_crossbuild
  error('build_native_components=false is only valid for Windows PE-only cross builds')
endif
if native_bridge_only and dxmt_crossbuild
  error('native_bridge_only requires a native macOS build, not a Windows cross file')
endif

dxmt_include_dirs = [
  './include',
  './libs'
]

dxmt_native_include_dirs = [
  './include/native/windows',
  './include/native/directx',
]

if not dxmt_crossbuild
  dxmt_include_dirs += dxmt_native_include_dirs
endif

source_prefix_path = fs.relative_to(
    meson.project_source_root(),
    meson.project_build_root()
)

compiler_args = [
  '-Wimplicit-fallthrough',
  # gcc
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-cast-function-type', # Needed for GetProcAddress.
  # clang
  '-Wno-unused-private-field',
  '-Wno-microsoft-exception-spec',
  '-Wno-extern-c-compat',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
  '-fmacro-prefix-map=' + source_prefix_path + '/=',
]

if cpu_family == 'x86'
  compiler_args += [
    '-msse',
    '-msse2',
    '-msse3',
    '-mfpmath=sse',
  ]
endif

native_compiler_args = [
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-Wno-unused-private-field',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
]

link_args = []

compiler_args += [
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
]

link_args += [
  '-static',
  # -static is sufficient for clang, while c runtime will be linked to api-ms-win-crt-* (ucrt)
  # '-static-libgcc',
  # '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]
if native_compiler_arch == 'arm64'
  # Keep writable IAT/data away from executable code on Darwin 16KB host pages.
  link_args += ['-Wl,--section-alignment=16384']
endif

# Wine's built-in back traces only work with dwarf4 symbols
if get_option('debug')
  compiler_args += [
    '-gdwarf-4',
  ]
endif

dxmt_include_path = include_directories(dxmt_include_dirs)
dxmt_native_include_path = include_directories(dxmt_native_include_dirs)

if not fs.is_file('include/native/directx/d3d11.h')
  error('Don\'t forget to clone submodules. `git submodule update --init --recursive`')
endif

if dxmt_crossbuild
  res_ext = '.o'
  wrc = find_program('windres')
  wrc_generator = generator(wrc,
    output    : [ '@BASENAME@' + res_ext ],
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@' ],
  )
else
  wrc           = find_program('touch')
  wrc_generator = generator(wrc, output : [ '@BASENAME@_ignored.h' ], arguments : [ '@OUTPUT@' ] )
endif

xcrun = find_program('xcrun')

metalir_generator = generator(xcrun,
  output    : [ '@BASENAME@.air'],
  depfile   : '@BASENAME@.d',
  arguments : [ '-sdk', 'macosx', 'metal', '-MMD', '-MF', '@DEPFILE@', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@'],
)

metallib_generator = generator(xcrun,
  output    : [ '@BASENAME@.metallib'],
  arguments : [ '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@'],
)

xxd = find_program('xxd')

hexdump_generator = generator(xxd,
  output    : [ '@BASENAME@.h'],
  arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@'],
)

dxmt_debug = get_option('dxmt_debug')

if dxmt_debug
add_project_arguments('-DDXMT_DEBUG=1', language: 'cpp')
endif

add_project_arguments('-DDXMT_PAGE_SIZE=4096', language: 'cpp')

dxmt_version = vcs_tag(
  command: ['git', 'describe', '--always'],
  input:  'version.h.in',
  output: 'version.h',
)

if dxmt_crossbuild
  wine_builtin_dll = get_option('wine_builtin_dll')

  if cpu_family == 'x86'
    # Enable stdcall fixup on 32-bit
    link_args += [
      '-Wl,--enable-stdcall-fixup',
      '-Wl,--kill-at',
    ]
    # Fix stack alignment issues with mingw on 32-bit
    compiler_args += [
      '-mpreferred-stack-boundary=2'
    ]
    if wine_builtin_dll
      windows_native_install_dir = 'i386-windows'
    else
      windows_native_install_dir = 'syswow64'
    endif
    windows_builtin_install_dir = 'i386-windows'
  elif cpu_family == 'x86_64'
    if wine_builtin_dll
      windows_native_install_dir = 'x86_64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'x86_64-windows'
  elif cpu_family == 'aarch64'
    windows_abi = meson.get_external_property('windows_abi', 'arm64')
    if windows_abi not in ['arm64', 'arm64ec', 'arm64x']
      error('Unknown ARM Windows ABI: ' + windows_abi)
    endif
    if windows_abi != 'arm64' and not cc.compiles('''
      #if !defined(__arm64ec__) && !defined(_M_ARM64EC)
      #error ARM64EC compiler required
      #endif
      int abi_check;
    ''', name: 'ARM64EC compiler ABI')
      error('ARM64EC/ARM64X requires an ARM64EC PE compiler, not plain ARM64')
    endif
    windows_pe_dir = windows_abi == 'arm64ec' ? 'arm64ec-windows' : 'aarch64-windows'
    if wine_builtin_dll
      windows_native_install_dir = windows_pe_dir
    else
      windows_native_install_dir = 'system32'
    endif
    # Wine merges native+EC SDK archives under aarch64-windows in ARM64X mode.
    windows_builtin_install_dir = windows_pe_dir
  else
    error('invalid cpu family')
  endif

  unix_install_dir = native_cpu_family + '-unix'
else
  wine_builtin_dll = false
  windows_native_install_dir = ''
endif

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_arguments(clangpp.get_supported_arguments(native_compiler_args), language: 'cpp', native: true)
add_project_arguments(cc.get_supported_arguments(compiler_args), language: 'c')
add_project_arguments(clang.get_supported_arguments(native_compiler_args), language: 'c', native: true)
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')
add_project_link_arguments(cc.get_supported_link_arguments(link_args), language: 'c')
add_project_arguments(['-arch', native_compiler_arch], language: 'c', native: true)
add_project_arguments(['-arch', native_compiler_arch], language: 'cpp', native: true)
add_project_link_arguments(['-arch', native_compiler_arch], language: 'c', native: true)
add_project_link_arguments(['-arch', native_compiler_arch], language: 'cpp', native: true)

subdir('libs')
subdir('src')

if get_option('enable_tests')
subdir('tests')
endif

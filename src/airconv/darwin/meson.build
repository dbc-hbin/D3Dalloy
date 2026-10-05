native_llvm_path = get_option('native_llvm_path')

if not native_llvm_path.startswith('/')
  # Meson only allows relative include paths in case the toolchains are located inside
  # of the project source root, so handle that here.
  llvm_include_path_darwin = include_directories(join_paths('../../..', native_llvm_path, 'include'))
  native_llvm_path = join_paths(meson.project_source_root(), native_llvm_path)
else
  llvm_include_path_darwin = include_directories(join_paths(native_llvm_path, 'include'))
endif

llvm_ld_flags_darwin = ['-L' + join_paths(native_llvm_path, 'lib')]
llvm_system_dependencies = []
llvm_native_deps = []
llvm_config_darwin = find_program(join_paths(native_llvm_path, 'bin/llvm-config'), native: true, required: false)
if llvm_config_darwin.found()
  if run_command(llvm_config_darwin, '--version', check: true).stdout().strip() != '15.0.7'
    error('The owned AIR compiler requires LLVM 15.0.7')
  endif
  llvm_native_deps = run_command(llvm_config_darwin, '--link-static', '--libs', 'bitwriter', 'passes', check: true).stdout().strip().split()
  llvm_system_flags = run_command(llvm_config_darwin, '--link-static', '--system-libs', check: true).stdout().strip().split()
  foreach flag : llvm_system_flags
    if flag == '-lzstd'
      llvm_system_dependencies += clangpp.find_library('zstd', static: true, dirs: [
        join_paths(native_llvm_path, 'lib'), '/opt/homebrew/opt/zstd/lib', '/usr/local/opt/zstd/lib',
      ])
    else
      llvm_ld_flags_darwin += flag
    endif
  endforeach
else
  # The source-built static SDK intentionally omits LLVM tools, but exports
  # the same authoritative component/dependency graph through CMake.
  llvm_cmake_dependency = dependency('LLVM', method: 'cmake', version: ['>=15.0', '<16.0'],
    native: true, static: true, modules: ['bitwriter', 'passes'],
    cmake_module_path: [join_paths(native_llvm_path, 'lib/cmake/llvm')],
    cmake_args: ['-DCMAKE_PREFIX_PATH=' + native_llvm_path,
                 '-DCMAKE_OSX_ARCHITECTURES=' + native_compiler_arch])
  if llvm_cmake_dependency.version() != '15.0.7'
    error('The owned AIR compiler requires LLVM 15.0.7')
  endif
  llvm_system_dependencies += llvm_cmake_dependency
endif
if fs.is_file(join_paths(native_llvm_path, 'lib/libunwind.a'))
  llvm_ld_flags_darwin += join_paths(native_llvm_path, 'lib/libunwind.a')
endif

airconv_lib_darwin = static_library('airconv', airconv_src,
  include_directories : [ dxmt_include_path, llvm_include_path_darwin ],
  cpp_args            : [ airconv_args ],
  dependencies        : [ dxbc_parser_native_dep ],
  link_args           : llvm_ld_flags_darwin,
  native              : dxmt_crossbuild
)

airconv_dep_darwin = declare_dependency(
  link_with           : [ airconv_lib_darwin ],
  include_directories : [ dxmt_include_path, include_directories('..') ],
  dependencies        : llvm_system_dependencies,
  link_args           : [ llvm_ld_flags_darwin, llvm_native_deps ]
)

fp64_lowering_test = executable('fp64-lowering-test',
  files('../../../graphics-translator/tests/fp64-lowering.test.cpp'),
  include_directories: [dxmt_include_path, llvm_include_path_darwin],
  cpp_args: [airconv_args],
  dependencies: [airconv_dep_darwin, dxbc_parser_native_dep],
  native: dxmt_crossbuild,
)
test('fp64-lowering', fp64_lowering_test)

atomic64_phase_test = executable('atomic64-phase-test',
  files('../../../graphics-translator/tests/atomic64-phase.test.cpp'),
  include_directories: [dxmt_include_path, llvm_include_path_darwin],
  cpp_args: [airconv_args],
  dependencies: [airconv_dep_darwin, dxbc_parser_native_dep],
  native: dxmt_crossbuild,
)
test('atomic64-phase', atomic64_phase_test)

executable('airconv', airconv_src + airconv_cli_src,
  include_directories : [ dxmt_include_path, llvm_include_path_darwin ],
  cpp_args            : [ airconv_args ],
  dependencies        : [ dxbc_parser_native_dep, llvm_system_dependencies ],
  link_args           : [ llvm_ld_flags_darwin, llvm_native_deps ],
  native              : dxmt_crossbuild
)
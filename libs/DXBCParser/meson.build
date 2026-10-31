shader_bytecode_src = files('../../src/airconv/shader_bytecode.rs')
shader_bytecode_modules = files(
  '../../src/airconv/cache_wire.rs',
  '../../src/airconv/cache_stage.rs',
  '../../src/airconv/cache_pipeline.rs',
  '../../src/airconv/cache_library.rs',
  '../../src/airconv/dxbc_program.rs',
  '../../src/airconv/dxbc_signature.rs',
  '../../src/airconv/pe_image.rs',
  '../../src/airconv/pso_stream.rs',
  '../../src/airconv/rtas_serialized.rs',
)
rustc = find_program('rustc', native: true)
rust_optimization = get_option('optimization')
if rust_optimization == 'g'
  rust_optimization = '0'
endif
shader_bytecode_args = ['--edition=2021', '--crate-type=staticlib', '-Cpanic=abort',
                       '-Copt-level=' + rust_optimization]
shader_bytecode_dep = declare_dependency()
shader_bytecode_native_dep = declare_dependency()
if build_native_components
  shader_bytecode_native = custom_target('shader-bytecode-native',
    input: shader_bytecode_src, output: 'libshader_bytecode_native.a',
    depend_files: shader_bytecode_modules,
    command: [rustc, shader_bytecode_args, '--target', native_cpu_family + '-apple-darwin',
              '@INPUT@', '-o', '@OUTPUT@'],
  )
  # Meson classifies custom targets as host outputs; keep this explicitly
  # native archive as a generated link dependency.
  shader_bytecode_native_dep = declare_dependency(sources: shader_bytecode_native)
  shader_bytecode_dep = shader_bytecode_native_dep
endif
if dxmt_crossbuild
  if cpu_family == 'x86'
    shader_bytecode_target = 'i686-pc-windows-gnu'
  elif cpu_family == 'x86_64'
    shader_bytecode_target = 'x86_64-pc-windows-gnu'
  elif cpu_family == 'aarch64'
    shader_bytecode_target = meson.get_external_property('windows_abi', 'arm64') == 'arm64' ? \
        'aarch64-pc-windows-msvc' : 'arm64ec-pc-windows-msvc'
  else
    error('Unsupported Windows architecture for shader bytecode parsing: ' + cpu_family)
  endif
  shader_bytecode_windows = custom_target('shader-bytecode-windows',
    input: shader_bytecode_src, output: 'libshader_bytecode_windows.a',
    depend_files: shader_bytecode_modules,
    command: [rustc, shader_bytecode_args, '--target', shader_bytecode_target, '@INPUT@', '-o', '@OUTPUT@'],
  )
  shader_bytecode_dep = declare_dependency(link_with: shader_bytecode_windows)
endif

dxbc_parser_src = files([
  'DXBCUtils.cpp',
  'BlobContainer.cpp',
  'ShaderBinary.cpp',
])

dxbc_parser_args = [
  '-Wno-extern-c-compat',
  '-Wno-unknown-pragmas',
]

dxbc_parser_lib = static_library('DXBCParser', dxbc_parser_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ dxbc_parser_args ],
)

dxbc_parser_dep = declare_dependency(
  link_with           : [ dxbc_parser_lib ],
  include_directories : [ dxmt_include_path ],
  dependencies        : [ shader_bytecode_dep ],
)

if build_native_components and (cpu_family == 'x86_64' or cpu_family == 'aarch64' or not dxmt_crossbuild)

dxbc_parser_native_lib = static_library('DXBCParserNative', dxbc_parser_src,
  include_directories : [ dxmt_native_include_path ],
  cpp_args            : [ dxbc_parser_args ],
  native              : dxmt_crossbuild,
)

dxbc_parser_native_dep = declare_dependency(
  link_with           : [ dxbc_parser_native_lib ],
  include_directories : [ dxmt_native_include_path ],
  dependencies        : [ shader_bytecode_native_dep ],
)

endif
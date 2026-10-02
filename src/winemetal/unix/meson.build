if wine_build_path != ''
  winemac_so = join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so')
  ntdll_so = join_paths(wine_build_path, 'dlls/ntdll/ntdll.so')
elif wine_install_path != ''
  winemac_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'winemac.so')
  ntdll_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'ntdll.so')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

foreach native_wine_library : [winemac_so, ntdll_so]
  if not fs.is_file(native_wine_library)
    error('Missing native Wine SDK library: ' + native_wine_library)
  endif
  if run_command(lipo, '-verify_arch', native_compiler_arch, native_wine_library, check: false).returncode() != 0
    error('Wine SDK library does not provide native_arch=' + native_compiler_arch + ': ' + native_wine_library)
  endif
endforeach

winemetal_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal.so',
  winemac_so, ntdll_so,
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'CoreVideo', '-weak_framework', 'IOKit',
  '-weak_framework', 'ColorSync',  '-weak_framework', 'Cocoa',
  '-weak_framework', 'IOSurface',
  '-lsqlite3',
]
winemetal_unix_link_depends = []

shared_library('winemetal', winemetal_unix_sources,
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  include_directories : [dxmt_include_path, include_directories('../../')],
  c_args              : ['-ObjC'],
  link_args           : winemetal_unix_ld_args,
  native              : true,
  name_suffix         : 'so',
  install_rpath       : '',
  build_rpath         : '@loader_path/:@loader_path/../../',
  override_options    : { 'b_asneeded' : false },
)

meson.add_install_script(
  'install.sh', get_option('strip').to_string(), unix_install_dir, 
  install_tag: 'runtime'
)
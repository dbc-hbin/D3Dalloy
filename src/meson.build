winemetal_unix_sources = files(
  'winemetal/unix/winemetal_unix.c',
  'winemetal/unix/winemetal_metal4.c',
  'winemetal/unix/winemetal_metal4_pipeline.c',
  'winemetal/unix/winemetal_metal4_query.c',
  'winemetal/unix/winemetal_metal4_fx.mm',
  'winemetal/unix/metalfx-backend.mm',
  'winemetal/unix/metalfx_frame_generation.mm',
  'winemetal/unix/winemetal_rt.c',
  'winemetal/unix/rt_acceleration_structure.c',
  'winemetal/unix/cache.c',
)

if build_native_components
  configure_file(
    input: 'winemetal/unix/fsr-kernels.inc.in',
    output: 'fsr-kernels.inc',
    configuration: {'FSR_KERNELS_SOURCE': fs.read('winemetal/unix/fsr-kernels.metal')},
  )
endif

if get_option('enable_fsr') and (native_bridge_only or not dxmt_crossbuild or cpu_family != 'x86_64' or not get_option('enable_d3d12'))
  error('enable_fsr requires a Windows x64 cross build with enable_d3d12=true')
endif

if not native_bridge_only
subdir('util')
endif
subdir('airconv')

if not dxmt_crossbuild
subdir('nativemetal')
else
subdir('winemetal')
endif

if not native_bridge_only
subdir('dxmt')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

if get_option('enable_d3d12')
subdir('d3d12')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif

if get_option('enable_fsr')
subdir('fsr')
subdir('fsrfg')
endif
endif
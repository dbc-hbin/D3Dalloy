d3d12_src = [
  'd3d12.cpp',
  'd3d12_sdk.cpp',
  'd3d12_acceleration_structure.cpp',
  'd3d12_atomic64_phase.cpp',
  'd3d12_buffer.cpp',
  'd3d12_command_allocator.cpp',
  'd3d12_command_list.cpp',
  'd3d12_command_queue.cpp',
  'd3d12_command_signature.cpp',
  'd3d12_descriptor_heap.cpp',
  'd3d12_device.cpp',
  'd3d12_diagnostics.cpp',
  'd3d12_dxil_subobjects.cpp',
  'd3d12_fence.cpp',
  'd3d12_heap.cpp',
  'd3d12_pipeline_compute.cpp',
  'd3d12_pipeline_cache.cpp',
  'd3d12_pipeline_graphics.cpp',
  'd3d12_pipeline_library.cpp',
  'd3d12_query_heap.cpp',
  'd3d12_reserved_resource.cpp',
  'd3d12_resource_helper.cpp',
  'd3d12_root_signature.cpp',
  'd3d12_sampler.cpp',
  'd3d12_sampler_feedback.cpp',
  'd3d12_shader_cache.cpp',
  'd3d12_sharing.cpp',
  'd3d12_state_object.cpp',
  'd3d12_work_graph.cpp',
  'd3d12_swapchain.cpp',
  'd3d12_texture.cpp',
  'd3d12_temporal.cpp',
  'd3d12_frame_generation.cpp',
  'd3d12_ray_regeneration.cpp',
  '../d3d10/d3d10_blob.cpp',
]

d3d12_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
d3d12_link_depends = []

# Core and Main must be from the same configuration, not just the same API headers.
core_generation = configuration_data()
core_generation_id = get_option('d3d12_core_generation')
if core_generation_id == ''
  core_generation_id = run_command(
    find_program('python3'), '-c', 'import uuid; print(uuid.uuid4().hex)', check: true
  ).stdout().strip()
endif
core_generation.set_quoted('OWNED_D3D12_CORE_GENERATION', core_generation_id)
configure_file(output: 'd3d12_core_generation.h', configuration: core_generation)

d3d12_dll = shared_library('d3d12', d3d12_src,
  name_prefix         : '',
  dependencies        : [ dxgi_dep, d3dalloy_dep, airconv_forward_dep, util_dep, winemetal_dep, dxbc_parser_dep ],
  include_directories : [ d3dalloy_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d12.def',
  link_args           : d3d12_ld_args,
  link_depends        : [ d3d12_link_depends ],
)

d3d12_dep = declare_dependency(
  link_with           : [ d3d12_dll ],
  include_directories : [ d3dalloy_include_path, include_directories('.') ],
)

# Core has no Unix library boundary. It must remain an ordinary PE image:
# Wine builtin resolution can redirect separate requested paths by basename.
d3d12core_dll = shared_library('d3d12core', 'd3d12_core.cpp',
  name_prefix         : '',
  dependencies        : [ d3d12_dep, cpp.find_library('dxguid') ],
  include_directories : [ d3dalloy_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d12_core.def',
  link_args           : d3d12_ld_args,
)

if wine_builtin_dll
custom_target('postprocess_libd3d12',
  input : d3d12_dll,
  output: 'd3d12.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d12_dll,
  build_by_default : true
)
endif

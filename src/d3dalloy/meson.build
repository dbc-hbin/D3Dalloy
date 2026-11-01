d3dalloy_src = [
  'd3dalloy_format.cpp',
  'd3dalloy_names.cpp',
  'd3dalloy_command_queue.cpp',
  'd3dalloy_command.cpp',
  'd3dalloy_capture.cpp',
  'd3dalloy_info.cpp',
  'd3dalloy_device.cpp',
  'd3dalloy_buffer.cpp',
  'd3dalloy_texture.cpp',
  'd3dalloy_context.cpp',
  'd3dalloy_dynamic.cpp',
  'd3dalloy_staging.cpp',
  'd3dalloy_hud_state.cpp',
  'd3dalloy_allocation.cpp',
  'd3dalloy_presenter.cpp',
  'd3dalloy_sampler.cpp',
  'd3dalloy_resource_initializer.cpp',
  'd3dalloy_shader_cache.cpp',
  'd3dalloy_scaler.cpp',
  'd3dalloy_subresource.cpp',
  'd3dalloy_deptrack.cpp',
  'd3dalloy_counter.cpp',
  'd3dalloy_fence.cpp',
  'd3dalloy_keyed_mutex.cpp',
]

d3dalloy_shaders = [
  'd3dalloy_command.metal'
]

d3dalloy_shaders_ir = metalir_generator.process(d3dalloy_shaders)

d3dalloy_shaders_lib = metallib_generator.process(d3dalloy_shaders_ir)

d3dalloy_shaders_header = hexdump_generator.process(d3dalloy_shaders_lib)

d3dalloy_lib = static_library('d3dalloy', d3dalloy_src, d3dalloy_shaders_header, d3dalloy_version,
  dependencies        : [ util_dep, airconv_forward_dep, winemetal_dep ],
  include_directories : [ d3dalloy_include_path ],
)

d3dalloy_dep = declare_dependency(
  link_with           : [ d3dalloy_lib ],
  include_directories : [ d3dalloy_include_path, include_directories('.') ],
)

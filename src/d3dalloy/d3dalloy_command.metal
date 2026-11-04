/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Paweł Kołodziejski
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <metal_stdlib>
#include <metal_common>
#include <metal_texture>
#include <metal_math>
#include "d3dalloy_command_constants.hpp"
#include "../airconv/shaders/conservative_polygon.h"

using namespace metal;
using namespace d3dalloy;

#include "d3d12_work_graph_scheduler.metal"

// Publish only bytes changed by a cast shadow; inactive descriptor-table
// aliases must not overwrite an actively written native allocation.
kernel void d3d12_cast_merge(device const uchar *shadow [[buffer(0)]],
                            device const uchar *baseline [[buffer(1)]],
                            device uchar *native [[buffer(2)]],
                            constant uint4 &layout [[buffer(3)]],
                            uint3 index [[thread_position_in_grid]]) {
  const ulong offset = (ulong(index.z) * layout.z + index.y) * layout.y + index.x;
  if (shadow[offset] != baseline[offset]) native[offset] = shadow[offset];
}

// Buffers contain row-major blit bytes, never a private texture's physical
// heap representation. Publish only changed format units through the actual
// sparse texture so native swizzling stays Metal's responsibility.
kernel void d3d12_volume_render_publish(device const uchar *shadow [[buffer(0)]],
                                        device const uchar *baseline [[buffer(1)]],
                                        texture2d_array<uint, access::read_write> atlas [[texture(0)]],
                                        constant uint *p [[buffer(3)]],
                                        uint2 position [[thread_position_in_grid]]) {
  const uint offset = (position.y * p[3] + position.x) * p[5];
  bool changed = false;
  uint4 words = 0;
  uint4 changed_bytes = 0;
  for (uint byte = 0; byte < p[5]; ++byte) {
    bool different = shadow[offset + byte] != baseline[offset + byte];
    changed |= different;
    if (different) changed_bytes[byte / 4] |= 255u << ((byte % 4) * 8);
    words[byte / 4] |= uint(shadow[offset + byte]) << ((byte % 4) * 8);
  }
  if (!changed) return;
  uint4 current = atlas.read(uint2(p[0], p[1]) + position, p[2]);
  uint4 current_words = 0;
  uint current_bit = 0;
  for (uint lane = 0; lane < 4; ++lane) {
    const uint width = (p[6] >> (lane * 8)) & 255;
    if (width) {
      current_words[current_bit / 32] |= current[lane] << (current_bit % 32);
      current_bit += width;
    }
  }
  words = (words & changed_bytes) | (current_words & ~changed_bytes);
  uint4 value = 0;
  uint bit = 0;
  for (uint lane = 0; lane < 4; ++lane) {
    const uint width = (p[6] >> (lane * 8)) & 255;
    if (width) {
      value[lane] = words[bit / 32] >> (bit % 32);
      if (width < 32) value[lane] &= (1u << width) - 1;
      bit += width;
    }
  }
  atlas.write(value, uint2(p[0], p[1]) + position, p[2]);
}

// D3D12 R24G8 placed depth plane is D24X8 (low 24 bits, high byte zero),
// while Metal's emulated D24 texture stores its depth plane as float32.
kernel void d3d12_d24_to_float(device const uchar *source [[buffer(0)]],
                                device uchar *destination [[buffer(1)]],
                                constant uint2 &layout [[buffer(2)]],
                                uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, column = pos.x;
  const uint packed = *reinterpret_cast<device const uint *>(source + row * layout.x + column * 4);
  *reinterpret_cast<device float *>(destination + row * layout.y + column * 4) =
      precise::divide(float(packed & 0xffffffu), 16777215.0f);
}

kernel void d3d12_float_to_d24(device const uchar *source [[buffer(0)]],
                                device uchar *destination [[buffer(1)]],
                                constant uint2 &layout [[buffer(2)]],
                                uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, column = pos.x;
  const float depth = *reinterpret_cast<device const float *>(source + row * layout.x + column * 4);
  const float normalized = clamp(depth, 0.0f, 1.0f);
  // Scaling by 2^24 is exact for a float32 source. Decompose the fractional
  // part before subtracting depth, so float32 cannot round the large product
  // with 0xffffff to an integer (losing the D24 half-step).
  const float scaled = normalized * 16777216.0f;
  int packed = int(scaled);
  const float delta = (scaled - float(packed)) - normalized;
  if (delta > 0.5f || (delta == 0.5f && (packed & 1)))
    ++packed;
  else if (delta < -0.5f || (delta == -0.5f && (packed & 1)))
    --packed;
  *reinterpret_cast<device uint *>(destination + row * layout.y + column * 4) =
      min(0xffffffu, uint(packed));
}

// D3D12-only logical texture layouts. Each invocation owns one RGB texel or
// one packed 4:2:2 pair, so readback never races another invocation on a row.
kernel void d3d12_rgb32_expand(device const uchar *source [[buffer(0)]],
                               device uchar *destination [[buffer(1)]],
                               constant uint2 &layout [[buffer(2)]],
                               uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, x = pos.x;
  device const uint *src = reinterpret_cast<device const uint *>(source + row * layout.x + x * 12);
  device uint *dst = reinterpret_cast<device uint *>(destination + row * layout.y + x * 16);
  dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0;
}

kernel void d3d12_rgb32_compact(device const uchar *source [[buffer(0)]],
                                device uchar *destination [[buffer(1)]],
                                constant uint2 &layout [[buffer(2)]],
                                uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, x = pos.x;
  device const uint *src = reinterpret_cast<device const uint *>(source + row * layout.x + x * 16);
  device uint *dst = reinterpret_cast<device uint *>(destination + row * layout.y + x * 12);
  dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
}

// layout.z is logical pixel width, layout.w selects the byte order. The first member of
// each pair owns R/B; the two G bytes belong to consecutive logical pixels.
kernel void d3d12_packed422_expand(device const uchar *source [[buffer(0)]],
                                   device uchar *destination [[buffer(1)]],
                                   constant uint4 &layout [[buffer(2)]],
                                   uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, pair = pos.x;
  const bool variant = layout.w != 0;
  device const uchar *src = source + row * layout.x + pair * 4;
  device uchar *left = destination + row * layout.y + pair * 8;
  const uchar r = variant ? src[1] : src[0];
  const uchar b = variant ? src[3] : src[2];
  left[0] = r; left[1] = variant ? src[0] : src[1]; left[2] = b; left[3] = 255;
  if (pair * 2 + 1 < layout.z) {
    left[4] = r; left[5] = variant ? src[2] : src[3]; left[6] = b; left[7] = 255;
  }
}

kernel void d3d12_packed422_compact(device const uchar *source [[buffer(0)]],
                                    device uchar *destination [[buffer(1)]],
                                    constant uint4 &layout [[buffer(2)]],
                                    uint2 pos [[thread_position_in_grid]]) {
  const uint row = pos.y, pair = pos.x;
  const bool variant = layout.w != 0;
  device const uchar *left = source + row * layout.x + pair * 8;
  device uchar *dst = destination + row * layout.y + pair * 4;
  const uchar g1 = pair * 2 + 1 < layout.z ? left[5] : 0;
  if (variant) {
    dst[0] = left[1]; dst[1] = left[0]; dst[2] = g1; dst[3] = left[2];
  } else {
    dst[0] = left[0]; dst[1] = left[1]; dst[2] = left[2]; dst[3] = g1;
  }
}

[[vertex]] float4 msaa_fullscreen_vertex(uint vertex_id [[vertex_id]]) {
  const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  return float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
}

[[vertex]] float4 msaa_atomic_tile_vertex(uint vertex_id [[vertex_id]]) {
  const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  // An unclipped triangle over a 32x32 surface. The app's quad-aligned 8x8
  // tile at (4,4) is interior to all three edges; clipping must not create
  // internal fan edges with native-helper peers inside application quads.
  return float4(corner.x - 1.0f, 1.0f - corner.y, 0.0f, 1.0f);
}

struct D3DALLOYMSAAFullscreenLayerVertex {
  float4 position [[position]];
  uint layer [[render_target_array_index]];
};

[[vertex]] D3DALLOYMSAAFullscreenLayerVertex msaa_fullscreen_layer_vertex(
    uint vertex_id [[vertex_id]], constant uint &layer [[buffer(23)]]) {
  const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  return {float4(corner * 2.0f - 1.0f, 0.0f, 1.0f), layer};
}

[[fragment]] uint msaa_clear_coverage() {
  return 0;
}

struct D3DALLOYMSAANoPixelInput {
  float4 position [[position]];
  interpolant<float, interpolation::no_perspective> ndc_depth [[user(msaa_ndc_depth)]];
};

struct D3DALLOYMSAALineInput {
  float4 position [[position]];
  interpolant<float, interpolation::no_perspective> ndc_depth [[user(msaa_ndc_depth)]];
  float2 start [[user(msaa_line_start)]];
  float2 end [[user(msaa_line_end)]];
  float2 depth_pair [[user(msaa_line_depth_pair)]];
  interpolant<float2, interpolation::no_perspective> carrier_screen [[user(msaa_carrier_screen)]];
};

struct D3DALLOYMSAATile {
  uint x, y, sample_count, sample_mask;
  float min_depth, max_depth;
  uint primitive_id;
  uint primitive_slot;
  float viewport_origin_x, viewport_origin_y, viewport_width, viewport_height;
  uint output_mask, sample_base;
  uint shading_rate;
  uint shading_rate_image_width, shading_rate_image_height;
  uint programmable_sample_count;
  float programmable_sample_positions[8][2];
  uint depth_bounds_enabled;
  uint target_layer, render_width, render_height;
  uint scissor_left, scissor_top, scissor_right, scissor_bottom;
  uint task_active;
};
static_assert(sizeof(D3DALLOYMSAATile) == 43 * sizeof(uint));

// GPU tile tasks keep the original viewport and layer while bounding replay
// geometry. Fragment outputs still need an exact kill: a zero sample mask alone
// does not suppress every stencil operation.
inline bool msaa_task_contains(uint2 pixel, constant D3DALLOYMSAATile &tile) {
  return tile.task_active && pixel.x >= tile.x && pixel.y >= tile.y &&
      pixel.x - tile.x < 8 && pixel.y - tile.y < 8 &&
      pixel.x < tile.render_width && pixel.y < tile.render_height &&
      pixel.x >= tile.scissor_left && pixel.y >= tile.scissor_top &&
      pixel.x < tile.scissor_right && pixel.y < tile.scissor_bottom;
}

inline float4 msaa_fullscreen_position_task(uint vertex_id, constant D3DALLOYMSAATile &tile) {
  if (!tile.task_active || !tile.render_width || !tile.render_height)
    return float4(2.0f, 2.0f, 0.0f, 1.0f);
  const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  // Keep every application's 2x2 quad inside the triangle. Exact clipping is
  // performed by the fragment guard, not by an internal diagonal through a tile.
  const float2 pixel = float2(tile.x, tile.y) - 2.0f + corner * 12.0f;
  const float2 extent = float2(tile.render_width, tile.render_height);
  return float4(pixel / extent * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

[[vertex]] float4 msaa_fullscreen_vertex_task(
    uint vertex_id [[vertex_id]], constant D3DALLOYMSAATile &tile [[buffer(23)]]) {
  return msaa_fullscreen_position_task(vertex_id, tile);
}

[[vertex]] D3DALLOYMSAAFullscreenLayerVertex msaa_fullscreen_layer_vertex_task(
    uint vertex_id [[vertex_id]], constant D3DALLOYMSAATile &tile [[buffer(23)]]) {
  return {msaa_fullscreen_position_task(vertex_id, tile), tile.target_layer};
}

[[fragment]] uint msaa_clear_coverage_task(
    float4 position [[position]], constant D3DALLOYMSAATile &tile [[buffer(23)]]) {
  if (!msaa_task_contains(uint2(position.xy), tile)) {
    discard_fragment();
    return 0;
  }
  return 0;
}

struct D3DALLOYMSAAPixelLayout {
  uint input_record_dwords, output_record_dwords;
  uint input_claim_dword, input_coverage_dword;
  uint output_valid_dword, output_coverage_dword;
  uint flags, tile_width, input_record_count, helper_record_base;
  uint center_z_dword, sample_z_dword, z_claim_dword, z_anchor_dword;
  uint line_mode, line_mask_dword, line_alpha_dword;
  uint inner_mask_dword;
  uint depth_bounds_mask_dword;
};
static_assert(sizeof(D3DALLOYMSAAPixelLayout) == 19 * sizeof(uint));

struct D3DALLOYMSAADepthBoundsProbe {
  uint x, y, input_record_dwords, mask_dword;
  float min_depth, max_depth;
};
static_assert(sizeof(D3DALLOYMSAADepthBoundsProbe) == 6 * sizeof(uint));

// This is a separate, fenced compute pass. Reading the live depth attachment in the raster pass would
// create forbidden attachment feedback, and a draw-wide snapshot would miss previous-primitive writes.
[[kernel]] void msaa_depth_bounds_probe_single(
    depth2d<float, access::read> depth [[texture(0)]], device uint *records [[buffer(0)]],
    constant D3DALLOYMSAADepthBoundsProbe &probe [[buffer(1)]], uint2 pixel [[thread_position_in_grid]]) {
  const uint2 xy = uint2(probe.x, probe.y) + pixel;
  uint mask = 0;
  if (xy.x < depth.get_width() && xy.y < depth.get_height()) {
    const float z = depth.read(xy);
    mask = probe.min_depth <= probe.max_depth && clamp(probe.min_depth, 0.0f, 1.0f) <= z &&
           z <= clamp(probe.max_depth, 0.0f, 1.0f) ? 1u : 0u;
  }
  records[(pixel.y * 8 + pixel.x) * probe.input_record_dwords + probe.mask_dword] = mask;
}

[[kernel]] void msaa_depth_bounds_probe(
    depth2d_ms<float, access::read> depth [[texture(0)]], device uint *records [[buffer(0)]],
    constant D3DALLOYMSAADepthBoundsProbe &probe [[buffer(1)]], uint2 pixel [[thread_position_in_grid]]) {
  const uint2 xy = uint2(probe.x, probe.y) + pixel;
  uint mask = 0;
  if (xy.x < depth.get_width() && xy.y < depth.get_height())
    for (uint sample = 0; sample < depth.get_num_samples(); ++sample) {
      const float z = depth.read(xy, sample);
      if (probe.min_depth <= probe.max_depth && clamp(probe.min_depth, 0.0f, 1.0f) <= z &&
          z <= clamp(probe.max_depth, 0.0f, 1.0f)) mask |= 1u << sample;
    }
  records[(pixel.y * 8 + pixel.x) * probe.input_record_dwords + probe.mask_dword] = mask;
}

struct D3DALLOYMSAAQueryCoverage {
  uint mask [[sample_mask]];
};

// A task's target layer is GPU-written. Keep the full DSV array bound and
// select that layer here; the host does not read tile coordinates or layers.
[[kernel]] void msaa_depth_bounds_probe_single_task(
    depth2d_array<float, access::read> depth [[texture(0)]], device uint *records [[buffer(0)]],
    constant D3DALLOYMSAADepthBoundsProbe &probe [[buffer(1)]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]], uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= 8 || pixel.y >= 8) return;
  const uint2 xy = uint2(tile.x, tile.y) + pixel;
  uint mask = 0;
  if (msaa_task_contains(xy, tile) && xy.x < depth.get_width() && xy.y < depth.get_height() &&
      tile.target_layer < depth.get_array_size()) {
    const float z = depth.read(xy, tile.target_layer);
    mask = probe.min_depth <= probe.max_depth && clamp(probe.min_depth, 0.0f, 1.0f) <= z &&
           z <= clamp(probe.max_depth, 0.0f, 1.0f) ? 1u : 0u;
  }
  records[(pixel.y * 8 + pixel.x) * probe.input_record_dwords + probe.mask_dword] = mask;
}

[[kernel]] void msaa_depth_bounds_probe_task(
    depth2d_ms_array<float, access::read> depth [[texture(0)]], device uint *records [[buffer(0)]],
    constant D3DALLOYMSAADepthBoundsProbe &probe [[buffer(1)]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]], uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= 8 || pixel.y >= 8) return;
  const uint2 xy = uint2(tile.x, tile.y) + pixel;
  uint mask = 0;
  if (msaa_task_contains(xy, tile) && xy.x < depth.get_width() && xy.y < depth.get_height() &&
      tile.target_layer < depth.get_array_size()) {
    for (uint sample = 0; sample < depth.get_num_samples(); ++sample) {
      const float z = depth.read(xy, tile.target_layer, sample);
      if (probe.min_depth <= probe.max_depth && clamp(probe.min_depth, 0.0f, 1.0f) <= z &&
          z <= clamp(probe.max_depth, 0.0f, 1.0f)) mask |= 1u << sample;
    }
  }
  records[(pixel.y * 8 + pixel.x) * probe.input_record_dwords + probe.mask_dword] = mask;
}

inline uint4 msaa_coverage_read(texture2d_ms<uint, access::read> coverage, uint2 xy, uint sample, uint = 0) {
  return coverage.read(xy, sample);
}

inline uint4 msaa_coverage_read(texture2d<uint, access::read> coverage, uint2 xy, uint, uint = 0) {
  return coverage.read(xy, 0); // mip zero, not a multisample sample index
}

inline uint4 msaa_coverage_read(texture2d_ms_array<uint, access::read> coverage, uint2 xy, uint sample, uint layer) {
  return coverage.read(xy, layer, sample);
}

inline uint4 msaa_coverage_read(texture2d_array<uint, access::read> coverage, uint2 xy, uint, uint layer) {
  return coverage.read(xy, layer, 0);
}

// Visibility counts raster samples, not samples in a single-sample RTV.
// The original PS has already run into the output record; never invoke it a
// second time just to answer a query.
template<bool Task = false, typename Coverage>
inline D3DALLOYMSAAQueryCoverage msaa_query_coverage_impl(
    float4 position, uint sample, constant D3DALLOYMSAATile &tile,
    constant D3DALLOYMSAAPixelLayout &layout, device const uint *outputs, Coverage coverage,
    device const uint *inputs) {
  const uint2 xy = uint2(position.xy);
  if constexpr (Task) {
    if (!msaa_task_contains(xy, tile) || sample >= min(tile.sample_count, 8u)) {
      discard_fragment();
      return {0};
    }
  }
  if (xy.x < tile.x || xy.y < tile.y || xy.x >= tile.x + 8 ||
      xy.y >= tile.y + 8 || sample >= min(tile.sample_count, 8u)) return {0};
  const uint2 local = xy - uint2(tile.x, tile.y);
  const device uint *record = outputs + (local.y * 8 + local.x) * layout.output_record_dwords;
  // Writes are enabled when any output sample in SampleMask & the PS coverage output survives (tile.output_mask
  // holds SampleMask restricted to the RTV samples). D3D permits either zero or full raster coverage when every
  // output sample is masked; consistently choose zero as recommended by the functional spec.
  const bool writes = record[0] && (record[1] & tile.output_mask);
  // Split 16x: the coverage texture holds only the last half; the input record has the logical union.
  const bool covered = tile.sample_count > 8
      ? ((inputs[(local.y * 8 + local.x) * layout.input_record_dwords + layout.input_coverage_dword] &
          tile.sample_mask) >> (sample + tile.sample_base)) & 1u
      : msaa_coverage_read(coverage, xy, sample, Task ? tile.target_layer : 0u).x != 0;
  return {writes && covered ? 1u << sample : 0u};
}

[[fragment]] D3DALLOYMSAAQueryCoverage msaa_query_coverage(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *outputs [[buffer(22)]],
    device const uint *inputs [[buffer(21)]],
    texture2d_ms<uint, access::read> coverage [[texture(24)]]) {
  return msaa_query_coverage_impl(position, sample, tile, layout, outputs, coverage, inputs);
}

[[fragment]] D3DALLOYMSAAQueryCoverage msaa_query_coverage_single(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *outputs [[buffer(22)]],
    device const uint *inputs [[buffer(21)]],
    texture2d<uint, access::read> coverage [[texture(24)]]) {
  return msaa_query_coverage_impl(position, sample, tile, layout, outputs, coverage, inputs);
}

struct D3DALLOYMSAAMarker {
  uint value [[color(0)]];
  uint mask [[sample_mask]];
};

// Diamond exit, not a nearest-distance test: an endpoint inside the last
// diamond cannot light that pixel. Rotating by 45 degrees makes four diamond
// half-planes into two independent slabs.
inline bool d3dalloy_line_diamond_exit(float2 start, float2 end, float2 center) {
  const float2 a = float2(start.x + start.y - center.x - center.y,
                         start.x - start.y - center.x + center.y);
  const float2 b = float2(end.x + end.y - center.x - center.y,
                         end.x - end.y - center.x + center.y);
  float enter = 0.0f, exit = 1.0f;
  for (uint axis = 0; axis < 2; axis++) {
    const float delta = b[axis] - a[axis];
    if (delta == 0.0f) {
      if (a[axis] < -0.5f || a[axis] > 0.5f) return false;
      continue;
    }
    const float first = (-0.5f - a[axis]) / delta;
    const float second = (0.5f - a[axis]) / delta;
    enter = max(enter, min(first, second));
    exit = min(exit, max(first, second));
  }
  return enter < exit && exit < 1.0f;
}

// Exact area of the unit-width butt-capped line rectangle intersected with
// this pixel's unit square. The specification permits implementation-specific
// AA filters; this geometric coverage is deterministic and preserves junctions.
inline float d3dalloy_line_alpha_area(float2 start, float2 end, float2 pixel) {
  // Work in pixel-local coordinates: the shoelace sum otherwise cancels
  // large screen-space products and loses coverage on distant render targets.
  start -= pixel;
  end -= pixel;
  const float2 direction = end - start;
  const float length = metal::length(direction);
  if (!(length > 0.0f) || !isfinite(length)) return 0.0f;
  const float2 normal = float2(-direction.y, direction.x) * (0.5f / length);
  float2 polygon[12] = {start - normal, start + normal, end + normal, end - normal};
  float2 clipped[12];
  uint count = 4;
  for (uint edge = 0; edge < 4; edge++) {
    uint next_count = 0;
    const uint axis = edge & 1;
    const float boundary = edge >= 2 ? 1.0f : 0.0f;
    for (uint i = 0; i < count; i++) {
      const float2 a = polygon[i], b = polygon[(i + 1) % count];
      const bool inside_a = edge >= 2 ? a[axis] <= boundary : a[axis] >= boundary;
      const bool inside_b = edge >= 2 ? b[axis] <= boundary : b[axis] >= boundary;
      if (inside_a != inside_b) {
        const float t = (boundary - a[axis]) / (b[axis] - a[axis]);
        clipped[next_count++] = mix(a, b, t);
      }
      if (inside_b) clipped[next_count++] = b;
    }
    count = next_count;
    for (uint i = 0; i < count; i++) polygon[i] = clipped[i];
    if (!count) return 0.0f;
  }
  float twice_area = 0.0f;
  for (uint i = 0; i < count; i++) {
    const float2 a = polygon[i], b = polygon[(i + 1) % count];
    twice_area += a.x * b.y - a.y * b.x;
  }
  return clamp(abs(twice_area) * 0.5f, 0.0f, 1.0f);
}

template<bool Task = false>
inline D3DALLOYMSAAMarker msaa_gather_depth_impl(
    float4 position, float center, thread const float *sample_depths,
    float2 line_start, float2 line_end, uint native_coverage,
    constant D3DALLOYMSAATile &tile, constant D3DALLOYMSAAPixelLayout &layout,
    device uint *records, bool conservative = false, bool conservative_covered = false,
    bool inner_covered = false) {
  const uint2 xy = uint2(position.xy) - uint2(tile.x, tile.y);
  const uint pixel = xy.y * 8 + xy.x;
  // ForcedSampleCount 16 gathers in two native 8x halves: native sample s is logical sample s + base.
  const uint native_count = min(tile.sample_count, 8u);
  const uint base = tile.sample_count > 8 ? tile.sample_base : 0u;
  const uint logical_full = tile.sample_count >= 32 ? ~0u : (1u << tile.sample_count) - 1u;
  const float alpha = layout.line_mode == 2
      ? d3dalloy_line_alpha_area(line_start, line_end, floor(position.xy)) : 1.0f;
  // Conservative rasterization covers every sample of a rasterized pixel (the replay geometry covers them all).
  const uint geometric = conservative ? (conservative_covered ? logical_full : 0u)
      : layout.line_mode == 1
      ? (d3dalloy_line_diamond_exit(line_start, line_end, floor(position.xy) + 0.5f) ? logical_full : 0u)
      : layout.line_mode == 2 ? (alpha > 0.0f ? logical_full : 0u)
      : native_coverage << base;
  const uint coverage = ((geometric & tile.sample_mask) >> base) & ((1u << native_count) - 1u);
  const float anchor = layout.line_mode
      ? fma(center, tile.max_depth - tile.min_depth, tile.min_depth) : position.z;
  // Conservative rasterization extrapolates the plane beyond the primitive; D3D clamps it to the viewport range
  // (the Clamp-mode fragment position is not clamped, clampz probe).
  const float depth_low = min(tile.min_depth, tile.max_depth), depth_high = max(tile.min_depth, tile.max_depth);
  float sample_z[8];
  float derivative_x[9], derivative_y[9];
  derivative_x[0] = dfdx(center);
  derivative_y[0] = dfdy(center);
  for (uint sample = 0; sample < native_count; ++sample) {
    const float ndc = sample_depths[sample];
    sample_z[sample] = layout.line_mode
        ? fma(ndc, tile.max_depth - tile.min_depth, tile.min_depth)
        : fma(ndc - center, tile.max_depth - tile.min_depth, anchor);
    if (conservative) sample_z[sample] = clamp(sample_z[sample], depth_low, depth_high);
    derivative_x[sample + 1] = dfdx(ndc);
    derivative_y[sample + 1] = dfdy(ndc);
  }
  const float published_anchor = conservative ? clamp(anchor, depth_low, depth_high) : anchor;
  uint2 helper_xy[4];
  float helper_center_z[4], helper_sample_z[4][8];
  for (ushort lane = 0; lane < 4; ++lane) {
    helper_xy[lane] = uint2(quad_shuffle(position.xy, lane)) - uint2(tile.x, tile.y);
    helper_center_z[lane] = quad_shuffle(published_anchor, lane);
    for (uint sample = 0; sample < native_count; ++sample)
      helper_sample_z[lane][sample] = quad_shuffle(sample_z[sample], lane);
  }
  if constexpr (Task) {
    // Complete derivative and quad exchange before the exact output kill so
    // live lanes retain the original helper values at tile/scissor edges.
    if (!msaa_task_contains(uint2(position.xy), tile)) {
      discard_fragment();
      return {0, 0};
    }
  }
  if (xy.x < 8 && xy.y < 8) {
    device uint *record = records + pixel * layout.input_record_dwords;
    if (layout.line_mask_dword != 0xffffffffu)
      atomic_store_explicit((device atomic_uint *)(record + layout.line_mask_dword),
                            geometric, memory_order_relaxed);
    if (layout.line_alpha_dword != 0xffffffffu)
      atomic_store_explicit((device atomic_uint *)(record + layout.line_alpha_dword),
                            as_type<uint>(alpha), memory_order_relaxed);
    if (layout.inner_mask_dword != 0xffffffffu)
      atomic_store_explicit((device atomic_uint *)(record + layout.inner_mask_dword),
                            inner_covered ? 1u : 0u, memory_order_relaxed);
    for (uint i = 0; i <= native_count; ++i) {
      if (i == 0 && base) continue;
      const uint slot = i ? i + base : 0u;
      atomic_store_explicit((device atomic_uint *)(record + layout.z_anchor_dword + slot * 2),
                            as_type<uint>(derivative_x[i]), memory_order_relaxed);
      atomic_store_explicit((device atomic_uint *)(record + layout.z_anchor_dword + slot * 2 + 1),
                            as_type<uint>(derivative_y[i]), memory_order_relaxed);
    }
    // The claim makes the first native invocation of a pass publish Z. The second 16x half has its own samples
    // and stores them unconditionally: every invocation interpolates the identical plane.
    if (base || atomic_exchange_explicit((device atomic_uint *)(record + layout.z_claim_dword),
                                         1u, memory_order_relaxed) == 0u) {
      if (!base) record[layout.center_z_dword] = as_type<uint>(published_anchor);
      for (uint sample = 0; sample < native_count; ++sample)
        record[layout.sample_z_dword + base + sample] = as_type<uint>(sample_z[sample]);
    }
  }
  // Helpers cannot store directly; active quad lanes transfer each neighbor's
  // separately captured values into a distinct helper-record bank.
  for (ushort lane = 0; lane < 4; ++lane) {
    const uint2 target = helper_xy[lane];
    if (target.x >= 8 || target.y >= 8) continue;
    device uint *helper = records + (layout.helper_record_base + target.y * 8 + target.x) *
                                   layout.input_record_dwords;
    if (!base && atomic_exchange_explicit((device atomic_uint *)(helper + layout.z_claim_dword),
                                          1u, memory_order_relaxed) != 0u) continue;
    if (!base) helper[layout.center_z_dword] = as_type<uint>(helper_center_z[lane]);
    for (uint sample = 0; sample < native_count; ++sample)
      helper[layout.sample_z_dword + base + sample] = as_type<uint>(helper_sample_z[lane][sample]);
  }
  return {xy.x < 8 && xy.y < 8 ? 1u : 0u, coverage};
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_depth(
    D3DALLOYMSAANoPixelInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device uint *records [[buffer(21)]]) {
  float depths[8];
  for (uint sample = 0; sample < min(tile.sample_count, 8u); ++sample)
    depths[sample] = input.ndc_depth.interpolate_at_sample(sample);
  return msaa_gather_depth_impl(input.position, input.ndc_depth.interpolate_at_center(), depths,
                                float2(0), float2(0),
                                native_coverage, tile, layout, records);
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_depth_task(
    D3DALLOYMSAANoPixelInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device uint *records [[buffer(21)]]) {
  float depths[8];
  for (uint sample = 0; sample < min(tile.sample_count, 8u); ++sample)
    depths[sample] = input.ndc_depth.interpolate_at_sample(sample);
  return msaa_gather_depth_impl<true>(input.position, input.ndc_depth.interpolate_at_center(), depths,
                                      float2(0), float2(0), native_coverage, tile, layout, records);
}

template<bool Task>
inline D3DALLOYMSAAMarker msaa_gather_line_depth_impl(
    D3DALLOYMSAALineInput input, uint native_coverage,
    constant D3DALLOYMSAATile &tile, constant D3DALLOYMSAAPixelLayout &layout, device uint *records) {
  const float2 direction = input.end - input.start;
  const float length_squared = dot(direction, direction);
  const float inverse_length_squared = length_squared > 0.0f ? 1.0f / length_squared : 0.0f;
  const float2 screen_center = input.carrier_screen.interpolate_at_center();
  const float q = dot(screen_center - input.start, direction) * inverse_length_squared;
  const float center = mix(input.depth_pair.x, input.depth_pair.y, q);
  float depths[8];
  for (uint sample = 0; sample < min(tile.sample_count, 8u); ++sample) {
    const float2 screen_sample = input.carrier_screen.interpolate_at_sample(sample);
    const float sample_q = dot(screen_sample - input.start, direction) * inverse_length_squared;
    depths[sample] = mix(input.depth_pair.x, input.depth_pair.y, sample_q);
  }
  return msaa_gather_depth_impl<Task>(input.position, center, depths, input.start, input.end,
                                      native_coverage, tile, layout, records);
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_line_depth(
    D3DALLOYMSAALineInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]], device uint *records [[buffer(21)]]) {
  return msaa_gather_line_depth_impl<false>(input, native_coverage, tile, layout, records);
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_line_depth_task(
    D3DALLOYMSAALineInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]], device uint *records [[buffer(21)]]) {
  return msaa_gather_line_depth_impl<true>(input, native_coverage, tile, layout, records);
}

/* Flat replay outputs of a conservative triangle (msaa_vertex.cpp expandedConservative): the primitive's clip
 * positions, its clip distances (vertex v, distance k at component v * 8 + k) and flags (bit 3 depth clipping,
 * bits 4-7 clip distance count). */
struct D3DALLOYMSAAConservativeInput {
  float4 position [[position]];
  float4 p0 [[user(msaa_conservative_p0), flat]];
  float4 p1 [[user(msaa_conservative_p1), flat]];
  float4 p2 [[user(msaa_conservative_p2), flat]];
  float4 clip0 [[user(msaa_conservative_clip0), flat]];
  float4 clip1 [[user(msaa_conservative_clip1), flat]];
  float4 clip2 [[user(msaa_conservative_clip2), flat]];
  float4 clip3 [[user(msaa_conservative_clip3), flat]];
  float4 clip4 [[user(msaa_conservative_clip4), flat]];
  float4 clip5 [[user(msaa_conservative_clip5), flat]];
  uint flags [[user(msaa_conservative_flags), flat]];
  interpolant<float2, interpolation::no_perspective> carrier_screen [[user(msaa_carrier_screen)]];
};

// Conservative rasterization: a pixel is covered (every sample) when its square overlaps the clipped, snapped
// primitive (conservative_polygon.h). Depth is the primitive's plane, extrapolated to every sample.
template<bool Task>
inline D3DALLOYMSAAMarker msaa_gather_conservative_depth_impl(
    D3DALLOYMSAAConservativeInput input, uint native_coverage,
    constant D3DALLOYMSAATile &tile, constant D3DALLOYMSAAPixelLayout &layout, device uint *records) {
  const float4 clip_vectors[6] = {input.clip0, input.clip1, input.clip2, input.clip3, input.clip4, input.clip5};
  float clip[24];
  for (uint i = 0; i < 24; i++) clip[i] = clip_vectors[i / 4][i % 4];
  const float4 viewport = float4(tile.viewport_origin_x, tile.viewport_origin_y, tile.viewport_width,
                                 tile.viewport_height);
  float det;
  float2 polygon[d3dalloy_conservative::kMaxVertices];
  const uint count = d3dalloy_conservative::clipped_polygon(input.p0, input.p1, input.p2, viewport, clip,
                                                        (input.flags >> 4) & 15, input.flags & 8, polygon, det);
  const long area = count ? d3dalloy_conservative::polygon_area(polygon, count) : 0;
  bool inner = false;
  const bool covered = count && d3dalloy_conservative::square_coverage(
      polygon, count, floor(input.position.xy), area, inner);
  // Native depth interpolation on the expanded fan quantizes its plane independently.
  // Derive the D3D plane from the original snapped vertices instead, using the
  // carrier only to recover the render pass's actual sample locations.
  const float4 positions[3] = {input.p0, input.p1, input.p2};
  float3 screen[3];
  for (uint i = 0; i < 3; ++i) {
    const float4 p = positions[i];
    float2 numerator = viewport.xy * p.w + float2(p.x + p.w, p.w - p.y) * viewport.zw * 0.5f;
    if (p.w != 0.0f) numerator = rint(d3dalloy_conservative::screen_of(p, viewport) * 256.0f) / 256.0f * p.w;
    screen[i] = float3(numerator, p.w);
  }
  const float3 c0 = cross(screen[1], screen[2]);
  const float denominator = dot(screen[0], c0);
  const bool degenerate = !area ||
      d3dalloy_conservative::snapped_triangle_degenerate(input.p0, input.p1, input.p2, viewport);
  float3 plane;
  if (degenerate)
    plane = float3(0, 0, input.p0.z / (input.p0.w != 0.0f ? input.p0.w : 1.0f));
  else if (denominator != 0.0f)
    plane = (c0 * input.p0.z + cross(screen[2], screen[0]) * input.p1.z +
             cross(screen[0], screen[1]) * input.p2.z) / denominator;
  else
    plane = float3(0, 0, input.p0.z / (input.p0.w != 0.0f ? input.p0.w : 1.0f));
  const float2 gradient = plane.xy;
  const float2 center_xy = floor(input.position.xy) + 0.5f;
  const float center = dot(float3(center_xy, 1.0f), plane);
  const float2 carrier_center = input.carrier_screen.interpolate_at_center();
  float depths[8];
  for (uint sample = 0; sample < min(tile.sample_count, 8u); ++sample)
    depths[sample] = center + dot(input.carrier_screen.interpolate_at_sample(sample) - carrier_center, gradient);
  float4 position = input.position;
  position.z = fma(center, tile.max_depth - tile.min_depth, tile.min_depth);
  return msaa_gather_depth_impl<Task>(position, center, depths,
                                      float2(0), float2(0), native_coverage, tile, layout, records, true, covered, inner);
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_conservative_depth(
    D3DALLOYMSAAConservativeInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]], device uint *records [[buffer(21)]]) {
  return msaa_gather_conservative_depth_impl<false>(input, native_coverage, tile, layout, records);
}

[[fragment]] D3DALLOYMSAAMarker msaa_gather_conservative_depth_task(
    D3DALLOYMSAAConservativeInput input [[stage_in]], uint native_coverage [[sample_mask]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]], device uint *records [[buffer(21)]]) {
  return msaa_gather_conservative_depth_impl<true>(input, native_coverage, tile, layout, records);
}

struct D3DALLOYMSAADepthMarker {
  uint value [[color(0)]];
  float depth [[depth(any)]];
  uint mask [[sample_mask]];
};

template<bool Task = false, typename Coverage>
inline D3DALLOYMSAADepthMarker msaa_explicit_depth_impl(
    float4 position, uint sample, constant D3DALLOYMSAATile &tile,
    constant D3DALLOYMSAAPixelLayout &layout, device const uint *records, Coverage coverage) {
  const uint2 xy = uint2(position.xy);
  if constexpr (Task) {
    if (!msaa_task_contains(xy, tile) || sample >= tile.sample_count) {
      discard_fragment();
      return {0, 0, 0};
    }
  }
  uint mask = msaa_coverage_read(coverage, xy, sample, Task ? tile.target_layer : 0u).x ? 1u << sample : 0u;
  const uint2 local = xy - uint2(tile.x, tile.y);
  if (local.x >= 8 || local.y >= 8 || sample >= tile.sample_count) return {0, 0, 0};
  const device uint *record = records + (local.y * 8 + local.x) * layout.input_record_dwords;
  if (tile.depth_bounds_enabled && layout.depth_bounds_mask_dword != 0xffffffffu)
    mask &= record[layout.depth_bounds_mask_dword];
  // Sample-mask output alone is not a kill before stencil commit. This internal pass performs the
  // application early depth/stencil work, so bounds-failing samples must be discarded here, before
  // returning any depth or coverage. The deferred application PS/UAV sees only the surviving marker.
  if (!mask) discard_fragment();
  const float raw_z = as_type<float>(record[layout.sample_z_dword + sample]);
  return {1u, clamp(raw_z, tile.min_depth, tile.max_depth), mask};
}

[[fragment]] D3DALLOYMSAADepthMarker msaa_explicit_depth(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_ms<uint, access::read> coverage [[texture(24)]]) {
  return msaa_explicit_depth_impl(position, sample, tile, layout, records, coverage);
}

[[fragment]] D3DALLOYMSAADepthMarker msaa_explicit_depth_single(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d<uint, access::read> coverage [[texture(24)]]) {
  return msaa_explicit_depth_impl(position, sample, tile, layout, records, coverage);
}

struct D3DALLOYMSAADepthOnly {
  float depth [[depth(any)]];
  uint mask [[sample_mask]];
};

[[fragment]] D3DALLOYMSAADepthOnly msaa_explicit_depth_only(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_ms<uint, access::read> coverage [[texture(24)]]) {
  const auto result = msaa_explicit_depth_impl(position, sample, tile, layout, records, coverage);
  return {result.depth, result.mask};
}

[[fragment]] D3DALLOYMSAADepthOnly msaa_explicit_depth_only_single(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d<uint, access::read> coverage [[texture(24)]]) {
  const auto result = msaa_explicit_depth_impl(position, sample, tile, layout, records, coverage);
  return {result.depth, result.mask};
}

// The task family reads full coverage arrays. Legacy helpers keep their
// single-layer views and their existing entry points for the fallback path.
[[fragment]] D3DALLOYMSAADepthMarker msaa_explicit_depth_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_ms_array<uint, access::read> coverage [[texture(24)]]) {
  return msaa_explicit_depth_impl<true>(position, sample, tile, layout, records, coverage);
}

[[fragment]] D3DALLOYMSAADepthMarker msaa_explicit_depth_single_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_array<uint, access::read> coverage [[texture(24)]]) {
  return msaa_explicit_depth_impl<true>(position, sample, tile, layout, records, coverage);
}

[[fragment]] D3DALLOYMSAADepthOnly msaa_explicit_depth_only_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_ms_array<uint, access::read> coverage [[texture(24)]]) {
  const auto result = msaa_explicit_depth_impl<true>(position, sample, tile, layout, records, coverage);
  return {result.depth, result.mask};
}

[[fragment]] D3DALLOYMSAADepthOnly msaa_explicit_depth_only_single_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *records [[buffer(21)]],
    texture2d_array<uint, access::read> coverage [[texture(24)]]) {
  const auto result = msaa_explicit_depth_impl<true>(position, sample, tile, layout, records, coverage);
  return {result.depth, result.mask};
}

[[fragment]] D3DALLOYMSAAQueryCoverage msaa_query_coverage_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *outputs [[buffer(22)]], device const uint *inputs [[buffer(21)]],
    texture2d_ms_array<uint, access::read> coverage [[texture(24)]]) {
  return msaa_query_coverage_impl<true>(position, sample, tile, layout, outputs, coverage, inputs);
}

[[fragment]] D3DALLOYMSAAQueryCoverage msaa_query_coverage_single_task(
    float4 position [[position]], uint sample [[sample_id]],
    constant D3DALLOYMSAATile &tile [[buffer(23)]],
    constant D3DALLOYMSAAPixelLayout &layout [[buffer(28)]],
    device const uint *outputs [[buffer(22)]], device const uint *inputs [[buffer(21)]],
    texture2d_array<uint, access::read> coverage [[texture(24)]]) {
  return msaa_query_coverage_impl<true>(position, sample, tile, layout, outputs, coverage, inputs);
}

[[kernel]] void clear_texture_1d_uint(
    texture1d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  if(width > pos) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_1d_array_uint(
    texture1d_array<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint array_len = tex.get_array_size();
  if(width > pos.x && array_len > pos.y) {
    tex.write(value, pos.x, pos.y);
  }
}

[[kernel]] void clear_texture_2d_uint(
    texture2d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  if(width > pos.x && height > pos.y) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_2d_array_uint(
    texture2d_array<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint array_len = tex.get_array_size();
  if(width > pos.x && height > pos.y && array_len > pos.z) {
    tex.write(value, pos.xy, pos.z);
  }
}

[[kernel]] void clear_texture_3d_uint(
    texture3d<uint, access::read_write> tex [[texture(0)]],
    constant uint4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint depth = tex.get_depth();
  if(width > pos.x && height > pos.y && depth > pos.z) {
    tex.write(value, pos);
  }
}

struct D3DalloyClearTextureBufferUInt {
  uint4 value;
  uint offset;
  uint size;
};

[[kernel]] void clear_texture_buffer_uint(
    texture_buffer<uint, access::read_write> tex [[texture(0)]],
    constant D3DalloyClearTextureBufferUInt& args [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint offset = pos + args.offset;
  if (args.size > pos && width > offset) {
    tex.write(args.value, offset);
  }
}

[[kernel]] void clear_buffer_uint(
    device uint* tex [[buffer(0)]],
    constant uint& size [[buffer(2)]],
    constant uint4& value [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if(pos < size) {
    tex[pos] = value.x;
  }
}

[[kernel]] void query_binary_normalize(
    device const ulong* source [[buffer(0)]],
    device ulong* destination [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  destination[pos] = source[pos] ? 1ul : 0ul;
}

// D3D12 vertex stream output (see d3d12_command_encoder.hpp StreamOutputDrawData).
struct D3DALLOYSOElement {
  uint slot;
  uint byte_offset;
  uint dword_offset;
  uint component_count;
};

struct D3DALLOYSOAssembly {
  uint topology; // 0 point, 1/2 line list/strip, 3/4 triangle list/strip, 5 fan, 6/7 line adj, 8/9 triangle adj
  uint indexed;
  uint index_size; // 2/4: native index data; 8: GPU-selected bounded index-view control
  uint cut_enabled; // strip topology with a matching D3D12 IBStripCutValue
  uint cut_value;
  uint vertex_count; // per instance: VertexCountPerInstance / IndexCountPerInstance
  uint instance_count;
  uint record_dwords;
  uint element_count;
  uint referenced_mask;
  uint bound_mask;
  uint predicate_enabled;
  uint predicate_not_equal;
  uint index_skip; // bytes from the 4-aligned index binding to the first index
  uint padding[2];
  ulong size[4];
  uint stride[4];
  D3DALLOYSOElement elements[128];
  // Capture tail: float4 SV_Position, float[8] SV_ClipDistance, float[8] SV_CullDistance.
  uint statistics_capture_offset; // dwords from the start of each captured record
  uint clip_mask;
  uint cull_mask;
  uint statistics_enabled;
  uint rasterization_enabled;
  uint depth_clip_enabled;
  uint stream_output_enabled;
  uint statistics_padding;
};

struct D3DALLOYSOControl {
  uint positions_per_instance;
  uint first_position;
  uint record_count;
  uint index_size;
  uint index_first;
  int base_vertex;
  uint padding[2];
};

inline uint d3dalloy_so_read_index(device const uchar *indices, uint size, uint k) {
  if (size == 8) {
    device const uint *view = reinterpret_cast<device const uint *>(indices);
    const ulong address = ulong(view[0]) | (ulong(view[1]) << 32);
    const uint width = view[3] == 42 ? 4 : 2; // DXGI_FORMAT_R32_UINT / R16_UINT
    const uint offset = (view[4] + k) * width;
    if (!address || (view[3] != 42 && view[3] != 57) || view[2] < width || offset > view[2] - width)
      return 0;
    device const uchar *at = reinterpret_cast<device const uchar *>(address + ulong(offset));
    const uint low = uint(at[0]) | (uint(at[1]) << 8);
    return width == 4 ? low | (uint(at[2]) << 16) | (uint(at[3]) << 24) : low;
  }
  if (size == 2)
    return uint(indices[2 * k]) | (uint(indices[2 * k + 1]) << 8);
  return uint(indices[4 * k]) | (uint(indices[4 * k + 1]) << 8) | (uint(indices[4 * k + 2]) << 16) |
         (uint(indices[4 * k + 3]) << 24);
}

// IA positions of an indexed stream-output draw: the draw is issued over this uint32 buffer so
// Metal's vertex id is the position k; a D3D12 strip cut (strip topologies only) becomes Metal's
// 0xFFFFFFFF restart, and every other index value, 0xFFFF included, is an ordinary vertex.
[[kernel]] void so_index_positions(
    constant D3DALLOYSOAssembly &params [[buffer(0)]],
    device const uchar *indices [[buffer(1)]],
    device uint *positions [[buffer(2)]],
    uint k [[thread_position_in_grid]]
) {
  if (k >= params.vertex_count)
    return;
  indices += params.index_skip;
  const uint index = d3dalloy_so_read_index(indices, params.index_size, k);
  positions[k] = params.cut_enabled && index == params.cut_value ? 0xffffffffu : k;
}

// Mirrors MSAAPrimitiveAssemblyParams and MSAAPrimitiveIndirectArguments. The output
// reserves one indirect draw per possible complete primitive. Empty slots are safe to
// submit without asking the CPU whether an indirect count, predicate or cut removed them.
struct D3DALLOYMSAAPrimitiveAssembly {
  uint topology;
  uint indexed;
  uint index_size;
  uint cut_enabled;
  uint cut_value;
  uint vertex_count;
  uint instance_count;
  uint first_index;
  uint positions_per_instance;
  uint first_position;
  uint max_primitives;
  uint predicate_enabled;
  uint predicate_not_equal;
  uint index_skip;
  uint line_mode;
  /* Triangles: 24-word slots of 18 replay fan corners (16 * slot + corner), the 3 original ordinals, 3 unused. */
  uint conservative;
};

struct D3DALLOYMSAAPrimitiveDraw {
  uint index_count;
  uint instance_count;
  uint index_start;
  int base_vertex;
  uint base_instance;
  uint primitive_id;
};

// This single ordered scan is necessary for strip restart and parity: parallel
// independent windows cannot determine the first vertex after an arbitrary cut.
[[kernel]] void msaa_assemble_primitives(
    constant D3DALLOYMSAAPrimitiveAssembly &params [[buffer(0)]],
    device const uchar *indices [[buffer(1)]],
    device const ulong *predicate [[buffer(2)]],
    device uint *replay_indices [[buffer(3)]],
    device D3DALLOYMSAAPrimitiveDraw *draws [[buffer(4)]],
    device uint3 *capture_dispatch [[buffer(5)]]
) {
  const bool line_quad = params.line_mode && (params.topology == 1 || params.topology == 2);
  const bool conservative = params.conservative && params.topology >= 3;
  const uint index_stride = line_quad ? 20 : conservative ? 24 : 3;
  for (uint i = 0; i < params.max_primitives; i++)
    draws[i] = {0, 0, index_stride * i, 0, 0, 0};
  capture_dispatch[0] = uint3(uint((ulong(params.vertex_count) * params.instance_count + 63) / 64), 1, 1);
  if (params.predicate_enabled &&
      (params.predicate_not_equal ? predicate[0] != 0 : predicate[0] == 0)) {
    capture_dispatch[0].x = 0;
    return;
  }
  indices += params.index_skip;
  const uint vpp = params.topology == 0 ? 1 : params.topology <= 2 ? 2 : 3;
  const bool strip = params.topology == 2 || params.topology == 4 || params.topology == 5;
  uint slot = 0;
  for (uint instance = 0; instance < params.instance_count; instance++) {
    uint primitive_id = 0;
    uint window[3];
    uint count = 0;
    uint strip_index = 0;
    for (uint k = 0; k < params.vertex_count; k++) {
      if (params.cut_enabled && params.indexed &&
          d3dalloy_so_read_index(indices, params.index_size, params.first_index + k) == params.cut_value) {
        count = 0;
        strip_index = 0;
        continue;
      }
      const uint ordinal = instance * params.positions_per_instance + k - params.first_position;
      uint primitive[3];
      bool complete = false;
      if (!strip) {
        window[count++] = ordinal;
        if (count == vpp) {
          for (uint v = 0; v < vpp; v++) primitive[v] = window[v];
          count = 0;
          complete = true;
        }
      } else if (params.topology == 2) {
        if (!count) {
          window[0] = ordinal;
          count = 1;
        } else {
          primitive[0] = window[0];
          primitive[1] = ordinal;
          window[0] = ordinal;
          strip_index++;
          complete = true;
        }
      } else if (count < 2) {
        window[count++] = ordinal;
      } else if (params.topology == 5) {
        primitive[0] = window[1];
        primitive[1] = ordinal;
        primitive[2] = window[0];
        window[1] = ordinal;
        strip_index++;
        complete = true;
      } else {
        primitive[0] = window[0];
        primitive[1] = (strip_index & 1) ? ordinal : window[1];
        primitive[2] = (strip_index & 1) ? window[1] : ordinal;
        window[0] = window[1];
        window[1] = ordinal;
        strip_index++;
        complete = true;
      }
      if (!complete) continue;
      if (slot >= params.max_primitives) { primitive_id++; continue; }
      if (line_quad) {
        const uint corners[18] = {0, 1, 2, 2, 1, 3,
                                  2, 3, 4, 4, 3, 5,
                                  4, 5, 6, 6, 5, 7};
        const uint first = params.line_mode == 3 ? 6 : 0;
        const uint count = params.line_mode == 3 ? 6 : 18;
        for (uint v = 0; v < count; v++)
          replay_indices[20 * slot + v] = 8 * slot + corners[first + v];
        replay_indices[20 * slot + 18] = primitive[0];
        replay_indices[20 * slot + 19] = primitive[1];
      } else if (conservative) {
        // Fans (0, 1, 2), (0, 2, 3), (0, 3, 4) over corners 0-4 and 5-9 of the replay geometry
        // (d3dalloy.conservative.corner).
        const uint corners[9] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
        for (uint v = 0; v < 18; v++)
          replay_indices[24 * slot + v] = 16 * slot + corners[v % 9] + (v < 9 ? 0 : 5);
        for (uint v = 0; v < 3; v++)
          replay_indices[24 * slot + 18 + v] = primitive[v];
      } else {
        for (uint v = 0; v < vpp; v++) replay_indices[3 * slot + v] = primitive[v];
      }
      draws[slot] = {line_quad ? (params.line_mode == 3 ? 6u : 18u) : conservative ? 18u : vpp,
                     1, index_stride * slot, 0, 0, primitive_id++};
      slot++;
    }
  }
}

struct D3DALLOYMSAABoundsParams {
  uint record_dwords;
  uint position_offset;
  uint viewport_index_offset;
  uint target_width;
  uint target_height;
  uint viewport_count;
  uint scissor_count;
  uint max_primitives;
  uint render_target_array_index_offset;
  uint target_array_length;
  uint line_mode;
  /* Triangles use 24-word conservative slots (MSAAPrimitiveAssemblyParams::conservative). */
  uint conservative;
  float4 viewports[16];
  uint4 scissors[16];
  uint view_viewport_offset;
  uint view_layer_offset;
  uint view_offsets_enabled;
  uint primitive_id_offset;
};

struct D3DALLOYMSAATileBounds {
  uint left, top, right, bottom, viewport_index, target_layer, primitive_id;
};

// Execute after VS capture and primitive assembly. GPU tile-task generation
// consumes private bounds; only the legacy path maps them on the FIFO worker
// after producer retirement. Ambiguous projection/clipping keeps a viewport
// bound instead of dropping a sample at an edge.
[[kernel]] void msaa_primitive_bounds(
    constant D3DALLOYMSAABoundsParams &params [[buffer(0)]],
    device const uint *vertices [[buffer(1)]],
    device const uint *indices [[buffer(2)]],
    device const D3DALLOYMSAAPrimitiveDraw *draws [[buffer(3)]],
    device D3DALLOYMSAATileBounds *bounds [[buffer(4)]],
    uint primitive [[thread_position_in_grid]]
) {
  if (primitive >= params.max_primitives) return;
  D3DALLOYMSAATileBounds result = {0, 0, 0, 0, 0, 0, 0};
  const uint count = params.line_mode && draws[primitive].index_count ? 2
      : params.conservative && draws[primitive].index_count ? 3 : draws[primitive].index_count;
  if (!count || !params.viewport_count || !params.target_width || !params.target_height) {
    bounds[primitive] = result;
    return;
  }
  const uint index_base = params.line_mode ? 20 * primitive + 18
      : params.conservative ? 24 * primitive + 18 : 3 * primitive;
  const uint first = indices[index_base];
  // Native mesh capture leaves unused primitive slots zeroed. A zero homogeneous
  // position for every vertex cannot survive D3D clipping; do not replay that slot.
  if (params.position_offset != 0xffffffffu) {
    bool empty = true;
    for (uint i = 0; i < count && empty; ++i) {
      device const uint *position = vertices + ulong(indices[index_base + i]) * params.record_dwords + params.position_offset;
      empty = !(position[0] | position[1] | position[2] | position[3]);
    }
    if (empty) { bounds[primitive] = result; return; }
  }
  uint viewport_index = 0;
  if (params.viewport_index_offset != 0xffffffffu)
    viewport_index = vertices[ulong(first) * params.record_dwords + params.viewport_index_offset];
  uint target_layer = 0;
  if (params.render_target_array_index_offset != 0xffffffffu)
    target_layer = vertices[ulong(first) * params.record_dwords + params.render_target_array_index_offset];
  const uint layer_count = max(1u, params.target_array_length);
  if (params.view_offsets_enabled) {
    // View instance location: the sum selects index 0 when out of range, exactly as the amplification
    // mapping of the replay pipelines does.
    viewport_index += params.view_viewport_offset;
    target_layer += params.view_layer_offset;
    if (viewport_index >= params.viewport_count) viewport_index = 0;
    if (target_layer >= layer_count) target_layer = 0;
  }
  viewport_index = min(viewport_index, params.viewport_count - 1);
  result.viewport_index = viewport_index;
  result.primitive_id = params.primitive_id_offset == 0xffffffffu ? draws[primitive].primitive_id
      : vertices[ulong(first) * params.record_dwords + params.primitive_id_offset];
  result.target_layer = min(target_layer, layer_count - 1);
  const float4 viewport = params.viewports[viewport_index];
  float2 low = float2(INFINITY);
  float2 high = float2(-INFINITY);
  bool uncertain = params.position_offset == 0xffffffffu;
  for (uint i = 0; i < count && !uncertain; i++) {
    device const uint *record = vertices + ulong(indices[index_base + i]) * params.record_dwords;
    const uint offset = params.position_offset;
    const float4 position = float4(as_type<float>(record[offset]), as_type<float>(record[offset + 1]),
                                   as_type<float>(record[offset + 2]), as_type<float>(record[offset + 3]));
    if (!all(isfinite(position)) || position.w <= 0.0f) {
      uncertain = true;
      break;
    }
    const float2 ndc = position.xy / position.w;
    const float2 screen = viewport.xy + float2((ndc.x + 1.0f) * viewport.z * 0.5f,
                                                (1.0f - ndc.y) * viewport.w * 0.5f);
    low = min(low, screen);
    high = max(high, screen);
  }
  if (uncertain) {
    low = min(viewport.xy, viewport.xy + viewport.zw);
    high = max(viewport.xy, viewport.xy + viewport.zw);
  }
  // Two-pixel conservative fringe includes sample locations, line diamond
  // exit and finite float-to-fixed raster coordinate conversion.
  const float2 fringe = float2(2.0f);
  low = clamp(floor(low - fringe), float2(0.0f), float2(params.target_width, params.target_height));
  high = clamp(ceil(high + fringe), float2(0.0f), float2(params.target_width, params.target_height));
  uint4 rect = uint4(uint2(low), uint2(high));
  if (viewport_index < params.scissor_count) {
    const uint4 scissor = params.scissors[viewport_index];
    rect.xy = max(rect.xy, scissor.xy);
    rect.zw = min(rect.zw, scissor.zw);
  }
  if (any(rect.zw <= rect.xy)) {
    bounds[primitive] = result;
    return;
  }
  result.left = rect.x & ~7u;
  result.top = rect.y & ~7u;
  result.right = min((rect.z + 7u) & ~7u, params.target_width);
  result.bottom = min((rect.w + 7u) & ~7u, params.target_height);
  bounds[primitive] = result;
}

inline void d3dalloy_so_store_dword(device uchar *target, ulong offset, uint value) {
  target[offset] = uchar(value);
  target[offset + 1] = uchar(value >> 8);
  target[offset + 2] = uchar(value >> 16);
  target[offset + 3] = uchar(value >> 24);
}

inline uint d3dalloy_so_load_dword(device const uchar *source, ulong offset) {
  return uint(source[offset]) | (uint(source[offset + 1]) << 8) | (uint(source[offset + 2]) << 16) |
         (uint(source[offset + 3]) << 24);
}

#pragma METAL fp math_mode(safe)

struct D3DalloyClipVertex {
  float4 position;
  float clip[8];
};

inline float d3dalloy_clip_plane(D3DalloyClipVertex point, uint plane) {
  if (plane == 0) return point.position.z;
  if (plane == 1) return point.position.w - point.position.z;
  if (plane == 2) return point.position.w;
  return point.clip[plane - 3];
}

inline bool d3dalloy_clip_active(constant D3DALLOYSOAssembly &params, uint plane) {
  return plane == 2 || (plane < 2 && params.depth_clip_enabled) ||
         (plane >= 3 && (params.clip_mask & (1u << (plane - 3))));
}

inline bool d3dalloy_clip_inside(float distance, uint plane) {
  return plane == 2 ? distance > 0.0f : distance >= 0.0f;
}

inline D3DalloyClipVertex d3dalloy_clip_interpolate(D3DalloyClipVertex a, D3DalloyClipVertex b, float t) {
  D3DalloyClipVertex out;
  out.position = mix(a.position, b.position, t);
  for (uint i = 0; i < 8; i++) out.clip[i] = mix(a.clip[i], b.clip[i], t);
  return out;
}

// A primitive reaches the clipper before face/zero-area culling. X/Y clipping is left to Metal's
// guard band (its width is implementation-dependent); Z and user planes are independent of it.
inline uint d3dalloy_clipped_primitives(constant D3DALLOYSOAssembly &params,
                                    device const uint *scratch, thread const uint *primitive, uint vpp) {
  D3DalloyClipVertex vertices[3];
  device const uint *records[3];
  for (uint v = 0; v < vpp; v++) {
    records[v] = scratch + ulong(primitive[v]) * params.record_dwords + params.statistics_capture_offset;
    for (uint c = 0; c < 4; c++) vertices[v].position[c] = as_type<float>(records[v][c]);
    if (any(isnan(vertices[v].position))) return 0;
    // FS 15.4 permits discarding XYZ-infinite input. Do not replace its finite Z/W
    // components with AIR's rasterization workaround and invent surviving geometry.
    if (any(isinf(vertices[v].position.xyz))) return 0;
    for (uint c = 0; c < 8; c++) {
      vertices[v].clip[c] = (params.clip_mask & (1u << c)) ? as_type<float>(records[v][4 + c]) : 0.0f;
      if ((params.clip_mask & (1u << c)) && isnan(vertices[v].clip[c])) return 0;
    }
  }
  // A cull plane discards the whole primitive iff every vertex is outside that plane.
  // NaN is outside, rather than an intersection with a well-defined boundary.
  for (uint c = 0; c < 8; c++) {
    if (!(params.cull_mask & (1u << c))) continue;
    bool all_outside = true;
    for (uint v = 0; v < vpp; v++)
      all_outside = all_outside && !(as_type<float>(records[v][12 + c]) >= 0.0f);
    if (all_outside) return 0;
  }

  if (vpp == 1) {
    for (uint p = 0; p < 11; p++)
      if (d3dalloy_clip_active(params, p) && !d3dalloy_clip_inside(d3dalloy_clip_plane(vertices[0], p), p))
        return 0;
    return 1;
  }
  if (vpp == 2) {
    float begin = 0.0f, end = 1.0f;
    for (uint p = 0; p < 11; p++) {
      if (!d3dalloy_clip_active(params, p)) continue;
      float a = d3dalloy_clip_plane(vertices[0], p), b = d3dalloy_clip_plane(vertices[1], p);
      bool inside_a = d3dalloy_clip_inside(a, p), inside_b = d3dalloy_clip_inside(b, p);
      if (!inside_a && !inside_b) return 0;
      if (inside_a == inside_b) continue;
      float t = a / (a - b);
      if (!inside_a) begin = max(begin, t);
      else end = min(end, t);
      if (begin > end) return 0;
    }
    // W is a strict half-space: a segment reduced to its W=0 intersection is empty.
    float w0 = vertices[0].position.w, w1 = vertices[1].position.w;
    if (w0 <= 0.0f && w1 > 0.0f && end <= -w0 / (w1 - w0)) return 0;
    if (w0 > 0.0f && w1 <= 0.0f && begin >= -w0 / (w1 - w0)) return 0;
    return any(isnan(d3dalloy_clip_interpolate(vertices[0], vertices[1], begin).position)) ||
           any(isnan(d3dalloy_clip_interpolate(vertices[0], vertices[1], end).position)) ? 0 : 1;
  }

  // Convex polygon clipping. Each plane can add at most one vertex; three input vertices plus
  // 2 depth, W, and 8 user planes fit in these 16-element arrays.
  D3DalloyClipVertex polygon[16], next[16];
  for (uint v = 0; v < 3; v++) polygon[v] = vertices[v];
  uint count = 3;
  for (uint p = 0; p < 11 && count; p++) {
    if (!d3dalloy_clip_active(params, p)) continue;
    uint output_count = 0;
    for (uint v = 0; v < count; v++) {
      D3DalloyClipVertex a = polygon[v], b = polygon[(v + 1) % count];
      float da = d3dalloy_clip_plane(a, p), db = d3dalloy_clip_plane(b, p);
      bool inside_a = d3dalloy_clip_inside(da, p), inside_b = d3dalloy_clip_inside(db, p);
      if (inside_a) next[output_count++] = a;
      if (inside_a != inside_b)
        next[output_count++] = d3dalloy_clip_interpolate(a, b, da / (da - db));
    }
    count = output_count;
    for (uint v = 0; v < count; v++) polygon[v] = next[v];
  }
  bool positive_w = false;
  for (uint v = 0; v < count; v++) {
    positive_w = positive_w || polygon[v].position.w > 0.0f;
    if (any(isnan(polygon[v].position))) return 0;
    for (uint c = 0; c < 8; c++)
      if ((params.clip_mask & (1u << c)) && isnan(polygon[v].clip[c])) return 0;
  }
  return count >= 3 && positive_w ? count - 2 : 0;
}

#pragma METAL fp math_mode(fast)

// Assembles the captured records of one draw into complete list primitives in IA order and appends
// them to the bound targets (D3D11.3 FS 14.5/14.6). Fixed-width lists with disjoint outputs
// use one SIMD group; the scalar path also handles strips, adjacency and missing claims.
[[kernel]] void so_assemble(
    constant D3DALLOYSOAssembly &params [[buffer(0)]],
    constant D3DALLOYSOControl &control [[buffer(1)]],
    device const uint *scratch [[buffer(2)]],
    device const uchar *indices [[buffer(3)]],
    device ulong2 *counters [[buffer(4)]],
    device const ulong *predicate [[buffer(5)]],
    device uchar *target0 [[buffer(6)]],
    device uchar *target1 [[buffer(7)]],
    device uchar *target2 [[buffer(8)]],
    device uchar *target3 [[buffer(9)]],
    device uchar *filled0 [[buffer(10)]],
    device uchar *filled1 [[buffer(11)]],
    device uchar *filled2 [[buffer(12)]],
    device uchar *filled3 [[buffer(13)]],
    device uint *status [[buffer(14)]],
    device ulong *statistics [[buffer(15)]],
    uint tid [[thread_position_in_grid]], uint group_width [[threads_per_threadgroup]], uint simd_width [[threads_per_simdgroup]]
) {
  const bool parallel = group_width == 32 && simd_width == 32;
  if (!parallel && tid) return;
  if (!tid) status[0] = 0;
  if (params.predicate_enabled && (params.predicate_not_equal ? predicate[0] != 0 : predicate[0] == 0))
    return;
  indices += params.index_skip;
  device uchar *targets[4] = {target0, target1, target2, target3};
  device uchar *filled_sizes[4] = {filled0, filled1, filled2, filled3};
  ulong filled[4] = {0, 0, 0, 0};
  if (params.stream_output_enabled)
    for (uint s = 0; s < 4; s++)
      if (params.bound_mask & (1u << s))
        filled[s] = d3dalloy_so_load_dword(filled_sizes[s], 0);

  const uint topology = params.topology;
  const uint vpp = topology == 0 ? 1 : (topology <= 2 || topology == 6 || topology == 7) ? 2 : 3;
  const uint input_vpp = topology >= 8 ? 6 : topology >= 6 ? 4 : vpp;
  if (parallel) {
    // The CPU only enables this grid for complete, non-restarting list topology.
    // Full primitive capacity is the minimum over the bound target views.
    const uint per_instance = params.vertex_count / vpp;
    const ulong needed = ulong(per_instance) * params.instance_count;
    ulong capacity = needed;
    for (uint s = 0; s < 4; ++s) if (params.referenced_mask & (1u << s)) {
      ulong room = params.size[s] - min(params.size[s], filled[s]);
      capacity = min(capacity, room / (ulong(vpp) * params.stride[s]));
    }
    // A missing capture compacts later primitives in the ordered reference.
    // Validate the writable prefix and its first overflow attempt before scattering.
    bool valid = true;
    device const uint *claims = scratch + ulong(control.record_count) * params.record_dwords;
    ulong checked = capacity + (capacity < needed);
    for (ulong p = tid; p < checked; p += 32) {
      ulong first = (p / per_instance) * control.positions_per_instance + (p % per_instance) * vpp;
      for (uint v = 0; v < vpp; ++v)
        valid &= first + v < control.record_count && claims[first + v] != 0;
    }
    if (simd_all(valid)) {
      simdgroup_barrier(mem_flags::mem_device);
      for (ulong p = tid; p < capacity; p += 32) {
        ulong first = (p / per_instance) * control.positions_per_instance + (p % per_instance) * vpp;
        for (uint v = 0; v < vpp; ++v) {
          device const uint *source = scratch + (first + v) * params.record_dwords;
          for (uint e = 0; e < params.element_count; ++e) {
            const D3DALLOYSOElement element = params.elements[e];
            ulong base = filled[element.slot] + (p * vpp + v) * params.stride[element.slot] + element.byte_offset;
            for (uint c = 0; c < element.component_count; ++c)
              d3dalloy_so_store_dword(targets[element.slot], base + 4 * c, source[element.dword_offset + c]);
          }
        }
      }
      simdgroup_barrier(mem_flags::mem_device);
      if (!tid) {
        for (uint s = 0; s < 4; ++s) if (params.referenced_mask & (1u << s))
          d3dalloy_so_store_dword(filled_sizes[s], 0, uint(filled[s] + capacity * vpp * params.stride[s]));
        counters[0] += ulong2(capacity, needed);
      }
      return;
    }
    if (tid) return;
  }
  const bool strip = topology == 2 || topology == 4 || topology == 5 || topology == 7 || topology == 9;
  // A slot the declaration writes but no buffer is bound to counts as full (14.6).
  bool stopped = (params.referenced_mask & ~params.bound_mask) != 0;
  ulong written = 0, needed = 0, unclaimed = 0;
  ulong ia_vertices = 0, ia_primitives = 0, clip_invocations = 0, clip_primitives = 0;

  for (uint instance = 0; instance < params.instance_count; instance++) {
    uint window[6];
    uint count = 0;   // vertices collected in the current list primitive / strip
    uint strip_index = 0;
    for (uint k = 0; k < params.vertex_count; k++) {
      // Records are keyed by IA position; the index value only decides strip cuts.
      if (params.cut_enabled && d3dalloy_so_read_index(indices, params.index_size, k) == params.cut_value) {
        count = 0;
        strip_index = 0;
        continue;
      }
      const uint record = instance * control.positions_per_instance + k - control.first_position;
      uint primitive[3];
      bool complete = false;
      if (topology >= 6) {
        window[count++] = record;
        if (count == input_vpp) {
          // FS ch.14: adjacency is discarded at SO; only interior vertices are written.
          if (vpp == 2) {
            primitive[0] = window[1];
            primitive[1] = window[2];
          } else {
            primitive[0] = window[0];
            primitive[1] = window[(strip_index & 1) ? 4 : 2];
            primitive[2] = window[(strip_index & 1) ? 2 : 4];
          }
          if (strip) {
            const uint step = topology == 7 ? 1 : 2;
            for (uint v = 0; v < input_vpp - step; v++)
              window[v] = window[v + step];
            count = input_vpp - step;
            strip_index++;
          } else {
            count = 0;
          }
          complete = true;
        }
      } else if (!strip) {
        window[count++] = record;
        if (count == vpp) {
          for (uint v = 0; v < vpp; v++)
            primitive[v] = window[v];
          count = 0;
          complete = true;
        }
      } else if (topology == 2) {
        if (count == 0) {
          window[0] = record;
          count = 1;
        } else {
          primitive[0] = window[0];
          primitive[1] = record;
          window[0] = record;
          strip_index++;
          complete = true;
        }
      } else {
        if (count < 2) {
          window[count++] = record;
        } else if (topology == 5) {
          primitive[0] = window[1];
          primitive[1] = record;
          primitive[2] = window[0];
          window[1] = record;
          strip_index++;
          complete = true;
        } else {
          // D3D triangle strips keep the first vertex leading: triangle i is (i, i+1, i+2) for even i
          // and (i, i+2, i+1) for odd i, so every triangle has the same winding.
          primitive[0] = window[0];
          primitive[1] = (strip_index & 1) ? record : window[1];
          primitive[2] = (strip_index & 1) ? window[1] : record;
          window[0] = window[1];
          window[1] = record;
          strip_index++;
          complete = true;
        }
      }
      if (!complete)
        continue;
      if (params.statistics_enabled) {
        ia_primitives++;
        // Strip vertices are consumed once, not once per overlapping primitive. An incomplete
        // run does not contribute, and a restart starts a new run.
        ia_vertices += !strip || strip_index == 1u ? input_vpp : topology == 9 ? 2u : 1u;
      }
      if (!params.stream_output_enabled && !params.statistics_enabled)
        continue;
      if (params.stream_output_enabled) needed++;
      // Captured clipper inputs are independent of SO buffer exhaustion. The claim check must
      // likewise precede the SO-only overflow path when pipeline statistics are requested.
      bool valid = true;
      if (params.statistics_enabled || !stopped) {
        for (uint v = 0; v < vpp; v++)
          valid = valid && primitive[v] < control.record_count;
        device const uint *claims = scratch + ulong(control.record_count) * params.record_dwords;
        for (uint v = 0; v < vpp && valid; v++)
          valid = claims[primitive[v]] != 0;
        if (!valid) unclaimed++;
      }
      if (params.statistics_enabled && valid && params.rasterization_enabled) {
        clip_invocations++;
        clip_primitives += d3dalloy_clipped_primitives(params, scratch, primitive, vpp);
      }
      if (!params.stream_output_enabled) continue;
      if (!stopped) {
        for (uint s = 0; s < 4; s++) {
          if ((params.referenced_mask & (1u << s)) &&
              (filled[s] > params.size[s] || ulong(vpp) * params.stride[s] > params.size[s] - filled[s]))
            stopped = true;
        }
      }
      if (stopped)
        continue;
      // Every vertex of a complete primitive is shaded, so its record is claimed. A missing claim is
      // a translator invariant failure: it is reported through `status` (logged by the queue) and
      // the primitive is neither written nor counted as written.
      if (!valid) {
        continue;
      }
      for (uint v = 0; v < vpp; v++) {
        device const uint *source = scratch + ulong(primitive[v]) * params.record_dwords;
        for (uint e = 0; e < params.element_count; e++) {
          const D3DALLOYSOElement element = params.elements[e];
          const ulong base = filled[element.slot] + ulong(v) * params.stride[element.slot] + element.byte_offset;
          for (uint c = 0; c < element.component_count; c++)
            d3dalloy_so_store_dword(targets[element.slot], base + 4 * c, source[element.dword_offset + c]);
        }
      }
      for (uint s = 0; s < 4; s++)
        if (params.referenced_mask & (1u << s))
          filled[s] += ulong(vpp) * params.stride[s];
      written++;
    }
  }
  if (params.stream_output_enabled) {
    for (uint s = 0; s < 4; s++)
      if ((params.referenced_mask & params.bound_mask) & (1u << s))
        d3dalloy_so_store_dword(filled_sizes[s], 0, uint(filled[s]));
    counters[0] += ulong2(written, needed);
  }
  if (params.statistics_enabled) {
    statistics[0] += ia_vertices;
    statistics[1] += ia_primitives;
    statistics[5] += clip_invocations;
    statistics[6] += clip_primitives;
  }
  status[0] = uint(unclaimed);
}

/* Stream output after a GS (stream 0), run once after the draw's render pass. The GS mesh function captures,
 * per invocation key ((draw instance * primitives per instance + primitive id) * GS instances + GS instance, the
 * D3D output order), up to dcl_maxout (`params.vertex_count`) records of record_dwords + 1 dwords; the extra dword
 * is the vertex's index inside its output strip. One header word per key follows all records and holds the
 * number of captured vertices. `control.record_count` is the number of keys. Output strips are written as lists
 * with the IA strip order (triangle i: (i, i+1, i+2), odd i: (i, i+2, i+1)), appended in key order (FS 14.5/14.6).
 * `params.topology` is the GS output topology: 0 point list, 2 line strip, 4 triangle strip. */
static void so_write_gs_primitive(constant D3DALLOYSOAssembly &params, device const uint *records,
    thread const uint *primitive, uint vpp, ulong first_vertex, thread const ulong *filled,
    device uchar *thread *targets) {
  for (uint p = 0; p < vpp; ++p) {
    device const uint *source = records + ulong(primitive[p]) * (params.record_dwords + 1);
    for (uint e = 0; e < params.element_count; ++e) {
      const auto element = params.elements[e];
      const ulong base = filled[element.slot] + (first_vertex + p) * params.stride[element.slot] + element.byte_offset;
      for (uint c = 0; c < element.component_count; ++c)
        d3dalloy_so_store_dword(targets[element.slot], base + 4 * c, source[element.dword_offset + c]);
    }
  }
}

static bool so_gs_primitive(uint topology, uint position, uint v, uint run_start, thread uint *primitive) {
  bool complete = false;
  if (topology == 0) {
    primitive[0] = v;
    complete = true;
  } else if (topology == 2 && position >= 1) {
    primitive[0] = v - 1;
    primitive[1] = v;
    complete = true;
  } else if (topology == 4 && position >= 2) {
    const uint i = position - 2;
    primitive[0] = run_start + i;
    primitive[1] = (i & 1) ? v : run_start + i + 1;
    primitive[2] = (i & 1) ? run_start + i + 1 : v;
    complete = true;
  }
  return complete;
}

[[kernel]] void so_assemble_gs(
    constant D3DALLOYSOAssembly &params [[buffer(0)]],
    constant D3DALLOYSOControl &control [[buffer(1)]],
    device const uint *scratch [[buffer(2)]],
    device const uchar *unused [[buffer(3)]],
    device ulong2 *counters [[buffer(4)]],
    device const ulong *predicate [[buffer(5)]],
    device uchar *target0 [[buffer(6)]],
    device uchar *target1 [[buffer(7)]],
    device uchar *target2 [[buffer(8)]],
    device uchar *target3 [[buffer(9)]],
    device uchar *filled0 [[buffer(10)]],
    device uchar *filled1 [[buffer(11)]],
    device uchar *filled2 [[buffer(12)]],
    device uchar *filled3 [[buffer(13)]],
    device uint *status [[buffer(14)]], uint tid [[thread_position_in_grid]],
    uint group_width [[threads_per_threadgroup]], uint simd_width [[threads_per_simdgroup]]
) {
  const bool parallel = group_width == 32 && simd_width == 32;
  if (!parallel && tid) return;
  if (!tid) status[0] = 0;
  if (params.predicate_enabled && (params.predicate_not_equal ? predicate[0] != 0 : predicate[0] == 0))
    return;
  device uchar *targets[4] = {target0, target1, target2, target3};
  device uchar *filled_sizes[4] = {filled0, filled1, filled2, filled3};
  ulong filled[4] = {0, 0, 0, 0};
  for (uint s = 0; s < 4; s++)
    if (params.bound_mask & (1u << s))
      filled[s] = d3dalloy_so_load_dword(filled_sizes[s], 0);
  const uint topology = params.topology;
  const uint vpp = topology == 0 ? 1 : topology == 2 ? 2 : 3;
  const uint max_vertices = params.vertex_count; // dcl_maxout
  const uint stride = params.record_dwords + 1;
  device const uint *headers = scratch + ulong(control.record_count) * max_vertices * stride;
  if (parallel) {
    ulong capacity = (params.referenced_mask & ~params.bound_mask) ? 0 : ~0ul;
    for (uint s = 0; s < 4; ++s) if (params.referenced_mask & (1u << s)) {
      const ulong room = params.size[s] - min(params.size[s], filled[s]);
      capacity = min(capacity, room / (ulong(vpp) * params.stride[s]));
    }
    simdgroup_barrier(mem_flags::mem_device);
    ulong needed = 0;
    for (ulong first = 0; first < control.record_count; first += 32) {
      const ulong key = first + tid;
      const bool live = key < control.record_count;
      const uint count = live ? min(headers[key], max_vertices) : 0;
      device const uint *records = scratch + (live ? key : 0) * max_vertices * stride;
      uint produced = count;
      if (vpp != 1) {
        produced = 0;
        for (uint v = 0; v < count; ++v)
          produced += records[ulong(v) * stride + params.record_dwords] >= vpp - 1;
      }
      // A lane owns one complete invocation. Prefix over actual emissions, including
      // strip cuts, assigns each key its destination in the application's order.
      ulong ordinal = needed + simd_prefix_exclusive_sum(produced);
      needed += simd_sum(produced);
      uint run_start = 0;
      for (uint v = 0; v < count && ordinal < capacity; ++v) {
        const uint position = records[ulong(v) * stride + params.record_dwords];
        if (!position) run_start = v;
        uint primitive[3];
        if (so_gs_primitive(topology, position, v, run_start, primitive))
          so_write_gs_primitive(params, records, primitive, vpp, ordinal++ * vpp, filled, targets);
      }
    }
    simdgroup_barrier(mem_flags::mem_device);
    if (!tid) {
      const ulong written = min(needed, capacity);
      for (uint s = 0; s < 4; ++s) if ((params.referenced_mask & params.bound_mask) & (1u << s))
        d3dalloy_so_store_dword(filled_sizes[s], 0, uint(filled[s] + written * vpp * params.stride[s]));
      counters[0] += ulong2(written, needed);
    }
    return;
  }
  bool stopped = (params.referenced_mask & ~params.bound_mask) != 0;
  ulong written = 0, needed = 0;
  for (uint key = 0; key < control.record_count; key++) {
    const uint count = min(headers[key], max_vertices);
    device const uint *records = scratch + ulong(key) * max_vertices * stride;
    uint run_start = 0;
    for (uint v = 0; v < count; v++) {
      const uint position = records[ulong(v) * stride + params.record_dwords];
      if (position == 0)
        run_start = v;
      uint primitive[3];
      if (!so_gs_primitive(topology, position, v, run_start, primitive))
        continue;
      needed++;
      if (!stopped) {
        for (uint s = 0; s < 4; s++)
          if ((params.referenced_mask & (1u << s)) &&
              (filled[s] > params.size[s] || ulong(vpp) * params.stride[s] > params.size[s] - filled[s]))
            stopped = true;
      }
      if (stopped)
        continue;
      so_write_gs_primitive(params, records, primitive, vpp, 0, filled, targets);
      for (uint s = 0; s < 4; s++)
        if (params.referenced_mask & (1u << s))
          filled[s] += ulong(vpp) * params.stride[s];
      written++;
    }
  }
  for (uint s = 0; s < 4; s++)
    if ((params.referenced_mask & params.bound_mask) & (1u << s))
      d3dalloy_so_store_dword(filled_sizes[s], 0, uint(filled[s]));
  counters[0] += ulong2(written, needed);
}

#include "d3d12_capture_frontier.metal"

// D3D12_QUERY_DATA_PIPELINE_STATISTICS is eleven consecutive 64-bit counters.
[[kernel]] void query_pipeline_statistics_end(
    device const ulong *counter [[buffer(0)]],
    device const ulong *begin [[buffer(1)]],
    device ulong *result [[buffer(2)]]) {
  for (uint i = 0; i < 11; i++) result[i] = counter[i] - begin[i];
}

// D3D12_QUERY_DATA_PIPELINE_STATISTICS1: the eleven counters plus AS/MS invocations and MS primitives.
[[kernel]] void query_pipeline_statistics1_end(
    device const ulong *counter [[buffer(0)]],
    device const ulong *begin [[buffer(1)]],
    device ulong *result [[buffer(2)]]) {
  for (uint i = 0; i < 14; i++) result[i] = counter[i] - begin[i];
}

// D3D12 SO statistics EndQuery: result = stream counters now - counters at BeginQuery.
[[kernel]] void query_so_statistics_end(
    device const ulong2* counter [[buffer(0)]],
    device const ulong2* begin [[buffer(1)]],
    device ulong2* result [[buffer(2)]]
) {
  result[0] = counter[0] - begin[0];
}

[[kernel]] void query_accumulate_visibility(
    device const ulong* source [[buffer(0)]],
    device ulong* destination [[buffer(1)]]
) {
  destination[0] += source[0];
}

[[kernel]] void query_accumulate_visibility_batch(
    device const ulong *source [[buffer(0)]], device ulong *destination [[buffer(1)]],
    constant ulong2 *indices [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  const ulong2 pair = indices[i];
  destination[pair.y] += source[pair.x];
}

[[kernel]] void clear_texture_1d_float(
    texture1d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  if(width > pos) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_1d_array_float(
    texture1d_array<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint array_len = tex.get_array_size();
  if(width > pos.x && array_len > pos.y) {
    tex.write(value, pos.x, pos.y);
  }
}

[[kernel]] void clear_texture_2d_float(
    texture2d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  if(width > pos.x && height > pos.y) {
    tex.write(value, pos);
  }
}

[[kernel]] void clear_texture_2d_array_float(
    texture2d_array<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint array_len = tex.get_array_size();
  if(width > pos.x && height > pos.y && array_len > pos.z) {
    tex.write(value, pos.xy, pos.z);
  }
}

[[kernel]] void clear_texture_3d_float(
    texture3d<float, access::read_write> tex [[texture(0)]],
    constant float4& value [[buffer(1)]],
    ushort3 pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint height = tex.get_height();
  uint depth = tex.get_depth();
  if(width > pos.x && height > pos.y && depth > pos.z) {
    tex.write(value, pos);
  }
}

struct D3DalloyClearTextureBufferFloat {
  float4 value;
  uint offset;
  uint size;
};

[[kernel]] void clear_texture_buffer_float(
    texture_buffer<float, access::read_write> tex [[texture(0)]],
    constant D3DalloyClearTextureBufferFloat& args [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  uint width = tex.get_width();
  uint offset = pos + args.offset;
  if (args.size > pos && width > offset) {
    tex.write(args.value, offset);
  }
}

struct clear_data {
  float4 position [[position]];
  uint array_index [[render_target_array_index]];
};

[[vertex]] clear_data vs_clear_rt(
  ushort id [[vertex_id]],
  ushort instance_id [[instance_id]]
) {
  clear_data output;
  float2 uv = float2((id << 1) & 2, id & 2);
  output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  output.array_index = instance_id;
  return output;
}

[[fragment]] float4 fs_clear_rt_float(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return clear_value;
}

[[fragment]] uint4 fs_clear_rt_uint(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return (uint4)clear_value;
}

[[fragment]] int4 fs_clear_rt_sint(
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return (int4)clear_value;
}

struct depth_out {
  float depth [[depth(any)]];
};

[[fragment]] depth_out fs_clear_rt_depth (
  constant float4& clear_value [[buffer(kCustomBufferArgumentIndex0)]]
) {
  return {clear_value.x};
}

struct present_data {
  float4 position [[position]];
  float2 uv [[user(coord)]];
};

[[vertex]] present_data vs_present_quad(ushort id [[vertex_id]]) {
  present_data output;
  float2 uv = float2((id << 1) & 2, id & 2);
	output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  output.uv = uv;
  return output;
}

constant constexpr float PQ_M1 = 0.1593017578125;
constant constexpr float PQ_M2 = 78.84375;
constant constexpr float PQ_C1 = 0.8359375;
constant constexpr float PQ_C2 = 18.8515625;
constant constexpr float PQ_C3 = 18.6875;

float linear_to_pq(float linear) {
  linear = linear / 10000;
  return pow((PQ_C1 + PQ_C2 * pow(abs(linear), PQ_M1)) / (1.0f + PQ_C3 * pow(abs(linear), PQ_M1)), PQ_M2);
}

float3 linear_to_pq(float3 linear) {
  linear = linear / 10000;
  return pow((PQ_C1 + PQ_C2 * pow(abs(linear), PQ_M1)) / (1.0f + PQ_C3 * pow(abs(linear), PQ_M1)), PQ_M2);
}

float pq_to_linear(float norm) {
  return 10000 * pow(abs(max(pow(norm, 1.0 / PQ_M2) -PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * pow(abs(norm), 1.0f / PQ_M2))), 1.0f / PQ_M1 );
}

float3 pq_to_linear(float3 norm) {
  return 10000 * pow(abs(max(pow(norm, 1.0 / PQ_M2) -PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * pow(abs(norm), 1.0f / PQ_M2))), 1.0f / PQ_M1 );
}

// See BT.2408 Annex 5
float EETF(float E, float Lw, float Lb, float Lmax, float Lmin) {
  float pq_Lb = linear_to_pq(Lb);
  float pqdiff_LwLb = linear_to_pq(Lw) - pq_Lb;
  float min_lum = (linear_to_pq(Lmin) - pq_Lb) / pqdiff_LwLb;
  float max_lum = (linear_to_pq(Lmax) - pq_Lb) / pqdiff_LwLb;
  E = (E - pq_Lb) / pqdiff_LwLb;
  float KS = 1.5 * max_lum - 0.5;
  float b = min_lum;
  // When the display reaches the content peak, KS >= 1: there is no
  // highlight shoulder, and at equality its peak T would be 0/0. Keep the
  // black-level lift below, which still maps Lb to Lmin.
  if (KS < 1.0f && E >= KS) {
    float T = (E - KS) /  (1 - KS);
    float T_2 = T * T;
    float T_3 = T_2 * T;
    E = (2 * T_3 - 3 * T_2 + 1) * KS 
        + (T_3 - 2 * T_2 + T) * (1 - KS)
        + (- 2 * T_3 + 3 * T_2) * max_lum;
  }
  E = E + b * pow(1 - E, 4);
  return E * pqdiff_LwLb + pq_Lb;
}

constant bool present_backbuffer_size_matched [[function_constant(kPresentFCIndex_BackbufferSizeMatched)]];
constant bool present_backbuffer_is_srgb [[function_constant(kPresentFCIndex_BackbufferIsSRGB)]];
constant bool present_hdr_pq [[function_constant(kPresentFCIndex_HDRPQ)]];
constant bool present_with_hdr_metadata [[function_constant(kPresentFCIndex_WithHDRMetadata)]];
constant bool present_backbuffer_is_ms [[function_constant(kPresentFCIndex_BackbufferIsMS)]];
constant bool present_backbuffer_is_not_ms = !present_backbuffer_is_ms;
constant bool present_gamma_enabled [[function_constant(kPresentFCIndex_GammaEnabled)]];
constant bool present_composition_residual [[function_constant(kPresentFCIndex_CompositionResidual)]];
constant bool present_linear_composition_transport [[function_constant(kPresentFCIndex_LinearCompositionTransport)]];

constexpr sampler s(coord::normalized);
constexpr sampler gamma_sampler(coord::normalized, filter::linear, address::clamp_to_edge);

struct D3DalloyPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  struct {
    float source_width, source_height;
    float destination_width, destination_height;
    uint rotation, scaling_none, right_to_left;
    float background[4];
    uint restrict_output;
    float output_clip[4];
  } geometry;
  uint alpha;
  uint composition_color_space;
};

float3 to_srgb(float3 linear) {
  return select(1.055 * pow(linear, 1.0 / 2.4) - 0.055, linear * 12.92, linear < 0.0031308); 
}

struct D3DalloyPresentOutput {
  float4 base [[color(0)]];
  float4 residual [[color(1), function_constant(present_composition_residual)]];
};

D3DalloyPresentOutput present_output(float4 color, constant D3DalloyPresentMetadata &meta) {
  if (present_linear_composition_transport) {
    // Platform domain: finite source RGB, alpha in [0,1]; G22/PQ straight
    // values are bounded [0,1], while G10 scRGB retains signed extended RGB.
    if (meta.alpha == 2 && color.w <= 0) color = float4(0);
    // Unassociate in the source transfer domain, convert, then associate.
    // At alpha zero premultiplied RGB is additive energy, not straight color.
    float3 straight = color.xyz;
    if (present_backbuffer_is_srgb) straight = to_srgb(straight);
    if (meta.alpha == 1 && color.w > 0) straight /= color.w;
    float3 linear = straight;
    if (meta.composition_color_space == 1 || meta.composition_color_space == 4)
      linear = select(pow(max((straight + 0.055) / 1.055, float3(0)), 2.4),
                      straight / 12.92, straight <= 0.04045);
    else if (meta.composition_color_space == 3)
      linear = pq_to_linear(clamp(straight, float3(0), float3(1))) / 100.0f;
    // D65 BT.2020 -> BT.709. Preserve signed and extended components.
    if (meta.composition_color_space == 3 || meta.composition_color_space == 4)
      linear = float3(dot(linear, float3(1.660491, -0.587641, -0.072850)),
                      dot(linear, float3(-0.124550, 1.132900, -0.008349)),
                      dot(linear, float3(-0.018151, -0.100579, 1.118730)));
    linear *= meta.edr_scale;
    color.xyz = meta.alpha == 2 || color.w > 0 ? linear * color.w : linear;
  }
  D3DalloyPresentOutput result;
  result.base = color;
  if (present_composition_residual) {
    // Export source-over RGB separately. The opaque additive plane
    // prevents native alpha-zero transport from erasing additive colour.
    // Keep signed energy on the opaque plane too: CA may discard negative
    // or zero-coverage color on an unassociated transparent export.
    result.base = float4(clamp(color.xyz, float3(0), float3(color.w)), color.w);
    result.residual = float4(color.xyz - result.base.xyz, 1);
  }
  return result;
}

[[fragment]] D3DalloyPresentOutput fs_present_quad(
    present_data input [[stage_in]],
    texture2d<float, access::sample> source [[texture(0), function_constant(present_backbuffer_is_not_ms)]],
    texture2d_ms<float, access::read> source_ms [[texture(0), function_constant(present_backbuffer_is_ms)]],
    texture2d<float, access::sample> gamma_lut [[texture(1), function_constant(present_gamma_enabled)]],
    constant D3DalloyPresentMetadata& meta [[buffer(0)]]
) {
  float4 output = float4(0);
  if (meta.geometry.restrict_output &&
      (input.position.x < meta.geometry.output_clip[0] ||
       input.position.y < meta.geometry.output_clip[1] ||
       input.position.x >= meta.geometry.output_clip[2] ||
       input.position.y >= meta.geometry.output_clip[3]))
    return present_output(float4(0), meta);
  float2 sample_position = input.position.xy;
  if (meta.geometry.source_width > 0) {
    float2 source_size = float2(meta.geometry.source_width, meta.geometry.source_height);
    float2 target_size = float2(meta.geometry.destination_width, meta.geometry.destination_height);
    bool quarter_turn = meta.geometry.rotation == 2 || meta.geometry.rotation == 4;
    float2 rotated_size = quarter_turn ? source_size.yx : source_size;
    float2 image_size = meta.geometry.scaling_none ? rotated_size : target_size;
    float2 origin = float2(meta.geometry.scaling_none && meta.geometry.right_to_left
                                ? target_size.x - rotated_size.x : 0.0f, 0.0f);
    float2 uv = (input.position.xy - origin) / image_size;
    if (any(uv < 0.0f) || any(uv >= 1.0f)) {
      float4 background = float4(meta.geometry.background[0], meta.geometry.background[1],
                                meta.geometry.background[2], meta.geometry.background[3]);
      return present_output(background, meta);
    }
    if (meta.geometry.rotation == 2)
      uv = float2(uv.y, 1.0f - uv.x);
    else if (meta.geometry.rotation == 3)
      uv = 1.0f - uv;
    else if (meta.geometry.rotation == 4)
      uv = float2(1.0f - uv.y, uv.x);
    sample_position = uv * source_size;
  }
  if (present_backbuffer_is_ms) {
    const uint count = source_ms.get_num_samples();
    for (uint i = 0; i < count; i++) {
      output += source_ms.read(uint2(sample_position), i);
    }
    output /= count;
  } else {
    output = present_backbuffer_size_matched
      ? source.read(uint2(sample_position))
      : source.sample(s, meta.geometry.source_width > 0
                             ? sample_position / float2(source.get_width(), source.get_height())
                             : input.uv);
  }
  if (present_linear_composition_transport)
    return present_output(output, meta);
  float3 output_rgb = output.xyz;
  if (meta.alpha == 2 && output.w <= 0)
    return present_output(float4(0), meta);
  if (meta.alpha == 1 && output.w > 0) {
    // sRGB texture reads already decoded the premultiplied encoded value.
    // Undo that decode before unpremultiplication, then resume the color path.
    if (present_backbuffer_is_srgb)
      output_rgb = to_srgb(output_rgb);
    output_rgb /= output.w;
    if (present_backbuffer_is_srgb)
      output_rgb = select(pow((output_rgb + 0.055) / 1.055, 2.4),
                          output_rgb / 12.92, output_rgb <= 0.04045);
  }
  if (present_gamma_enabled && !present_hdr_pq && !present_with_hdr_metadata) {
    output_rgb = float3(
        gamma_lut.sample(gamma_sampler, float2(output_rgb.r, 0.5)).r,
        gamma_lut.sample(gamma_sampler, float2(output_rgb.g, 0.5)).g,
        gamma_lut.sample(gamma_sampler, float2(output_rgb.b, 0.5)).b);
  }
  float edr_scale = meta.edr_scale;
  if (present_backbuffer_is_srgb)
    output_rgb = to_srgb(output_rgb);
  if (present_hdr_pq)
    output_rgb = pq_to_linear(output_rgb);
  else if (present_with_hdr_metadata)
    output_rgb = output_rgb * 80; // to nits
  if (present_with_hdr_metadata) {
    // tone mapping: BT.2408 EETF + maxRGB
    float max_content_luminance = meta.max_content_luminance;
    float min_content_luminance = 1; // in case of division by 0
    float max_display_luminance = meta.max_display_luminance;
    float min_display_luminance = 0;
    float3 clamped = clamp(output_rgb, float3(min_content_luminance), float3(max_content_luminance));
    float3 clamped_pq = linear_to_pq(clamped);
    float maxRGB = max3(clamped_pq.x, clamped_pq.y, clamped_pq.z);
    float maxRGB2 = EETF(
      maxRGB, max_content_luminance, min_content_luminance, max_display_luminance, min_display_luminance
    );
    output_rgb = pq_to_linear(maxRGB2) / pq_to_linear(maxRGB) * clamped;
  }
  output_rgb *= edr_scale;
  if (present_hdr_pq)
    output_rgb = linear_to_pq(output_rgb);
  else if (present_with_hdr_metadata)
    output_rgb = output_rgb / 80;
  // Premultiplied alpha permits additive RGB even at alpha zero.
  if (meta.alpha == 2 || (meta.alpha == 1 && output.w > 0))
    output_rgb *= output.w;
  return present_output(float4(output_rgb, output.w), meta);
}

struct D3DalloyDispatchArguments {
  uint x;
  uint y;
  uint z;
};

struct D3DALLOYTSDispatchMarshal {
  constant uint2& draw_arguments; // (vertex|index_count, instance_count)
  device D3DalloyDispatchArguments& dispatch_arguments_out;
  ulong max_object_threadgroups;
  ushort control_point_count;
  ushort patch_per_group;
  uint end_of_command;
};

[[vertex]] void ts_draw_arguments_marshal(
    constant D3DALLOYTSDispatchMarshal* tasks [[buffer(kCustomBufferArgumentIndex0)]]
) {
  uint index = 0;
  for(;;) {
    constant D3DALLOYTSDispatchMarshal& task = tasks[index];

    device D3DalloyDispatchArguments& output = task.dispatch_arguments_out;

    uint patch_count_per_instance = task.draw_arguments.x / (uint)task.control_point_count;
    uint x = (patch_count_per_instance - 1) / task.patch_per_group + 1;
    if ((task.draw_arguments.x * task.draw_arguments.y) == 0 ||
        x * task.draw_arguments.y > task.max_object_threadgroups) {
      output.x = 0;
      output.y = 0;
      output.z = 0;
    } else {
      output.x = x;
      output.y = task.draw_arguments.y;
      output.z = 1;
    }

    if (task.end_of_command)
      break;
    index++;
  };
}

struct D3DALLOYGSDispatchMarshal {
  constant uint2& draw_arguments; // (vertex|index_count, instance_count)
  device D3DalloyDispatchArguments& dispatch_arguments_out;
  ulong max_object_threadgroups;
  uint vertex_count_per_warp;
  uint end_of_command;
};

[[vertex]] void gs_draw_arguments_marshal(
    constant D3DALLOYGSDispatchMarshal* tasks [[buffer(kCustomBufferArgumentIndex0)]]
) {
  uint index = 0;
  for(;;) {
    constant D3DALLOYGSDispatchMarshal& task = tasks[index];

    device D3DalloyDispatchArguments& output = task.dispatch_arguments_out;

    uint x = (task.draw_arguments.x - 1) / task.vertex_count_per_warp + 1;
    if ((task.draw_arguments.x * task.draw_arguments.y) == 0 ||
        x * task.draw_arguments.y > task.max_object_threadgroups) {
      output.x = 0;
      output.y = 0;
      output.z = 0;
    } else {
      output.x = x;
      output.y = task.draw_arguments.y;
      output.z = 1;
    }

    if (task.end_of_command)
      break;
    index++;
  };
}

struct depth_stencil_out {
  float depth [[depth(any)]];
  uint stencil [[stencil]]; 
};

struct packed_d32s8x24 {
  float depth;
  uchar stencil;
  uchar unused[3];
};

struct linear_texture_desc {
  uint bytes_per_row;
  uint bytes_per_image;
};

[[fragment]] depth_stencil_out fs_copy_from_buffer_d32s8(
  present_data input [[stage_in]],
  device char* buffer [[buffer(kCustomBufferArgumentIndex0)]],
  constant linear_texture_desc& desc [[buffer(kCustomBufferArgumentIndex1)]]
) {
  depth_stencil_out result;
  uint2 pos = uint2(input.position.xy);
  uint buffer_offset = pos.x * sizeof(packed_d32s8x24) 
                        + pos.y * desc.bytes_per_row;
  packed_d32s8x24 data = *reinterpret_cast<device packed_d32s8x24 *>(buffer + buffer_offset);
  result.depth = data.depth;
  result.stencil = data.stencil;
  return result;
}

[[fragment]] depth_stencil_out fs_copy_from_buffer_d24s8(
  present_data input [[stage_in]],
  device char* buffer [[buffer(kCustomBufferArgumentIndex0)]],
  constant linear_texture_desc& desc [[buffer(kCustomBufferArgumentIndex1)]]
) {
  depth_stencil_out result;
  uint2 pos = uint2(input.position.xy);
  uint buffer_offset = pos.x * sizeof(uint) 
                        + pos.y * desc.bytes_per_row;
  uint data = *reinterpret_cast<device uint *>(buffer + buffer_offset);
  result.depth = float(data & 0xffffff) / float(0xffffff);
  result.stencil = data >> 24;
  return result;
}

[[kernel]] void cs_copy_to_buffer_d32s8(
  texture2d<float, access::read> tex_depth [[texture(0)]],
  texture2d<uint, access::read> tex_stencil [[texture(1)]],
  device char* buffer [[buffer(0)]],
  constant linear_texture_desc& desc [[buffer(1)]],
  ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex_depth.get_width();
  uint height = tex_depth.get_height();
  uint buffer_offset = pos.x * sizeof(packed_d32s8x24) 
                        + pos.y * desc.bytes_per_row;
  if (width > pos.x && height > pos.y) {
    auto dst = reinterpret_cast<device packed_d32s8x24 *>(buffer + buffer_offset);
    dst->depth = tex_depth.read(pos).x;
    dst->stencil = tex_stencil.read(pos).x;
  }
}

[[kernel]] void cs_copy_to_buffer_d24s8(
  texture2d<float, access::read> tex_depth [[texture(0)]],
  texture2d<uint, access::read> tex_stencil [[texture(1)]],
  device char* buffer [[buffer(0)]],
  constant linear_texture_desc& desc [[buffer(1)]],
  ushort2 pos [[thread_position_in_grid]]
) {
  uint width = tex_depth.get_width();
  uint height = tex_depth.get_height();
  uint buffer_offset = pos.x * sizeof(uint) 
                        + pos.y * desc.bytes_per_row;
  if (width > pos.x && height > pos.y) {
    auto dst = reinterpret_cast<device uint *>(buffer + buffer_offset);
    float depth = tex_depth.read(pos).x;
    uint stencil = tex_stencil.read(pos).x;
    *dst = (uint(0xffffff * clamp(depth, 0.0, 1.0)) & 0xffffff) | (stencil << 24);
  }
}

struct D3DalloyClearFloatMetadata {
  float4 value;
  uint2 offset;
  uint2 size;
};

struct D3DalloyClearUintMetadata {
  uint4 value;
  uint2 offset;
  uint2 size;
};

[[kernel]] void cs_clear_texture2d_float(
    texture2d<float, access::write> tex [[texture(0)]],
    constant D3DalloyClearFloatMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  if (all(pos < meta.size))
    tex.write(meta.value, meta.offset + pos);
}

[[kernel]] void cs_clear_texture2d_array_float(
    texture2d_array<float, access::write> tex [[texture(0)]],
    constant D3DalloyClearFloatMetadata& meta [[buffer(1)]],
    uint3 pos [[thread_position_in_grid]]
) {
  if (all(pos.xy < meta.size))
    tex.write(meta.value, meta.offset + pos.xy, pos.z);
}

struct D3DalloyReservedVolumeClear {
  device const uint *layout;
  uint mip;
  uint first_w_slice;
  uint w_size;
};

bool reserved_volume_clear_address(device const uint *layout, uint mip, uint3 position, thread uint3 &address) {
  device const uint *m = layout + 14 + 4 * mip;
  const uint3 shape(layout[4], layout[5], layout[6]);
  const uint3 tile_position = position / shape;
  const uint tile = m[0] + (tile_position.z * m[2] + tile_position.y) * m[1] + tile_position.x;
  device const uint *status = *reinterpret_cast<device const uint *device const *>(layout + 14 + 4 * layout[3]);
  if (status && !status[tile]) return false;
  const uint3 local = position % shape;
  const uint flat = local.x + shape.x * (local.y + shape.y * local.z);
  const uint page_width = layout[9], page_height = layout[10];
  const uint columns = layout[11], rows = layout[12];
  address = uint3((tile % columns) * page_width + flat % page_width,
      ((tile / columns) % rows) * page_height + flat / page_width, tile / (columns * rows));
  return true;
}

[[kernel]] void cs_clear_reserved_volume_float(
    texture2d_array<float, access::write> tex [[texture(0)]],
    constant D3DalloyClearFloatMetadata &meta [[buffer(1)]],
    constant D3DalloyReservedVolumeClear &volume [[buffer(2)]],
    uint3 position [[thread_position_in_grid]]) {
  if (all(position.xy < meta.size) && position.z < volume.w_size) {
    uint3 address;
    if (reserved_volume_clear_address(volume.layout, volume.mip,
        uint3(meta.offset + position.xy, volume.first_w_slice + position.z), address))
      tex.write(meta.value, address.xy, address.z);
  }
}

[[kernel]] void cs_clear_reserved_volume_uint(
    texture2d_array<uint, access::write> tex [[texture(0)]],
    constant D3DalloyClearUintMetadata &meta [[buffer(1)]],
    constant D3DalloyReservedVolumeClear &volume [[buffer(2)]],
    uint3 position [[thread_position_in_grid]]) {
  if (all(position.xy < meta.size) && position.z < volume.w_size) {
    uint3 address;
    if (reserved_volume_clear_address(volume.layout, volume.mip,
        uint3(meta.offset + position.xy, volume.first_w_slice + position.z), address))
      tex.write(meta.value, address.xy, address.z);
  }
}

[[kernel]] void cs_clear_texture3d_float(
    texture3d<float, access::write> tex [[texture(0)]],
    constant D3DalloyClearFloatMetadata& meta [[buffer(1)]],
    constant uint& first_w_slice [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  if (all(pos.xy < meta.size))
    tex.write(meta.value, uint3(meta.offset + pos.xy, first_w_slice + pos.z));
}

[[kernel]] void cs_clear_texture2d_uint(
    texture2d<uint, access::write> tex [[texture(0)]],
    constant D3DalloyClearUintMetadata& meta [[buffer(1)]],
    uint2 pos [[thread_position_in_grid]]
) {
  if (all(pos < meta.size))
    tex.write(meta.value, meta.offset + pos);
}

[[kernel]] void cs_clear_texture2d_array_uint(
    texture2d_array<uint, access::write> tex [[texture(0)]],
    constant D3DalloyClearUintMetadata& meta [[buffer(1)]],
    uint3 pos [[thread_position_in_grid]]
) {
  if (all(pos.xy < meta.size))
    tex.write(meta.value, meta.offset + pos.xy, pos.z);
}

[[kernel]] void cs_clear_texture3d_uint(
    texture3d<uint, access::write> tex [[texture(0)]],
    constant D3DalloyClearUintMetadata& meta [[buffer(1)]],
    constant uint& first_w_slice [[buffer(2)]],
    uint3 pos [[thread_position_in_grid]]
) {
  if (all(pos.xy < meta.size))
    tex.write(meta.value, uint3(meta.offset + pos.xy, first_w_slice + pos.z));
}

[[kernel]] void cs_clear_tbuffer_float(
    texture_buffer<float, access::write> tex [[texture(0)]],
    constant D3DalloyClearFloatMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if (pos < meta.size.x)
    tex.write(meta.value, meta.offset.x + pos);
}

[[kernel]] void cs_clear_tbuffer_uint(
    texture_buffer<uint, access::write> tex [[texture(0)]],
    constant D3DalloyClearUintMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if (pos < meta.size.x)
    tex.write(meta.value, meta.offset.x + pos);
}

[[kernel]] void cs_clear_buffer_float(
    device float* buffer [[buffer(0)]],
    constant D3DalloyClearFloatMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if (pos < meta.size.x)
    buffer[meta.offset.x + pos] = meta.value.x;
}

[[kernel]] void cs_clear_buffer_uint(
    device uint* buffer [[buffer(0)]],
    constant D3DalloyClearUintMetadata& meta [[buffer(1)]],
    uint pos [[thread_position_in_grid]]
) {
  if (pos < meta.size.x)
    buffer[meta.offset.x + pos] = meta.value.x;
}

[[kernel]] void cs_downscale_dilated_mv(
  uint2 pos [[thread_position_in_grid]],
  constant float2& mv_scale [[buffer(0)]],
  texture2d<float, access::read> dilated [[texture(0)]],
  texture2d<float, access::write> downscaled [[texture(1)]]
) {
  float2 source_size = float2(dilated.get_width(), dilated.get_height());
  float2 viewport_size = float2(downscaled.get_width(), downscaled.get_height());
  float2 scale = viewport_size / source_size;
  float2 sample_uv = (float2(pos) + 0.5) / viewport_size;
  uint2 sample_pos = uint2(sample_uv * source_size);
  float2 hi_mv = dilated.read(sample_pos).xy;
  float2 hi_mv_pixel = hi_mv * mv_scale;
  float2 lo_mv_pixel = hi_mv_pixel * scale;
  downscaled.write(lo_mv_pixel.xyxy, pos); 
}

[[kernel]] void tile_barrier(ushort2 pos [[thread_position_in_threadgroup]]) {
  // empty
}

struct D3DalloyResolveVertexOutput {
  float4 position [[position]];
};

struct D3DalloyResolveParameters {
  int2 source_delta;
  uint sample_count;
  uint mode;
  uint predicate_enabled;
  uint predicate_not_equal;
};

[[vertex]] D3DalloyResolveVertexOutput d3dalloy_resolve_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  return {float4(positions[vertex_id], 0.0, 1.0)};
}

// Writable MSAA samples live in single-sample arrays while a shader pass runs.
// The baseline makes exporting inactive placed UAV aliases a no-op.
#define D3DALLOY_WRITABLE_MSAA(T, NAME) \
[[kernel]] void d3dalloy_msaa_import_##NAME( \
    texture2d_ms<T, access::read> source [[texture(0)]], \
    texture2d_array<T, access::write> destination [[texture(1)]], \
    texture2d_array<T, access::write> baseline [[texture(2)]], \
    constant uint2 &indices [[buffer(0)]], uint2 position [[thread_position_in_grid]]) { \
  if (position.x >= source.get_width() || position.y >= source.get_height()) return; \
  auto value = source.read(position, indices.y); \
  destination.write(value, position, indices.x); \
  baseline.write(value, position, indices.x); \
} \
[[kernel]] void d3dalloy_msaa_import_array_##NAME( \
    texture2d_ms_array<T, access::read> source [[texture(0)]], \
    texture2d_array<T, access::write> destination [[texture(1)]], \
    texture2d_array<T, access::write> baseline [[texture(2)]], \
    constant uint2 &indices [[buffer(0)]], uint2 position [[thread_position_in_grid]]) { \
  if (position.x >= source.get_width() || position.y >= source.get_height()) return; \
  auto value = source.read(position, indices.x, indices.y); \
  destination.write(value, position, indices.x); \
  baseline.write(value, position, indices.x); \
} \
[[fragment]] vec<T, 4> d3dalloy_msaa_export_##NAME( \
    D3DalloyResolveVertexOutput input [[stage_in]], uint sample [[sample_id]], \
    texture2d_array<T, access::read> source [[texture(0)]], \
    texture2d_array<T, access::read> baseline [[texture(1)]], \
    constant uint2 &indices [[buffer(0)]]) { \
  if (sample != indices.y) discard_fragment(); \
  auto value = source.read(uint2(input.position.xy), indices.x); \
  auto old = baseline.read(uint2(input.position.xy), indices.x); \
  if (all(as_type<uint4>(value) == as_type<uint4>(old))) discard_fragment(); \
  return value; \
}
D3DALLOY_WRITABLE_MSAA(float, float)
D3DALLOY_WRITABLE_MSAA(uint, uint)
#undef D3DALLOY_WRITABLE_MSAA

// Depth textures cannot be reinterpreted as integer textures or blitted while
// multisampled. Capture each sample into raw bytes before uploading color views.
void d3dalloy_depth_cast_store(float depth, uint3 position, device uchar *bytes, constant uint *layout) {
  const uint array = position.z / layout[3], sample = position.z % layout[3];
  const ulong offset = (ulong(sample) * layout[2] + array) * layout[5] +
                       ulong(position.y) * layout[4] + position.x * layout[6];
  if (layout[6] == 2)
    *reinterpret_cast<device ushort *>(bytes + offset) = ushort(rint(depth * 65535.0f));
  else
    *reinterpret_cast<device uint *>(bytes + offset) = as_type<uint>(depth);
}

[[kernel]] void d3dalloy_depth_msaa_import(
    depth2d_ms<float, access::read> source [[texture(0)]], device uchar *bytes [[buffer(0)]],
    constant uint *layout [[buffer(1)]], uint3 position [[thread_position_in_grid]]) {
  if (position.x >= layout[0] || position.y >= layout[1] || position.z >= layout[2] * layout[3]) return;
  d3dalloy_depth_cast_store(source.read(position.xy, position.z % layout[3]), position, bytes, layout);
}

[[kernel]] void d3dalloy_depth_msaa_import_array(
    depth2d_ms_array<float, access::read> source [[texture(0)]], device uchar *bytes [[buffer(0)]],
    constant uint *layout [[buffer(1)]], uint3 position [[thread_position_in_grid]]) {
  if (position.x >= layout[0] || position.y >= layout[1] || position.z >= layout[2] * layout[3]) return;
  const uint array = position.z / layout[3] + layout[7], sample = position.z % layout[3];
  d3dalloy_depth_cast_store(source.read(position.xy, array, sample), position, bytes, layout);
}

template <typename T>
vec<T, 4> d3dalloy_resolve_value(texture2d_ms<T, access::read> source, uint2 coordinate,
                             constant D3DalloyResolveParameters &parameters, uint sample_index) {
  if (parameters.mode == 3) return source.read(coordinate, sample_index);
  vec<T, 4> value = source.read(coordinate, 0);
  if (parameters.mode == 0) {
    for (uint sample = 1; sample < parameters.sample_count; ++sample)
      value += source.read(coordinate, sample);
    return value / T(parameters.sample_count);
  }
  for (uint sample = 1; sample < parameters.sample_count; ++sample) {
    const vec<T, 4> next = source.read(coordinate, sample);
    value = parameters.mode == 1 ? metal::min(value, next) : metal::max(value, next);
  }
  return value;
}

bool d3dalloy_resolve_predicate_passes(device const ulong *predicate,
                                    constant D3DalloyResolveParameters &parameters) {
  if (!parameters.predicate_enabled)
    return true;
  return parameters.predicate_not_equal ? predicate[0] == 0 : predicate[0] != 0;
}

[[fragment]] float4 d3dalloy_resolve_float(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d_ms<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters))
    discard_fragment();
  return d3dalloy_resolve_value(source, uint2(int2(input.position.xy) + parameters.source_delta), parameters, sample_index);
}

[[fragment]] uint4 d3dalloy_resolve_uint(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d_ms<uint, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters))
    discard_fragment();
  return d3dalloy_resolve_value(source, uint2(int2(input.position.xy) + parameters.source_delta), parameters, sample_index);
}

[[fragment]] int4 d3dalloy_resolve_sint(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d_ms<int, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters))
    discard_fragment();
  return d3dalloy_resolve_value(source, uint2(int2(input.position.xy) + parameters.source_delta), parameters, sample_index);
}

struct D3DalloyDepthResolveOutput {
  float depth [[depth(any)]];
};

static float d3dalloy_resolve_depth_value(D3DalloyResolveVertexOutput input, depth2d_ms<float, access::read> source,
    constant D3DalloyResolveParameters &parameters, uint sample_index) {
  const uint2 coordinate = uint2(int2(input.position.xy) + parameters.source_delta);
  if (parameters.mode == 3) return source.read(coordinate, sample_index);
  float value = source.read(coordinate, 0);
  for (uint sample = 1; sample < parameters.sample_count; ++sample) {
    const float next = source.read(coordinate, sample);
    value = parameters.mode == 0 ? value + next :
            parameters.mode == 1 ? (value < next ? value : next) : (value > next ? value : next);
  }
  if (parameters.mode == 0) value /= parameters.sample_count;
  return value;
}

[[fragment]] D3DalloyDepthResolveOutput d3dalloy_resolve_depth(
    D3DalloyResolveVertexOutput input [[stage_in]], depth2d_ms<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return {d3dalloy_resolve_depth_value(input, source, parameters, sample_index)};
}

[[fragment]] float4 d3dalloy_resolve_depth_color(
    D3DalloyResolveVertexOutput input [[stage_in]], depth2d_ms<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return float4(d3dalloy_resolve_depth_value(input, source, parameters, sample_index), 0, 0, 1);
}

struct D3DalloyStencilResolveOutput {
  uint stencil [[stencil]];
};

[[fragment]] D3DalloyStencilResolveOutput d3dalloy_resolve_stencil(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d_ms<uint, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]],
    uint sample_index [[sample_id]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters))
    discard_fragment();
  const uint2 coordinate = uint2(int2(input.position.xy) + parameters.source_delta);
  if (parameters.mode == 3) return {source.read(coordinate, sample_index).x};
  uint value = source.read(coordinate, 0).x;
  for (uint sample = 1; sample < parameters.sample_count; ++sample) {
    const uint next = source.read(coordinate, sample).x;
    value = parameters.mode == 1 ? (value < next ? value : next)
                                 : (value > next ? value : next);
  }
  return {value};
}


// Single-sample resources can carry private color/depth compression too.
[[fragment]] float4 d3dalloy_decompress_float_single(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return source.read(uint2(int2(input.position.xy) + parameters.source_delta));
}
[[fragment]] uint4 d3dalloy_decompress_uint_single(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d<uint, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return source.read(uint2(int2(input.position.xy) + parameters.source_delta));
}
[[fragment]] int4 d3dalloy_decompress_sint_single(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d<int, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return source.read(uint2(int2(input.position.xy) + parameters.source_delta));
}
[[fragment]] D3DalloyDepthResolveOutput d3dalloy_decompress_depth_single(
    D3DalloyResolveVertexOutput input [[stage_in]], depth2d<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return {source.read(uint2(int2(input.position.xy) + parameters.source_delta))};
}
[[fragment]] float4 d3dalloy_decompress_depth_color_single(
    D3DalloyResolveVertexOutput input [[stage_in]], depth2d<float, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return float4(source.read(uint2(int2(input.position.xy) + parameters.source_delta)), 0, 0, 1);
}
[[fragment]] D3DalloyStencilResolveOutput d3dalloy_decompress_stencil_single(
    D3DalloyResolveVertexOutput input [[stage_in]], texture2d<uint, access::read> source [[texture(0)]],
    constant D3DalloyResolveParameters &parameters [[buffer(0)]], device const ulong *predicate [[buffer(1)]]) {
  if (!d3dalloy_resolve_predicate_passes(predicate, parameters)) discard_fragment();
  return {source.read(uint2(int2(input.position.xy) + parameters.source_delta)).x};
}
#include "../d3d12/d3d12_standard_swizzle_gpu.hpp"

static ulong d3d12_standard_swizzle_offset(constant D3D12StandardSwizzleParams &p, uint3 coord) {
  return p.tile_offset + (swizzle_deposit(coord.x * p.bytes_per_unit, p.x_moves) |
                          swizzle_deposit(coord.y, p.y_moves) | swizzle_deposit(coord.z, p.z_moves));
}

// Both kernels copy opaque format units. The native-texture blit before/after
// these kernels supplies the linear staging representation, so BC blocks and
// typed integer/float/sRGB texels keep their exact bits without shader sampling.

kernel void d3d12_swizzle_import(device const uchar *canonical [[buffer(0)]],
                                  device uchar *linear [[buffer(1)]],
                                  constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                  uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch +
                    ulong(coord.x) * p.bytes_per_unit;
  for (uint byte = 0; byte < p.bytes_per_unit; ++byte) linear[row + byte] = canonical[tiled + byte];
}

kernel void d3d12_swizzle_export(device const uchar *linear [[buffer(0)]],
                                  device uchar *canonical [[buffer(1)]],
                                  constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                  uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch +
                    ulong(coord.x) * p.bytes_per_unit;
  for (uint byte = 0; byte < p.bytes_per_unit; ++byte) canonical[tiled + byte] = linear[row + byte];
}

// A descriptor-table UAV can name an inactive placed alias. Compare each
// potential writer against a pre-pass canonical snapshot before exporting;
// comparing to live canonical bytes would misclassify an active buffer alias.
kernel void d3d12_swizzle_dirty(device const uchar *linear [[buffer(0)]],
                                 device const uchar *canonical [[buffer(1)]],
                                 constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                 device atomic_uint *dirty [[buffer(3)]],
                                 uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch +
                    ulong(coord.x) * p.bytes_per_unit;
  for (uint byte = 0; byte < p.bytes_per_unit; ++byte)
    if (linear[row + byte] != canonical[tiled + byte]) {
      atomic_store_explicit(dirty, 1u, memory_order_relaxed);
      break;
    }
}

kernel void d3d12_swizzle_export_if_dirty(device const uchar *linear [[buffer(0)]],
                                           device uchar *canonical [[buffer(1)]],
                                           constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                           device atomic_uint *dirty [[buffer(3)]],
                                           uint3 coord [[thread_position_in_grid]]) {
  if (!atomic_load_explicit(dirty, memory_order_relaxed)) return;
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch +
                    ulong(coord.x) * p.bytes_per_unit;
  for (uint byte = 0; byte < p.bytes_per_unit; ++byte) canonical[tiled + byte] = linear[row + byte];
}

// The two 4:2:2 DXGI formats store one four-byte pair in the STANDARD tile.
// Their native backing is two RGBA8 pixels, with shared R/B and separate G.
kernel void d3d12_swizzle_packed422_import(device const uchar *canonical [[buffer(0)]],
                                            device uchar *linear [[buffer(1)]],
                                            constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                            uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch + ulong(coord.x) * 8ul;
  const uchar r = canonical[tiled + (p.packed422_variant ? 1 : 0)];
  const uchar b = canonical[tiled + (p.packed422_variant ? 3 : 2)];
  linear[row + 0] = r;
  linear[row + 1] = canonical[tiled + (p.packed422_variant ? 0 : 1)];
  linear[row + 2] = b;
  linear[row + 3] = 255;
  if (coord.x * 2 + 1 < p.tile_pixel_width) {
    linear[row + 4] = r;
    linear[row + 5] = canonical[tiled + (p.packed422_variant ? 2 : 3)];
    linear[row + 6] = b;
    linear[row + 7] = 255;
  }
}

kernel void d3d12_swizzle_packed422_export(device const uchar *linear [[buffer(0)]],
                                            device uchar *canonical [[buffer(1)]],
                                            constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                            uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch + ulong(coord.x) * 8ul;
  const uchar g1 = coord.x * 2 + 1 < p.tile_pixel_width
      ? linear[row + 5] : canonical[tiled + (p.packed422_variant ? 2 : 3)];
  if (p.packed422_variant) {
    canonical[tiled + 0] = linear[row + 1];
    canonical[tiled + 1] = linear[row + 0];
    canonical[tiled + 2] = g1;
    canonical[tiled + 3] = linear[row + 2];
  } else {
    canonical[tiled + 0] = linear[row + 0];
    canonical[tiled + 1] = linear[row + 1];
    canonical[tiled + 2] = linear[row + 2];
    canonical[tiled + 3] = g1;
  }
}

kernel void d3d12_swizzle_packed422_dirty(device const uchar *linear [[buffer(0)]],
                                           device const uchar *canonical [[buffer(1)]],
                                           constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                           device atomic_uint *dirty [[buffer(3)]],
                                           uint3 coord [[thread_position_in_grid]]) {
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch + ulong(coord.x) * 8ul;
  const bool changed = p.packed422_variant
      ? linear[row + 1] != canonical[tiled + 0] || linear[row + 0] != canonical[tiled + 1] ||
        linear[row + 2] != canonical[tiled + 3] ||
        (coord.x * 2 + 1 < p.tile_pixel_width && linear[row + 5] != canonical[tiled + 2])
      : linear[row + 0] != canonical[tiled + 0] || linear[row + 1] != canonical[tiled + 1] ||
        linear[row + 2] != canonical[tiled + 2] ||
        (coord.x * 2 + 1 < p.tile_pixel_width && linear[row + 5] != canonical[tiled + 3]);
  if (changed) atomic_store_explicit(dirty, 1u, memory_order_relaxed);
}

kernel void d3d12_swizzle_packed422_export_if_dirty(device const uchar *linear [[buffer(0)]],
                                                     device uchar *canonical [[buffer(1)]],
                                                     constant D3D12StandardSwizzleParams &p [[buffer(2)]],
                                                     device atomic_uint *dirty [[buffer(3)]],
                                                     uint3 coord [[thread_position_in_grid]]) {
  if (!atomic_load_explicit(dirty, memory_order_relaxed)) return;
  const ulong tiled = d3d12_standard_swizzle_offset(p, coord);
  const ulong row = ulong(coord.z) * p.slice_pitch + ulong(coord.y) * p.row_pitch + ulong(coord.x) * 8ul;
  const uchar g1 = coord.x * 2 + 1 < p.tile_pixel_width
      ? linear[row + 5] : canonical[tiled + (p.packed422_variant ? 2 : 3)];
  if (p.packed422_variant) {
    canonical[tiled + 0] = linear[row + 1];
    canonical[tiled + 1] = linear[row + 0];
    canonical[tiled + 2] = g1;
    canonical[tiled + 3] = linear[row + 2];
  } else {
    canonical[tiled + 0] = linear[row + 0];
    canonical[tiled + 1] = linear[row + 1];
    canonical[tiled + 2] = linear[row + 2];
    canonical[tiled + 3] = g1;
  }
}

// Keep the acceleration-structure helper kernels in the same metallib as the existing
// internal kernels: InternalCommandLibrary loads this one library before asking for
// the AS prepass, shadow, postbuild, serialization and visualization kernels.
#include "../winemetal/unix/rt_as_prepass.metal"

#include "../d3d12/d3d12_atomic64_gpu.hpp"

kernel void d3d12_atomic64_import(constant Atomic64TransferParams &p [[buffer(0)]],
                                  uint cell [[thread_position_in_grid]]) {
  device const Atomic64ImportCell *sources = reinterpret_cast<device const Atomic64ImportCell *>(p.sources);
  ulong value = 0;
  for (uint byte = 0; byte < 8; ++byte) {
    const ulong address = sources[cell].bytes[byte];
    if (address) value |= ulong(*reinterpret_cast<device const uchar *>(address)) << (8 * byte);
  }
  reinterpret_cast<device ulong *>(p.baseline)[cell] = value;
  reinterpret_cast<device uint4 *>(p.nodes)[cell] = uint4(0, uint(value), uint(value >> 32), 0);
}

kernel void d3d12_atomic64_export_buffers(constant Atomic64TransferParams &p [[buffer(0)]],
                                          uint cell [[thread_position_in_grid]]) {
  device const Atomic64ImportCell *sources = reinterpret_cast<device const Atomic64ImportCell *>(p.sources);
  const uint root = reinterpret_cast<device const uint *>(p.roots)[cell];
  const uint4 node = reinterpret_cast<device const uint4 *>(p.nodes)[root];
  const ulong value = ulong(node.y) | ulong(node.z) << 32;
  const ulong baseline = reinterpret_cast<device const ulong *>(p.baseline)[cell];
  if (value == baseline) return;
  for (uint byte = 0; byte < 8; ++byte) {
    const ulong address = sources[cell].bytes[byte];
    if (address && uchar(value >> (8 * byte)) != uchar(baseline >> (8 * byte)))
      *reinterpret_cast<device uchar *>(address) = uchar(value >> (8 * byte));
  }
}

static bool d3d12_atomic64_texel(constant Atomic64TransferParams &p, uint3 coord, thread uint4 &value) {
  const ulong begin = p.byteBase + ulong(coord.z) * p.rowPitch * p.height +
                      ulong(coord.y) * p.rowPitch + ulong(coord.x) * p.texelStride;
  device const uint *map = reinterpret_cast<device const uint *>(p.cellMap);
  device const uint *roots = reinterpret_cast<device const uint *>(p.roots);
  device const uint4 *nodes = reinterpret_cast<device const uint4 *>(p.nodes);
  device const ulong *baseline = reinterpret_cast<device const ulong *>(p.baseline);
  // UIntFormat uses 1/2/4/8/16-byte units. Rows are 8-byte aligned and
  // byteBase and texelStride are multiples of unitBytes, including MSAA.
  uint4 words = 0;
  bool changed = false;
  for (uint byte = 0; byte < p.unitBytes; byte += 8) {
    const ulong offset = begin + byte;
    const uint cell = map[offset >> 3];
    if (cell == 0xffffffffu) return false;
    const uint4 node = nodes[roots[cell]];
    const ulong word = ulong(node.y) | ulong(node.z) << 32;
    const uint shift = uint(offset & 7) * 8;
    const ulong packed = word >> shift;
    words[byte / 4] = uint(packed);
    words[byte / 4 + 1] = uint(packed >> 32);
    const ulong diff = (word ^ baseline[cell]) >> shift;
    changed |= p.unitBytes >= 8 ? diff != 0 : (diff << (64 - p.unitBytes * 8)) != 0;
  }
  if (!changed) return false;
  value = uint4(0);
  uint bit = 0;
  for (uint channel = 0; channel < p.channels; ++channel) {
    const uint bits = (p.channelBits >> (channel * 8)) & 255u;
    // Each UIntFormat channel fits one word, including packed RGB10A2.
    value[channel] = (words[bit / 32] >> (bit & 31)) & (~0u >> (32 - bits));
    bit += bits;
  }
  return true;
}

kernel void d3d12_atomic64_export_2d(constant Atomic64TransferParams &p [[buffer(0)]],
                                    texture2d<uint, access::write> destination [[texture(0)]],
                                    uint3 coord [[thread_position_in_grid]]) {
  uint4 value;
  if (d3d12_atomic64_texel(p, coord, value)) destination.write(value, coord.xy);
}
kernel void d3d12_atomic64_export_array(constant Atomic64TransferParams &p [[buffer(0)]],
                                       texture2d_array<uint, access::write> destination [[texture(0)]],
                                       uint3 coord [[thread_position_in_grid]]) {
  uint4 value;
  if (d3d12_atomic64_texel(p, coord, value)) {
    uint3 position = coord;
    if (p.nativePositions)
      position = reinterpret_cast<device const uint4 *>(p.nativePositions)[
          (ulong(coord.z) * p.height + coord.y) * p.width + coord.x].xyz;
    destination.write(value, position.xy, position.z);
  }
}
kernel void d3d12_atomic64_export_3d(constant Atomic64TransferParams &p [[buffer(0)]],
                                    texture3d<uint, access::write> destination [[texture(0)]],
                                    uint3 coord [[thread_position_in_grid]]) {
  uint4 value;
  if (d3d12_atomic64_texel(p, coord, value)) destination.write(value, coord);
}

// Sampler feedback transcoding (src/d3d12/d3d12_sampler_feedback.hpp). A map is u32 cells:
// MIN_MIP holds 255 - mip (0 = no mip requested, decoded 0xFF); MIP_REGION_USED holds 0 or 1
// (decoded 0 or 0xFF). One invocation per decoded R8_UINT texel of the rect.
struct d3d12_feedback_transcode {
  uint cell_base;  // first cell of the subresource grid
  uint grid_w;     // cells per row of the subresource grid
  uint src_x, src_y, width, height;
  uint dst_x, dst_y;
  uint row_pitch;  // bytes per row of the R8 side
  uint min_mip;
};

kernel void d3d12_feedback_decode(device const uint *cells [[buffer(0)]],
                                  device uchar *decoded [[buffer(1)]],
                                  constant d3d12_feedback_transcode &p [[buffer(2)]],
                                  uint index [[thread_position_in_grid]]) {
  if (index >= p.width * p.height) return;
  const uint x = index % p.width, y = index / p.width;
  const uint cell = cells[p.cell_base + (p.src_y + y) * p.grid_w + p.src_x + x];
  decoded[(p.dst_y + y) * p.row_pitch + p.dst_x + x] =
      p.min_mip ? uchar(255u - min(cell, 255u)) : (cell ? uchar(0xff) : uchar(0));
}

kernel void d3d12_feedback_encode(device const uchar *decoded [[buffer(0)]],
                                  device uint *cells [[buffer(1)]],
                                  constant d3d12_feedback_transcode &p [[buffer(2)]],
                                  uint index [[thread_position_in_grid]]) {
  if (index >= p.width * p.height) return;
  const uint x = index % p.width, y = index / p.width;
  const uint value = decoded[(p.src_y + y) * p.row_pitch + p.src_x + x];
  cells[p.cell_base + (p.dst_y + y) * p.grid_w + p.dst_x + x] = p.min_mip ? 255u - value : uint(value != 0);
}

#include "d3d12_msaa_tile_tasks.metal"

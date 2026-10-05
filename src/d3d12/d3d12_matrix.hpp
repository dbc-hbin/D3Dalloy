#pragma once

#include <cstdint>
#include <limits>

namespace dxmt::matrix {
inline uint32_t ComponentBytes(uint32_t type) {
  switch (type) {
  case 2: case 3: case 7: return 2;
  case 4: case 5: case 8: return 4;
  case 18: case 19: case 20: case 21: return 1;
  default: return 0;
  }
}
inline bool MultiplySupported(uint32_t wave, uint32_t a, uint32_t b, uint32_t accumulator) {
  if ((wave && wave != 32) || !ComponentBytes(a) || !ComponentBytes(b)) return false;
  const bool af = a == 7 || a == 8 || a == 20 || a == 21;
  const bool bf = b == 7 || b == 8 || b == 20 || b == 21;
  return af && bf ? accumulator == 7 || accumulator == 8
                  : !af && !bf && (accumulator == 4 || accumulator == 5);
}

// The owned optimal layouts are canonical row-major, shared with dxil_matrix.cpp.
// V1 ignores the supplied stride for opaque layouts, which use dense rows.
inline bool Footprint(uint32_t rows, uint32_t columns, uint32_t type, uint32_t layout,
                      uint32_t stride, bool destination, uint32_t &bytes, uint32_t &effective_stride) {
  const uint32_t component = ComponentBytes(type);
  if (!component || !rows || !columns || layout > 3) return false;
  const uint64_t minor = uint64_t(layout == 1 ? rows : columns) * component;
  const uint64_t major = layout == 1 ? columns : rows;
  uint64_t pitch = layout >= 2 ? minor : stride;
  if (pitch < minor || pitch > std::numeric_limits<uint32_t>::max() ||
      pitch % component || (destination && layout < 2 && stride % 16)) return false;
  const uint64_t size = (major - 1) * pitch + minor;
  const uint64_t aligned = destination ? (size + 15) & ~uint64_t(15) : size;
  if (aligned > std::numeric_limits<uint32_t>::max()) return false;
  bytes = uint32_t(aligned); effective_stride = uint32_t(pitch);
  return true;
}

struct ConversionParams {
  uint32_t rows, columns, component_type, src_layout, dest_layout, src_stride, dest_stride;
};

inline constexpr const char *kConversionSource = R"metal(
#include <metal_stdlib>
using namespace metal;
struct ConversionParams { uint rows, columns, component_type, src_layout, dest_layout, src_stride, dest_stride; };
uint component_bytes(uint t) { return t == 4 || t == 5 || t == 8 ? 4 : t == 2 || t == 3 || t == 7 ? 2 : 1; }
kernel void dxmt_convert_matrix(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]],
                                constant ConversionParams &p [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
  uint column = xy.x, row = xy.y;
  if (row >= p.rows || column >= p.columns) return;
  uint bytes = component_bytes(p.component_type);
  ulong so = p.src_layout == 1 ? ulong(column) * p.src_stride + ulong(row) * bytes
                              : ulong(row) * p.src_stride + ulong(column) * bytes;
  ulong offset = p.dest_layout == 1 ? ulong(column) * p.dest_stride + ulong(row) * bytes
                                  : ulong(row) * p.dest_stride + ulong(column) * bytes;
  // Byte writes own disjoint bytes, including packed FP8/integer8 matrices.
  for (uint i = 0; i < bytes; ++i) dst[offset + i] = src[so + i];
}
)metal";
} // namespace dxmt::matrix

// CPU oracle for PSInvocations of natively multisampled draws with a forced-early-depth PS (D3D11.3 17.18, 3.5.6):
// primitives rasterize in API order against a per-sample depth buffer; a (pixel, primitive) pair counts once iff at
// least one of its covered samples passes the depth test. Standard D3D sample positions (1/16 pixel units), top-left
// fill rule. Shared by owned-d3d12-geometry.cpp and owned-d3d12-tessellation.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace early_depth_oracle {

struct Vertex {
  double x, y, z; // pixels, depth
};
struct Triangle {
  Vertex v[3];
};

inline const std::vector<std::array<int, 2>> &positions(unsigned samples) {
  static const std::vector<std::array<int, 2>> p2 = {{4, 4}, {-4, -4}};
  static const std::vector<std::array<int, 2>> p4 = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
  static const std::vector<std::array<int, 2>> p8 = {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
  return samples == 2 ? p2 : samples == 4 ? p4 : p8;
}

// Top-left rule (D3D11.3 3.4.4.1): a sample on an edge is covered only for top or left edges.
inline bool covers(const Triangle &t, double sx, double sy, double &depth) {
  const double area = (t.v[1].x - t.v[0].x) * (t.v[2].y - t.v[0].y) - (t.v[1].y - t.v[0].y) * (t.v[2].x - t.v[0].x);
  if (area == 0)
    return false;
  const double sign = area > 0 ? 1 : -1;
  double w[3];
  for (int k = 0; k < 3; ++k) {
    const Vertex &a = t.v[(k + 1) % 3], &b = t.v[(k + 2) % 3];
    const double e = ((b.x - a.x) * (sy - a.y) - (b.y - a.y) * (sx - a.x)) * sign;
    if (e < 0)
      return false;
    if (e == 0) {
      const double dx = (b.x - a.x) * sign, dy = (b.y - a.y) * sign;
      const bool top = dy == 0 && dx > 0, left = dy < 0;
      if (!top && !left)
        return false;
    }
    w[k] = e / (area * sign);
  }
  depth = w[0] * t.v[0].z + w[1] * t.v[1].z + w[2] * t.v[2].z;
  return true;
}

/* `occluders` fill the depth buffer first (no count); `drawn` are counted. Depth test LESS_EQUAL, writes on pass;
 * every triangle has the constant depth `z` of its vertices (the fixtures set it by the viewport depth range). */
inline uint64_t ps_invocations(unsigned width, unsigned height, unsigned samples, const std::vector<Triangle> &occluders,
                               const std::vector<Triangle> &drawn) {
  const auto &pos = positions(samples);
  std::vector<double> depth(size_t(width) * height * samples, 1.0);
  uint64_t count = 0;
  auto raster = [&](const Triangle &t, bool counted) {
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        bool passed = false;
        for (unsigned s = 0; s < samples; ++s) {
          double z;
          if (!covers(t, x + 0.5 + pos[s][0] / 16.0, y + 0.5 + pos[s][1] / 16.0, z))
            continue;
          double &d = depth[(size_t(y) * width + x) * samples + s];
          if (z <= d) {
            d = z;
            passed = true;
          }
        }
        count += counted && passed;
      }
  };
  for (const auto &t : occluders)
    raster(t, false);
  for (const auto &t : drawn)
    raster(t, true);
  return count;
}

} // namespace early_depth_oracle

// Independent D3D11/D3D12 fixed-function tessellator domain-point oracle (point sets only), written from the
// D3D11.3 functional spec section 11.7 fixed-point rules. Shared by owned-d3d12-tessellation.cpp; the GSTS
// probe harness checks it against the published reference tessellator before use.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tess_oracle {

enum Partitioning { Integer, Pow2, FractionalOdd, FractionalEven };
struct Point {
  float u, v;
};

using Fxp = uint32_t;
constexpr Fxp kOne = 0x10000, kHalf = 0x8000, kOneThird = 0x5555, kTwoThirds = 0xaaaa;

/* D3D float -> 16.16 fixed conversion: round to nearest even. */
inline Fxp to_fixed(float f) {
  return Fxp(std::nearbyint(double(f) * 65536.0));
}
inline float to_float(Fxp f) {
  return float(f >> 16) + float(f & 0xffff) / 65536.0f;
}
inline uint32_t remove_msb(uint32_t v) {
  return v ? v & ~(1u << (31 - __builtin_clz(v))) : 0;
}
inline Fxp reciprocal(int n) {
  return n ? (kOne + n / 2) / n : 0xffffffffu;
}

struct Edge {
  bool odd;
  Fxp factor;
  int half_points, split;
  Fxp fraction, inv_floor, inv_ceil;

  Edge(Fxp f, bool odd_parity) : odd(odd_parity), factor(f) {
    Fxp half = (f + 1) / 2;
    if (odd || half == kHalf)
      half += kHalf;
    const Fxp fl = half & 0x7fff0000u, ce = (half & 0xffff) ? fl + kOne : half;
    fraction = half - fl;
    half_points = int(ce >> 16);
    if (ce == fl)
      split = half_points + 1;
    else if (odd)
      split = fl == kOne ? 0 : int(remove_msb((fl >> 16) - 1) << 1) + 1;
    else
      split = int(remove_msb(fl >> 16) << 1) + 1;
    int floor_segments = int((fl * 2) >> 16), ceil_segments = int((ce * 2) >> 16);
    if (odd) {
      floor_segments--;
      ceil_segments--;
    }
    inv_floor = reciprocal(floor_segments);
    inv_ceil = reciprocal(ceil_segments);
  }
  int points() const {
    if (odd)
      return int((((kHalf + (factor + 1) / 2) + 0xffff) & 0xffff0000u) * 2 >> 16);
    return int(((((factor + 1) / 2 + 0xffff) & 0xffff0000u) * 2 >> 16) + 1);
  }
  Fxp place(int point) const {
    bool flip = false;
    if (point >= half_points) {
      point = (half_points << 1) - point - (odd ? 1 : 0);
      flip = true;
    }
    if (point == half_points)
      return kHalf;
    const unsigned on_ceil = point, on_floor = point > split ? point - 1 : point;
    Fxp location = on_floor * inv_floor * (kOne - fraction) + on_ceil * inv_ceil * fraction;
    location = (location + kHalf) >> 16;
    return flip ? kOne - location : location;
  }
};

struct Factors {
  float edge[4];
  float inside[2];
};

inline bool culled(const float *edges, int count) {
  for (int i = 0; i < count; ++i)
    if (!(edges[i] > 0))
      return true;
  return false;
}

inline void bounds(Partitioning p, float &lo, float &hi) {
  lo = p == FractionalEven ? 2.0f : 1.0f;
  hi = p == FractionalOdd ? 63.0f : 64.0f;
}

inline bool integral(Partitioning p) {
  return p == Integer || p == Pow2;
}

/* Integer/pow2 parity follows the rounded factor; only an inside factor of 1 is treated as even. */
inline bool parity_odd(Partitioning p, float f, bool inside = false) {
  if (p == FractionalOdd)
    return true;
  if (p == FractionalEven)
    return false;
  return (int(f) & 1) && !(inside && f == 1.0f);
}

/* Unique domain points of a tri patch (u, v; w = 1 - u - v). Empty for a culled patch. */
inline std::vector<Point> tri(Partitioning p, const Factors &in) {
  std::vector<Point> out;
  if (culled(in.edge, 3))
    return out;
  float lo, hi;
  bounds(p, lo, hi);
  float edges[3];
  for (int i = 0; i < 3; ++i) {
    edges[i] = std::min(hi, std::max(lo, in.edge[i]));
    if (integral(p))
      edges[i] = std::ceil(edges[i]);
  }
  float inside_lo = lo;
  if (p == FractionalOdd && (edges[0] > 1.0f || edges[1] > 1.0f || edges[2] > 1.0f))
    inside_lo = 1.0f + 1.0f / 65536.0f;
  float inside = std::min(hi, std::max(inside_lo, in.inside[0]));
  if (integral(p))
    inside = std::ceil(inside);
  if ((integral(p) || p == FractionalOdd) && inside == 1.0f && edges[0] == 1.0f && edges[1] == 1.0f &&
      edges[2] == 1.0f) {
    out = {{0, 1}, {0, 0}, {1, 0}};
    return out;
  }
  const bool inside_odd = parity_odd(p, inside, true);
  Edge inner(to_fixed(inside), inside_odd);
  int inner_points = inner.points();
  // A picture frame needs an inside ring: at least one interior segment.
  const int minimum = inside_odd ? 4 : 3;
  if (inner_points < minimum) {
    inner = Edge(to_fixed(inside_odd ? 3.0f : 2.0f), inside_odd);
    inner_points = inner.points();
  }
  for (int e = 0; e < 3; ++e) {
    Edge edge(to_fixed(edges[e]), parity_odd(p, edges[e]));
    const int end = edge.points() - 1;
    for (int q0 = 0; q0 < end; ++q0) {
      const int q = (e & 1) ? q0 : end - q0;
      const Fxp t = edge.place(q);
      if (e == 0)
        out.push_back({0, to_float(t)});
      else
        out.push_back({to_float(t), e == 2 ? to_float(kOne - t) : 0.0f});
    }
  }
  const int rings = inner_points >> 1;
  for (int ring = 1; ring < rings; ++ring) {
    const int start = ring, end = inner_points - 1 - ring;
    for (int e = 0; e < 3; ++e) {
      Fxp perp = inner.place(start) * kTwoThirds;
      perp = (perp + kHalf) >> 16;
      for (int q0 = start; q0 < end; ++q0) {
        const int q = (e & 1) ? q0 : end - (q0 - start);
        const Fxp t = inner.place(q) - (perp + 1) / 2;
        if (e == 0)
          out.push_back({to_float(perp), to_float(t)});
        else if (e == 1)
          out.push_back({to_float(t), to_float(perp)});
        else
          out.push_back({to_float(t), to_float(kOne - t - perp)});
      }
    }
  }
  if (!inside_odd)
    out.push_back({to_float(kOneThird), to_float(kOneThird)});
  return out;
}

/* Unique domain points of a quad patch (u, v). Empty for a culled patch. */
inline std::vector<Point> quad(Partitioning p, const Factors &in) {
  std::vector<Point> out;
  if (culled(in.edge, 4))
    return out;
  float lo, hi;
  bounds(p, lo, hi);
  float edges[4];
  for (int i = 0; i < 4; ++i) {
    edges[i] = std::min(hi, std::max(lo, in.edge[i]));
    if (integral(p))
      edges[i] = std::ceil(edges[i]);
  }
  float inside_lo = lo;
  if (p == FractionalOdd) {
    bool frame = in.inside[0] > 1.0f || in.inside[1] > 1.0f;
    for (float e : edges)
      frame = frame || e > 1.0f;
    if (frame)
      inside_lo = 1.0f + 1.0f / 65536.0f;
  }
  float inside[2];
  for (int i = 0; i < 2; ++i) {
    inside[i] = std::min(hi, std::max(inside_lo, in.inside[i]));
    if (integral(p))
      inside[i] = std::ceil(inside[i]);
  }
  bool all_one = inside[0] == 1.0f && inside[1] == 1.0f;
  for (float e : edges)
    all_one = all_one && e == 1.0f;
  if ((integral(p) || p == FractionalOdd) && all_one) {
    out = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    return out;
  }
  bool odd[2];
  Edge axis[2] = {Edge(0, false), Edge(0, false)};
  int axis_points[2];
  for (int a = 0; a < 2; ++a) {
    odd[a] = parity_odd(p, inside[a], true);
    axis[a] = Edge(to_fixed(inside[a]), odd[a]);
    axis_points[a] = axis[a].points();
    const int minimum = odd[a] ? 4 : 3;
    if (axis_points[a] < minimum) {
      axis[a] = Edge(to_fixed(odd[a] ? 3.0f : 2.0f), odd[a]);
      axis_points[a] = axis[a].points();
    }
  }
  for (int e = 0; e < 4; ++e) {
    Edge edge(to_fixed(edges[e]), parity_odd(p, edges[e]));
    const int parity = e & 1, end = edge.points() - 1;
    for (int q0 = 0; q0 < end; ++q0) {
      const int q = (e == 1 || e == 2) ? q0 : end - q0;
      const Fxp t = edge.place(q);
      if (parity)
        out.push_back({to_float(t), e == 3 ? 1.0f : 0.0f});
      else
        out.push_back({e == 2 ? 1.0f : 0.0f, to_float(t)});
    }
  }
  const int rings = std::min(axis_points[0], axis_points[1]) >> 1;
  for (int ring = 1; ring < rings; ++ring) {
    const int start = ring;
    const int end[2] = {axis_points[0] - 1 - start, axis_points[1] - 1 - start};
    for (int e = 0; e < 4; ++e) {
      const int perp_axis = e & 1, along = (e + 1) & 1;
      const int perp_point = e < 2 ? start : end[perp_axis];
      const Fxp perp = axis[perp_axis].place(perp_point);
      for (int q0 = start; q0 < end[along]; ++q0) {
        const int q = (e == 1 || e == 2) ? q0 : end[along] - (q0 - start);
        const Fxp t = axis[along].place(q);
        if (along)
          out.push_back({to_float(perp), to_float(t)});
        else
          out.push_back({to_float(t), to_float(perp)});
      }
    }
  }
  // An even inner axis closes the frame with a row (or single point) of centre points.
  if (axis_points[0] > axis_points[1] && !odd[1]) {
    const int start = rings, end = axis_points[0] - 1 - start;
    for (int q = start; q <= end; ++q)
      out.push_back({to_float(axis[0].place(q)), to_float(axis[1].place(start))});
  } else if (axis_points[1] >= axis_points[0] && !odd[0]) {
    const int start = rings, end = axis_points[1] - 1 - start;
    for (int q = end; q >= start; --q)
      out.push_back({to_float(axis[0].place(start)), to_float(axis[1].place(q))});
  }
  return out;
}

/* Isoline domain points (D3D11.3 11.6 / 11.7.9): `detail` is SV_TessFactor[0] (U, the partitioning's rules),
 * `density` SV_TessFactor[1] (V, always integer [1, 64]). Lines at V = k / density for k < density, each with the
 * detail edge's points. Empty for a culled patch. */
inline std::vector<Point> isoline(Partitioning p, float detail, float density) {
  std::vector<Point> out;
  if (!(detail > 0) || !(density > 0))
    return out;
  float lo, hi;
  bounds(p, lo, hi);
  detail = std::min(hi, std::max(lo, detail));
  if (integral(p))
    detail = std::ceil(detail);
  const float lines = std::ceil(std::min(64.0f, std::max(1.0f, density)));
  const Edge u(to_fixed(detail), parity_odd(p, detail));
  const Edge v(to_fixed(lines), (int(lines) & 1) != 0);
  for (int line = 0; line < int(lines); ++line)
    for (int q = 0; q < u.points(); ++q)
      out.push_back({to_float(u.place(q)), to_float(v.place(line))});
  return out;
}

} // namespace tess_oracle

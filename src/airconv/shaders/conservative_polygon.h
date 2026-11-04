/*
 * Shared conservative raster geometry for captured replay and coverage gathering.
 * Homogeneous clipping precedes 16.8 snapping. Outer and inner uncertainty are
 * one fixed-point unit (1/256 pixel), strictly above the 1/512 minimum. All edge
 * predicates use signed 64-bit fixed-point arithmetic, not rounded float products.
 * Post-snap lines/points survive; true pre-snap zero-area triangles may be culled.
 */
#pragma once

#include <metal_stdlib>

namespace d3dalloy_conservative {
using namespace metal;

constant uint kMaxClip = 8;
// W, depth, four viewport guard planes, then user clip planes.
constant uint kMaxVertices = 3 + 3 + 4 + kMaxClip;

inline bool finite_bits(float value) { return (as_type<uint>(value) & 0x7f800000u) != 0x7f800000u; }

inline float2 screen_of(float4 clip, float4 viewport) {
  #pragma clang fp contract(off) reassociate(off)
  return float2(viewport.x + (clip.x / clip.w + 1.0f) * viewport.z * 0.5f,
                viewport.y + (1.0f - clip.y / clip.w) * viewport.w * 0.5f);
}

inline long polygon_area(thread const float2 *polygon, uint count) {
  const long2 origin = long2(polygon[0] * 256.0f);
  long area = 0;
  for (uint i = 0; i < count; ++i) {
    const long2 a = long2(polygon[i] * 256.0f) - origin;
    const long2 b = long2(polygon[(i + 1) % count] * 256.0f) - origin;
    area += a.x * b.y - a.y * b.x;
  }
  return area;
}

inline bool snapped_triangle_degenerate(float4 p0, float4 p1, float4 p2, float4 viewport) {
  if (p0.w == 0.0f || p1.w == 0.0f || p2.w == 0.0f) return false;
  const float2 a = rint(screen_of(p0, viewport) * 256.0f);
  const float2 b = rint(screen_of(p1, viewport) * 256.0f);
  const float2 c = rint(screen_of(p2, viewport) * 256.0f);
  // Exact determinant for bounded projected vertices. Viewport clipping below
  // handles far/horizon vertices without unbounded float-to-int conversion.
  if (all(abs(a) <= 0x1p28f) && all(abs(b) <= 0x1p28f) && all(abs(c) <= 0x1p28f)) {
    const long2 u = long2(b) - long2(a), v = long2(c) - long2(a);
    return u.x * v.y - u.y * v.x == 0;
  }
  const float2 u = b - a, v = c - a;
  return finite_bits(u.x) && finite_bits(u.y) && finite_bits(v.x) && finite_bits(v.y) &&
         u.x * v.y - u.y * v.x == 0.0f;
}

/* det is the original homogeneous orientation. The viewport guard is two pixels
 * outside the viewport, so it cannot change covered pixels or their inner status.
 * It bounds the projected fixed-point coordinates (including perspective horizons)
 * to the D3D viewport domain, keeping every integer product well below int64 limits.
 */
inline uint clipped_polygon(float4 p0, float4 p1, float4 p2, float4 viewport, thread const float *clip,
                            uint clip_count, bool depth_clip, thread float2 *out, thread float &det) {
  #pragma clang fp contract(off) reassociate(off)
  const float3 origin(p0.x, p0.y, p0.w);
  const float3 normal = cross(float3(p1.x, p1.y, p1.w) - origin, float3(p2.x, p2.y, p2.w) - origin);
  det = dot(normal, origin);
  if (!finite_bits(det)) {
    // A finite far triangle can overflow an unscaled homogeneous determinant.
    // Positive per-axis scaling preserves its zero/sign without rejecting it.
    const float3 scale = max(max(abs(float3(p0.x, p0.y, p0.w)), abs(float3(p1.x, p1.y, p1.w))),
                             max(abs(float3(p2.x, p2.y, p2.w)), float3(1.0f)));
    const float3 a = origin / scale, b = float3(p1.x, p1.y, p1.w) / scale,
                 c = float3(p2.x, p2.y, p2.w) / scale;
    det = dot(cross(b - a, c - a), a);
  }
  const float max_w = max(max(p0.w, p1.w), p2.w);
  if (!(det != 0.0f) || !finite_bits(det) || !(max_w > 0.0f) || !finite_bits(max_w) ||
      !(viewport.z > 0.0f) || !(viewport.w > 0.0f))
    return 0;
  clip_count = min(clip_count, kMaxClip);
  float3 polygon[kMaxVertices] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
  float3 next[kMaxVertices];
  uint count = 3;
  const float epsilon = max_w * 0x1p-20f;
  const float3 x(p0.x, p1.x, p2.x), y(p0.y, p1.y, p2.y);
  const float3 z(p0.z, p1.z, p2.z), w(p0.w, p1.w, p2.w);
  const uint fixed_planes = depth_clip ? 7u : 5u;
  bool original_vertices = true;
  for (uint plane = 0; plane < fixed_planes + clip_count && count; ++plane) {
    float3 distance;
    if (plane == 0)
      distance = w - epsilon;
    else if (plane <= 4) {
      // NDC guard is viewport +/- two pixels. Clip before projection/snapping.
      if (plane == 1) distance = x + (1.0f + 4.0f / viewport.z) * w;
      if (plane == 2) distance = (1.0f + 4.0f / viewport.z) * w - x;
      if (plane == 3) distance = y + (1.0f + 4.0f / viewport.w) * w;
      if (plane == 4) distance = (1.0f + 4.0f / viewport.w) * w - y;
    } else if (plane == 5 && depth_clip)
      distance = z;
    else if (plane == 6 && depth_clip)
      distance = w - z;
    else {
      const uint k = plane - fixed_planes;
      distance = float3(clip[k], clip[kMaxClip + k], clip[2 * kMaxClip + k]);
    }
    const float distance_scale = max(max(abs(distance.x), abs(distance.y)), abs(distance.z));
    if (distance_scale > 1.0f) distance /= distance_scale;
    // Before the first clip the three barycentrics are the unit vectors.
    // With finite nonnegative distances every edge emits exactly its first vertex.
    if (original_vertices && all(distance >= 0.0f) && all(distance <= 1.0f)) continue;
    original_vertices = false;
    uint kept = 0;
    for (uint i = 0; i < count; ++i) {
      const float3 a = polygon[i], b = polygon[(i + 1) % count];
      const float da = dot(distance, a), db = dot(distance, b);
      if (da >= 0.0f && kept < kMaxVertices) next[kept++] = a;
      if ((da >= 0.0f) != (db >= 0.0f) && kept < kMaxVertices) next[kept++] = mix(a, b, da / (da - db));
    }
    count = kept;
    for (uint i = 0; i < count; ++i) polygon[i] = next[i];
  }
  if (!count) return 0;
  for (uint i = 0; i < count; ++i) {
    const float3 b = polygon[i];
    const float4 position(dot(x, b), dot(y, b), dot(z, b), max(dot(w, b), epsilon));
    out[i] = rint(screen_of(position, viewport) * 256.0f) / 256.0f;
    if (!finite_bits(out[i].x) || !finite_bits(out[i].y)) return 0;
  }
  // Rounding a clipped convex polygon can turn nearly collinear vertices inward.
  // SAT needs the convex hull of its snapped vertices, not those spurious edges.
  if (count > 3) {
    bool positive = false, negative = false;
    for (uint i = 0; i < count; ++i) {
      const long2 a = long2(out[i] * 256.0f), b = long2(out[(i + 1) % count] * 256.0f),
                  c = long2(out[(i + 2) % count] * 256.0f);
      const long cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
      positive |= cross > 0; negative |= cross < 0;
    }
    if (positive && negative) {
      float2 hull[kMaxVertices];
      uint first = 0;
      for (uint i = 1; i < count; ++i)
        if (out[i].x < out[first].x || (out[i].x == out[first].x && out[i].y < out[first].y)) first = i;
      uint current = first, kept = 0;
      do {
        hull[kept++] = out[current];
        uint next = (current + 1) % count;
        const long2 a = long2(out[current] * 256.0f);
        for (uint i = 0; i < count; ++i) {
          const long2 u = long2(out[next] * 256.0f) - a, v = long2(out[i] * 256.0f) - a;
          const long cross = u.x * v.y - u.y * v.x;
          if (cross < 0 || (cross == 0 && v.x * v.x + v.y * v.y > u.x * u.x + u.y * u.y)) next = i;
        }
        current = next;
      } while (current != first && kept < count);
      count = kept;
      for (uint i = 0; i < count; ++i) out[i] = hull[i];
    }
  }
  return count;
}

/* SAT against a pixel square, with a one-unit Minkowski dilation. For a snapped
 * degenerate, its collinear edges constrain both sides of the line; the axis bounds
 * constrain its endpoints (and a point). Contact belongs to the uncertainty region.
 */
inline bool square_coverage(thread const float2 *polygon, uint count, float2 pixel,
                            long area, thread bool &inner) {
  const long2 origin = long2(pixel * 256.0f);
  long2 low = long2(0x7fffffffffffffffL), high = long2(-0x7fffffffffffffffL);
  inner = false;
  const long orientation = area >= 0 ? 1 : -1;
  for (uint i = 0; i < count; ++i) {
    const long2 a = long2(polygon[i] * 256.0f) - origin;
    low = min(low, a);
    high = max(high, a);
  }
  if (high.x < -1 || low.x > 257 || high.y < -1 || low.y > 257) return false;
  bool all_inside = area != 0;
  for (uint i = 0; i < count; ++i) {
    const long2 a = long2(polygon[i] * 256.0f) - origin;
    const long2 b = long2(polygon[(i + 1) % count] * 256.0f) - origin;
    const long2 normal = long2(b.y - a.y, a.x - b.x) * orientation;
    const long nearest = min(0L, normal.x * 256) + min(0L, normal.y * 256);
    const long uncertainty = long(abs(normal.x)) + long(abs(normal.y));
    const long edge = normal.x * a.x + normal.y * a.y;
    if (nearest > edge + uncertainty) return false;
    // All four corners must clear every nonzero edge, eroded by one
    // fixed-point unit. Snapped lines and points can only have outer coverage.
    const long farthest = max(0L, normal.x * 256) + max(0L, normal.y * 256);
    all_inside &= !uncertainty || farthest < edge - uncertainty;
  }
  inner = all_inside;
  return true;
}

} // namespace d3dalloy_conservative

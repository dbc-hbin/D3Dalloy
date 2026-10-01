/*
 * D3D12 conservative rasterization: the exact screen polygon of one triangle, shared by the replay vertex helper
 * (src/airconv/shaders/air_conservative.metal) and the coverage gather fragment msaa_gather_conservative_depth
 * (src/dxmt/dxmt_command.metal, compiled with fast math: finiteness is tested on the bit patterns).
 *
 * The primitive is the original triangle clipped in homogeneous space to w >= epsilon, to 0 <= z <= w when depth
 * clipping is enabled and to every SV_ClipDistance >= 0; the clipped vertices are projected through the viewport
 * and snapped to the 16.8 fixed-point grid. A snapped polygon with zero area is culled (Tier 1 permits culling
 * post-snap degenerates). A pixel is conservatively rasterized when its square overlaps this polygon with positive
 * area: no false negatives, and no false positives beyond the fixed-point snapping of the vertices (inside the
 * 1/2 pixel uncertainty region of Tier 1). SV_CullDistance is applied to the whole primitive by the replay.
 *
 * `clip` holds the clip distances of the three original vertices (vertex v, distance k at clip[v * 8 + k]).
 */
#pragma once

#include <metal_stdlib>

namespace dxmt_conservative {

using namespace metal;

constant uint kMaxClip = 8;
/* Each clip plane (w >= epsilon, z >= 0, z <= w, then the clip distances) adds at most one vertex. */
constant uint kMaxVertices = 3 + 3 + kMaxClip;

inline bool finite_bits(float value) { return (as_type<uint>(value) & 0x7f800000u) != 0x7f800000u; }

inline float2 screen_of(float4 clip, float4 viewport) {
  return float2(viewport.x + (clip.x / clip.w + 1.0f) * viewport.z * 0.5f,
                viewport.y + (1.0f - clip.y / clip.w) * viewport.w * 0.5f);
}

/* Returns the vertex count (0 when culled); `det` is the projected orientation (positive: counter-clockwise in
 * NDC), valid for any W signs. */
inline uint clipped_polygon(float4 p0, float4 p1, float4 p2, float4 viewport, thread const float *clip,
                            uint clip_count, bool depth_clip, thread float2 *out, thread float &det) {
  det = determinant(float3x3(float3(p0.x, p0.y, p0.w), float3(p1.x, p1.y, p1.w), float3(p2.x, p2.y, p2.w)));
  const float max_w = max(max(p0.w, p1.w), p2.w);
  if (!(det != 0.0f) || !finite_bits(det) || !(max_w > 0.0f) || !finite_bits(max_w) ||
      !(viewport.z > 0.0f) || !(viewport.w > 0.0f))
    return 0;
  clip_count = min(clip_count, kMaxClip);
  // Barycentric weights carry the clip distances through homogeneous clipping.
  float3 polygon[kMaxVertices] = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
  float3 next[kMaxVertices];
  uint count = 3;
  const float epsilon = max_w * 0x1p-20f;
  const float3 x = float3(p0.x, p1.x, p2.x), y = float3(p0.y, p1.y, p2.y);
  const float3 z = float3(p0.z, p1.z, p2.z), w = float3(p0.w, p1.w, p2.w);
  const uint fixed_planes = depth_clip ? 3u : 1u;
  for (uint plane = 0; plane < fixed_planes + clip_count && count; plane++) {
    // Per-vertex signed distance of the plane: w - epsilon, z, w - z, then each clip distance.
    float3 distance;
    if (plane == 0)
      distance = w - epsilon;
    else if (plane == 1 && depth_clip)
      distance = z;
    else if (plane == 2 && depth_clip)
      distance = w - z;
    else {
      const uint k = plane - fixed_planes;
      distance = float3(clip[k], clip[kMaxClip + k], clip[2 * kMaxClip + k]);
    }
    uint kept = 0;
    for (uint i = 0; i < count; i++) {
      const float3 a = polygon[i], b = polygon[(i + 1) % count];
      const float da = dot(distance, a), db = dot(distance, b);
      if (da >= 0.0f && kept < kMaxVertices) next[kept++] = a;
      if ((da >= 0.0f) != (db >= 0.0f) && kept < kMaxVertices) next[kept++] = mix(a, b, da / (da - db));
    }
    count = kept;
    for (uint i = 0; i < count; i++) polygon[i] = next[i];
  }
  if (count < 3)
    return 0;
  for (uint i = 0; i < count; i++) {
    const float3 b = polygon[i];
    const float4 position = float4(dot(x, b), dot(y, b), dot(z, b), max(dot(w, b), epsilon));
    out[i] = rint(screen_of(position, viewport) * 256.0f) / 256.0f;
    if (!finite_bits(out[i].x) || !finite_bits(out[i].y))
      return 0;
  }
  float twice_area = 0.0f;
  for (uint i = 0; i < count; i++) {
    const float2 a = out[i] - out[0], b = out[(i + 1) % count] - out[0];
    twice_area += a.x * b.y - a.y * b.x;
  }
  return twice_area != 0.0f ? count : 0;
}

/* Separating-axis test of the convex polygon against the pixel square [pixel, pixel + 1]: true when they overlap
 * with positive area. Coordinates are taken relative to the pixel so large targets keep their precision; snapped
 * coordinates are multiples of 1/256, so every product below is exact for targets up to 2^14 pixels. */
inline bool square_overlaps(thread const float2 *polygon, uint count, float2 pixel) {
  float2 low = float2(INFINITY), high = float2(-INFINITY);
  float twice_area = 0.0f;
  for (uint i = 0; i < count; i++) {
    const float2 a = polygon[i] - pixel, b = polygon[(i + 1) % count] - pixel;
    low = min(low, a);
    high = max(high, a);
    twice_area += a.x * b.y - a.y * b.x;
  }
  if (!(high.x > 0.0f && low.x < 1.0f && high.y > 0.0f && low.y < 1.0f))
    return false;
  const float orientation = twice_area > 0.0f ? 1.0f : -1.0f;
  for (uint i = 0; i < count; i++) {
    const float2 a = polygon[i] - pixel, b = polygon[(i + 1) % count] - pixel;
    // Outward normal of edge a -> b; the square is separated when its nearest corner is not inside.
    const float2 normal = float2(b.y - a.y, a.x - b.x) * orientation;
    if (normal.x == 0.0f && normal.y == 0.0f)
      continue;
    const float nearest = min(0.0f, normal.x) + min(0.0f, normal.y);
    if (nearest >= dot(normal, a))
      return false;
  }
  return true;
}

} // namespace dxmt_conservative

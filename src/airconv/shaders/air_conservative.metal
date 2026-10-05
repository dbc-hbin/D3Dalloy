#include <metal_stdlib>

#include "conservative_polygon.h"

using namespace metal;

/*
 * D3D12 conservative rasterization helpers, linked into the captured-vertex replay function (msaa_vertex.cpp,
 * SM50_SHADER_D3D12_CONSERVATIVE_RASTER).
 *
 * The replay draws the screen rectangle around the clipped primitive (conservative_polygon.h) grown by two pixels
 * and limited to the viewport grown by two pixels: it contains every sample of every conservatively covered pixel,
 * and msaa_gather_conservative_depth decides each pixel exactly. The rectangle is split by the line 1/w = kappa of
 * the triangle's plane into two convex fans of up to three triangles, wound like the original triangle so native
 * face selection and culling are unchanged:
 * - corners 0-4, the front part (1/w >= kappa): each corner is the point of the triangle's homogeneous plane that
 *   projects onto it, with affine weights (alpha0, alpha1, alpha2, 1), sum(alpha) = 1. sum(alpha * clip) and
 *   sum(alpha * attribute) are the exact plane extrapolation; native perspective-correct interpolation reproduces
 *   it everywhere, so attributes, SV_Position and depth are exact.
 * - corners 5-9, the remainder, where the plane lies beyond the horizon or behind the eye (only reached by the
 *   extrapolation of perspective slivers): the exact screen-affine depth z/w of the plane at constant W, attributes
 *   of the cut line.
 * dxmt.conservative.corner returns the attribute weights (zero rejects the corner),
 * dxmt.conservative.corner_position the clip position. `flags`: bits 0-3 corner, bit 4 depth clipping (clip and
 * cull distances only shrink the primitive, the gather applies them); bit 5 FrontCounterClockwise.
 */

namespace {

float2 ndc_of(float2 screen, float4 viewport) {
  return float2((screen.x - viewport.x) / viewport.z * 2.0f - 1.0f,
                1.0f - (screen.y - viewport.y) / viewport.w * 2.0f);
}

/* The plane point projecting onto `ndc` spans the null space of the two projection equations; returns its 1/w. */
float plane_point(float4 p0, float4 p1, float4 p2, float2 ndc, thread float3 &alpha) {
  #pragma clang fp contract(off) reassociate(off)
  const float3 scale = max(max(abs(float3(p0.x, p0.y, p0.w)), abs(float3(p1.x, p1.y, p1.w))),
                           max(abs(float3(p2.x, p2.y, p2.w)), float3(1.0f)));
  p0.xyw /= scale; p1.xyw /= scale; p2.xyw /= scale;
  ndc *= float2(scale.z / scale.x, scale.z / scale.y);
  const float3 w = float3(p0.w, p1.w, p2.w);
  const float3 u = float3(p0.x, p1.x, p2.x) - ndc.x * w;
  const float3 v = float3(p0.y, p1.y, p2.y) - ndc.y * w;
  const float3 a = cross(u, v);
  // Evaluate the affine 1/w plane from one fixed homogeneous determinant. Summing the
  // three projection cofactors and then dotting their nearly cancelling weights with W
  // is unstable at the horizon of a sliver, creating rejected fan corners and coverage holes.
  const float3 origin(p0.x, p0.y, p0.w);
  const float3 normal = cross(float3(p1.x, p1.y, p1.w) - origin, float3(p2.x, p2.y, p2.w) - origin);
  const float sum = dot(normal, float3(ndc, 1.0f));
  alpha = a / sum;
  alpha.x = 1.0f - alpha.y - alpha.z;
  return (sum / dot(normal, origin)) / scale.z;
}

} // namespace

/* Corner `flags & 15` (0-9) of the replay geometry; see the file comment. */
struct ConservativeCorner {
  float4 alpha;     // affine weights (alpha0, alpha1, alpha2, 1) of the attributes; zero rejects
  float4 position;  // clip position to emit
};

ConservativeCorner corner_of(float4 p0, float4 p1, float4 p2, float4 viewport, uint flags) {
  const ConservativeCorner reject = {float4(0.0f), float4(-2.0f, -2.0f, 0.0f, 1.0f)};
  float det;
  float2 clipped[dxmt_conservative::kMaxVertices];
  // Clip and cull distances only shrink the primitive; the gather applies them exactly.
  const uint clipped_count =
      dxmt_conservative::clipped_polygon(p0, p1, p2, viewport, nullptr, 0, flags & 16, clipped, det);
  if (!clipped_count)
    return reject;
  const bool degenerate = !dxmt_conservative::polygon_area(clipped, clipped_count) ||
      dxmt_conservative::snapped_triangle_degenerate(p0, p1, p2, viewport);
  // Coverage uses the clipped polygon, but interpolation must use the viewport-scaled
  // 16.8 vertices, just as ordinary D3D rasterization does. Extrapolating the
  // unsnapped plane changes both per-sample depth and perspective attributes.
  float4 positions[3] = {p0, p1, p2};
  for (uint i = 0; i < 3; ++i) {
    // A projective direction at W == 0 has no finite screen coordinate.
    // Keep its homogeneous numerator; clipping produces finite raster vertices.
    if (positions[i].w == 0.0f)
      continue;
    const float2 screen = rint(dxmt_conservative::screen_of(positions[i], viewport) * 256.0f) / 256.0f;
    if (all(isfinite(screen))) positions[i].xy = ndc_of(screen, viewport) * positions[i].w;
  }
  p0 = positions[0];
  p1 = positions[1];
  p2 = positions[2];
  float2 low = float2(INFINITY), high = float2(-INFINITY);
  for (uint i = 0; i < clipped_count; i++) {
    low = min(low, clipped[i]);
    high = max(high, clipped[i]);
  }
  low = max(low - 2.0f, viewport.xy - 2.0f);
  high = min(high + 2.0f, viewport.xy + viewport.zw + 2.0f);
  if (!all(low < high))
    return reject;
  // Clockwise in NDC (screen y points down).
  const float2 rectangle[4] = {low, float2(high.x, low.y), high, float2(low.x, high.y)};
  if (degenerate) {
    // Post-snap degenerates must rasterize, are back-facing under either PSO winding,
    // and use vertex zero for every interpolated attribute (including depth).
    if ((flags & 15) >= 5) return reject;
    const uint k = min(flags & 15, 3u);
    const uint index = (flags & 32) == 0 && k ? 4 - k : k;
    const float depth = p0.z / (p0.w != 0.0f ? p0.w : 1.0f);
    const float clip_w = p0.w > 0.0f ? p0.w : 1.0f;
    return {float4(1, 0, 0, 1), float4(ndc_of(rectangle[index], viewport) * clip_w, depth * clip_w, clip_w)};
  }
  // Inside the clipped primitive 1/w >= 1 / max_w > kappa, so the front part always contains it.
  const float kappa = (1.0f / max(max(p0.w, p1.w), p2.w)) / 1024.0f;
  float g[4];
  for (uint k = 0; k < 4; k++) {
    float3 alpha;
    g[k] = plane_point(p0, p1, p2, ndc_of(rectangle[k], viewport), alpha);
    if (!isfinite(g[k])) g[k] = -INFINITY;
  }
  // 1/w is affine on the screen: the half-plane 1/w >= kappa splits the rectangle into the front part (exact plane
  // geometry) and the remainder, where the plane is beyond the horizon or behind the eye.
  const bool back = (flags & 15) >= 5;
  float2 polygon[5];
  bool cut[5];
  uint count = 0;
  for (uint k = 0; k < 4; k++) {
    const uint next = (k + 1) % 4;
    const bool inside = g[k] >= kappa, next_inside = g[next] >= kappa;
    if (inside != back && count < 5) {
      cut[count] = false;
      polygon[count++] = rectangle[k];
    }
    if (inside != next_inside && count < 5) {
      const float t = isfinite(g[k]) && isfinite(g[next]) ? (g[k] - kappa) / (g[k] - g[next])
                    : inside ? 1.0f : 0.0f;
      cut[count] = true;
      polygon[count++] = mix(rectangle[k], rectangle[next], t);
    }
  }
  if (count < 3)
    return reject;
  // Fan (0, 1, 2), (0, 2, 3), (0, 3, 4) of each part: missing corners repeat the last one (empty triangles). A
  // counter-clockwise original (det > 0, the orientation of every clipped part of it) walks the polygon backwards.
  const uint k = min((flags & 15) - (back ? 5u : 0u), count - 1);
  const uint index = det > 0.0f && k ? count - k : k;
  const float2 ndc = ndc_of(polygon[index], viewport);
  float3 alpha;
  const float reciprocal_w = plane_point(p0, p1, p2, ndc, alpha);
  if (!back) {
    if (!(reciprocal_w > 0.0f) || !all(isfinite(alpha)))
      return reject;
    const float clip_w = 1.0f / reciprocal_w;
    return {float4(alpha, 1.0f), float4(ndc * clip_w, dot(alpha, float3(p0.z, p1.z, p2.z)), clip_w)};
  }
  // Remainder: constant W = 1 / kappa (the front part's W along the shared cut), exact screen-affine depth
  // z/w of the plane, and the attributes of the first cut point (finite; D3D leaves them undefined beyond the
  // horizon).
  const float depth = dot(alpha, float3(p0.z, p1.z, p2.z)) * reciprocal_w;
  uint first_cut = 0;
  while (first_cut < count && !cut[first_cut]) first_cut++;
  if (first_cut == count || !isfinite(depth))
    return reject;
  float3 cut_alpha;
  plane_point(p0, p1, p2, ndc_of(polygon[first_cut], viewport), cut_alpha);
  if (!all(isfinite(cut_alpha)))
    return reject;
  const float big_w = 1.0f / kappa;
  return {float4(cut_alpha, 1.0f), float4(ndc * big_w, depth * big_w, big_w)};
}

float4 conservative_corner(float4 p0, float4 p1, float4 p2, float4 viewport, uint flags)
    asm("dxmt.conservative.corner");
float4 conservative_corner(float4 p0, float4 p1, float4 p2, float4 viewport, uint flags) {
  return corner_of(p0, p1, p2, viewport, flags).alpha;
}

float4 conservative_corner_position(float4 p0, float4 p1, float4 p2, float4 viewport, uint flags)
    asm("dxmt.conservative.corner_position");
float4 conservative_corner_position(float4 p0, float4 p1, float4 p2, float4 viewport, uint flags) {
  return corner_of(p0, p1, p2, viewport, flags).position;
}

uint conservative_degenerate(float4 p0, float4 p1, float4 p2, float4 viewport, uint depth_clip)
    asm("dxmt.conservative.degenerate");
uint conservative_degenerate(float4 p0, float4 p1, float4 p2, float4 viewport, uint depth_clip) {
  float det;
  float2 polygon[dxmt_conservative::kMaxVertices];
  const uint count = dxmt_conservative::clipped_polygon(p0, p1, p2, viewport, nullptr, 0, depth_clip != 0,
                                                        polygon, det);
  return count && (!dxmt_conservative::polygon_area(polygon, count) ||
                  dxmt_conservative::snapped_triangle_degenerate(p0, p1, p2, viewport));
}

uint conservative_polygon(float4 p0, float4 p1, float4 p2, float4 viewport, thread const float *clip,
                          uint flags, thread float4 *out) asm("dxmt.conservative.polygon");

/* The clipped, snapped screen polygon (conservative_polygon.h) as kMaxVertices / 2 float4 vertex pairs; returns
 * the vertex count, 0 for a culled primitive. Unused pairs repeat the last vertex. `flags`: bit 3 depth clipping,
 * bits 4-7 the clip distance count. */
uint conservative_polygon(float4 p0, float4 p1, float4 p2, float4 viewport, thread const float *clip,
                          uint flags, thread float4 *out) {
  float det;
  float2 polygon[dxmt_conservative::kMaxVertices];
  const uint count = dxmt_conservative::clipped_polygon(p0, p1, p2, viewport, clip, (flags >> 4) & 15, flags & 8,
                                                        polygon, det);
  for (uint part = 0; part < dxmt_conservative::kMaxVertices / 2; part++)
    out[part] = count ? float4(polygon[min(2 * part, count - 1)], polygon[min(2 * part + 1, count - 1)])
                      : float4(0.0f);
  return count;
}

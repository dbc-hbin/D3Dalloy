// Derived from AMD FidelityFX SDK v2.1.0 RCAS at commit
// 0836aa7f24058b3f8d035a88c0774457d78fc98e.
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#include <metal_stdlib>
using namespace metal;

// SR and FG encode exact pixel grids with dispatchThreads, including the
// partial threadgroup at an edge. Active rects can still be smaller than a grid.
struct YaaglFsrParams {
    uint2 sourceOrigin;
    uint2 compositionOrigin;
    uint2 destinationOrigin;
    uint2 extent;
    uint2 dispatchOrigin;
    uint2 dispatchExtent;
    uint transfer;
    uint flags;
    float sharpness;
    float exposure;
};

enum : uint {
    YaaglFsrHasReactive = 1u << 0,
    YaaglFsrHasComposition = 1u << 1,
    YaaglFsrSharpen = 1u << 2,
    YaaglFsrHasExposure = 1u << 3,
    YaaglFsrCenteredOutput = 1u << 4,
    YaaglFsrAlphaUpscaling = 1u << 5,
};

static float3 inverse_srgb(float3 value) {
    return select(value / 12.92f, powr((value + 0.055f) / 1.055f, 2.4f),
                  value > 0.04045f);
}

static float3 forward_srgb(float3 value) {
    value = max(value, 0.0f);
    return select(value * 12.92f, 1.055f * powr(value, 1.0f / 2.4f) - 0.055f,
                  value > 0.0031308f);
}

static float3 inverse_pq(float3 value) {
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    float3 p = powr(clamp(value, 0.0f, 1.0f), 1.0f / m2);
    return powr(max(p - c1, 0.0f) / max(c2 - c3 * p, 1.0e-7f), 1.0f / m1);
}

static float3 forward_pq(float3 value) {
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    float3 p = powr(max(value, 0.0f), m1);
    return powr((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

static float3 inverse_transfer(float3 value, uint transfer) {
    if (transfer == 1u) return inverse_srgb(value);
    if (transfer == 2u) return inverse_pq(value);
    return value;
}

static float3 forward_transfer(float3 value, uint transfer) {
    if (transfer == 1u) return forward_srgb(value);
    if (transfer == 2u) return forward_pq(value);
    return value;
}

kernel void yaagl_fsr_linearize(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 sourcePosition = params.sourceOrigin + tid;
    uint2 destinationPosition = params.destinationOrigin + tid;
    float4 color = source.read(sourcePosition);
    color.rgb = inverse_transfer(color.rgb, params.transfer);
    destination.write(color, destinationPosition);
}

// Same transfer and float-to-storage conversions as the two standalone passes.
// Alpha padding reads clamped source coverage; only active color is transformed,
// and its existing edge-extension pass consumes the stored result afterward.
kernel void yaagl_sr_linearize_alpha(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> colorDestination [[texture(1)]],
    texture2d<float, access::write> alphaDestination [[texture(2)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    float4 color = source.read(params.sourceOrigin + min(tid, params.extent - 1u));
    alphaDestination.write(float4(color.a, color.a, color.a, 1.0f), tid);
    if (all(tid < params.extent)) {
        color.rgb = inverse_transfer(color.rgb, params.transfer);
        colorDestination.write(color, params.destinationOrigin + tid);
    }
}

kernel void yaagl_fsr_combine_masks(
    texture2d<float, access::read> reactive [[texture(0)]],
    texture2d<float, access::read> composition [[texture(1)]],
    texture2d<float, access::write> destination [[texture(2)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    // Dispatch covers the descriptor capacity; padding replicates active edges.
    uint2 sourceOffset = min(tid, params.extent - 1u);
    float r = (params.flags & YaaglFsrHasReactive)
        ? reactive.read(params.sourceOrigin + sourceOffset).r : 0.0f;
    float c = (params.flags & YaaglFsrHasComposition)
        ? composition.read(params.compositionOrigin + sourceOffset).r : 0.0f;
    destination.write(float4(max(clamp(r, 0.0f, 1.0f), clamp(c, 0.0f, 1.0f))),
                      params.destinationOrigin + tid);
}

kernel void yaagl_fsr_extend_edges(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 position = params.dispatchOrigin + tid;
    // Source and destination alias, but active source pixels are never written.
    destination.write(source.read(min(position, params.extent - 1u)), position);
}

// Copy an active subrect to origin and replicate its edges over the dispatch
// extent. Half/float element types preserve the stored source values exactly.
kernel void yaagl_sr_copy_pad_half(
    texture2d<half, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    destination.write(source.read(params.sourceOrigin + min(tid, params.extent - 1u)), tid);
}

kernel void yaagl_sr_copy_pad_float(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    destination.write(source.read(params.sourceOrigin + min(tid, params.extent - 1u)), tid);
}

static float3 rcas(float3 b, float3 d, float3 e, float3 f, float3 h, float sharpness, float exposure) {
    float bL = b.b * 0.5f + (b.r * 0.5f + b.g);
    float dL = d.b * 0.5f + (d.r * 0.5f + d.g);
    float eL = e.b * 0.5f + (e.r * 0.5f + e.g);
    float fL = f.b * 0.5f + (f.r * 0.5f + f.g);
    float hL = h.b * 0.5f + (h.r * 0.5f + h.g);

    float range = max(max(max(bL, dL), max(eL, fL)), hL) -
                  min(min(min(bL, dL), min(eL, fL)), hL);
    float nz = saturate(abs(0.25f * (bL + dL + fL + hL) - eL) / range);
    nz = -0.5f * nz + 1.0f;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float lowerLimiterMultiplier = saturate(eL / min(min(bL, dL), min(fL, hL)));
    float3 hitMin = mn4 / (4.0f * mx4) * lowerLimiterMultiplier;
    float3 hitMax = (1.0f - mx4) / (4.0f * mn4 - 4.0f);
    float3 lobes = max(-hitMin, hitMax);
    float lobe = max(-0.1875f, min(max(max(lobes.r, lobes.g), lobes.b), 0.0f));

    // FidelityFX maps API sharpness [0,1] to stops (2 - 2*sharpness), then FsrRcasCon
    // maps stops with exp2(-stops).
    lobe *= exp2(-((-2.0f * sharpness) + 2.0f)) * nz;
    float reciprocal = 1.0f / (4.0f * lobe + 1.0f);
    return (lobe * (b + d + h + f) + e) * reciprocal / exposure;
}

kernel void yaagl_fsr_finish(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    texture2d<float, access::read> exposureTexture [[texture(2)]],
    texture2d<float, access::read> alphaTexture [[texture(3)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 destinationPosition = params.dispatchOrigin + tid;
    if ((params.flags & YaaglFsrCenteredOutput) &&
        (any(destinationPosition < params.destinationOrigin) ||
         any(destinationPosition >= params.destinationOrigin + params.extent))) {
        destination.write(float4(0.0f, 0.0f, 0.0f, 1.0f), destinationPosition);
        return;
    }
    uint2 sourcePosition = params.sourceOrigin + destinationPosition - params.destinationOrigin;
    float4 color = source.read(sourcePosition);
    if (params.flags & YaaglFsrAlphaUpscaling)
        color.a = clamp(alphaTexture.read(sourcePosition).r, 0.0f, 1.0f);
    if (params.flags & YaaglFsrSharpen) {
        float exposure = (params.flags & YaaglFsrHasExposure)
            ? max(exposureTexture.read(uint2(0)).r, 1.0e-7f) : params.exposure;
        uint2 lower = params.sourceOrigin, upper = lower + params.extent - 1u;
        color.rgb = rcas(source.read(uint2(sourcePosition.x, max(sourcePosition.y, lower.y + 1u) - 1u)).rgb * exposure,
                         source.read(uint2(max(sourcePosition.x, lower.x + 1u) - 1u, sourcePosition.y)).rgb * exposure,
                         color.rgb * exposure,
                         source.read(uint2(min(sourcePosition.x + 1u, upper.x), sourcePosition.y)).rgb * exposure,
                         source.read(uint2(sourcePosition.x, min(sourcePosition.y + 1u, upper.y))).rgb * exposure,
                         params.sharpness, exposure);
    }
    color.rgb = forward_transfer(color.rgb, params.transfer);
    destination.write(color, destinationPosition);
}

// Keep the two independent RCAS evaluations from being reassociated together:
// inlining changes the last UNORM code on values near the rounding boundary.
__attribute__((noinline))
static float3 rcas_pair_taps(float3 b, float3 d, float3 e, float3 f, float3 h, float sharpness, float exposure) {
    return rcas(b,d,e,f,h,sharpness,exposure);
}

// Two adjacent output pixels share their horizontal taps. The linear finish
// path supplies a zero-origin scratch image and dispatches width / 2 threads.
kernel void yaagl_sr_rcas_pair(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& p [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 a = uint2(2u * tid.x, tid.y), b = a + uint2(1,0);
    uint top = max(a.y, 1u) - 1u, bottom = min(a.y + 1u, p.extent.y - 1u);
    float4 left = source.read(a), right = source.read(b);
    float3 l = left.rgb * p.exposure, r = right.rgb * p.exposure;
    left.rgb = rcas_pair_taps(source.read(uint2(a.x,top)).rgb * p.exposure,
                    source.read(uint2(max(a.x,1u)-1u,a.y)).rgb * p.exposure, l, r,
                    source.read(uint2(a.x,bottom)).rgb * p.exposure, p.sharpness, p.exposure);
    right.rgb = rcas_pair_taps(source.read(uint2(b.x,top)).rgb * p.exposure, l, r,
                     source.read(uint2(min(b.x+1u,p.extent.x-1u),b.y)).rgb * p.exposure,
                     source.read(uint2(b.x,bottom)).rgb * p.exposure, p.sharpness, p.exposure);
    destination.write(left, p.destinationOrigin + a);
    destination.write(right, p.destinationOrigin + b);
}

// Alpha is a linear coverage signal, never scene luminance or exposure. Native
// temporal reconstruction consumes its replicated RGB, not MetalFX output.a.
kernel void yaagl_sr_extract_alpha(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFsrParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    float alpha = source.read(params.sourceOrigin + min(tid, params.extent - 1u)).a;
    destination.write(float4(alpha, alpha, alpha, 1.0f), tid);
}

struct YaaglSrDebugParams {
    uint2 outputOrigin;
    uint2 outputExtent;
    uint2 depthOrigin;
    uint2 depthExtent;
    uint2 motionOrigin;
    uint2 motionExtent;
    float2 motionScale;
    uint depthInverted;
    uint transfer;
};

// Provider diagnostic layout: thirds show reconstructed color, device
// depth (white = far), and signed pixel motion (RG; B = magnitude / 32 pixels).
static float sr_debug_depth(texture2d<float, access::read> depth, uint2 p) { return depth.read(p).r; }
static float sr_debug_depth(depth2d<float, access::read> depth, uint2 p) { return depth.read(p); }

template<typename Depth>
static void sr_debug_view(texture2d<float, access::read> color, Depth depth,
    texture2d<float, access::read> motion, texture2d<float, access::write> output,
    constant YaaglSrDebugParams& params, uint2 tid) {
    uint panel = min((tid.x * 3u) / params.outputExtent.x, 2u);
    uint start = (panel * params.outputExtent.x + 2u) / 3u;
    uint end = ((panel + 1u) * params.outputExtent.x + 2u) / 3u;
    float2 uv = (float2(tid.x - start, tid.y) + 0.5f) /
                float2(max(end - start, 1u), params.outputExtent.y);
    float3 value;
    if (panel == 0u) {
        uint2 p = min(uint2(uv * float2(color.get_width(), color.get_height())),
                      uint2(color.get_width(), color.get_height()) - 1u);
        value = forward_transfer(color.read(p).rgb, params.transfer);
    } else if (panel == 1u) {
        uint2 p = params.depthOrigin + min(uint2(uv * float2(params.depthExtent)), params.depthExtent - 1u);
        float d = sr_debug_depth(depth, p);
        value = float3(params.depthInverted ? 1.0f - d : d);
    } else {
        uint2 p = params.motionOrigin + min(uint2(uv * float2(params.motionExtent)), params.motionExtent - 1u);
        float2 m = motion.read(p).rg * params.motionScale;
        value = float3(0.5f + m / 64.0f, length(m) / 32.0f);
    }
    output.write(float4(saturate(value), 1.0f), params.outputOrigin + tid);
}

kernel void yaagl_sr_debug_view(
    texture2d<float, access::read> color [[texture(0)]],
    texture2d<float, access::read> depth [[texture(1)]],
    texture2d<float, access::read> motion [[texture(2)]],
    texture2d<float, access::write> output [[texture(3)]],
    constant YaaglSrDebugParams& params [[buffer(0)]], uint2 tid [[thread_position_in_grid]]) {
    sr_debug_view(color, depth, motion, output, params, tid);
}
kernel void yaagl_sr_debug_depth_view(
    texture2d<float, access::read> color [[texture(0)]],
    depth2d<float, access::read> depth [[texture(1)]],
    texture2d<float, access::read> motion [[texture(2)]],
    texture2d<float, access::write> output [[texture(3)]],
    constant YaaglSrDebugParams& params [[buffer(0)]], uint2 tid [[thread_position_in_grid]]) {
    sr_debug_view(color, depth, motion, output, params, tid);
}

// Frame interpolation performs transfer conversion in an RGBA16Float scene domain.
struct YaaglFgParams {
    int2 sourceOrigin;
    uint2 extent;
    uint2 sourceExtent;
    uint2 motionTargetExtent;
    uint2 outputExtent;
    float2 motionScale;
    float2 jitterCancellation;
    float minLuminance;
    float maxLuminance;
    uint transfer;
    uint debugFlags;
    uint reset;
    uint disableHint;
    uint2 uiOrigin, sceneOrigin, presentOrigin;
    uint uiMode;
    uint reconstructCameraMotion;
    float4x4 clipToPreviousClip;
    float motionVectorsInvalidValue;
    uint motionVectorsInvalidValuePresent;
    // Render-grid reconstruction: pick the nearest-camera depth of each 3x3
    // neighborhood (FSR3 ReconstructAndDilate) and read motion at that sample.
    uint2 depthSourceExtent;
    uint depthDilation;
    uint depthInverted;
};

static uint2 fg_source_position(uint2 tid, uint2 extent, uint2 sourceExtent) {
    return min(uint2((float2(tid) + 0.5f) * float2(sourceExtent) / float2(extent)), sourceExtent - 1u);
}

// The center sample wins ties; neighbors replace it only when strictly nearer.
static uint2 fg_nearest_depth(texture2d<float, access::read> depth, uint2 center,
                              uint2 extent, bool inverted, uint2 origin = uint2(0u)) {
    uint2 nearest = center;
    float nearestDepth = depth.read(origin + center).r;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 position = int2(center) + int2(x, y);
            if ((x == 0 && y == 0) || any(position < 0) || any(position >= int2(extent))) continue;
            float candidate = depth.read(origin + uint2(position)).r;
            if (inverted ? candidate > nearestDepth : candidate < nearestDepth) {
                nearest = uint2(position);
                nearestDepth = candidate;
            }
        }
    return nearest;
}

static float fg_store_half_range(float value) {
    // Round-to-nearest half store; finite overflow saturates, NaN/Inf are preserved.
    return isfinite(value) ? clamp(value, -65504.0f, 65504.0f) : value;
}

static float3 fg_decode(float3 value, constant YaaglFgParams& params) {
    if (params.transfer == 0u) return inverse_srgb(value);
    if (params.transfer == 1u)
        return inverse_pq(value) * (10000.0f / params.maxLuminance);
    float low = params.minLuminance / 80.0f;
    float range = (params.maxLuminance - params.minLuminance) / 80.0f;
    return (value - low) / range;
}

static float3 fg_encode(float3 value, constant YaaglFgParams& params) {
    if (params.transfer == 0u) return forward_srgb(value);
    if (params.transfer == 1u)
        return forward_pq(value * (params.maxLuminance / 10000.0f));
    float low = params.minLuminance / 80.0f;
    float range = (params.maxLuminance - params.minLuminance) / 80.0f;
    return value * range + low;
}

kernel void yaagl_fg_decode_crop(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    float4 color = source.read(uint2(params.sourceOrigin) + tid);
    color.rgb = fg_decode(color.rgb, params);
    destination.write(color, tid);
}

kernel void yaagl_fg_decode_distortion(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    // NGX PrecisionInfo: hi = lo * Scale + Bias, for all four field channels.
    destination.write(source.read(uint2(params.sourceOrigin) + tid) * params.motionScale.x + params.motionScale.y, tid);
}

static uint2 fg_depth_position(uint2 tid, texture2d<float, access::read> sourceDepth, constant YaaglFgParams& params) {
    uint2 position = fg_source_position(tid, params.extent, params.depthSourceExtent);
    if (params.depthDilation)
        position = fg_nearest_depth(sourceDepth, position, params.depthSourceExtent, params.depthInverted != 0u, params.sceneOrigin);
    return position;
}

static float2 fg_object_motion(texture2d<float, access::read> source, uint2 sourcePosition,
                               constant YaaglFgParams& params) {
    float2 value = source.read(uint2(params.sourceOrigin) + sourcePosition).xy;
    // Either invalid component invalidates the object vector; camera reprojection is still applied below.
    if (params.motionVectorsInvalidValuePresent &&
        (value.x == params.motionVectorsInvalidValue || value.y == params.motionVectorsInvalidValue))
        value = 0.0f;
    return (value * params.motionScale - params.jitterCancellation) *
           float2(params.outputExtent) / float2(params.motionTargetExtent);
}

// Object motion is shared by all pixels sampling the same depth winner.
// Camera reprojection uses each destination pixel's own position.
static half4 fg_normalized_motion(float2 pixels, uint2 tid, float clipDepth, constant YaaglFgParams& params) {
    if (params.reconstructCameraMotion) {
        float2 uv = (float2(tid) + 0.5f) / float2(params.extent);
        float4 clip = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), clipDepth, 1.0f);
        // NGX rows occupy Metal columns: this is the public row-vector transform.
        float4 previous = params.clipToPreviousClip * clip;
        if (all(isfinite(previous)) && abs(previous.w) > 1.0e-6f)
            pixels += (previous.xy / previous.w - clip.xy) * float2(0.5f, -0.5f) * float2(params.outputExtent);
    }
    return half4(half(fg_store_half_range(pixels.x)), half(fg_store_half_range(pixels.y)), 0.0h, 0.0h);
}

kernel void yaagl_fg_normalize_motion(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]],
    texture2d<float, access::read> depth [[texture(2)]],
    texture2d<float, access::read> sourceDepth [[texture(4)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 sourcePosition = params.depthDilation
        ? fg_source_position(fg_depth_position(tid, sourceDepth, params), params.depthSourceExtent, params.sourceExtent)
        : fg_source_position(tid, params.extent, params.sourceExtent);
    float clipDepth = params.reconstructCameraMotion ? depth.read(tid).r : 0.0f;
    destination.write(fg_normalized_motion(fg_object_motion(source, sourcePosition, params), tid, clipDepth, params), tid);
}

// yaagl_fg_resample_depth followed by yaagl_fg_normalize_motion on the same grid:
// one nearest-depth search serves both, and camera reconstruction uses the
// depth value this thread stores instead of reading it back.
kernel void yaagl_fg_normalize_depth_motion(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]],
    texture2d<float, access::write> depth [[texture(2)]],
    texture2d<float, access::read> sourceDepth [[texture(4)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 depthPosition = fg_depth_position(tid, sourceDepth, params);
    float4 depthValue = sourceDepth.read(params.sceneOrigin + depthPosition);
    depth.write(depthValue, tid);
    uint2 mv = params.depthDilation ? fg_source_position(depthPosition, params.depthSourceExtent, params.sourceExtent)
                                    : fg_source_position(tid, params.extent, params.sourceExtent);
    destination.write(fg_normalized_motion(fg_object_motion(source, mv, params), tid, depthValue.r, params), tid);
}

// Exact doubling uses a fixed four-texel loop.
kernel void yaagl_fg_normalize_depth_motion_2x(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]],
    texture2d<float, access::write> depth [[texture(2)]],
    texture2d<float, access::read> sourceDepth [[texture(4)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 center [[thread_position_in_grid]]) {
    uint2 winner = fg_nearest_depth(sourceDepth, center, params.depthSourceExtent,
                                    params.depthInverted != 0u, params.sceneOrigin);
    float4 depthValue = sourceDepth.read(params.sceneOrigin + winner);
    uint2 mv = fg_source_position(winner, params.depthSourceExtent, params.sourceExtent);
    float2 objectMotion = fg_object_motion(source, mv, params);
    for (uint y = 0; y < 2; ++y) for (uint x = 0; x < 2; ++x) {
        uint2 tid = center * 2u + uint2(x, y);
        depth.write(depthValue, tid);
        destination.write(fg_normalized_motion(objectMotion, tid, depthValue.r, params), tid);
    }
}

// First target texel whose sample center maps to this source coordinate.
// Correct ties using the scalar shader's float mapping. D3D12 2D texture
// extents (<= 16384) keep the integer product in range.
static uint2 fg_target_begin(uint2 s, uint2 target, uint2 source) {
    uint2 begin = (2u * s * target + source - 1u) / (2u * source);
    uint2 previous = max(begin, 1u) - 1u;
    begin -= uint2((begin > 0u) & (fg_source_position(previous, target, source) >= s));
    begin += uint2((begin < target) & (fg_source_position(begin, target, source) < s));
    return begin;
}

kernel void yaagl_fg_normalize_depth_motion_source(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]],
    texture2d<float, access::write> depth [[texture(2)]],
    texture2d<float, access::read> sourceDepth [[texture(4)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 center [[thread_position_in_grid]]) {
    uint2 winner = fg_nearest_depth(sourceDepth, center, params.depthSourceExtent,
                                    params.depthInverted != 0u, params.sceneOrigin);
    float4 depthValue = sourceDepth.read(params.sceneOrigin + winner);
    uint2 begin = fg_target_begin(center, params.extent, params.depthSourceExtent);
    uint2 end = fg_target_begin(center + 1u, params.extent, params.depthSourceExtent);
    uint2 mv = fg_source_position(winner, params.depthSourceExtent, params.sourceExtent);
    float2 objectMotion = fg_object_motion(source, mv, params);
    for (uint y = begin.y; y < end.y; ++y) for (uint x = begin.x; x < end.x; ++x) {
        uint2 tid = uint2(x, y);
        depth.write(depthValue, tid);
        destination.write(fg_normalized_motion(objectMotion, tid, depthValue.r, params), tid);
    }
}

kernel void yaagl_fg_encode_scatter(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    texture2d<float, access::read> depth [[texture(2)]],
    texture2d<float, access::read> motion [[texture(3)]],
    texture2d<float, access::read> ui [[texture(4)]],
    texture2d<float, access::read> scene [[texture(5)]],
    texture2d<float, access::read> present [[texture(6)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    float4 color = source.read(tid);
    color.rgb = fg_encode(color.rgb, params);
    if (params.uiMode) {
        float4 overlay = ui.read(params.uiOrigin + tid);
        float alpha = clamp(params.uiMode == 1u ? overlay.a : overlay.r, 0.0f, 1.0f);
        // NGX defines premultiplication in the supplied color domain. Recompose
        // after encoding, not through MetalFX's undocumented UI alpha convention.
        float3 uiColor = params.uiMode == 1u ? overlay.rgb :
            present.read(params.presentOrigin + tid).rgb -
            (1.0f - alpha) * scene.read(params.sceneOrigin + tid).rgb;
        color.rgb = uiColor + (1.0f - alpha) * color.rgb;
        color.a = alpha + (1.0f - alpha) * color.a;
    }
    // Provider diagnostics: generated color / normalized game motion / depth.
    // MetalFX does not expose its internal optical-flow or disocclusion buffers.
    if (params.debugFlags & 4u) {
        uint panel = min(tid.x * 3u / params.extent.x, 2u);
        uint2 position = uint2(min((tid.x * 3u) % params.extent.x, params.extent.x - 1u), tid.y);
        if (panel == 0u) {
            color = source.read(position);
            color.rgb = fg_encode(color.rgb, params);
        } else if (panel == 1u) {
            uint2 guidePosition = fg_source_position(position, params.extent, params.depthSourceExtent);
            float2 mv = motion.read(guidePosition).xy *
                        float2(params.extent) / float2(params.depthSourceExtent);
            color = float4(clamp(0.5f + mv * 0.1f, 0.0f, 1.0f), 0.5f, 1.0f);
        } else {
            uint2 guidePosition = fg_source_position(position, params.extent, params.depthSourceExtent);
            float value = clamp(depth.read(guidePosition).r, 0.0f, 1.0f);
            color = float4(value, value, value, 1.0f);
        }
    }
    if ((params.debugFlags & 1u) && (tid.x < 2u || tid.x + 2u >= params.extent.x))
        color = float4(0.0f, 1.0f, 0.0f, 1.0f);
    if ((params.debugFlags & 2u) && tid.x < 16u && tid.y < 16u)
        color = params.reset ? float4(1.0f, 0.0f, 0.0f, 1.0f) : float4(0.0f, 1.0f, 0.0f, 1.0f);
    destination.write(color, uint2(params.sourceOrigin) + tid);
}



kernel void yaagl_fg_resample_depth(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> destination [[texture(1)]],
    constant YaaglFgParams& params [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint2 sourcePosition = fg_source_position(tid, params.extent, params.sourceExtent);
    if (params.depthDilation)
        sourcePosition = fg_nearest_depth(source, sourcePosition, params.sourceExtent, params.depthInverted != 0u, uint2(params.sourceOrigin));
    destination.write(source.read(uint2(params.sourceOrigin) + sourcePosition), tid);
}

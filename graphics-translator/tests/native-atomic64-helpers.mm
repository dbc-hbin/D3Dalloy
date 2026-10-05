// CPU-ready: clang++ -std=c++20 -fobjc-arc -framework Foundation -framework Metal
// native-atomic64-helpers.mm -o native-atomic64-helpers. Run only under the GPU lease.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <algorithm>
#include <bit>

struct Record {
  uint32_t claimed, helpers, logical_helper, sample, wave_lanes, logical_active_wave, logical_helper_wave;
  float dx, dy;
  uint32_t quad_helpers, quad_xy[3], quad_values[3], quad_logical[3], quad_samples[3];
  uint64_t physical_before, physical_after, active_before, active_after, helper_before, helper_after;
};
struct Control { uint32_t width, height, samples, sparse, sample_base, discarded, physical_origin; };

int main(int argc, char **argv) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 2;
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Record {
  uint claimed, helpers, logical_helper, sample, wave_lanes, logical_active_wave, logical_helper_wave;
  float dx, dy;
  uint quad_helpers, quad_xy[3], quad_values[3], quad_logical[3], quad_samples[3];
  ulong physical_before, physical_after, active_before, active_after, helper_before, helper_after;
};
struct Control { uint width, height, samples, sparse, sample_base, discarded, physical_origin; };
vertex float4 full(uint vertex_id [[vertex_id]]) {
  float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  return float4(corner * 2 - 1, 0, 1);
}
vertex float4 unclipped(uint vertex_id [[vertex_id]]) {
  float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
  // Native pixel triangle (0,0),(32,0),(0,32): no clipping or internal fan.
  return float4(corner.x - 1, 1 - corner.y, 0, 1);
}
fragment void probe(float4 pos [[position]], uint native_sample [[sample_id]],
    device atomic_uint *claims [[buffer(0)]], device Record *records [[buffer(1)]],
    constant Control &control [[buffer(2)]], device const uint *srv [[buffer(3)]],
    device atomic_uint *user_writes [[buffer(4)]]) {
  float2 virtual_position = pos.xy - float(control.physical_origin);
  uint2 xy = uint2(max(virtual_position, float2(0)));
  bool native_helper = simd_is_helper_thread();
  uint helpers = simd_sum(uint(native_helper));
  uint wave_lanes = simd_sum(1u);
  bool physical = all(virtual_position >= float2(0)) && all(virtual_position < float2(8)) && !native_helper;
  bool logical = xy.x >= control.width || xy.y >= control.height ||
      (control.sparse && (xy.x != 6 || xy.y != 4)) || (control.discarded && xy.x == 6);
  ulong physical_before = ulong(simd_ballot(physical));
  ulong active_before = ulong(simd_ballot(physical && !logical));
  ulong helper_before = ulong(simd_ballot(physical && logical));
  uint quad_helpers = uint(native_helper);
  uint packed_xy = (xy.x & 65535u) | ((xy.y & 65535u) << 16);
  uint quad_xy[3], quad_samples[3];
  uint sample = native_sample + control.sample_base;
  for (ushort direction = 1; direction <= 3; ++direction) {
    quad_helpers |= quad_shuffle_xor(uint(native_helper), direction) << direction;
    quad_xy[direction - 1] = quad_shuffle_xor(packed_xy, direction);
    quad_samples[direction - 1] = quad_shuffle_xor(sample, direction);
  }
  // Exactly the production inTile && !nativeHelper branch, before protected
  // reads and the app. Extra whole-helper quads may still share a SIMD wave.
  if (!physical) { discard_fragment(); return; }
  uint record = (xy.y * 8 + xy.x) * control.samples + sample;
  uint expected = 0;
  bool claimed = false;
  for (uint attempt = 0; attempt < 64 && !claimed; ++attempt) {
    claimed = atomic_compare_exchange_weak_explicit(claims + record, &expected, 1,
        memory_order_relaxed, memory_order_relaxed);
    expected = 0;
  }
  ulong physical_after = ulong(simd_ballot(true));
  ulong active_after = ulong(simd_ballot(!logical));
  ulong helper_after = ulong(simd_ballot(logical));
  float read = float(srv[xy.y * 8 + xy.x]);
  float dx = dfdx(read), dy = dfdy(read);
  uint logical_active_wave = simd_sum(uint(!logical));
  uint logical_helper_wave = simd_sum(uint(logical));
  Record result;
  result.claimed = uint(claimed); result.helpers = helpers; result.logical_helper = uint(logical);
  result.sample = sample; result.wave_lanes = wave_lanes; result.logical_active_wave = logical_active_wave;
  result.logical_helper_wave = logical_helper_wave; result.dx = dx; result.dy = dy; result.quad_helpers = quad_helpers;
  result.physical_before = physical_before; result.physical_after = physical_after;
  result.active_before = active_before; result.active_after = active_after;
  result.helper_before = helper_before; result.helper_after = helper_after;
  for (ushort direction = 1; direction <= 3; ++direction) {
    result.quad_xy[direction - 1] = quad_xy[direction - 1];
    result.quad_samples[direction - 1] = quad_samples[direction - 1];
    result.quad_values[direction - 1] = quad_shuffle_xor(uint(read), direction);
    result.quad_logical[direction - 1] = quad_shuffle_xor(uint(logical), direction);
  }
  records[record] = result;
  // Ordinary user writes obey logical activity; protective metadata above does not.
  if (!logical) atomic_fetch_add_explicit(user_writes + record, 1, memory_order_relaxed);
}
)MSL"];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    if (argc == 2 && std::strcmp(argv[1], "--compile-only") == 0) {
      std::puts("NATIVE_ATOMIC_HELPERS_MSL_COMPILED no_command_queue_or_gpu_submission");
      return 0;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    uint32_t srv[64];
    for (unsigned i = 0; i < 64; ++i) srv[i] = i % 8 + 10 * (i / 8);
    id<MTLBuffer> input = [device newBufferWithBytes:srv length:sizeof(srv) options:MTLResourceStorageModeShared];
    unsigned failures = 0;
    for (unsigned samples : {1u, 4u, 8u}) for (unsigned virtual_samples : {0u, 1u})
      for (unsigned sparse : {0u, 1u}) for (unsigned discarded : {0u, 1u}) for (unsigned promoted : {1u, 0u}) {
      if (samples == 1 && virtual_samples) continue;
      // Focus new proof/failure localization without rerunning exercised native1 cases.
      if (argc == 2 && std::strcmp(argv[1], "--virtual-only") == 0 && (!virtual_samples || samples == 1)) continue;
      if (argc == 2 && std::strcmp(argv[1], "--native4-only") == 0 && (virtual_samples || samples != 4)) continue;
      if (argc == 2 && std::strcmp(argv[1], "--native8-only") == 0 && (virtual_samples || samples != 8)) continue;
      const unsigned native_samples = virtual_samples ? 1 : samples;
      if (![device supportsTextureSampleCount:native_samples]) {
        std::printf("NATIVE_ATOMIC_HELPERS_UNSUPPORTED samples=%u native_samples=%u\n", samples, native_samples);
        continue;
      }
      MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
      pipeline.vertexFunction = [library newFunctionWithName:promoted ? @"unclipped" : @"full"];
      pipeline.fragmentFunction = [library newFunctionWithName:@"probe"];
      pipeline.rasterSampleCount = native_samples;
      id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
      if (!pso) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
      id<MTLBuffer> claims = [device newBufferWithLength:64 * samples * sizeof(uint32_t) options:MTLResourceStorageModeShared];
      id<MTLBuffer> records = [device newBufferWithLength:64 * samples * sizeof(Record) options:MTLResourceStorageModeShared];
      id<MTLBuffer> user_writes = [device newBufferWithLength:64 * samples * sizeof(uint32_t) options:MTLResourceStorageModeShared];
      std::memset(claims.contents, 0, claims.length); std::memset(records.contents, 0, records.length);
      std::memset(user_writes.contents, 0, user_writes.length);
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.renderTargetWidth = promoted ? 32 : 7; pass.renderTargetHeight = promoted ? 32 : 5;
      pass.defaultRasterSampleCount = native_samples;
      id<MTLCommandBuffer> command = [queue commandBuffer];
      for (unsigned sample_base = 0; sample_base < samples; sample_base += native_samples) {
      id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
      [encoder setRenderPipelineState:pso];
      [encoder setViewport:MTLViewport{0, 0, double(pass.renderTargetWidth), double(pass.renderTargetHeight), 0, 1}];
      [encoder setScissorRect:sparse && !promoted ? MTLScissorRect{6, 4, 1, 1}
          : MTLScissorRect{0, 0, pass.renderTargetWidth, pass.renderTargetHeight}];
      [encoder setFragmentBuffer:claims offset:0 atIndex:0];
      [encoder setFragmentBuffer:records offset:0 atIndex:1];
      const Control control{7, 5, samples, sparse, sample_base, discarded, promoted ? 4u : 0u};
      [encoder setFragmentBytes:&control length:sizeof(control) atIndex:2];
      [encoder setFragmentBuffer:input offset:0 atIndex:3];
      [encoder setFragmentBuffer:user_writes offset:0 atIndex:4];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
      }
      [command commit]; [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 2;
      }
      const auto *data = static_cast<const Record *>(records.contents);
      const auto *metadata = static_cast<const uint32_t *>(claims.contents);
      const auto *writes = static_cast<const uint32_t *>(user_writes.contents);
      unsigned physical = 0, logical = 0, helper_observations = 0;
      unsigned mixed_quads = 0, ballot_faults = 0, quad_faults = 0, side_effect_faults = 0;
      unsigned wave_min = UINT32_MAX, wave_max = 0, active_min = UINT32_MAX, active_max = 0, helper_min = UINT32_MAX, helper_max = 0;
      for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) for (unsigned sample = 0; sample < samples; ++sample) {
        const auto &record = data[(y * 8 + x) * samples + sample];
        const bool expected = promoted || (x < 7 && y < 5 && (!sparse || (x == 6 && y == 4)));
        const bool expected_logical = x >= 7 || y >= 5 || (sparse && (x != 6 || y != 4)) || (discarded && x == 6);
        const unsigned index = (y * 8 + x) * samples + sample;
        if (record.claimed != expected) ++failures;
        if (metadata[index] != expected || writes[index] != (expected && !expected_logical)) { ++failures; ++side_effect_faults; }
        if (!record.claimed) continue;
        ++physical; logical += !record.logical_helper; helper_observations += record.helpers;
        wave_min = std::min(wave_min, record.wave_lanes); wave_max = std::max(wave_max, record.wave_lanes);
        active_min = std::min(active_min, record.logical_active_wave); active_max = std::max(active_max, record.logical_active_wave);
        helper_min = std::min(helper_min, record.logical_helper_wave); helper_max = std::max(helper_max, record.logical_helper_wave);
        if (record.sample != sample || record.logical_helper != expected_logical) ++failures;
        // Compare exact lane identity before/after filtering, not just a count.
        if (record.physical_before != record.physical_after || record.active_before != record.active_after ||
            record.helper_before != record.helper_after || (record.active_after & record.helper_after) ||
            (record.active_after | record.helper_after) != record.physical_after ||
            std::popcount(record.active_after) != record.logical_active_wave ||
            std::popcount(record.helper_after) != record.logical_helper_wave) {
          ++failures; ++ballot_faults;
          if (ballot_faults <= 16) std::printf(
              "NATIVE_ATOMIC_HELPERS_BALLOT_FAULT xy=%u,%u sample=%u physical=%016llx/%016llx active=%016llx/%016llx helper=%016llx/%016llx counts=%u,%u\n",
              x, y, sample, static_cast<unsigned long long>(record.physical_before), static_cast<unsigned long long>(record.physical_after),
              static_cast<unsigned long long>(record.active_before), static_cast<unsigned long long>(record.active_after),
              static_cast<unsigned long long>(record.helper_before), static_cast<unsigned long long>(record.helper_after),
              record.logical_active_wave, record.logical_helper_wave);
        }
        mixed_quads += record.quad_helpers != 0;
        if (promoted) {
          unsigned faults = (record.quad_helpers != 0) + (record.dx != 1 || record.dy != 10);
          for (unsigned direction = 1; direction <= 3; ++direction) {
            const unsigned peer_x = x ^ (direction & 1), peer_y = y ^ ((direction >> 1) & 1);
            const unsigned peer_logical = peer_x >= 7 || peer_y >= 5 ||
                (sparse && (peer_x != 6 || peer_y != 4)) || (discarded && peer_x == 6);
            faults += record.quad_xy[direction - 1] != (peer_x | (peer_y << 16));
            faults += record.quad_values[direction - 1] != peer_x + 10 * peer_y;
            faults += record.quad_logical[direction - 1] != peer_logical;
            faults += record.quad_samples[direction - 1] != sample;
          }
          if (faults) {
            ++failures; ++quad_faults;
            if (quad_faults <= 16) std::printf(
                "NATIVE_ATOMIC_HELPERS_QUAD_FAULT xy=%u,%u sample=%u helpers=0x%x peer_xy=%08x,%08x,%08x peer_values=%u,%u,%u peer_logical=%u,%u,%u peer_samples=%u,%u,%u derivative=%g,%g\n",
                x, y, sample, record.quad_helpers, record.quad_xy[0], record.quad_xy[1], record.quad_xy[2],
                record.quad_values[0], record.quad_values[1], record.quad_values[2], record.quad_logical[0], record.quad_logical[1],
                record.quad_logical[2], record.quad_samples[0], record.quad_samples[1], record.quad_samples[2], record.dx, record.dy);
          }
        }
        if (promoted && !record.logical_helper && (sparse || x == 6 || y == 4) && !record.logical_helper_wave) ++failures;
      }
      std::printf("NATIVE_ATOMIC_HELPERS samples=%u native_samples=%u sparse=%u discarded=%u promoted=%u physical=%u logical=%u helper_observations=%u\n",
          samples, native_samples, sparse, discarded, promoted, physical, logical, helper_observations);
      std::printf("NATIVE_ATOMIC_HELPERS_WAVES lanes=%u..%u logical_active=%u..%u logical_helpers=%u..%u\n",
          wave_min, wave_max, active_min, active_max, helper_min, helper_max);
      std::printf("NATIVE_ATOMIC_HELPERS_ORACLES mixed_quad_records=%u ballot_faults=%u quad_faults=%u side_effect_faults=%u\n",
          mixed_quads, ballot_faults, quad_faults, side_effect_faults);
      if (physical != (promoted ? 64 : sparse ? 1 : 35) * samples ||
          logical != (sparse ? !discarded : discarded ? 30 : 35) * samples) ++failures;
      if (!promoted && !helper_observations) ++failures;
      if (!promoted && !mixed_quads) ++failures;
      if (failures) {
        std::printf("NATIVE_ATOMIC_HELPERS_RESULT failures=%u classification=%s\n", failures,
            promoted ? "strict_promotion_failure" : "negative_baseline_diagnostic");
        return promoted ? 1 : 3; // No subsequent render submission after the first failed case.
      }
    }
    std::printf("NATIVE_ATOMIC_HELPERS_RESULT failures=%u\n", failures);
    return failures ? 1 : 0;
  }
}

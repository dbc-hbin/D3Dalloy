// SPDX-License-Identifier: LGPL-2.1-or-later
// Exact runtime helper kernels against independent, serial primitive-order oracles.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "owned_primitive_scan.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/sysctl.h>
#include <vector>

using namespace d3dalloy;
constexpr uint32_t kGuard = 0xcab005e5;
constexpr uint64_t kStatistics = uint64_t(1) << 60;

static void require(bool ok, const std::string &message) {
  if (!ok) {
    std::fprintf(stderr, "PRIMITIVE_SCAN_PROBE_FAIL %s\n", message.c_str());
    std::exit(1);
  }
}

struct Metal {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> strip_reduce, strip_emit, tess_reduce, tess_emit, scan;

  explicit Metal(const char *source_path) {
    device = MTLCreateSystemDefaultDevice();
    require(device != nil, "no Metal device");
    queue = [device newCommandQueue];
    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:@(source_path) encoding:NSUTF8StringEncoding error:&error];
    require(source != nil, error ? error.description.UTF8String : "cannot read helper source");
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, error ? error.description.UTF8String : "cannot compile helper source");
    auto pipeline = [&](NSString *name) {
      id<MTLFunction> function = [library newFunctionWithName:name];
      require(function != nil, std::string("missing function ") + name.UTF8String);
      id<MTLComputePipelineState> result = [device newComputePipelineStateWithFunction:function error:&error];
      require(result != nil, error ? error.description.UTF8String : "cannot create pipeline");
      require(result.maxTotalThreadsPerThreadgroup >= kPrimitiveScanWidth, "unsupported scan width");
      return result;
    };
    strip_reduce = pipeline(@"d3dalloy_mesh_strip_reduce");
    strip_emit = pipeline(@"d3dalloy_mesh_strip_cut");
    tess_reduce = pipeline(@"d3dalloy_tess_capture_reduce");
    tess_emit = pipeline(@"d3dalloy_tess_capture_compact");
    scan = pipeline(@"d3dalloy_primitive_scan");
    std::printf("PRIMITIVE_SCAN_DEVICE name=%s\n", device.name.UTF8String);
  }

  id<MTLBuffer> buffer(uint64_t bytes) {
    id<MTLBuffer> result = [device newBufferWithLength:std::max<uint64_t>(bytes, 16)
                                              options:MTLResourceStorageModeShared];
    require(result != nil, "buffer allocation failed");
    return result;
  }

  void dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, uint32_t count,
                uint32_t width = kPrimitiveScanWidth) {
    [encoder setComputePipelineState:pipeline];
    [encoder dispatchThreadgroups:MTLSizeMake(std::max(1u, PrimitiveScanThreadgroups(count, width)), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
  }

  void hierarchy(id<MTLComputeCommandEncoder> encoder, const PrimitiveScanPlan &plan, id<MTLBuffer> scratch) {
    for (uint32_t i = 0; i < plan.level_count; ++i) {
      [encoder setBytes:&plan.levels[i] length:sizeof(PrimitiveScanLevel) atIndex:0];
      [encoder setBuffer:scratch offset:0 atIndex:1];
      dispatch(encoder, scan, plan.levels[i].count);
    }
  }

  static void finish(id<MTLCommandBuffer> command, id<MTLComputeCommandEncoder> encoder) {
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted,
            command.error ? command.error.description.UTF8String : "command buffer failed");
  }
};

static void guard(id<MTLBuffer> buffer, uint64_t first_word, const std::string &name) {
  const auto *words = static_cast<const uint32_t *>(buffer.contents);
  for (uint64_t i = first_word; i < buffer.length / 4; ++i)
    if (words[i] != kGuard) require(false, name + " unwritten/guard word " + std::to_string(i));
}

static void fillGuard(id<MTLBuffer> buffer) {
  std::fill_n(static_cast<uint32_t *>(buffer.contents), buffer.length / 4, kGuard);
}

struct StripOracle {
  std::vector<uint32_t> list;
  uint64_t vertices = 0;
};

// Deliberately process entire restart-delimited segments, without block summaries or prefix scans.
static StripOracle assemble(const std::vector<uint32_t> &input, uint32_t topology, bool cut, uint32_t cut_value) {
  StripOracle out;
  const uint32_t vpp = topology == 3 ? 2 : topology == 5 || topology == 6 ? 3 : topology == 11 ? 4 : 6;
  uint32_t start = 0;
  for (uint32_t end = 0; end <= input.size(); ++end) {
    if (end < input.size() && (!cut || input[end] != cut_value)) continue;
    const uint32_t length = end - start;
    uint32_t n = 0;
    if (topology == 3 && length >= 2) n = length - 1;
    if ((topology == 5 || topology == 6) && length >= 3) n = length - 2;
    if (topology == 11 && length >= 4) n = length - 3;
    if (topology == 13 && length >= 6) n = (length - 4) / 2;
    if (n) out.vertices += topology == 13 ? 2 * n + 4 : n + vpp - 1;
    for (uint32_t i = 0; i < n; ++i) {
      std::array<uint32_t, 6> v{};
      if (topology == 3) v = {i, i + 1};
      else if (topology == 5) v = {i, i + 1 + (i & 1), i + 2 - (i & 1)};
      else if (topology == 6) v = {i + 1, i + 2, 0};
      else if (topology == 11) v = {i, i + 1, i + 2, i + 3};
      else if (n == 1) v = {0, 1, 2, 5, 4, 3};
      else if (i == 0) v = {0, 1, 2, 6, 4, 3};
      else if (i & 1) v = {2 * i, 2 * i + 3, 2 * i + 4, 2 * i + (i + 1 == n ? 5u : 6u), 2 * i + 2, 2 * i - 2};
      else v = {2 * i, 2 * i - 2, 2 * i + 2, 2 * i + (i + 1 == n ? 5u : 6u), 2 * i + 4, 2 * i + 3};
      for (uint32_t j = 0; j < vpp; ++j) out.list.push_back(input[start + v[j]]);
    }
    start = end + 1;
  }
  return out;
}

static uint32_t runStrip(Metal &metal, const std::string &name, const std::vector<uint32_t> &values,
                         MeshStripCutControl control, uint64_t predicate = 1) {
  control.count = uint32_t(values.size());
  const auto plan = MakePrimitiveScanPlan(control.count, control.topology);
  const uint32_t vpp = control.topology == 3 ? 2 : control.topology == 5 || control.topology == 6 ? 3 :
                       control.topology == 11 ? 4 : 6;
  const bool suppressed = control.predicate_enabled &&
      (control.predicate_not_equal ? predicate != 0 : predicate == 0);
  StripOracle expected = suppressed ? StripOracle{} : assemble(values, control.topology, control.cut_enabled,
                                                               control.cut_value);
  // A suppressed draw binds too little index storage for the advertised count: validation catches premature reads.
  id<MTLBuffer> indices = metal.buffer(suppressed ? 16 :
      control.index_skip + uint64_t(values.size()) * control.index_size + 64);
  fillGuard(indices);
  auto *bytes = static_cast<uint8_t *>(indices.contents) + control.index_skip;
  for (uint32_t i = 0; !suppressed && i < values.size(); ++i)
    for (uint32_t b = 0; b < control.index_size; ++b)
      bytes[uint64_t(i) * control.index_size + b] = values[i] >> (8 * b);
  id<MTLBuffer> list = metal.buffer((uint64_t(values.size()) * vpp + 16) * 4);
  id<MTLBuffer> arguments = metal.buffer(16 * 4), statistics = metal.buffer(8 * 4);
  id<MTLBuffer> scratch = metal.buffer(plan.scratch_bytes + 64);
  fillGuard(list); fillGuard(arguments); fillGuard(statistics); fillGuard(scratch);
  *static_cast<uint64_t *>(statistics.contents) = kStatistics;
  auto command = [metal.queue commandBuffer];
  auto encoder = [command computeCommandEncoder];
  auto bind = [&] {
    [encoder setBytes:&control length:sizeof(control) atIndex:0];
    [encoder setBuffer:indices offset:0 atIndex:1];
    [encoder setBuffer:list offset:0 atIndex:2];
    [encoder setBuffer:arguments offset:0 atIndex:3];
    [encoder setBuffer:statistics offset:0 atIndex:4];
    [encoder setBytes:&predicate length:sizeof(predicate) atIndex:5];
    [encoder setBuffer:scratch offset:0 atIndex:6];
  };
  if (plan.level_count) {
    bind();
    metal.dispatch(encoder, metal.strip_reduce, control.count);
    metal.hierarchy(encoder, plan, scratch);
  }
  bind();
  metal.dispatch(encoder, metal.strip_emit, control.count);
  Metal::finish(command, encoder);
  const auto *got = static_cast<const uint32_t *>(list.contents);
  for (size_t i = 0; i < expected.list.size(); ++i)
    if (got[i] != expected.list[i]) require(false, name + " list word " + std::to_string(i));
  guard(list, expected.list.size(), name + " list");
  std::array<uint32_t, 11> wanted{};
  if (!suppressed) {
    wanted[0] = uint32_t(expected.list.size()); wanted[1] = control.instances;
    wanted[3] = uint32_t(control.base_vertex); wanted[4] = control.start_instance;
    const uint64_t groups = wanted[0] ? (wanted[0] - 1) / control.per_group + 1 : 0;
    if (wanted[0] && control.instances && groups * control.instances <= control.max_threadgroups) {
      wanted[8] = uint32_t(groups); wanted[9] = control.instances; wanted[10] = 1;
    }
  }
  require(std::memcmp(arguments.contents, wanted.data(), sizeof(wanted)) == 0, name + " arguments");
  guard(arguments, 11, name + " arguments");
  const uint64_t correction = !suppressed && control.statistics_enabled ?
      (expected.list.size() - expected.vertices) * control.instances : 0;
  require(*static_cast<const uint64_t *>(statistics.contents) == kStatistics - correction, name + " statistics");
  guard(statistics, 2, name + " statistics");
  guard(scratch, plan.scratch_bytes / 4, name + " scratch");
  std::printf("PRIMITIVE_SCAN_CASE_PASS kind=strip name=%s count=%u levels=%u emitted=%zu\n",
              name.c_str(), control.count, plan.level_count, expected.list.size());
  return plan.level_count;
}

static uint32_t runTess(Metal &metal, const std::string &name, uint32_t slots, uint32_t maximum, bool empty) {
  const TessCompactControl control{slots, maximum, 10, 0};
  const auto plan = MakePrimitiveScanPlan(slots, 0);
  id<MTLBuffer> counts = metal.buffer(uint64_t(slots) * 4);
  auto *input = static_cast<uint32_t *>(counts.contents);
  std::vector<uint32_t> expected;
  for (uint32_t slot = 0; slot < slots; ++slot) {
    input[slot] = empty ? 0 : slots > 1000000 ? (slot % 4093 == 0 || slot == slots - 1) :
        slot % 11 == 0 ? UINT32_MAX : (slot * 17u + slot / 251) % (maximum + 3);
    for (uint32_t p = 0; p < std::min(input[slot], maximum); ++p) expected.push_back(slot * maximum + p);
  }
  id<MTLBuffer> list = metal.buffer((4 + uint64_t(slots) * maximum + 16) * 4);
  id<MTLBuffer> dispatch = metal.buffer(8 * 4), scratch = metal.buffer(plan.scratch_bytes + 64);
  fillGuard(list); fillGuard(dispatch); fillGuard(scratch);
  auto command = [metal.queue commandBuffer];
  auto encoder = [command computeCommandEncoder];
  auto bind = [&] {
    [encoder setBytes:&control length:sizeof(control) atIndex:0];
    [encoder setBuffer:counts offset:0 atIndex:1];
    [encoder setBuffer:list offset:0 atIndex:2];
    [encoder setBuffer:dispatch offset:0 atIndex:3];
    [encoder setBuffer:scratch offset:0 atIndex:4];
  };
  if (plan.level_count) {
    bind();
    metal.dispatch(encoder, metal.tess_reduce, slots);
    metal.hierarchy(encoder, plan, scratch);
  }
  bind();
  metal.dispatch(encoder, metal.tess_emit, slots, TessCompactThreadgroupWidth(slots));
  Metal::finish(command, encoder);
  const auto *got = static_cast<const uint32_t *>(list.contents);
  require(got[0] == expected.size() && got[1] == 0 && got[2] == 0 && got[3] == 0, name + " list header");
  for (size_t i = 0; i < expected.size(); ++i)
    if (got[4 + i] != expected[i]) require(false, name + " list word " + std::to_string(i));
  guard(list, 4 + expected.size(), name + " list");
  const auto *grid = static_cast<const uint32_t *>(dispatch.contents);
  const uint32_t groups = expected.empty() ? 0 : uint32_t((expected.size() - 1) / control.per_group + 1);
  require(grid[0] == groups && grid[1] == 1 && grid[2] == 1, name + " dispatch");
  guard(dispatch, 3, name + " dispatch");
  guard(scratch, plan.scratch_bytes / 4, name + " scratch");
  std::printf("PRIMITIVE_SCAN_CASE_PASS kind=tess name=%s count=%u levels=%u emitted=%zu\n",
              name.c_str(), slots, plan.level_count, expected.size());
  return plan.level_count;
}

int main(int argc, const char **argv) {
  @autoreleasepool {
    require(argc == 2 || (argc == 3 && std::strcmp(argv[2], "--skip-large") == 0),
            "usage: probe helpers.metal [--skip-large]");
    int translated = 0;
    size_t size = sizeof(translated);
    sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0);
#if defined(__aarch64__)
    constexpr const char *arch = "arm64";
#else
    constexpr const char *arch = "x86_64";
#endif
    std::printf("NATIVE_PROCESS native_arch=%s proc_translated=%d\n", arch, translated);
    require(translated == 0, "translated test process");
    require(PrimitiveScanThreadgroups(UINT32_MAX) == 16777216, "uint32 group ceiling");
    require(MakePrimitiveScanPlan(0, 0).level_count == 0 && MakePrimitiveScanPlan(256, 0).level_count == 0 &&
            MakePrimitiveScanPlan(1024, 0).level_count == 0 && MakePrimitiveScanPlan(1025, 0).level_count == 1,
            "single-block plan");
    const auto largest = MakePrimitiveScanPlan(UINT32_MAX, 0);
    require(largest.level_count == 3 && largest.levels[0].count == 16777216 &&
            largest.levels[1].count == 65536 && largest.levels[2].count == 256 &&
            largest.scratch_bytes == uint64_t(16777216 + 65536 + 256) * sizeof(PrimitiveScanSummary),
            "uint32 hierarchy bounds");
    Metal metal(argv[1]);
    uint32_t cases = 0, max_levels = 0;
    auto strip = [&](const std::string &name, const std::vector<uint32_t> &values, MeshStripCutControl c,
                     uint64_t predicate = 1) {
      max_levels = std::max(max_levels, runStrip(metal, name, values, c, predicate));
      ++cases;
    };
    MeshStripCutControl control{};
    control.max_threadgroups = UINT64_MAX; control.instances = 3; control.base_vertex = -7;
    control.start_instance = 11; control.statistics_enabled = 1; control.per_group = 30; control.cut_enabled = 1;
    for (uint32_t topology : {3u, 5u, 6u, 11u, 13u}) {
      control.topology = topology;
      for (uint32_t width : {2u, 4u}) {
        control.index_size = width; control.cut_value = width == 2 ? 0xffff : 0xffffffff;
        control.index_skip = (topology + width) % 4;
        std::vector<uint32_t> input(66055);
        for (uint32_t i = 0; i < input.size(); ++i) input[i] = (i * 13 + 19) % 60000;
        // Long spans cross both hierarchy boundaries; odd/even short runs exercise restart-local winding.
        for (uint32_t k : {0u, 1u, 7u, 15u, 24u, 254u, 255u, 256u, 262u, 269u, 277u, 286u,
                           511u, 513u, 65534u, 65535u, 65536u, 65540u}) input[k] = control.cut_value;
        strip("boundaries-t" + std::to_string(topology) + "-i" + std::to_string(width), input, control);
        for (uint32_t i = 0, state = 0x12345678; i < 1027; ++i) {
          state = state * 1664525u + 1013904223u;
          input[i] = state % 7 == 0 ? control.cut_value : state % 60000;
        }
        input.resize(1027);
        strip("random-t" + std::to_string(topology) + "-i" + std::to_string(width), input, control);
      }
      control.index_size = 0; control.index_skip = 0; control.cut_enabled = 0;
      std::vector<uint32_t> positions(257);
      for (uint32_t i = 0; i < positions.size(); ++i) positions[i] = i;
      strip("nonindexed-t" + std::to_string(topology), positions, control);
      control.cut_enabled = 1;
    }
    control.topology = 13; control.index_size = 2; control.cut_value = 0xffff;
    for (uint32_t length : {1u, 5u, 6u, 7u, 8u, 9u, 255u, 256u, 257u}) {
      std::vector<uint32_t> input(length);
      for (uint32_t i = 0; i < length; ++i) input[i] = i;
      control.index_skip = length % 4;
      strip("adj-length-" + std::to_string(length), input, control);
    }
    strip("all-cuts", std::vector<uint32_t>(513, control.cut_value), control);
    std::vector<uint32_t> regular(769);
    for (uint32_t i = 0; i < regular.size(); ++i) regular[i] = i;
    control.topology = 5;
    for (uint32_t operation : {0u, 1u}) {
      control.predicate_enabled = 1; control.predicate_not_equal = operation;
      for (uint64_t value : {0ull, 1ull})
        strip("predicate-" + std::to_string(operation) + "-" + std::to_string(value), regular, control, value);
    }
    strip("predicate-single-block", std::vector<uint32_t>(17, 1), control, 1);
    control.predicate_enabled = 0; control.cut_enabled = 0;
    strip("disabled-cut-sentinel", std::vector<uint32_t>(257, control.cut_value), control);
    control.predicate_enabled = 0; control.statistics_enabled = 0; control.max_threadgroups = 1;
    strip("grid-limit-no-statistics", regular, control);
    control.max_threadgroups = UINT64_MAX; control.instances = 0;
    strip("zero-instances", regular, control);
    for (uint32_t slots : {0u, 1u, 255u, 256u, 257u, 512u, 1024u, 1025u, 65536u, 65537u}) {
      max_levels = std::max(max_levels, runTess(metal, "mixed-" + std::to_string(slots), slots, 7, false)); ++cases;
    }
    max_levels = std::max(max_levels, runTess(metal, "empty", 1025, 17, true)); ++cases;
    if (argc == 2) {
      max_levels = std::max(max_levels, runTess(metal, "three-level-sparse", 16777217, 1, false)); ++cases;
    }
    std::printf("PRIMITIVE_SCAN_PROBE_PASS cases=%u max_levels=%u\n", cases, max_levels);
  }
}

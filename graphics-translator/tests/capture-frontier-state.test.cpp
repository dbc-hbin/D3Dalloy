// SPDX-License-Identifier: LGPL-2.1-or-later
// Included helpers are extracted from production MSL by run-capture-frontier-cpu.py.
// No application VS/GS/DS or SO/raster consumer is emulated by this scheduler probe.
#include "capture-frontier-extracted.hpp"
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace {
uint cases = 0;
constexpr uint kMax = std::numeric_limits<uint>::max();

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "CAPTURE_FRONTIER_CPU_FAIL case=%s\n", message);
    std::exit(1);
  }
}

template <typename T> ulong address(T *value) {
  return reinterpret_cast<uintptr_t>(value);
}

CaptureFrontierParams parameters(CaptureFrontierCompletion &completion) {
  CaptureFrontierParams p{};
  p.Completion = address(&completion);
  p.ArgumentStride = 5 * sizeof(uint);
  p.Kind = kCaptureFrontierGeometry;
  p.Topology = 1;
  p.VertexPerWarp = p.WarpIncrement = p.PrimitivesPerGroup = 32;
  p.GSInstances = 1;
  p.ControlPoints = 4;
  p.PatchesPerGroup = 4;
  p.TessWorkloads = 67;
  p.ParentSlotCapacity = 4 * 67;
  p.TessPrimitives = 64;
  return p;
}

void poison_bindings(CaptureFrontierParams &p) {
  p.Flags = kCaptureFrontierIndexed;
  p.StagedRoots = p.StagedVertices = p.StagedIndexViews = 1;
  p.ActiveRoots = p.ActiveVertices = p.FixedIndexAddress = 1;
  p.RootBytes = p.RootStride = 16;
  p.VertexTableBytes = p.VertexStride = 32;
  p.IndexViewStride = sizeof(CaptureFrontierIndexView);
  p.StatisticsClaim = p.StatisticsClaimEntries = 1;
  p.StatisticsClaimBytes = 128;
}

void inactive_poison() {
  for (uint kind = 0; kind < 4; ++kind) {
    CaptureFrontierCompletion completion{};
    auto p = parameters(completion);
    poison_bindings(p);
    p.Arguments = 1;
    p.MaxCommands = kMax;
    uint count = 0;
    ulong predicate = 0;
    if (kind == 0) p.Count = address(&count);
    if (kind == 1 || kind == 2) {
      p.Predicate = address(&predicate);
      p.PredicateNotEqual = kind == 2;
      predicate = kind == 2 ? 17 : 0;
    }
    if (kind == 3) p.MaxCommands = 0;
    CaptureFrontierState s{};
    capture_frontier_prepare(s, p);
    check(s.Status == kCaptureFrontierDone && s.EffectiveCount == 0 && s.BatchActive == 0 &&
          s.ProducerSteps == 0 && s.ConsumerSteps == 0 && s.CommandsCompleted == 0 &&
          completion.Status == kCaptureFrontierDone, "inactive-skips-poison-addresses");
    ++cases;
  }
  for (uint kind = 0; kind < 3; ++kind) {
    CaptureFrontierCompletion completion{};
    auto p = parameters(completion);
    poison_bindings(p);
    std::array<uint, 5> draw{kind == 0 ? 0u : kind == 1 ? kMax : 3u,
                              kind == 1 ? 0u : kMax, kMax, kMax, kMax};
    p.Arguments = address(draw.data());
    p.MaxCommands = 1;
    p.Kind = kind == 2 ? kCaptureFrontierTessellationGeometry : kCaptureFrontierGeometry;
    CaptureFrontierState s{};
    capture_frontier_prepare(s, p);
    check(s.Status == kCaptureFrontierDone && s.CommandsCompleted == 1 && s.ProducerSteps == 0 &&
          s.ConsumerSteps == 0 && s.BatchActive == 0, "empty-command-skips-binding-metadata");
    ++cases;
  }
}

void snapshot_and_quantum() {
  CaptureFrontierCompletion completion{};
  auto p = parameters(completion);
  std::array<std::array<uint, 5>, 65> draws{};
  for (auto &draw : draws) draw = {1, 1, 17, 23, 0};
  uint count = 65;
  p.Arguments = address(draws.data());
  p.MaxCommands = 65;
  p.Count = address(&count);
  CaptureFrontierState s{};
  for (uint slot = 0; slot < 65; ++slot) {
    capture_frontier_prepare(s, p);
    check(s.Status == kCaptureFrontierRunning && s.BatchActive == 1 &&
          s.Command == slot && s.EffectiveCount == 65 && s.Indirect.Producer[0] == 1,
          "65-commands-prepare");
    count = slot & 1 ? kMax : 0; // the effective count was snapshotted only at initialization
    uint consume = 0;
    capture_frontier_advance(s, p, consume);
    if (slot == 0 || slot == 64) {
      uint publish = 1;
      capture_frontier_advance(s, p, publish);
      check(completion.Quantum == (slot ? 2u : 1u) &&
            completion.Status == (slot ? kCaptureFrontierDone : kCaptureFrontierRunning),
            "first-one-then64-quantum");
    }
  }
  check(s.CommandsCompleted == 65 && s.ProducerSteps == 65 && s.ConsumerSteps == 65 &&
        s.Status == kCaptureFrontierDone && s.Command == 65, "quantum-is-not-a-draw-limit");
  ++cases;
}

void huge_tuples() {
  for (uint kind : {uint(kCaptureFrontierVertex), uint(kCaptureFrontierGeometry), uint(kCaptureFrontierDomain)}) {
    CaptureFrontierCompletion completion{};
    auto p = parameters(completion);
    p.Kind = kind;
    p.Arguments = 1; // command metadata is already resident; a resumed chunk must not reread it.
    p.MaxCommands = kMax;
    p.VertexPerWarp = p.WarpIncrement = 30;
    p.PrimitivesPerGroup = 10;
    p.GSInstances = 32;
    p.Topology = kind == kCaptureFrontierVertex ? 0 : 4;
    CaptureFrontierState s{};
    s.Initialized = s.CommandReady = 1;
    s.EffectiveCount = s.InstanceCount = s.VertexCount = kMax;
    s.Command = s.Instance = kMax - 1;
    s.StartLocation = kMax - 13;
    s.BaseVertex = uint(-37);
    s.StartInstance = kMax - 19;
    if (kind == kCaptureFrontierVertex) s.InputPosition = kMax - 15;
    if (kind == kCaptureFrontierGeometry) s.ObjectGroup = (kMax - 1) / 30;
    if (kind == kCaptureFrontierDomain) s.ObjectGroup = ((kMax / 4) - 1) / 4;
    capture_frontier_prepare(s, p);
    check(s.Status == kCaptureFrontierRunning && s.InstanceDone &&
          s.Shader.instance_base == kMax - 1 && s.SO.BaseVertex == uint(-37) &&
          s.SO.BaseInstance == kMax - 19 && s.SO.StartVertex == kMax - 13, "huge-tuple-original-bases");
    if (kind == kCaptureFrontierVertex)
      check(s.Shader.producer_count == 15 && s.NextInputPosition == kMax &&
            s.Shader.first_position == kMax - 15, "huge-vertex-tail-no-wrap");
    if (kind == kCaptureFrontierGeometry)
      check(s.Shader.producer_count == 15 && s.Shader.local_primitive_count == 5 &&
            s.Shader.local_key_count == 160 && s.NextObjectGroup == 143165577,
            "huge-GS-tail-no-flattened-instance-key");
    if (kind == kCaptureFrontierDomain)
      check(s.ParentSlots == 3 * 67 && s.Shader.first_patch == 1073741820 &&
            s.NextObjectGroup == 268435456, "huge-TS-tail-active-slots");
    uint consume = 0;
    capture_frontier_advance(s, p, consume);
    check(s.Status == kCaptureFrontierDone && s.Command == kMax && s.CommandsCompleted == 1 &&
          s.Instance == kMax && s.InputPosition == 0 && s.ObjectGroup == 0 &&
          s.ConsumerSteps == 1, "huge-tuple-completes-without-wrap");
    ++cases;
  }
}

void positive_count_clamp() {
  CaptureFrontierCompletion completion{};
  auto p = parameters(completion);
  std::array<std::array<uint, 5>, 17> draws{};
  for (auto &draw : draws) draw = {1, 1, 7, 11, 0};
  uint count = 17;
  p.Arguments = address(draws.data());
  p.MaxCommands = 65;
  p.Count = address(&count);
  CaptureFrontierState s{};
  for (uint command = 0; command < 17; ++command) {
    capture_frontier_prepare(s, p);
    check(s.Status == kCaptureFrontierRunning && s.Command == command && s.EffectiveCount == 17 &&
          s.Indirect.Producer[0] == 1, "positive-count-clamped-below-MaxCommandCount");
    uint consume = 0;
    capture_frontier_advance(s, p, consume);
  }
  // There is no 18th record in this allocation. Even a terminal prepare may not
  // inspect it; ASan also guards the edge independently of the state assertions.
  capture_frontier_prepare(s, p);
  check(s.Status == kCaptureFrontierDone && s.Command == 17 && s.CommandsCompleted == 17 &&
        s.ProducerSteps == 17 && s.ConsumerSteps == 17, "positive-count-never-reads-18th-record");
  ++cases;
}

void query_epoch_wrap() {
  CaptureFrontierCompletion completion{};
  auto p = parameters(completion);
  std::array<uint, 8> header{};
  std::array<uint, 34> claims;
  claims.fill(0xfeed1234);
  std::array<uint, 5> draw{1, 1, 0, 0, 0};
  p.Arguments = address(draw.data());
  p.MaxCommands = 1;
  p.StatisticsClaim = address(header.data());
  p.StatisticsClaimEntries = address(claims.data() + 1);
  p.StatisticsClaimBytes = 32 * sizeof(uint);
  CaptureFrontierState s{};
  s.QueryEpoch = kMax;
  capture_frontier_prepare(s, p);
  check(s.QueryEpoch == 1 && (s.BatchActive & 4) && header[3] == 1 && header[5] == 0 &&
        s.Indirect.Clear[0] == 1, "query-epoch-wrap-requests-clear");
  for (uint word = 0; word < 64; ++word) capture_frontier_clear(s, p, word);
  check(claims.front() == 0xfeed1234 && claims.back() == 0xfeed1234 &&
        std::all_of(claims.begin() + 1, claims.end() - 1, [](uint value) { return value == 0; }),
        "query-epoch-wrap-clears-only-claim-entries");
  s.QueryEpoch = 1;
  s.BatchActive = 1;
  claims.fill(0x1234feed);
  capture_frontier_query_epoch(s, p);
  for (uint word = 0; word < 64; ++word) capture_frontier_clear(s, p, word);
  check(s.QueryEpoch == 2 && !(s.BatchActive & 4) &&
        std::all_of(claims.begin(), claims.end(), [](uint value) { return value == 0x1234feed; }),
        "ordinary-query-epoch-does-not-clear-parent-or-claims");
  ++cases;
}

void parent_workload_lifetime() {
  CaptureFrontierCompletion completion{};
  auto p = parameters(completion);
  p.Kind = kCaptureFrontierTessellationGeometry;
  p.PrimitivesPerGroup = 3;
  p.GSInstances = 2;
  std::array<uint, 5> draw{8, 1, 19, 7, 0}; // two active patches; reflection permits four complete W67 patches
  p.Arguments = address(draw.data());
  p.MaxCommands = 1;
  std::vector<uint> counts(p.ParentSlotCapacity, 0xf00df00d);
  std::vector<uint> list(4 + p.ParentSlotCapacity * p.TessPrimitives, 0xcafef00d);
  p.TessScratch = address(counts.data());
  p.TessList = address(list.data());
  p.TessScratchBytes = counts.size() * sizeof(uint);
  p.TessListBytes = list.size() * sizeof(uint);
  p.ClearDwords = uint(list.size());
  CaptureFrontierState s{};
  capture_frontier_prepare(s, p);
  check(s.ParentSlots == 134 && p.ParentSlotCapacity == 268 && (s.BatchActive & 2) &&
        s.ParentSteps == 1 && s.ProducerSteps == 1, "W67-whole-parent-active-slots");
  for (uint word = 0; word < p.ClearDwords; ++word) capture_frontier_clear(s, p, word);
  check(std::all_of(counts.begin(), counts.end(), [](uint value) { return value == 0; }) &&
        std::all_of(list.begin(), list.end(), [](uint value) { return value == 0; }), "new-parent-clear");
  // These are synthetic producer counts, not an emulation of tessellation output.
  counts[0] = 5;
  counts[66] = 3;
  counts[67] = 7;
  counts[133] = 1;
  std::fill(counts.begin() + 134, counts.end(), kMax); // inactive parent slots must not be inspected
  const std::vector<uint> expected{0, 1, 2, 3, 4, 66 * 64, 66 * 64 + 1, 66 * 64 + 2,
                                 67 * 64, 67 * 64 + 1, 67 * 64 + 2, 67 * 64 + 3,
                                 67 * 64 + 4, 67 * 64 + 5, 67 * 64 + 6, 133 * 64};
  uint point = 0;
  capture_frontier_consume(s, p, nullptr, point);
  check(s.ParentPrimitives == 16 && s.Shader.child_primitive_first == 0 &&
        s.Shader.local_primitive_count == 3 && s.Shader.local_key_count == 6 &&
        std::equal(expected.begin(), expected.end(), list.begin() + 4), "TS-child-global-primitive-rank");
  const auto kept_counts = counts;
  const auto kept_list = list;
  uint consumed = 0;
  for (;;) {
    const uint current = s.Shader.local_primitive_count;
    check(s.Shader.child_primitive_first == consumed && current == std::min(3u, 16u - consumed) &&
          s.ParentSteps == 1, "TS-child-cursor-and-parent-once");
    capture_frontier_advance(s, p, point);
    consumed += current;
    if (consumed == 16) break;
    check(s.Status == kCaptureFrontierRunning && s.ObjectGroup == 0 && s.ParentPrimitives == 16 &&
          s.ParentSlots == 134, "TS-parent-retained-until-child-drained");
    capture_frontier_prepare(s, p);
    check(s.Phase == kCaptureFrontierChildren && !(s.BatchActive & 2) &&
          s.Indirect.Producer[0] == 0, "TS-child-does-not-relaunch-parent");
    for (uint word = 0; word < p.ClearDwords; ++word) capture_frontier_clear(s, p, word);
    check(counts == kept_counts && list == kept_list, "TS-child-clear-preserves-parent-storage");
    capture_frontier_consume(s, p, nullptr, point);
  }
  check(s.Status == kCaptureFrontierDone && s.ParentSteps == 1 && s.ChildSteps == 6 &&
        s.ProducerSteps == 7 && s.ConsumerSteps == 6 && s.CommandsCompleted == 1 &&
        s.ParentPrimitives == 0 && s.ParentSlots == 0, "TS-last-child-releases-parent-cursor");
  ++cases;
}

void guarded_index_semantics() {
  std::array<uchar, 8> bytes{0x34, 0x12, 0x78, 0x56, 0xef, 0xcd, 0xab, 0x90};
  CaptureFrontierIndexView view{1, 0, 57, 0, {0, 0, 0}};
  check(capture_frontier_index(view, 0) == 0, "short-view-never-loads-poison");
  view = {0, kMax, 0, kMax, {0, 0, 0}};
  check(capture_frontier_index(view, kMax) == 0, "NULL-view-returns-zero");
  view = {address(bytes.data()), 8, 57, kMax, {0, 0, 0}};
  check(capture_frontier_index(view, 1) == 0x1234, "index-position-wrap-is-32-bit");
  view.Format = 42;
  view.FirstIndex = 0x40000000;
  check(capture_frontier_index(view, 0) == 0x56781234, "index-byte-offset-wrap-is-32-bit");
  view.Size = 3;
  check(capture_frontier_index(view, 0) == 0, "partial-R32-view-is-zero");
  ++cases;
  view = {address(bytes.data()), 2, 57, 0, {0, 0, 0}};
  check(capture_frontier_index(view, 1) == 0, "R16-offset-beyond-view-is-zero");
  view.Format = 42;
  view.Size = 4;
  check(capture_frontier_index(view, 1) == 0, "R32-offset-beyond-view-is-zero");
  ++cases;
  view = {1, kMax, 0, 0, {0, 0, 0}};
  check(capture_frontier_index(view, 0) == 0, "invalid-format-never-loads-poison");
  ++cases;
}
} // namespace

int main() {
  inactive_poison();
  snapshot_and_quantum();
  huge_tuples();
  positive_count_clamp();
  query_epoch_wrap();
  parent_workload_lifetime();
  guarded_index_semantics();
  std::printf("CAPTURE_FRONTIER_CPU_PASS cases=%u extraction=production_msl gpu_execution=not_tested\n", cases);
  return 0;
}

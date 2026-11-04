// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <array>
#include <cstdint>

namespace d3dalloy {

// Shared by direct recording, per-execution indirect captures and the helper MSL kernels.
constexpr uint32_t kPrimitiveScanWidth = 256;
constexpr uint32_t kPrimitiveScanMaxLevels = 3; // ceil(UINT32_MAX / 256) -> 65536 -> 256

struct PrimitiveScanSummary {
  uint32_t head, tail, primitives, vertices, cut;
};
struct PrimitiveScanLevel {
  uint32_t count, offset, next_offset, topology; // offsets are summary entries; topology 0 is a scalar sum
};
struct PrimitiveScanPlan {
  std::array<PrimitiveScanLevel, kPrimitiveScanMaxLevels> levels{};
  uint32_t level_count = 0;
  uint64_t scratch_bytes = 0;
};

constexpr uint32_t PrimitiveScanThreadgroups(uint32_t count, uint32_t width = kPrimitiveScanWidth) {
  return count ? (count - 1) / width + 1 : 0;
}

constexpr uint32_t TessCompactThreadgroupWidth(uint32_t count) {
  return count > kPrimitiveScanWidth && count <= 1024 ? 1024 : kPrimitiveScanWidth;
}

constexpr PrimitiveScanPlan MakePrimitiveScanPlan(uint32_t count, uint32_t topology) {
  PrimitiveScanPlan plan;
  uint32_t groups = PrimitiveScanThreadgroups(count), offset = 0;
  // A single block computes and emits its local prefix directly, without a scratch pass.
  if (groups <= 1 || (!topology && count <= 1024))
    return plan;
  for (;;) {
    const uint32_t next = groups > kPrimitiveScanWidth ? offset + groups : 0;
    plan.levels[plan.level_count++] = {groups, offset, next, topology};
    offset += groups;
    if (!next)
      break;
    groups = PrimitiveScanThreadgroups(groups);
  }
  plan.scratch_bytes = uint64_t(offset) * sizeof(PrimitiveScanSummary);
  return plan;
}

struct MeshStripCutControl {
  uint64_t max_threadgroups;
  uint32_t topology, index_size, cut_value, count, instances;
  int32_t base_vertex;
  uint32_t start_instance, index_skip, predicate_enabled, predicate_not_equal, statistics_enabled, per_group;
  uint32_t cut_enabled;
};
struct TessCompactControl {
  uint32_t slot_count, max_primitives, per_group, reserved;
};

static_assert(sizeof(PrimitiveScanSummary) == 20 && sizeof(PrimitiveScanLevel) == 16);
static_assert(sizeof(MeshStripCutControl) == 64 && sizeof(TessCompactControl) == 16);

} // namespace d3dalloy

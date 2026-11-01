/*
 * Copyright 2026 Feifan He for CodeWeavers
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Shader table (SBT) contract of the owned ray tracing path.
 *
 * Applications own the shader tables. A record is
 * `D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES` identifier bytes returned by
 * ID3D12StateObjectProperties::GetShaderIdentifier followed by the local root
 * arguments of that export, exactly as D3D12 describes them. This backend does
 * not rewrite, mirror, size or read back those records, and it does not
 * pre-process them either: the compiled traversal resolves a record on the GPU,
 * straight out of the application's table (EmitShaderRecordLookup in
 * dxil_rt_callbacks.cpp). The 32 byte identifier names the export (records of
 * another pipeline name none), and the bytes after it are read in place as the
 * export's local root arguments, in the D3D12 local root layout the compiled local
 * root signature uses. DispatchRays only validates the declared ranges and
 * publishes the table bases, strides and counts; an indirect dispatch has a GPU
 * kernel publish the same fields from the GPU-written description.
 *
 * The definitions here use plain integer types on purpose: the same header is
 * consumed by the PE runtime and by the native raytracing compiler.
 */
#pragma once

#include <cstdint>
#include <limits>

namespace d3dalloy::raytracing {

// D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT / _RECORD_BYTE_ALIGNMENT.
inline constexpr uint64_t kShaderTableAlignment = 64;
inline constexpr uint64_t kShaderRecordAlignment = 32;
// D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES.
inline constexpr uint64_t kShaderIdentifierSize = 32;
// D3D12 caps a shader record at 4KB.
inline constexpr uint64_t kMaxShaderRecordSize = 4096;
// D3D12_RAYTRACING_MAX_RAY_GENERATION_SHADER_THREADS.
inline constexpr uint32_t kMaxDispatchDimension = 1073741824u;

enum class SbtStatus : uint8_t {
  Ok,
  // The table description violates a D3D12 shader table alignment or size rule.
  InvalidDescription,
};

/** Plain description of one table, mirroring the D3D12 ranges. */
struct ShaderTableDescription {
  uint64_t address = 0;
  uint64_t size = 0;
  uint64_t stride = 0;
};

struct DispatchDescription {
  ShaderTableDescription ray_generation;
  ShaderTableDescription miss;
  ShaderTableDescription hit_group;
  ShaderTableDescription callable;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
};

/**
 * Validated view of one D3D12 shader table. A table with a stride of 0 is one
 * record every index resolves to (record_count 1, broadcast).
 */
struct ShaderTable {
  uint64_t base = 0;
  uint64_t size = 0;
  uint64_t stride = 0;
  uint32_t record_count = 0;
  bool broadcast = false;

  bool present() const { return size != 0; }
};

struct PreparedDispatch {
  ShaderTable ray_generation;
  ShaderTable miss;
  ShaderTable hit_group;
  ShaderTable callable;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
};

// Ray generation has no stride in D3D12: the range names exactly one record.
inline SbtStatus ValidateRayGenerationTable(uint64_t address, uint64_t size, ShaderTable &table) {
  table = {};
  if (!size || (address % kShaderTableAlignment) || size < kShaderRecordAlignment || size > kMaxShaderRecordSize)
    return SbtStatus::InvalidDescription;
  table.base = address;
  table.size = size;
  table.record_count = 1;
  table.broadcast = true;
  return SbtStatus::Ok;
}

// Miss / hit group / callable tables.
inline SbtStatus ValidateShaderTable(uint64_t address, uint64_t size, uint64_t stride, ShaderTable &table) {
  table = {};
  if (!size) {
    // An unused table must be described entirely by zeroes, otherwise the
    // application is addressing a record it did not declare.
    if (address || stride)
      return SbtStatus::InvalidDescription;
    return SbtStatus::Ok;
  }
  if ((address % kShaderTableAlignment) || size < kShaderRecordAlignment)
    return SbtStatus::InvalidDescription;
  table.base = address;
  table.size = size;
  if (!stride) {
    // Every index resolves to the same record.
    table.record_count = 1;
    table.broadcast = true;
    return SbtStatus::Ok;
  }
  // Only complete records inside the declared range are addressable.
  if ((stride % kShaderRecordAlignment) || stride > kMaxShaderRecordSize || stride > size)
    return SbtStatus::InvalidDescription;
  const uint64_t records = size / stride;
  if (!records || records > std::numeric_limits<uint32_t>::max())
    return SbtStatus::InvalidDescription;
  table.stride = stride;
  table.record_count = static_cast<uint32_t>(records);
  return SbtStatus::Ok;
}

inline SbtStatus PrepareDispatch(const DispatchDescription &desc, PreparedDispatch &prepared) {
  if (!desc.width || !desc.height || !desc.depth)
    return SbtStatus::InvalidDescription;
  if (desc.width > kMaxDispatchDimension || desc.height > kMaxDispatchDimension ||
      desc.depth > kMaxDispatchDimension)
    return SbtStatus::InvalidDescription;
  prepared = {};
  SbtStatus status =
      ValidateRayGenerationTable(desc.ray_generation.address, desc.ray_generation.size, prepared.ray_generation);
  if (status != SbtStatus::Ok)
    return status;
  status = ValidateShaderTable(desc.miss.address, desc.miss.size, desc.miss.stride, prepared.miss);
  if (status != SbtStatus::Ok)
    return status;
  status = ValidateShaderTable(desc.hit_group.address, desc.hit_group.size, desc.hit_group.stride, prepared.hit_group);
  if (status != SbtStatus::Ok)
    return status;
  status = ValidateShaderTable(desc.callable.address, desc.callable.size, desc.callable.stride, prepared.callable);
  if (status != SbtStatus::Ok)
    return status;
  prepared.width = desc.width;
  prepared.height = desc.height;
  prepared.depth = desc.depth;
  return SbtStatus::Ok;
}

/* ------------------------------------------------------------------------- */
/* Indirect DispatchRays (ExecuteIndirect DISPATCH_RAYS)                     */
/* ------------------------------------------------------------------------- */

/**
 * Plain parameters of one indirect resolve; matches d3dalloy_rays_indirect_control in MSL.
 *
 * Every slot gets its own SM50_RT_DISPATCH_ARGUMENTS block `arguments_stride` bytes
 * apart, initialized by the CPU from the dispatch's constant fields (export count,
 * pipeline token, recursion depth, sizes), and its threadgroup grid (3 x uint, 16 bytes
 * apart). The kernel fills the table bases, strides, counts, dimensions and the block's
 * own address from the application's GPU-written D3D12_DISPATCH_RAYS_DESC.
 */
struct RaysIndirectControl {
  uint64_t arguments;       // D3D12 GPU VA of the first D3D12_DISPATCH_RAYS_DESC
  uint64_t count;           // count buffer address or 0
  uint64_t predicate;       // predication snapshot address or 0
  uint64_t dispatch_blocks; // GPU address of the first SM50_RT_DISPATCH_ARGUMENTS block
  uint64_t grids;           // GPU address of the first threadgroup grid
  uint32_t stride;          // command signature ByteStride
  uint32_t max_commands;
  uint32_t arguments_stride;
  uint32_t predicate_not_equal;
  uint32_t group_width;     // traversal threadgroup shape the written counts are for (depth 1)
  uint32_t group_height;
};
static_assert(sizeof(RaysIndirectControl) == 64);

/**
 * One thread per command slot. A slot past the count buffer, a slot culled by
 * predication, and a description that violates the D3D12 shader table rules (the
 * PrepareDispatch rules above) all dispatch 0 threadgroups; a valid slot dispatches
 * ceil(Width / group_width) x ceil(Height / group_height) x Depth threadgroups of the
 * traversal's group shape (threads past the grid return in the raygen prologue).
 * Field offsets are SM50_RT_DISPATCH_ARGUMENTS's (dxil_raytracing.h), passed in as
 * the D3DALLOY_* macros so the two cannot disagree.
 */
inline constexpr const char kRaysIndirectMSL[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct d3dalloy_rays_indirect_control {
  ulong arguments;
  ulong count;
  ulong predicate;
  ulong dispatch_blocks;
  ulong grids;
  uint stride;
  uint max_commands;
  uint arguments_stride;
  uint predicate_not_equal;
  uint group_width;
  uint group_height;
};

struct d3dalloy_range { ulong address; ulong size; };
struct d3dalloy_strided { ulong address; ulong size; ulong stride; };
struct d3dalloy_rays_desc {
  d3dalloy_range ray_generation;
  d3dalloy_strided miss;
  d3dalloy_strided hit_group;
  d3dalloy_strided callable;
  uint width, height, depth;
};

/* ValidateShaderTable above; returns the record count (0 for an absent table) and flags a violation. */
static uint table_records(d3dalloy_strided t, thread bool &bad) {
  if (!t.size) {
    bad |= t.address != 0 || t.stride != 0;
    return 0;
  }
  if ((t.address & 63) || t.size < 32) { bad = true; return 0; }
  if (!t.stride) return 1;
  if ((t.stride & 31) || t.stride > 4096 || t.stride > t.size) { bad = true; return 0; }
  ulong records = t.size / t.stride;
  if (records > 0xffffffffull) { bad = true; return 0; }
  return uint(records);
}

kernel void d3dalloy_rays_indirect(constant d3dalloy_rays_indirect_control &control [[buffer(0)]],
                               uint slot [[thread_position_in_grid]]) {
  if (slot >= control.max_commands)
    return;
  device uint *grid = (device uint *)(control.grids + ulong(slot) * 16);
  const ulong self = control.dispatch_blocks + ulong(slot) * control.arguments_stride;
  device uchar *block = (device uchar *)self;
  uint count = control.count ? *(device const uint *)control.count : control.max_commands;
  bool culled = false;
  if (control.predicate) {
    ulong value = *(device const ulong *)control.predicate;
    culled = control.predicate_not_equal ? value != 0 : value == 0;
  }
  d3dalloy_rays_desc d = *(device const d3dalloy_rays_desc *)(control.arguments + ulong(slot) * control.stride);
  bool bad = slot >= count || culled;
  bad |= !d.width || !d.height || !d.depth || d.width > 1073741824u || d.height > 1073741824u ||
         d.depth > 1073741824u;
  bad |= !d.ray_generation.size || (d.ray_generation.address & 63) || d.ray_generation.size < 32 ||
         d.ray_generation.size > 4096;
  uint miss = table_records(d.miss, bad);
  uint hit = table_records(d.hit_group, bad);
  uint callable = table_records(d.callable, bad);
  if (bad) {
    grid[0] = grid[1] = grid[2] = 0;
    return;
  }
  *(device ulong *)(block + D3DALLOY_RAYGEN_RECORD) = d.ray_generation.address;
  *(device ulong *)(block + D3DALLOY_MISS_BASE) = d.miss.address;
  *(device ulong *)(block + D3DALLOY_HIT_BASE) = d.hit_group.address;
  *(device ulong *)(block + D3DALLOY_CALLABLE_BASE) = d.callable.address;
  *(device uint *)(block + D3DALLOY_MISS_STRIDE) = uint(d.miss.stride);
  *(device uint *)(block + D3DALLOY_MISS_COUNT) = miss;
  *(device uint *)(block + D3DALLOY_HIT_STRIDE) = uint(d.hit_group.stride);
  *(device uint *)(block + D3DALLOY_HIT_COUNT) = hit;
  *(device uint *)(block + D3DALLOY_CALLABLE_STRIDE) = uint(d.callable.stride);
  *(device uint *)(block + D3DALLOY_CALLABLE_COUNT) = callable;
  *(device uint *)(block + D3DALLOY_WIDTH) = d.width;
  *(device uint *)(block + D3DALLOY_HEIGHT) = d.height;
  *(device uint *)(block + D3DALLOY_DEPTH) = d.depth;
  *(device ulong *)(block + D3DALLOY_SELF) = self;
  grid[0] = (d.width + control.group_width - 1) / control.group_width;
  grid[1] = (d.height + control.group_height - 1) / control.group_height;
  grid[2] = d.depth;
}
)metal";

/** Metal function name of the indirect resolve kernel above. */
inline constexpr const char kRaysIndirectFunction[] = "d3dalloy_rays_indirect";

} // namespace d3dalloy::raytracing

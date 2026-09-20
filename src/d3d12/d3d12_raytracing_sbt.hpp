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
 * not rewrite, mirror, size or read back those records: DispatchRays validates
 * the declared ranges, resolves each table to the owning Metal buffer and lets a
 * small preparation pass on the GPU turn the records into the two views the
 * compiled traversal consumes:
 *
 *   - a `uint32_t` export index per record, which is where D3D12's opaque 32 byte
 *     identifier is resolved back into a compile time function index;
 *   - one local root argument table entry per record, holding the same bytes the
 *     application wrote, translated into the qword root-argument layout the root
 *     signature ABI already uses for every other stage.
 *
 * The definitions here use plain integer types on purpose: the same header is
 * consumed by the PE runtime and by the native raytracing compiler.
 */
#pragma once

#include <cstdint>
#include <limits>

namespace dxmt::raytracing {

// D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT / _RECORD_BYTE_ALIGNMENT.
inline constexpr uint64_t kShaderTableAlignment = 64;
inline constexpr uint64_t kShaderRecordAlignment = 32;
// D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES.
inline constexpr uint64_t kShaderIdentifierSize = 32;
// D3D12 caps a shader record at 4KB.
inline constexpr uint64_t kMaxShaderRecordSize = 4096;
// D3D12_RAYTRACING_MAX_RAY_GENERATION_SHADER_THREADS.
inline constexpr uint32_t kMaxDispatchDimension = 1073741824u;
/**
 * Written into the record index array when a record does not carry an identifier
 * of the pipeline being dispatched. The compiled traversal treats it as "no such
 * shader", never as a substitute export.
 */
inline constexpr uint32_t kInvalidRecordIndex = 0xffffffffu;

enum class SbtStatus : uint8_t {
  Ok,
  // The table description violates a D3D12 shader table alignment or size rule.
  InvalidDescription,
  // A declared range is valid but does not name one whole GPU allocation.
  UnresolvedAddress,
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
 * Validated view of one D3D12 shader table.
 *
 * `span` bounds how much of a record may be read: for a strided table it is the
 * stride (only complete records inside the declared range are addressable), for a
 * table with a stride of 0 it is the whole declared range, because every index
 * resolves to that single record.
 */
struct ShaderTable {
  uint64_t base = 0;
  uint64_t size = 0;
  uint64_t stride = 0;
  uint64_t span = 0;
  uint32_t record_count = 0;
  bool broadcast = false;

  bool present() const { return size != 0; }
  /** Bytes of local root data the record can carry. */
  uint64_t local_root_capacity() const {
    const uint64_t record_bytes = broadcast ? span : stride;
    return record_bytes > kShaderIdentifierSize ? record_bytes - kShaderIdentifierSize : 0;
  }
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
  table.span = size;
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
    // Every index resolves to the same record; the declared range bounds how much
    // of that record may be read.
    table.span = size;
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
  table.span = stride;
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
/* Local root argument translation                                           */
/* ------------------------------------------------------------------------- */

/**
 * One contiguous byte range copied out of the application's local root data into
 * the materialized qword layout entry for a record.
 *
 * `source_offset` is relative to the record's local root data (the byte after the
 * 32 byte identifier) and follows the D3D12 local root argument layout: root
 * constants are 4 byte packed, root descriptors and descriptor tables are 8 byte
 * aligned. `destination_offset` is relative to the entry and follows the existing
 * root argument qword layout of the root signature (`SlotQwordOffsets`).
 */
struct LocalRootCopy {
  uint32_t source_offset;
  uint32_t size;
  uint32_t destination_offset;
  uint32_t reserved;
};

/** Per export header of the copy list used by the GPU preparation pass. */
struct LocalRootLayout {
  uint32_t copy_offset;
  uint32_t copy_count;
  /** Bytes of local root data the export's root signature consumes. */
  uint32_t source_size;
  uint32_t reserved;
};

/* ------------------------------------------------------------------------- */
/* GPU preparation pass                                                      */
/* ------------------------------------------------------------------------- */

/** Plain parameters of one preparation pass; matches SbtPrepareParameters in MSL. */
struct SbtPrepareParameters {
  uint32_t record_count;
  /** Byte distance between records; 0 replicates the first record. */
  uint32_t record_stride;
  /** Bytes of local root data readable in each record. */
  uint32_t record_local_root_capacity;
  /** Byte distance between materialized local root entries. */
  uint32_t entry_stride;
  /** Number of exports; identifiers outside this range are rejected. */
  uint32_t export_count;
  /** Token of the state object being dispatched; foreign identifiers are rejected. */
  uint32_t pipeline_token;
  uint32_t reserved[2];
};

/**
 * Copies each record's identifier payload into the export index array and its
 * local root data into the materialized entry using the per-export copy list.
 * A record whose identifier does not belong to the pipeline gets
 * kInvalidRecordIndex, and the traversal must then skip it rather than fall back
 * to another export.
 */
inline constexpr const char kShaderTablePrepareMSL[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct dxmt_sbt_prepare_parameters {
  uint record_count;
  uint record_stride;
  uint record_local_root_capacity;
  uint entry_stride;
  uint export_count;
  uint pipeline_token;
  uint reserved[2];
};

struct dxmt_sbt_local_root_copy {
  uint source_offset;
  uint size;
  uint destination_offset;
  uint reserved;
};

struct dxmt_sbt_local_root_layout {
  uint copy_offset;
  uint copy_count;
  uint source_size;
  uint reserved;
};

/* OwnedShaderIdentifier in d3d12_state_object.hpp. */
struct dxmt_sbt_identifier {
  uint magic;
  ushort version;
  ushort kind;
  uint pipeline_token;
  uint export_index;
  uint flags;
};

kernel void dxmt_prepare_shader_table(
    device const uchar *records [[buffer(0)]],
    device uint *record_indices [[buffer(1)]],
    device uchar *local_root_entries [[buffer(2)]],
    constant dxmt_sbt_local_root_layout *layouts [[buffer(3)]],
    constant dxmt_sbt_local_root_copy *copies [[buffer(4)]],
    constant dxmt_sbt_prepare_parameters &parameters [[buffer(5)]],
    uint gid [[thread_position_in_grid]]) {
  if (gid >= parameters.record_count)
    return;

  device const uchar *record = records + gid * parameters.record_stride;
  device uchar *entry = local_root_entries + gid * parameters.entry_stride;
  for (uint i = 0; i < parameters.entry_stride; ++i)
    entry[i] = 0;

  // Records are 32 byte aligned, so the identifier loads stay naturally aligned.
  device const dxmt_sbt_identifier *identifier = (device const dxmt_sbt_identifier *)(record);
  const uint expected_magic = 0x544D5844u;
  const uint expected_version = 1;
  const uint expected_kind = 1;
  if (identifier->magic != expected_magic || identifier->version != expected_version ||
      identifier->kind != expected_kind || identifier->pipeline_token != parameters.pipeline_token ||
      identifier->export_index >= parameters.export_count) {
    // The identifier was not handed out by the pipeline being dispatched. Publishing
    // the sentinel lets the traversal skip the record instead of running a shader the
    // application never asked for.
    record_indices[gid] = 0xffffffffu;
    return;
  }
  uint export_index = identifier->export_index;
  record_indices[gid] = export_index;

  dxmt_sbt_local_root_layout layout = layouts[export_index];
  for (uint c = 0; c < layout.copy_count; ++c) {
    dxmt_sbt_local_root_copy copy = copies[layout.copy_offset + c];
    if (copy.source_offset >= parameters.record_local_root_capacity)
      continue;
    uint available = parameters.record_local_root_capacity - copy.source_offset;
    uint size = min(copy.size, available);
    for (uint b = 0; b < size; ++b)
      entry[copy.destination_offset + b] =
          record[sizeof(uint) * 8 + copy.source_offset + b];
  }
}
)metal";

/** Metal function name of the preparation kernel above. */
inline constexpr const char kShaderTablePrepareFunction[] = "dxmt_prepare_shader_table";

} // namespace dxmt::raytracing

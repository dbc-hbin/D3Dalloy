/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "Metal.hpp"
#include "d3d12_device.hpp"
#include "d3d12_raytracing_sbt.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt {

/*
 * D3D12 ray tracing state objects.
 *
 * A state object owns the application visible shader exports, their immutable
 * 32-byte shader identifiers and - for D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE -
 * one compiled Metal pipeline in which every export is a linked function.
 * Identifiers are produced by this backend, not by any external shader ABI:
 * the bytes are stable for the state object's lifetime because they live in the
 * export table the identifier pointer refers to.
 */

/** DXIL entry point shader kind, as stored in the DXIL entry point tag. */
enum OwnedRTStage : uint32_t {
  kOwnedRTStageLibrary = 6,
  kOwnedRTStageRayGeneration = 7,
  kOwnedRTStageIntersection = 8,
  kOwnedRTStageAnyHit = 9,
  kOwnedRTStageClosestHit = 10,
  kOwnedRTStageMiss = 11,
  kOwnedRTStageCallable = 12,
  kOwnedRTStageInvalid = 0xffffffff,
};

/** Sentinel for "no such export" in the hit group sub-object indexes. */
inline constexpr uint32_t kOwnedRTNoExport = 0xffffffff;

enum OwnedShaderIdentifierKind : uint16_t {
  // A collection has no compiled pipeline, so ID3D12StateObjectProperties returns
  // null for its exports and this kind never reaches a shader table record.
  kOwnedShaderIdentifierCollection = 0,
  kOwnedShaderIdentifierPipeline = 1,
};

enum OwnedShaderIdentifierFlags : uint32_t {
  kOwnedShaderIdentifierHitGroup = 1u << 0,
  kOwnedShaderIdentifierProcedural = 1u << 1,
};

/**
 * Layout of the 32 bytes ID3D12StateObjectProperties::GetShaderIdentifier returns.
 * The compiled traversal decodes exactly this layout out of the application's
 * shader table record; keep the field offsets stable.
 */
struct OwnedShaderIdentifier {
  uint32_t magic;
  uint16_t version;
  uint16_t kind;
  /** Unique per state object instance; rejects identifiers of other pipelines. */
  uint32_t pipeline_token;
  /** Index of the export inside the state object's export table. */
  uint32_t export_index;
  uint32_t flags;
  uint64_t reserved;
};

static_assert(sizeof(OwnedShaderIdentifier) == raytracing::kShaderIdentifierSize);
static_assert(offsetof(OwnedShaderIdentifier, version) == 4);
static_assert(offsetof(OwnedShaderIdentifier, pipeline_token) == 8);
static_assert(offsetof(OwnedShaderIdentifier, export_index) == 12);
static_assert(offsetof(OwnedShaderIdentifier, flags) == 16);
static_assert(offsetof(OwnedShaderIdentifier, reserved) == 24);

inline constexpr uint32_t kOwnedShaderIdentifierMagic = 0x544D5844; // 'DXMT'
inline constexpr uint16_t kOwnedShaderIdentifierVersion = 1;

class MTLD3D12StateObject : public ID3D12StateObject {
public:
  bool uses_atomic64 = false;
  ShaderImmutableConstants immutable_constants;
  /** One application visible export, subobject or hit group. */
  struct Export {
    /** Public D3D12 name of this export, as the application refers to it. */
    std::wstring name;
    /**
     * The library's own name for the entry point this export compiles from
     * (`!dx.entryPoints` operand 1, DXC-mangled). Empty on a hit group until its
     * members are resolved.
     */
    std::wstring entry_point;
    uint32_t stage = kOwnedRTStageInvalid;
    size_t library_index = 0;
    /** Namespace and dependency permissions of the defining collection (zero for direct libraries). */
    uint32_t scope = 0, collection_flags = 0;

    bool hit_group = false;
    D3D12_HIT_GROUP_TYPE hit_group_type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    /** Export indexes of the hit group's shaders, kOwnedRTNoExport when absent. */
    uint32_t closest_hit = kOwnedRTNoExport;
    uint32_t any_hit = kOwnedRTNoExport;
    uint32_t intersection = kOwnedRTNoExport;
    std::wstring closest_hit_name;
    std::wstring any_hit_name;
    std::wstring intersection_name;

    /**
     * An export compiled into the pipeline that the application cannot name: a member of a
     * hit group imported from a collection whose export list did not list the member. It
     * has no shader identifier and is not found by FindExport(name).
     */
    bool hidden = false;
    /**
     * Imported from an existing collection: its associations are the collection's and are
     * final. `collection` identifies the source only while the importing description is being
     * resolved; `collection_name` is the export's name inside that collection.
     */
    bool from_collection = false;
    const void *collection = nullptr;
    std::wstring collection_name;

    /** Local root signature of the export, null when the export has none. */
    MTLD3D12RootSignature *local_root_signature = nullptr;
    /** Global root signature associated with the export, null when none is. */
    MTLD3D12RootSignature *global_root_signature = nullptr;

    bool has_shader_config = false;
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    /**
     * Raytracing pipeline config (CONFIG or CONFIG1, the former with no flags) associated with
     * the export. D3D12 requires it on every export of a pipeline, matching across all of them.
     */
    bool has_pipeline_config = false;
    D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config{};

    bool compiled = false;

    OwnedShaderIdentifier identifier{};
  };

  virtual const Export *FindExport(const WCHAR *name) const = 0;
  virtual const Export *FindExport(uint32_t index) const = 0;
  virtual const std::vector<Export> &GetExports() const = 0;

  virtual MTLD3D12RootSignature *GetGlobalRootSignature() const = 0;
  /**
   * The root signature a DispatchRays of this pipeline binds from: its root arguments
   * entry and its static sampler table are both encoded from it.
   *
   * This is the pipeline's global root signature itself, except when a local root
   * signature defines a static sampler the global one does not. D3D12 then requires one
   * static sampler table for the union of both, and that union can only come from the
   * pipeline: the application's compute root signature *is* the shaders' global root
   * signature, so it cannot carry a sampler only a local signature defines. The state
   * object builds the union into a signature of its own - the global root signature's
   * parameters verbatim and in order, so the root argument layout the application wrote
   * through SetComputeRoot* is unchanged - and reports it here. Never null for a
   * compiled pipeline.
   */
  virtual MTLD3D12RootSignature *GetDispatchRootSignature() const = 0;
  virtual const D3D12_RAYTRACING_PIPELINE_CONFIG &GetPipelineConfig() const = 0;
  virtual D3D12_RAYTRACING_PIPELINE_FLAGS GetPipelineFlags() const = 0;
  virtual uint32_t GetPipelineToken() const = 0;
  /** Largest payload / attribute size the exports of this state object declare. */
  virtual uint32_t GetMaxPayloadSize() const = 0;
  virtual uint32_t GetMaxAttributeSize() const = 0;

  virtual WMT::Reference<WMT::ComputePipelineState> GetPipelineState() const = 0;
  /** Power-of-two thread count of one ray generation threadgroup (SM50_RT_RAYGEN_THREADGROUP_SIZE bound). */
  virtual uint32_t GetRayGenerationThreadgroupThreads() const = 0;
  /**
   * Tables of the compiled intersection functions. Miss, closest hit, any hit and
   * callable exports are reached by direct calls from the lowered traversal, so
   * only intersection functions need an intersection function table entry.
   */
  virtual WMT::Reference<WMT::IntersectionFunctionTable> GetIntersectionFunctionTable() const = 0;
  /**
   * The intersection function table one dispatch needs.
   *
   * Its functions are this state object's shared triangle and bounding-box callbacks.
   * They resolve the hit group from GPU shader-table records, not hardware IFT slots.
   * What differs per dispatch is the dispatch-arguments block the callback reads, which is
   * bound in the table's own buffer argument table at SM50_RT_BINDING_INDEX_DISPATCH - an
   * intersection function cannot see the compute encoder's bindings, so it has to be bound here.
   *
   * The state object's own table (GetIntersectionFunctionTable) is never mutated: it keeps serving
   * the ray generation shader's encoder binding, and a table a dispatch already uses stays
   * immutable while it is in flight. Returns null when the pipeline has no callback.
   */
  virtual WMT::Reference<WMT::IntersectionFunctionTable> CreateDispatchIntersectionFunctionTable(
      WMT::Buffer arguments, uint64_t arguments_offset,
      WMT::Reference<WMT::VisibleFunctionTable> visible_function_table
  ) = 0;
  /**
   * Table of every export the compiled traversal reaches by export index.
   *
   * Miss, closest hit, any hit and callable exports lower to `[[visible]]` Metal
   * functions and are called through this table, never directly: slot 0 is always
   * the null entry and export index N lives in slot N + 1, which is the layout
   * SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE documents. Intersection functions
   * are `[[intersection]]` and are reached through the intersection function table
   * above, so they do not occupy a slot here.
   */
  virtual WMT::Reference<WMT::VisibleFunctionTable> GetVisibleFunctionTable() const = 0;
};

HRESULT
CreateStateObject(MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object);

/**
 * ID3D12Device7::AddToStateObject: a new raytracing pipeline holding every export of
 * `parent` with its identifier unchanged, plus the exports `addition` defines.
 */
HRESULT AddToStateObject(
    MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *parent, REFIID riid,
    void **state_object
);

} // namespace dxmt

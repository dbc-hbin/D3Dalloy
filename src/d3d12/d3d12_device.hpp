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
#include "d3d12.h"
#include "Metal4.hpp"
#include "Metal4Pipeline.hpp"
#include "d3d12_interfaces_ext.hpp"
#include "d3d12_reflex.hpp"
#include "d3d12_antilag_interfaces.hpp"
#include "d3d12_diagnostics.hpp"
#include "d3d12_acceleration_structure.hpp"
#include "d3d12_command_encoder.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_raytracing_sbt.hpp"
#include "d3d12_reserved_resource.hpp"
#include "d3d12_sampler_feedback.hpp"
#include "d3d12_standard_swizzle.hpp"
#include "d3d12_sharing.hpp"
#include "d3d12_shader_cache.hpp"
#include "dxgi1_2.h"
#include "dxgi_interfaces.h"
#include "airconv_public.h"
#include "d3dalloy_buffer.hpp"
#include "d3dalloy_command.hpp"
#include "d3dalloy_fence.hpp"
#include "d3dalloy_format.hpp"
#include "d3dalloy_presenter.hpp"
#include "d3dalloy_texture.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "owned_trace.hpp"
#include "owned_bundle_recording.hpp"
#include "owned_rtas_lifetime.hpp"
#include "owned_rtas_mirror.hpp"
#include "owned_async_pipeline.hpp"
#include "owned_native_states.hpp"
#include "owned_gpu_execution_gate.hpp"
#include "config/config.hpp"
#include "thread.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#define IMPLEMENT_ME                                                                                                   \
  do {                                                                                                                 \
    Logger::err(str::format(__FILE__, ":", __FUNCTION__, "(", __LINE__, ") is not implemented."));                     \
    abort();                                                                                                           \
    __builtin_unreachable();                                                                                           \
  } while (0);

namespace d3dalloy {

class MTLD3D12PageableResidency;
struct D3DalloyPresentComposition;

struct D3DalloyPresentLatency {
  ~D3DalloyPresentLatency();
  HRESULT initializeWakeHandle(HANDLE semaphore);
  bool reserve(bool do_not_wait, UINT replaceable_queued = 0, bool *borrowed = nullptr);
  void cancel();
  void complete(HANDLE semaphore);
  void discardQueuedForRestart(UINT canceled, bool borrowed, HANDLE semaphore);
  void remove();
  HRESULT setMaximum(UINT maximum, HANDLE semaphore);
  UINT maximum() const;

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  uint64_t next_ticket_ = 0, serving_ticket_ = 0;
  UINT maximum_ = 1, outstanding_ = 0, credit_debt_ = 0;
  bool removed_ = false;
  HANDLE wake_semaphore_ = nullptr;
};

// A pending enhanced split barrier: BEGIN and END must carry identical layouts and accesses.
struct BarrierLayoutSplit {
  D3D12_BARRIER_LAYOUT layout_before, layout_after;
  D3D12_BARRIER_ACCESS access_before, access_after;
  bool operator==(const BarrierLayoutSplit &) const = default;
};

// A subresource holds either a legacy state (`current`) or an enhanced-barrier texture layout
// (`layout`), never both. D3D12_BARRIER_LAYOUT_COMMON is stored as the legacy COMMON state: the
// two are the same layout and are the only hand-off point between the barrier models.
struct BarrierSubresourceState {
  std::optional<D3D12_RESOURCE_STATES> current;
  std::optional<std::pair<D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>> split;
  std::optional<D3D12_BARRIER_LAYOUT> layout;
  std::optional<BarrierLayoutSplit> layout_split;
  bool implicitly_promoted = false;
  // `current` is still the legacy creation state, whose layout is defined by the enhanced
  // barrier translation table (only transitions made by ResourceBarrier must reach COMMON).
  bool creation_state = false;
  // API capture only: `current` was promoted by a barrier-free use of a subresource whose state
  // before this submission is unknown (its layout may be anything another queue left behind).
  bool promoted_from_unknown = false;
};

struct BarrierResourceState {
  BarrierSubresourceState all;
  std::unordered_map<UINT, BarrierSubresourceState> subresources;
};

// Kept alive by the resource and any command recording that mentions it, not
// by a device-wide registry. The device mutex serializes queue-worker updates.
struct BarrierTracker {
  const uint64_t resource_id;
  const uint64_t subresource_count;
  const bool broad_promotion;
  const std::optional<D3D12_RESOURCE_STATES> fixed_state;
  BarrierResourceState state;

  BarrierTracker(uint64_t id, uint64_t count, bool broad, D3D12_RESOURCE_STATES initial, D3D12_HEAP_TYPE heap_type,
                 std::optional<D3D12_BARRIER_LAYOUT> initial_layout = std::nullopt)
      : resource_id(id), subresource_count(count), broad_promotion(broad),
        fixed_state(heap_type == D3D12_HEAP_TYPE_UPLOAD ? std::optional{D3D12_RESOURCE_STATE_GENERIC_READ} :
                    heap_type == D3D12_HEAP_TYPE_READBACK ? std::optional{D3D12_RESOURCE_STATE_COPY_DEST} :
                    std::nullopt) {
    // Buffers and simultaneous-access textures have no layout; they start in COMMON.
    if (initial_layout && *initial_layout != D3D12_BARRIER_LAYOUT_COMMON && !broad) {
      state.all.layout = initial_layout;
    } else {
      state.all.current = initial_layout ? D3D12_RESOURCE_STATE_COMMON : initial;
      state.all.creation_state = !initial_layout;
    }
  }
};

// One advertised ceiling shared by creation/probe and feature queries. FL 12_0: every 11_1 requirement
// (OutputMergerLogicOp, UAV-only ForcedSampleCount 16 via two programmed 8x halves, ForcedSampleCount 1 on
// multisampled RTVs) plus 12_0's Tiled Tier 2, Resource Binding Tier 2+ and the typed UAV load format set are
// GPU-verified by the feature-level, tiled and bindless suites. FL 12_1 additionally has ROVs and
// ConservativeRasterizationTier 1, verified across all shader families by the conservative-raster suite.
inline constexpr D3D_FEATURE_LEVEL kOwnedMaxFeatureLevel = D3D_FEATURE_LEVEL_12_2;

// FL 12_2 requires Tiled Tier 3, depth bounds and unaligned BC textures in addition
// to the owned DXR 1.1+, VRS 2, mesh 1, feedback 0.9+, binding 3 and conservative 3 paths.
// Use the same adapter gate for creation (including null-output probes) and queries.
inline D3D_FEATURE_LEVEL OwnedFeatureLevel(WMT::Device metal) {
  if (!metal.supportsPlacementSparse())
    return D3D_FEATURE_LEVEL_11_1;
  return metal.supportsFamily(WMTGPUFamilyApple7) && metal.supportsBCTextureCompression()
      ? kOwnedMaxFeatureLevel : D3D_FEATURE_LEVEL_12_1;
}
/* Highest DXIL shader model the owned compiler accepts (SM50_MAX_DXIL_SHADER_MODEL). */
inline constexpr D3D_SHADER_MODEL kOwnedMaxShaderModel = static_cast<D3D_SHADER_MODEL>(SM50_MAX_DXIL_SHADER_MODEL);
/* SM 6.6 includes stage-qualified DXR payload transfer and selectable waves 4..32.
 * Restricted wider compute lowering does not expand the public wave-size range. */
inline constexpr D3D_SHADER_MODEL kOwnedReportedShaderModel = D3D_SHADER_MODEL_6_6;

class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandListPreview {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  uint64_t owned_recording = 0;
  ID3D12CommandAllocator *owned_allocator = nullptr;
  std::shared_ptr<std::atomic<uint64_t>> owned_allocator_generation;
  uint64_t owned_closed_generation = 0;
  EncoderData *entry;
  size_t encoder_count;
  bool owned_closed_valid = true;

  std::vector<owned_graphics::BundleReference> owned_bundle_references;
};

class MTLD3D12CommandAllocator : public ID3D12CommandAllocator {
public:
  virtual HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
      void **ppCommandList
  ) = 0;
};

class MTLD3D12CommandQueue : public ID3D12CommandQueue1 {
public:
  virtual HRESULT Present(Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable,
                          double after, const D3DalloyPresentGeometry &geometry,
                          WMT::Reference<WMT::PresentationObserver> observer, uint64_t present_id,
                          std::shared_ptr<std::atomic<bool>> observer_fault,
                          D3DalloyPresentComposition composition, bool do_not_wait,
                          std::shared_ptr<D3DalloyPresentLatency> latency, bool restart, bool borrowed,
                          bool closes_frame, std::shared_ptr<owned_graphics::LowLatencyScheduler::Frame> *latency_frame) = 0;
};

class MTLD3D12Resource : public ID3D12Resource2 {
public:
  // GetDesc() is the logical, normalized description, never the Metal backing description.
  D3D12_MIP_REGION sampler_feedback_mip_region = {};

  HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(REFIID, void **session) final {
    if (!session)
      return E_POINTER;
    *session = nullptr;
    return E_NOINTERFACE;
  }

  D3D12_RESOURCE_DESC1 *STDMETHODCALLTYPE GetDesc1(D3D12_RESOURCE_DESC1 *result) final {
    D3D12_RESOURCE_DESC desc;
    GetDesc(&desc);
    *result = {desc.Dimension, desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize,
               desc.MipLevels, desc.Format, desc.SampleDesc, desc.Layout, desc.Flags, sampler_feedback_mip_region};
    return result;
  }

  const uint64_t owned_id = owned_graphics::NewObjectId();
  D3D12_RESOURCE_STATES owned_initial_state = D3D12_RESOURCE_STATE_COMMON;
  bool owned_is_reserved = false;
  // Set by the ID3D12Device10 creation methods: the resource was created with an initial layout.
  std::optional<D3D12_BARRIER_LAYOUT> owned_initial_layout;
  // Formats a view may reinterpret this texture as (ID3D12Device10 castable format list).
  std::vector<DXGI_FORMAT> owned_castable_formats;
  HRESULT PublishOwnedFormatCasting();
  HRESULT ImportOwnedFormatCasting(MTLD3D12Device *device,const D3D12_RESOURCE_DESC &desc);
  std::shared_ptr<BarrierTracker> owned_barrier_state;
  // Set with release once owned_barrier_state is created (under owned_barrier_state_mutex); that
  // shared_ptr is never reassigned, so an acquire load seeing it set may copy it without the mutex.
  std::atomic<bool> owned_barrier_state_ready{false};
  Rc<Texture> texture;
  Rc<Buffer> buffer;
  std::unique_ptr<standard_swizzle::Layout> swizzle_layout;
  bool standard_swizzle_may_alias = false;
  // Committed shared resources: the process-wide share (section, token, rendezvous).
  std::shared_ptr<sharing::SharedObject> share;
  // Reserved (tiled) resources only: tiling and queue-ordered tile mapping state.
  std::unique_ptr<ReservedResource> reserved;
  // Sampler feedback maps only (d3d12_sampler_feedback.hpp): `buffer` holds the u32 cells.
  std::optional<sampler_feedback::Layout> feedback;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual bool TryAddRefPrivate() = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) = 0;

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) = 0;
};

class MTLD3D12Heap : public ID3D12Heap1 {
public:
  HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(REFIID, void **session) final {
    if (!session)
      return E_POINTER;
    *session = nullptr;
    return E_NOINTERFACE;
  }

  WMT::Reference<WMT::Heap> heap;
  /* Shared heaps: placed resources alias `share->payload()` through no-copy buffers. */
  std::shared_ptr<sharing::SharedObject> share;
  /* Shared heaps: a no-copy buffer over the whole payload, the heap's residency allocation. */
  WMT::Reference<WMT::Buffer> shared_buffer;
  /*
   * Non-null for heaps without an MTLHeap: section payload (shared heaps) or application host
   * memory (OpenExistingHeapFrom*). Placed buffers and ROW_MAJOR textures alias it by offset.
   */
  uint8_t *external_memory = nullptr;
#ifdef __i386__
  /*
   * CPU-visible heap in a 32-bit process: the MTLHeap's CPU mapping is not addressable here, so
   * placed buffers are created over this process-owned storage (offset = placement offset), the
   * same storage CpuPlaced committed buffers use. Placed resources retain the heap, so the storage
   * outlives them. Null for GPU-only heaps.
   */
  uint8_t *host_memory = nullptr;
#endif
};

class MTLD3D12Fence : public ID3D12Fence1 {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  Rc<Fence> fence;
  std::shared_ptr<sharing::SharedObject> share;
};

class MTLD3D12RootSignature : public ID3D12RootSignature {
public:
  struct TableRange {
    uint64_t offset;
    uint32_t count;
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    uint32_t register_space;
    uint32_t register_lower_bound;
    D3D12_SHADER_VISIBILITY visibility;
    D3D12_DESCRIPTOR_RANGE_FLAGS descriptor_flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
  };

  virtual UINT GetBlob(const void **ppBlob) = 0;
  virtual std::span<const TableRange> GetTableRanges(UINT root_parameter) const = 0;
  virtual const TableRange *GetRootDescriptorRange(UINT root_parameter) const = 0;

  // Enumerate only shader-used register/space intervals, never unrelated root
  // ranges. The caller clips the intervals to the actual descriptor heap.
  template <typename Emit>
  static void ForEachUsedDescriptorRange(std::span<const TableRange> ranges,
      std::span<const MTL_SM50_SHADER_ARGUMENT> bindings, Emit &&emit) {
    for (const auto &range : ranges) {
      for (const auto &binding : bindings) {
        const bool kind_matches =
            (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && binding.Type == SM50BindingType::ConstantBuffer) ||
            (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && binding.Type == SM50BindingType::SRV) ||
            (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && binding.Type == SM50BindingType::UAV);
        if (!kind_matches || binding.RegisterSpace != range.register_space || !binding.RegisterCount ||
            !(binding.Flags & (MTL_SM50_SHADER_ARGUMENT_READ_ACCESS | MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS)) ||
            (range.visibility != D3D12_SHADER_VISIBILITY_ALL &&
             !(binding.ShaderVisibilityMask & (1u << range.visibility)))) continue;
        const uint64_t first = std::max<uint64_t>(range.register_lower_bound, binding.RegisterLowerBound);
        const uint64_t end = std::min(
            range.count == UINT32_MAX ? uint64_t(1) << 32 : uint64_t(range.register_lower_bound) + range.count,
            binding.RegisterCount == UINT32_MAX ? uint64_t(1) << 32 : uint64_t(binding.RegisterLowerBound) + binding.RegisterCount);
        if (first < end) emit(range.offset + first - range.register_lower_bound, end - first, range.type);
      }
    }
  }

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

  uint32_t UploadQwords = 0;
  /* Qwords of the parameters alone; the heap, root-view status and view-id qwords follow them. */
  uint32_t ParameterQwords = 0;
  uint32_t ParameterSlots = 0;
  uint32_t const *SlotQwordOffsets = nullptr;
  D3D12_ROOT_PARAMETER_TYPE const *ParameterTypes = nullptr;
  UINT const *ParameterConstantCounts = nullptr;
  bool IsLocal = false;
  bool HasTextureDescriptorTables = false;
  /* D3D12_ROOT_SIGNATURE_FLAGS of the deserialized signature. */
  uint32_t Flags = 0;
  /**
   * First of the four root qwords a HEAP_DIRECTLY_INDEXED signature appends after its
   * parameters: {resource heap GPU base, resource heap NumDescriptors, sampler heap GPU
   * base, sampler heap NumDescriptors}, filled from SetDescriptorHeaps state whenever the
   * root arguments are uploaded. `~0u` for a signature without either flag. Included in
   * UploadQwords, so it can reach kMaxRootArgumentQwords.
   */
  uint32_t HeapQwordOffset = ~0u;
  static constexpr uint32_t kHeapQwords = 4;
  /**
   * Per parameter, the root qword holding the residency status base of a root SRV/UAV (TILED:
   * BufferAllocation::residencyStatusBase of a reserved buffer, 0 otherwise), or `~0u`. These qwords
   * follow the parameters and the heap qwords, in parameter order, and are included in UploadQwords.
   * Local root signatures have none (shader records carry the application's layout).
   */
  uint32_t const *StatusQwordOffsets = nullptr;
  /* At most 32 root descriptors fit in the 64 DWORD root signature budget. */
  static constexpr uint32_t kMaxRootViewStatusQwords = 32;
  /**
   * The root qword holding the current view instance (SV_ViewID, view instancing) of a non-local signature,
   * after the parameters, the heap qwords and the root-view status qwords; `~0u` for a local signature.
   * Included in UploadQwords. Low 32 bits are the logical view ID. ICB shader-owned view locations additionally
   * use bits 32..47 for the effective viewport count and bits 48..63 for the render-target array length.
   * Direct shaders truncate this qword to 32 bits; StageViewId clears the bounds outside an indirect replay.
   */
  uint32_t ViewIdQwordOffset = ~0u;
  static constexpr uint32_t kViewIdQwords = 1;
  static constexpr uint32_t kMaxRootArgumentQwords = 64 + kHeapQwords + kMaxRootViewStatusQwords + kViewIdQwords;

  size_t NumStaticSamplers;
  uint64_t const *EncodedStaticSamplers;
};

HRESULT AppendRootStaticSamplers(MTLD3D12RootSignature *root, std::vector<D3D12_STATIC_SAMPLER_DESC> &table, UINT *appended);

class MTLD3D12CommandSignature : public ID3D12CommandSignature {
public:
  D3D12_INDIRECT_ARGUMENT_TYPE CommandType;
  UINT ByteStride;
  UINT DrawArgumentOffset = ~0u;
  UINT IndexBufferArgumentOffset = ~0u;
  std::vector<Atomic64IndirectRootField> atomic64_root_fields;
  struct RootConstantRange { uint32_t parameter, first, count; };
  std::vector<RootConstantRange> root_constant_ranges;
  uint64_t RootParameterMask;
  uint32_t VertexBufferMask;
  UINT UpdateRootArguments : 1;
  UINT UpdateVertexBuffers : 1;
  UINT UpdateIndexBuffer   : 1;

  WMT::Reference<WMT::RenderPipelineState> render_resolver;
  WMT::Reference<WMT::ComputePipelineState> graphics_argument_resolver;
  WMT::Reference<WMT::ComputePipelineState> compute_resolver;
  /* DISPATCH_MESH / DISPATCH_RAYS with root argument updates: per-slot root argument writer (resolve_root_arguments). */
  WMT::Reference<WMT::ComputePipelineState> root_argument_resolver;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  D3D12_QUERY_HEAP_DESC desc{};
  WMT::Reference<WMT::MTL4CounterHeap> timestamp_samples;

  WMT::Reference<WMT::Buffer> timestamp_dummy;
  WMT::Reference<WMT::Buffer> visibility_results;
  WMT::Reference<WMT::ComputePipelineState> binary_normalize_pipeline;
  WMT::Reference<WMT::ComputePipelineState> visibility_accumulate_pipeline;
  /*
   * D3D12_QUERY_HEAP_TYPE_SO_STATISTICS storage. The first Count * 16 bytes are the
   * D3D12_QUERY_DATA_SO_STATISTICS results ResolveQueryData copies out; the next
   * Count * 16 bytes hold the stream counter pair snapshot taken at BeginQuery. EndQuery
   * writes result = counter - begin with `so_statistics_end_pipeline`.
   */
  WMT::Reference<WMT::Buffer> so_statistics;
  WMT::Reference<WMT::ComputePipelineState> so_statistics_end_pipeline;
  /* PIPELINE_STATISTICS(1): results followed by BeginQuery snapshots, each Count * `pipeline_statistics_record`
   * bytes (sizeof D3D12_QUERY_DATA_PIPELINE_STATISTICS or ..._STATISTICS1). */
  WMT::Reference<WMT::Buffer> pipeline_statistics;
  WMT::Reference<WMT::ComputePipelineState> pipeline_statistics_end_pipeline;
  uint32_t pipeline_statistics_record = 0;
};

class MTLD3D12Device;

struct ShaderImmutableConstants {
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t offset = 0;
  uint64_t gpu_address = 0;
  // Only Atomic64 pools retain a relocation recipe. The published PSO pool is
  // immutable; each execution patches a private copy to its own phase header.
  uint64_t size = 0, alignment = 0;
  const uint8_t *mapped = nullptr;
  std::vector<SM50_CONSTANT_RELOCATION> atomic64_relocations;

  /** `bitcode` owns `compiled`'s parts; null only when they are directly addressable (a loaded cache blob).
   * Relocations naming SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE resolve to `device`'s lock table. */
  HRESULT Initialize(MTLD3D12Device *device, const SM50_COMPILED_BITCODE &compiled, sm50_bitcode_t bitcode);
  HRESULT CopyForAtomic64(uint64_t context, uint64_t address, void *destination) const;
};

class DeviceShaderArtifactCache;
class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  UINT IsComputePipelineState;
  bool uses_atomic64 = false;
  std::vector<MTL_SM50_SHADER_ARGUMENT> resource_bindings;
  // Private C++ interface: named libraries retain this immutable snapshot rather
  // than round-tripping through the application's writable GetCachedBlob copy.
  virtual HRESULT GetPrivateCachedBlob(ID3DBlob **blob) { return GetCachedBlob(blob); }

  static HRESULT
  InitializeShader(
      DeviceShaderArtifactCache &artifacts, const char *Stage, D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader,
      struct MTL_SHADER_REFLECTION *pRefl, std::vector<MTL_SM50_SHADER_ARGUMENT> *resource_bindings = nullptr
  );
};

/*
 * Validated, owned copy of a graphics PSO's D3D12_STREAM_OUTPUT_DESC. `active` is false when the PSO
 * declares no stream output. Without a GS only stream 0 exists; after a GS every declared stream is
 * captured on its own.
 *
 * Target layout: each slot belongs to one stream and has the application stride and the bytes one
 * vertex writes (`bytes_per_vertex`, gaps included); every non-gap element records its byte offset
 * inside that slot's vertex. Capture layout: the last pre-rasterization stage writes each non-gap
 * element of stream s contiguously, in declaration order, into a private record of
 * `streams[s].record_dwords` dwords starting at `dword_offset` (SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE,
 * SM50_SHADER_D3D12_GS_STREAMS).
 */
struct StreamOutputLayout {
  struct Slot {
    uint32_t stride = 0;
    uint32_t bytes_per_vertex = 0;
  };
  struct Element {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t start_component = 0;
    uint32_t component_count = 0;
    uint32_t slot = 0;
    uint32_t byte_offset = 0;
    uint32_t dword_offset = 0;
    uint32_t stream = 0;
  };
  struct Stream {
    /* Bit i: the declaration writes stream s to slot i. A referenced slot with no buffer bound is full. */
    uint32_t referenced_slot_mask = 0;
    uint32_t record_dwords = 0;
  };
  bool active = false;
  /* Bit s: the declaration has entries for stream s. */
  uint32_t stream_mask = 0;
  /* 0..3 or D3D12_SO_NO_RASTERIZED_STREAM. */
  uint32_t rasterized_stream = 0;
  std::array<Stream, D3D12_SO_STREAM_COUNT> streams{};
  std::array<Slot, D3D12_SO_BUFFER_SLOT_COUNT> slots{};
  /* In declaration order; each element's `stream` selects its capture record. */
  std::vector<Element> elements;
};

struct PipelineStatisticsCaptureLayout {
  uint32_t record_dwords = 0;
  uint32_t capture_offset = 0;
  uint32_t clip_mask = 0;
  uint32_t cull_mask = 0;
  bool rasterization_enabled = false;
};

struct MSAAPipelineVariants {
  WMT::Reference<WMT::ComputePipelineState> capture;
  WMT::Reference<WMT::RenderPipelineState> coverage;
  WMT::Reference<WMT::RenderPipelineState> coverage_with_depth;
  WMT::Reference<WMT::RenderPipelineState> clear_coverage;
  WMT::Reference<WMT::RenderPipelineState> gather_depth;
  WMT::Reference<WMT::ComputePipelineState> depth_bounds_probe;
  WMT::Reference<WMT::RenderPipelineState> early_depth;
  WMT::Reference<WMT::RenderPipelineState> late_depth;
  WMT::Reference<WMT::RenderPipelineState> sample_pixel;
  WMT::Reference<WMT::DepthStencilState> capture_depth_state;
  WMT::Reference<WMT::RenderPipelineState> pixel;
  WMT::Reference<WMT::RenderPipelineState> replay;
  WMT::Reference<WMT::RenderPipelineState> replay_layer;
  WMT::Reference<WMT::RenderPipelineState> query_coverage;
  ShaderImmutableConstants capture_constants;
  ShaderImmutableConstants replay_vertex_constants;
  ShaderImmutableConstants coverage_constants;
  ShaderImmutableConstants pixel_constants;
  ShaderImmutableConstants replay_fragment_constants;
  ShaderImmutableConstants sample_fragment_constants;
  SM50_MSAA_PIXEL_LAYOUT layout{};
  SM50_MSAA_VERTEX_LAYOUT vertex_layout{};
  bool sample_frequency = false;
  bool gpu_tile_tasks = false;
};

/* A native SO frontier executes the application VS once in compute, then
 * replays its exact native return with the original raster/fragment state. */
struct CaptureFrontierPipelines {
  WMT::Reference<WMT::ComputePipelineState> capture;
  WMT::Reference<WMT::RenderPipelineState> replay;
  ShaderImmutableConstants capture_constants;
  ShaderImmutableConstants replay_vertex_constants;
  ShaderImmutableConstants replay_fragment_constants;
  SM50_MSAA_VERTEX_LAYOUT vertex_layout{};
};

/*
 * GS and HS/DS pipelines execute as Metal object+mesh pipelines (the DXBC multi-stage
 * converters): VS (+HS) in the object stage, GS or DS in the mesh stage. One variant is
 * built per draw-time input shape, keyed by MeshVariantKey.
 */
enum class MeshPipelineKind : uint8_t {
  None,
  Geometry,
  Tessellation,
  /* VS+HS+DS+GS in two passes: VS+HS object / DS capture mesh (rasterization off), a compaction kernel, then a
   * capture-reading object / GS mesh pass (keys with kMeshTessellationGeometryPass). */
  TessellationGeometry,
  /* D3D12 amplification (object) + mesh shaders, drawn with DispatchMesh. */
  MeshShader,
};

/* MeshVariantKey bit selecting the second (GS) pass of a TessellationGeometry pipeline. */
constexpr uint32_t kMeshTessellationGeometryPass = 16;
constexpr uint32_t kMeshMSAACapture = 64;
constexpr uint32_t kMeshMSAAReplay = 128;
constexpr uint32_t kMeshObjectCapture = 256;
constexpr uint32_t kMeshObjectReplay = 512;
constexpr uint32_t kMeshCaptureFrontier = 1024;
constexpr uint32_t kMeshMSAAGPUTileTasks = 2048;

struct MeshPipelineVariant {
  WMT::Reference<WMT::RenderPipelineState> pso;
  ShaderImmutableConstants object_constants;
  ShaderImmutableConstants mesh_constants;
  WMT::Reference<WMT4::Function> replay_function;
  SM50_MSAA_VERTEX_LAYOUT vertex_layout{};
  SM50_MSAA_MESH_LAYOUT capture_layout{};
  SM50_MSAA_OBJECT_LAYOUT object_capture_layout{};
};

/* index_format: SM50_INDEX_BUFFER_FORMAT; strip: GS input is a strip topology. */
constexpr uint32_t MeshVariantKey(uint32_t index_format, bool strip, bool statistics) {
  return index_format | (strip ? 4u : 0u) | (statistics ? 8u : 0u);
}

/* A VIEW_INSTANCING subobject with ViewInstanceCount >= 1 (validated by the stream parser). */
struct ViewInstancingState {
  UINT count = 0;
  bool masking = false;
  std::array<D3D12_VIEW_INSTANCE_LOCATION, D3D12_MAX_VIEW_INSTANCE_COUNT> locations{};
};

class MTLD3D12GraphicsPipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::RenderPipelineState> pso;
  /* d3d12.asyncPipelineCompile: deferred creation of `pso`. Every reader of `pso` calls
   * WaitNativePipeline() first; a failed job is a recording error, not a fallback. */
  std::shared_ptr<owned_graphics::AsyncCompileJob> async_job;
  /* Shaders expose no UAV and no side-effect path, so a pending draw may be omitted. */
  bool async_omittable = false;
  bool NativePipelinePending() const {
    return async_job && async_job->status() == owned_graphics::AsyncCompileJob::Pending;
  }
  bool WaitNativePipeline() { return !async_job || async_job->Wait(); }
  // Ready direct native MSAA slot; captured variants and ordinary pso remain independent.
  WMT::Reference<WMT::RenderPipelineState> native_msaa_pso;
  MeshPipelineKind mesh_kind = MeshPipelineKind::None;
  /* Geometry: GS input primitive (D3D10_SB_PRIMITIVE). */
  uint32_t mesh_input_primitive = 0;
  /* SO scratch records/key, output topology (D3D10_SB_PRIMITIVE_TOPOLOGY) and GS instances.
   * GS uses dcl_maxout; DS-only SO includes its workload vertex/index staging and has one instance per key. */
  uint32_t gs_max_output_vertices = 0;
  uint32_t gs_output_topology = 0;
  uint32_t gs_instance_count = 1;
  /* Tessellation: object threads per patch (HS reflection ThreadsPerPatch). */
  uint32_t tess_threads_per_patch = 0;
  /* DS capture layout (W, V, P, R, K), fixed at PSO creation; DS-only SO uses V for its staged SO records. */
  SM50_TESS_CAPTURE_LAYOUT tess_capture_layout{};
  /* MeshShader: [numthreads] of the AS (all 0 without one) and the MS. */
  std::array<uint32_t, 3> as_threads{};
  std::array<uint32_t, 3> ms_threads{};
  /* Null on failure; the caller records E_FAIL. */
  virtual const MeshPipelineVariant *GetMeshPipeline(uint32_t key) = 0;
  StreamOutputLayout stream_output;
  PipelineStatisticsCaptureLayout pipeline_statistics_capture;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE ib_strip_cut_value = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  ShaderImmutableConstants vertex_constants;
  ShaderImmutableConstants index_pull_vertex_constants;
  ShaderImmutableConstants fragment_constants;
  ShaderImmutableConstants pipeline_statistics_vertex_constants;
  ShaderImmutableConstants pipeline_statistics_fragment_constants;
  // ICB view-location epilogues have their own immutable-constant layout, separate from direct variants.
  ShaderImmutableConstants indirect_view_vertex_constants;
  ShaderImmutableConstants indirect_view_index_pull_vertex_constants;
  ShaderImmutableConstants indirect_view_pipeline_statistics_vertex_constants;
  uint32_t slot_mask = 0;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  enum WMTWinding winding;
  float depth_bias;
  float scole_scale;
  float depth_bias_clamp;
  uint32_t forced_sample_count;
  std::array<DXGI_FORMAT, 8> owned_rtv_formats{};
  UINT owned_rt_count = 0;
  DXGI_FORMAT owned_dsv_format = DXGI_FORMAT_UNKNOWN;
  DXGI_SAMPLE_DESC owned_sample_desc{};
  uint32_t owned_sample_mask = 0xffffffffu;
  SM50_MSAA_LINE_MODE owned_line_mode = SM50_MSAA_LINE_NONE;
  bool owned_has_pixel_shader = false;
  bool owned_shading_rate_pixel_shader = false;
  bool owned_sample_frequency_pixel_shader = false;
  /* The PS declares ForceEarlyDepthStencil (reflection PSForceEarlyDepthStencil). */
  bool owned_force_early_pixel_shader = false;
  /* GS/HS/DS or MS PSO whose natively multisampled statistics draws count PSInvocations once per (pixel, primitive)
   * by the claim buffer (SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM): the PS forces early depth/stencil with a depth or
   * stencil test, which runs an arbitrary subset of a pixel's covered samples (probe:
   * build/local/p3/p2left/probe/result4.txt), so the lowest-covered-sample rule cannot be used. */
  bool mesh_statistics_claim = false;
  bool owned_distance_interpolation = false;
  /* Rare MS signatures beyond Metal's 124 native scalars, or SV_CullDistance: capture the exact
   * output lanes and replay them instead of rejecting a valid D3D 32-row signature. */
  bool owned_mesh_output_replay = false;
  /* D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON: every draw runs the exact captured-pixel path with conservative
   * replay geometry and per-pixel coverage (SM50_SHADER_D3D12_CONSERVATIVE_RASTER). */
  bool owned_conservative = false;
  bool owned_depth_bounds = false;
  bool emulated_depth_bounds = false;
  WMT::Reference<WMT::DepthStencilState> depth_bounds_reject_state;
  bool needs_rov_buffer_marker = false;
  // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS / _DYNAMIC_INDEX_BUFFER_STRIP_CUT: the command list
  // may override depth bias / strip cut after SetPipelineState (ID3D12GraphicsCommandList9).
  bool dynamic_depth_bias = false;
  bool dynamic_index_buffer_strip_cut = false;
  /*
   * View instancing (ViewInstancingTier 1): every draw is replayed once per enabled view with the view id in the
   * root signature's view-id qword. `view_location_offsets` is set when some view location is not {0, 0}: the
   * rasterizing pipelines then carry maxVertexAmplificationCount 2 so a one-view amplification mapping adds the
   * view's viewport / render target array index to the shader's (an out-of-range sum selects index 0).
   */
  ViewInstancingState view_instancing{};
  bool view_location_offsets = false;
  bool submission_mapping_safe = false;
  uint8_t submission_color_output_mask = 0;

  virtual WMT::DepthStencilState GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) = 0;
  /* Ordinary mapping-safe primary PSO only. Zero/no emitted disabled outputs returns pso.
   * Nonzero variants zero logical color write masks without recompiling shaders.
   * Borrowed native handle; recorded consumers retain it until actual GPU retirement.
   * Null on unsupported nonzero request or native creation failure; never execute the base instead. */
  virtual WMT::RenderPipelineState GetColorWriteMaskPipelineState(uint8_t disabled_outputs) = 0;

  /**
   * The Metal pipeline a recorded indirect command buffer inherits.
   *
   * Direct draws use `pso` without the ICB capability. The indirect variant is
   * built on first use. View-location variants apply offsets in their vertex epilogue rather than inheriting
   * encoder viewMappings; their functions and immutable constants remain separate from the direct variant.
   * Null on failure; callers record an error instead of executing with a direct PSO.
   */
  virtual WMT::RenderPipelineState GetIndirectCommandBufferPipelineState() = 0;
  virtual WMT::RenderPipelineState GetIndexPullPipelineState(bool indirect) = 0;
  virtual WMT::RenderPipelineState GetNativeMSAAPipelineState(bool index_pull) = 0;
  virtual WMT::RenderPipelineState GetPipelineStatisticsPipelineState(bool indirect) = 0;
  /* `coarse`: variable-rate coarse shading variants (SM50_SHADER_D3D12_COARSE_SHADING); the layout carries
   * SM50_MSAA_PIXEL_COARSE when the PS is eligible, otherwise they shade at fine rate. */
  virtual const MSAAPipelineVariants *GetMSAAPipelineVariants(
      bool statistics = false, bool coarse = false, bool frontier = false) = 0;
  virtual const CaptureFrontierPipelines *GetCaptureFrontierPipelines(bool statistics) = 0;
  uint32_t EffectiveRasterSampleCount() const {
    return forced_sample_count ? forced_sample_count : owned_sample_desc.Count;
  }
  bool RequiresMSAAExecution() {
    // The captured-pixel path also preserves clip/cull interpolants that
    // Metal cannot carry natively, even on a single-sample target, and is
    // the only conservative rasterizer (native MSAA would shade per sample).
    return EffectiveRasterSampleCount() > 1 || owned_distance_interpolation || ForcedSampleReplay() ||
           owned_conservative || owned_mesh_output_replay || uses_atomic64 || emulated_depth_bounds;
  }
  /* Target-independent rasterization with RTVs: the forced raster pattern differs from the RTV sample count
   * (ForcedSampleCount > 1 on 1x RTVs, or ForcedSampleCount 1 on multisampled RTVs). The PS output is replayed
   * from captured records into every output sample in SampleMask of each pixel the forced pattern covers. */
  bool ForcedSampleReplay() const {
    return forced_sample_count && owned_rt_count &&
           (forced_sample_count > 1 ? owned_sample_desc.Count == 1 : owned_sample_desc.Count > 1);
  }

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12ComputePipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::ComputePipelineState> pso;
  /* d3d12.asyncPipelineCompile: deferred creation of `pso`, as for graphics PSOs. Every reader of
   * `pso` calls WaitNativePipeline() first; a failed job is a recording error, not a fallback. */
  std::shared_ptr<owned_graphics::AsyncCompileJob> async_job;
  bool WaitNativePipeline() { return !async_job || async_job->Wait(); }
  ShaderImmutableConstants compute_constants;
  ShaderImmutableConstants pipeline_statistics_compute_constants;
  WMTSize threadgroup_size;

  /* See MTLD3D12GraphicsPipelineState::GetIndirectCommandBufferPipelineState. */
  virtual WMT::ComputePipelineState GetIndirectCommandBufferPipelineState() = 0;
  virtual WMT::ComputePipelineState GetPipelineStatisticsPipelineState(bool indirect) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

/* Acceleration-structure sidecars are keyed by the allocation and byte offset of the D3D12
 * address the application built into, so a rebuild at the same address replaces the entry. */
struct RTASKeyHash {
  size_t
  operator()(const raytracing::ArenaKey &key) const noexcept {
    return std::hash<const void *>{}(key.allocationIdentity) ^ (std::hash<uint64_t>{}(key.offset) << 1);
  }
};

struct RTASState {
  WMT::Reference<WMT::AccelerationStructureSidecar> sidecar;
  uint64_t nativeSize = 0;
  uint32_t instanceCount = 0;
  std::shared_ptr<const raytracing::StructureInfo> info;
  // Metadata of a proven ordinary build/refit encoding, not CPU publication or
  // GPU completion. FindEncoded additionally requires the same FIFO queue.
  struct {
    raytracing::ShadowMetadata metadata{};
    uint64_t resourceID = 0;
    uint64_t nativeSize = 0;
    uint64_t instanceCount = 0;
  } execution{};
};

/*
 * What one D3D12 acceleration-structure address currently means.
 *
 * The recorded build is the most recently published one, which is what the record-time CPU
 * lookups (refit/copy/emit source resolution) resolve against. It is not necessarily the version
 * the GPU header holds: two queues may encode B before A while A executes first, so only the GPU
 * can say which native structure a ray dispatch actually reads. That answer arrives as the
 * version token the prepass publishes into the shared mirror and the queue reports once the
 * submission carrying it has completed (see owned_rtas_lifetime.hpp).
 *
 * The mirror is a page allocator whose pages are never resized, copied or freed while the device
 * lives, so a GPU write in flight always targets a buffer that still exists.
 */
using RTASLifetime = owned_graphics::RTASLifetime<raytracing::ArenaKey, RTASState, RTASKeyHash>;

// Tickets are released by GPU retirement, potentially on a different CPU thread.
// A shared_mutex lock is therefore not a suitable submission lifetime token.
struct Atomic64PhasePipelines;

/* d3d12.asyncPipelineCompile: device-owned workers for deferred graphics PSO linking, the
 * presentation epoch, and render-target recurrence for omitting draws while a PSO is pending. */
class AsyncPipelineCompiler {
public:
  /* `threads` is d3d12.asyncPipelineCompileThreads: 0 (default) uses max(1, logical CPUs / 2). */
  AsyncPipelineCompiler(bool enabled, int32_t threads) : enabled_(enabled), threads_(threads > 0 ? threads : 0) {}
  ~AsyncPipelineCompiler() {
    if (d3dalloy::this_thread::isInModuleDetachment()) {
      // Workers were terminated by ExitProcess and may own the queue lock: never lock or join.
      // Detaching closes their handles so ~ThreadFn does not terminate on a joinable thread.
      for (auto &worker : workers_)
        worker.detach();
      return;
    }
    queue_.Stop();
    for (auto &worker : workers_)
      worker.join();
  }
  bool Enabled() const { return enabled_; }
  void Submit(std::shared_ptr<owned_graphics::AsyncCompileJob> job) {
    // OWNED_ASYNC_PIPELINE_HOLD=1 (fixture control) never starts workers: queued links complete
    // only through a synchronous consumer, so pending omission is observable without timing.
    std::call_once(start_, [this] {
      if (d3dalloy::env::getEnvVar("OWNED_ASYNC_PIPELINE_HOLD") == "1")
        return;
      // Leave cores for the application's render thread; workers run below it so a link burst does
      // not preempt frame submission. Unclaimed jobs still run on a consumer that needs them.
      const uint32_t count = threads_ ? threads_ : std::max(1u, d3dalloy::thread::hardware_concurrency() / 2);
      for (uint32_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { queue_.RunWorker(); });
        workers_.back().set_priority(d3dalloy::ThreadPriority::Lowest);
      }
    });
    queue_.Submit(std::move(job));
  }
  uint64_t Epoch() const { return epoch_.load(std::memory_order_acquire); }
  void AdvanceEpoch() { epoch_.fetch_add(1, std::memory_order_acq_rel); }
  owned_graphics::RenderTargetHistory &Targets() { return targets_; }

private:
  const bool enabled_;
  const uint32_t threads_;
  std::once_flag start_;
  owned_graphics::AsyncCompileQueue queue_;
  std::vector<d3dalloy::thread> workers_;
  std::atomic<uint64_t> epoch_{1};
  owned_graphics::RenderTargetHistory targets_;
};

class MTLD3D12Device : public ID3D12Device14 {
public:
  RuntimeConfiguration runtime_configuration;
  bool independent_device = false;
  DeviceDiagnostics diagnostics{this};
  ReflexState reflex{Config::getInstance().getOption<bool>("d3d12.lowLatency", false)};
  GPUExecutionGate gpu_execution_gate;
  // Values only. An equality proof never extends a resource's lifetime.
  struct SwizzleCarry {
    uint64_t resource_id;
    obj_handle_t canonical, shadow;
    uintptr_t layout;
    std::array<uint64_t, 4> valid;
    uint64_t queue, submission;
  };
  struct {
    std::mutex mutex;
    std::array<SwizzleCarry, 64> entries;
    size_t count = 0;
    uint64_t serial = 0;
  } swizzle_proofs;
  AsyncPipelineCompiler async_pipelines{Config::getInstance().getOption<bool>("d3d12.asyncPipelineCompile", false),
                                        Config::getInstance().getOption<int32_t>("d3d12.asyncPipelineCompileThreads", 0)};
  std::shared_ptr<Atomic64PhasePipelines> atomic64_phase_pipelines;
  virtual HRESULT RemovedReason() const = 0;
  virtual void ReportRemoval(HRESULT reason) = 0;
  virtual bool RegisterFenceOrder(const std::shared_ptr<owned_graphics::FenceOrderEpoch> &order) = 0;

  virtual WMT::Device GetMTLDevice() = 0;
  virtual WMT4::Compiler GetMetal4Compiler() = 0;
  virtual DeviceShaderArtifactCache &GetShaderArtifactCache() = 0;
  virtual bool SupportsCenterSamplePattern() const = 0;

  virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;

  virtual HRESULT GetAdapter(REFIID riid, void **ppAdapter) = 0;

  virtual WMT::ResidencySet GetGlobalResidencySet() = 0;
  // Serialize the native borrowed residency snapshot with allocation removal.
  // This returns before GPU retirement; callers must wait outside the lock.
  virtual WMT::Reference<WMT::MTL4Submission> CommitMetal4CommandBuffers(
      WMT::MTL4CommandQueue queue, const WMT::MTL4CommandBuffer *buffers, uint64_t count) = 0;

  /* Placement sparse tile mappings are applied through this Metal 4 queue (created on first use). */
  virtual WMT::MTL4CommandQueue GetSparseMappingQueue() = 0;
  // Called under ReservedResource::MappingQueueMutex, after the corresponding native mappings.
  virtual HRESULT PublishSparseVolumeResidency(WMT::Buffer status,
      std::span<const std::pair<uint32_t, ReservedTileTarget>> assignments) = 0;
  virtual HRESULT CompleteSparseVolumeResidency() = 0;

  /**
   * GPU address of the device-global 64 bit atomic lock table
   * (SM50_ATOMIC_LOCK_TABLE_ENTRIES zeroed 32 bit words, resident in the global
   * residency set for the device lifetime), or 0 when it could not be allocated.
   */
  virtual uint64_t GetAtomicLockTableAddress() = 0;
  virtual uint64_t GetAtomic64ContextAddress() = 0;
  virtual WMT::Buffer GetAtomic64ContextBuffer() = 0;

  virtual HRESULT RegisterResidency(
      WMT::Allocation allocation, MTLD3D12PageableResidency *owner = nullptr, bool initially_resident = true
  ) = 0;

  virtual HRESULT UnregisterResidency(
      WMT::Allocation allocation, MTLD3D12PageableResidency *owner = nullptr
  ) = 0;
  virtual HRESULT UnregisterSwizzleShadowResidency(
      WMT::Allocation allocation, MTLD3D12PageableResidency *owner
  ) = 0;
  virtual HRESULT RegisterDescriptorResource(MTLD3D12Resource *resource) = 0;
  virtual void UnregisterDescriptorResource(MTLD3D12Resource *resource) = 0;
  virtual Com<MTLD3D12Resource, false> LookupDescriptorResource(const void *backing, uint64_t owner_id = 0) = 0;
  virtual Com<MTLD3D12Resource, false> LookupStandardSwizzleTexture(Texture *texture) = 0;
  virtual bool HasStandardSwizzleTextures() const = 0;

  virtual HRESULT RegisterResidencyAndVA(
      BufferAllocation *allocation, MTLD3D12PageableResidency *owner = nullptr, bool initially_resident = true
  ) = 0;

  virtual HRESULT UnregisterResidencyAndVA(
      BufferAllocation *allocation, MTLD3D12PageableResidency *owner = nullptr
  ) = 0;

  virtual Rc<BufferAllocation> LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) = 0;
  virtual std::vector<Rc<BufferAllocation>> SnapshotBufferAllocations() = 0;
  /* LookupBufferByVA that also reports the VA interval [*pBegin, *pEnd) over which the same
   * allocation is returned and the map generation observed; *pEnd is 0 for a null result. */
  virtual Rc<BufferAllocation> LookupBufferRangeByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset, uint64_t *pBegin,
                                                  uint64_t *pEnd, uint64_t *pGeneration) = 0;
  /* Advances whenever the VA map gains or loses a buffer; a cached lookup result is reusable
   * only while this value is unchanged. */
  std::atomic<uint64_t> buffer_va_generation{0};

  virtual InternalCommandLibrary& GetLib() = 0;

  /* Native sizes for one validated build description, so prebuild info and the actual build
   * agree on the same descriptor. */
  virtual HRESULT QueryRTASSizes(const raytracing::BuildInputs &inputs, raytracing::NativeSizes &sizes) = 0;

  /* Sidecar lookup/publish for the acceleration structure at a D3D12 address. */
  virtual bool FindRTAS(const raytracing::ArenaKey &key, RTASState &state) = 0;
  virtual bool FindRTASExecution(const raytracing::ArenaKey &key, uint64_t queue, RTASState &state) = 0;
  virtual bool AllocateRTAS(WMT::AccelerationStructureSidecar sidecar, uint64_t size, uint64_t count) = 0;

  /**
   * Publish a build for a D3D12 address. `token` receives the version token the prepass must
   * publish into the shared mirror, and `mirror`/`mirror_offset` the page it writes into.
   *
   * Returns false when the build cannot be tracked (mirror full), which the caller reports as an
   * out-of-memory build rather than recording a build whose version can never be identified.
   */
  virtual bool StoreRTAS(const raytracing::ArenaKey &key, RTASState state, uint32_t *token,
      WMT::Reference<WMT::Buffer> *mirror, uint64_t *mirror_offset) = 0;

  /**
   * Give up the recording lease a command list held on a published version. Called when the
   * command list that published it is reset or destroyed, so a build an application records and
   * never submits is released instead of accumulating (see owned_rtas_lifetime.hpp).
   */
  virtual void ReleaseRTASLease(const raytracing::ArenaKey &key, uint32_t token) = 0;

  /**
   * Take the submission lease and record this writer's FIFO execution provenance. A null
   * metadata pointer invalidates the ordinary-refit proof (copy/compact/deserialize, etc.).
   * This never observes a GPU token or completes a public fence.
   */
  virtual void RecordRTASExecution(const raytracing::ArenaKey &key, uint32_t token, uint64_t queue,
      const raytracing::ShadowMetadata *metadata) = 0;

  /**
   * Submission lifetime. A queue marks a submission active before it is enqueued and retires it
   * after Metal reports completion; every submission in flight at an observation blocks the
   * release of the version it superseded, and retiring one releases whatever expired with it.
   */
  virtual uint64_t BeginRTASSubmission() = 0;

  /** Observe GPU mirrors, release submission leases and retire under one residency lock/sweep. */
  virtual void CompleteRTASSubmission(uint64_t id,
      std::span<const std::pair<raytracing::ArenaKey, uint32_t>> leases, bool observe_gpu) = 0;

  /* Lazily compiled prepass/write/finalize kernels out of the internal command library. */
  virtual HRESULT GetRTASComputePipelines(RTASComputePipelines &pipelines) = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  EventListener event_listener;

  // Resource state publication remains device-wide; temporal history uses
  // independent per-feature reservations owned by the submitting segment.
  std::mutex owned_barrier_state_mutex;
  OwnedNativeStates owned_native_states;

  // Fixed-source, unspecialized command helpers. Only immutable native objects
  // are shared; argument tables and recording state remain command-list local.
  std::mutex owned_helper_pipeline_mutex;
  WMT::Reference<WMT::Library> owned_predication_library;
  std::array<WMT::Reference<WMT::ComputePipelineState>, 11> owned_predication_pipelines;
  std::array<WMT::Reference<WMT::ComputePipelineState>, 2> owned_feedback_pipelines;
  enum class CommandHelper {
    D24Upload, D24Readback, RGB32Expand, RGB32Compact, Packed422Expand, Packed422Compact,
    StreamOutputAssemble, StreamOutputPositions, StreamOutputGS,
    MSAAAssembly, MSAABounds, AtomicCopyU32, AtomicCopyU64, MatrixConversion, RaysIndirect,
    CaptureFrontierPrepare, CaptureFrontierClear, CaptureFrontierConsume, CaptureFrontierAdvance,
    MSAATilePrepare, MSAATileClear, MSAATileExtent, Count
  };
  struct CommandHelperEntry {
    WMT::Reference<WMT::Library> library;
    WMT::Reference<WMT::ComputePipelineState> pipeline;
  };
  std::array<CommandHelperEntry, size_t(CommandHelper::Count)> owned_command_helpers;

  struct ResolvePipelineKey {
    uint32_t attachment_format, raster_sample_count;
    uint8_t fragment_kind, dsv_plane, depth_write_enabled, stencil_enabled;
    bool operator==(const ResolvePipelineKey &) const = default;
  };
  struct ResolvePipelineCacheEntry {
    ResolvePipelineKey key{};
    WMT::Reference<WMT::RenderPipelineState> pipeline;
    WMT::Reference<WMT::DepthStencilState> depth_stencil_state;
  };
  // Bounded device lookup; command-list hot entries and recorded passes retain
  // evicted objects independently. No resource/encoder binding is stored here.
  std::array<ResolvePipelineCacheEntry, 8> owned_resolve_pipelines{};
  size_t owned_resolve_pipeline_count = 0, owned_resolve_pipeline_next = 0;

  // Static sampler states are shared across root signatures, with a device-wide
  // limit of 2032 distinct live states (not 2032 declarations per signature).
  using StaticSamplerKey = std::array<uint32_t, 10>;
  std::mutex static_sampler_mutex;
  std::map<StaticSamplerKey, size_t> static_sampler_uses;

  WMT::Reference<WMT::DepthStencilState> default_depth_stencil_state;
};

// Flip-sequential partial presents update one persistent image. The second
// texture snapshots only a scroll source rectangle before an overlapping move.
// The present-order GPU event serializes access across queues. Each submitted
// frame retains this generation until retirement, including after ResizeBuffers.
struct D3DalloyPresentHistory {
  Com<MTLD3D12Device> device;
  std::array<WMT::Reference<WMT::Texture>, 2> textures;
  std::array<bool, 2> resident{};
  ~D3DalloyPresentHistory() {
    for (size_t index = 0; index < textures.size(); ++index)
      if (resident[index])
        device->UnregisterResidency(textures[index]);
  }
};

struct D3DalloyPresentComposition {
  // CPU-only native presentation snapshot; never part of GPU shader constants.
  WMT::Reference<WMT::Object> surface;
  WMTCompositionFrame frame{};
  uint32_t sync_interval = 0;
  bool allow_tearing = false;
  std::shared_ptr<D3DalloyPresentHistory> history;
  bool has_previous = false;
  bool scroll = false;
  RECT scroll_rect{};
  POINT scroll_offset{};
  UINT width = 0, height = 0;
  std::vector<RECT> dirty_rects;
  bool full = true;
};

HRESULT CreateD3D12Device(IMTLDXGIAdapter *adapter, REFIID riid, void **ppDevice,
                         const RuntimeConfiguration &configuration, bool independent);

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue);

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator);

HRESULT
CreateClosedCommandList(
    MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandList
);

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
);

HRESULT
CreateQueryHeap(MTLD3D12Device *pDevice, const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppQueryHeap);

HRESULT CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
);

HRESULT
CreatePlacedTexture(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
);

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

/* Sampler feedback maps (d3d12_sampler_feedback.cpp); `Layout` comes from sampler_feedback::MakeLayout. */
HRESULT SamplerFeedbackAllocationInfo(MTLD3D12Device *pDevice, const sampler_feedback::Layout &Layout,
                                      D3D12_RESOURCE_ALLOCATION_INFO1 &Info);

HRESULT CreateSamplerFeedbackMap(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES &HeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC &Desc, const D3D12_MIP_REGION &MipRegion, const sampler_feedback::Layout &Layout,
    MTLD3D12Heap *pHeap, UINT64 HeapOffset,
    D3D12_RESOURCE_STATES InitialState, REFIID riid, void **ppResource
);

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap);

/*
 * OpenExistingHeapFromAddress(1)/FromFileMapping backends: a CUSTOM WRITE_BACK/L0 heap with
 * SHARED_CROSS_ADAPTER | ALLOW_ONLY_BUFFERS over application memory. `pAddress` and `Size` must be
 * 16 KiB (host page) aligned, committed and read-write; Size 0 takes the committed run at the address.
 */
HRESULT
CreateHeapFromHostMemory(MTLD3D12Device *pDevice, void *pAddress, UINT64 Size, REFIID riid, void **ppHeap);
HRESULT
CreateHeapFromFileMapping(MTLD3D12Device *pDevice, HANDLE hFileMapping, REFIID riid, void **ppHeap);

/* OpenSharedHandle backends: rebuild a device object over a process-wide share. */
HRESULT
OpenSharedHeap(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppHeap);
HRESULT
OpenSharedBuffer(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppResource);
HRESULT
OpenSharedTexture(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppResource);
HRESULT
OpenSharedFence(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppFence);

HRESULT CreateReservedBuffer(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT CreateReservedTexture(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
);

HRESULT
CreateRootSignature(
    MTLD3D12Device *pDevice, UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid,
    void **ppRootSignature
);

HRESULT
CreateCommandSignature(
    MTLD3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
    REFIID riid, void **ppCommandSignature
);

/*
 * Pipeline state stream fields the classic D3D12_GRAPHICS_PIPELINE_STATE_DESC cannot carry:
 * depth bounds, fractional DepthBias (D3D12_RASTERIZER_DESC1/2) and distinct back-face stencil masks
 * (D3D12_DEPTH_STENCIL_DESC2). Only set when they differ from what the classic desc holds, so
 * equivalent descriptions share a pipeline cache key.
 */
struct GraphicsPipelineExtras {
  bool depth_bounds = false;
  std::optional<float> depth_bias;
  std::optional<std::pair<UINT8, UINT8>> back_stencil_read_write_mask;
  std::optional<ViewInstancingState> view_instancing;
  /* Mesh shader pipeline (MeshShaderTier 1): the MS subobject, and the optional AS. Empty
   * bytecode means the stage is absent. */
  D3D12_SHADER_BYTECODE as{};
  D3D12_SHADER_BYTECODE ms{};
  bool
  empty() const {
    return !depth_bounds && !depth_bias && !back_stencil_read_write_mask && !view_instancing && !ms.pShaderBytecode;
  }
};

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    const GraphicsPipelineExtras *extras = nullptr
);

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain, IDXGIOutput *pRestrictToOutput, IUnknown *core_window, D3DalloySwapChainKind kind
);

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence);

/* Metal sampler reduction for a D3D12 filter: MINIMUM/MAXIMUM select min/max, anything else weighted average. */
inline uint32_t
SamplerReductionMode(D3D12_FILTER filter) {
  switch (D3D12_DECODE_FILTER_REDUCTION(filter)) {
  case D3D12_FILTER_REDUCTION_TYPE_MINIMUM:
    return 1;
  case D3D12_FILTER_REDUCTION_TYPE_MAXIMUM:
    return 2;
  default:
    return 0;
  }
}

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_STATIC_SAMPLER_DESC1 const &Desc);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_SAMPLER_DESC const &Desc);

inline std::tuple<MTLD3D12RenderTargetDescriptorHeap *, UINT>
GetRenderTargetHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12RenderTargetDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  auto [heap, index] = ResolveDescriptorHandle(Handle.ptr, DescriptorHandleKind::RenderTarget);
  return {static_cast<MTLD3D12RenderTargetDescriptorHeap *>(heap), index};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetRenderTargetDescriptor(MTLD3D12RenderTargetDescriptorHeap *pHeap, UINT Index) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  return {DescriptorHandleOf(pHeap, Index)};
#endif
}

inline std::tuple<MTLD3D12DescriptorHeap *, UINT>
GetShaderVisibleDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12DescriptorHeap>(), (UINT)impl.Descriptor};
#else
  auto [heap, index] = ResolveDescriptorHandle(Handle.ptr, DescriptorHandleKind::ShaderVisible);
  return {static_cast<MTLD3D12DescriptorHeap *>(heap), index};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetShaderVisibleDescriptor(MTLD3D12DescriptorHeap *pHeap, UINT Index) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  return {DescriptorHandleOf(pHeap, Index)};
#endif
  //
}

inline std::tuple<MTLD3D12SamplerDescriptorHeap *, UINT>
GetSamplerDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  const SIZE_T heap = ((Handle.ptr >> 26) << 9) | ((Handle.ptr & 63) << 3);
  return {reinterpret_cast<MTLD3D12SamplerDescriptorHeap *>(heap), UINT((Handle.ptr >> 6) & 0xfffff)};
#else
  auto [heap, index] = ResolveDescriptorHandle(Handle.ptr, DescriptorHandleKind::Sampler);
  return {static_cast<MTLD3D12SamplerDescriptorHeap *>(heap), index};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetSamplerDescriptor(MTLD3D12SamplerDescriptorHeap *pHeap, UINT Index) {
#ifdef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  // Six stride bits, twenty index bits, thirty-eight heap bits; retain the full CPU heap capacity.
  const SIZE_T heap = reinterpret_cast<SIZE_T>(pHeap);
  return {((heap >> 9) << 26) | ((heap >> 3) & 63) | (SIZE_T(Index) << 6)};
#else
  return {DescriptorHandleOf(pHeap, Index)};
#endif
}

template <typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const D3D12_RESOURCE_DESC &ResourceDesc, VIEW_DESC *pViewDescOut);

constexpr auto kDefaultShader4Component = 0b1'011'010'001'000;

HRESULT ValidateResourceStates(D3D12_RESOURCE_STATES State, const D3D12_HEAP_PROPERTIES *pHeapProps,
                               const D3D12_RESOURCE_DESC *pDesc);
bool IsShadingRateImageDesc(const D3D12_RESOURCE_DESC &desc);

HRESULT ValidateResourceDescs(MTLD3D12Device *device, const D3D12_RESOURCE_DESC *pDesc,
                              const D3D12_HEAP_PROPERTIES *pHeapProps);
HRESULT ValidateCastableFormats(MTLD3D12Device *device,const D3D12_RESOURCE_DESC &desc,UINT32 count,
                               const DXGI_FORMAT *formats);
std::optional<standard_swizzle::Layout> MakeStandardSwizzleLayout(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc
);

// Shared sample contract for feature queries, resource preflight and graphics PSOs.
bool IsD3D12SampleCountSupported(MTLD3D12Device *device, UINT count);
bool IsD3D12SampleQualitySupported(MTLD3D12Device *device, UINT count, UINT quality);
bool IsD3D12TextureFormatCompatible(DXGI_FORMAT format, const MTL_DXGI_FORMAT_DESC &mapped,
                                   FormatCapability capability, uint32_t native_texel_size);
bool IsD3D12TextureSampleSupported(MTLD3D12Device *device, DXGI_FORMAT format, const MTL_DXGI_FORMAT_DESC &mapped,
                                   D3D12_RESOURCE_FLAGS flags, UINT count,
                                   bool query_typeless_interpretations = false);
bool IsD3D12MSAAPixelFormatSupported(MTLD3D12Device *device, WMTPixelFormat format);
WMTPixelFormat GetD3D12PhysicalPixelFormat(DXGI_FORMAT format);
WMTPixelFormat GetD3D12ResourcePixelFormat(const MTL_DXGI_FORMAT_DESC &mapped,
                                          D3D12_RESOURCE_FLAGS flags);

HRESULT ValidateOptimizedClearValue(
    const D3D12_RESOURCE_DESC *pDesc, const D3D12_CLEAR_VALUE *optimized_clear_value
);

HRESULT ValidateHeapProperties(const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS Flags, bool AdapterIsNUMA);

D3D12_BOX GetResourceExtent(const D3D12_RESOURCE_DESC &Desc, UINT MipSlice);

UINT DecomposeSubresource(
    const D3D12_RESOURCE_DESC &Desc, UINT Subresource = 0, UINT *pMipSlice = NULL, UINT *pArraySlice = NULL,
    UINT *pPlaneSlice = NULL
);

bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps);

bool IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds);

struct ExperimentalDeviceRegistration;
HRESULT RegisterExperimentalDevice(
    MTLD3D12Device *device, std::shared_ptr<ExperimentalDeviceRegistration> &registration
);
void UnregisterExperimentalDevice(const std::shared_ptr<ExperimentalDeviceRegistration> &registration);

} // namespace d3dalloy

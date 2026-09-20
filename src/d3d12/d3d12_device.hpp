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
#include "d3d12_acceleration_structure.hpp"
#include "d3d12_command_encoder.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_raytracing_sbt.hpp"
#include "dxgi1_2.h"
#include "dxgi_interfaces.h"
#include "airconv_public.h"
#include "dxmt_buffer.hpp"
#include "dxmt_command.hpp"
#include "dxmt_fence.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"
#include "owned_trace.hpp"
#include "owned_bundle_recording.hpp"
#include "owned_rtas_lifetime.hpp"
#include "owned_rtas_mirror.hpp"
#include <map>
#include <mutex>
#include <unordered_map>

#define IMPLEMENT_ME                                                                                                   \
  do {                                                                                                                 \
    Logger::err(str::format(__FILE__, ":", __FUNCTION__, "(", __LINE__, ") is not implemented."));                     \
    abort();                                                                                                           \
    __builtin_unreachable();                                                                                           \
  } while (0);

namespace dxmt {

// One advertised ceiling shared by creation/probe and feature queries.
inline constexpr D3D_FEATURE_LEVEL kOwnedMaxFeatureLevel = D3D_FEATURE_LEVEL_11_0;

class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandList4 {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  uint64_t owned_recording = 0;
  ID3D12CommandAllocator *owned_allocator = nullptr;
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

class MTLD3D12CommandQueue : public ID3D12CommandQueue {
public:
  virtual HRESULT Present(Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after) = 0;
};

class MTLD3D12Resource : public ID3D12Resource {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  Rc<Texture> texture;
  Rc<Buffer> buffer;

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

class MTLD3D12Heap : public ID3D12Heap {
public:
  WMT::Reference<WMT::Heap> heap;
};

class MTLD3D12Fence : public ID3D12Fence1 {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  Rc<Fence> fence;
};

class MTLD3D12RootSignature : public ID3D12RootSignature {
public:
  virtual UINT GetBlob(const void **ppBlob) = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

  uint32_t UploadQwords;
  uint32_t ParameterSlots;
  uint32_t const *SlotQwordOffsets;

  /**
   * Local root arguments of a raytracing shader record. The application writes the
   * parameters of this root signature after the 32 byte shader identifier using the
   * D3D12 local root argument layout; the raytracing pipeline translates each
   * record into the qword root argument layout of this signature. `LocalRootCopies`
   * describes that translation and `LocalRootDataSize` is the number of bytes the
   * record must carry for it.
   */
  uint32_t LocalRootDataSize;
  uint32_t LocalRootCopyCount;
  raytracing::LocalRootCopy const *LocalRootCopies;

  size_t NumStaticSamplers;
  uint64_t const *EncodedStaticSamplers;
};

class MTLD3D12CommandSignature : public ID3D12CommandSignature {
public:
  D3D12_INDIRECT_ARGUMENT_TYPE CommandType;
  UINT UpdateRootArguments : 1;
  UINT UpdateVertexBuffers : 1;
  UINT UpdateIndexBuffer   : 1;

  WMT::Reference<WMT::RenderPipelineState> render_resolver;
  WMT::Reference<WMT::ComputePipelineState> compute_resolver;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  const uint64_t owned_id = owned_graphics::NewObjectId();
  D3D12_QUERY_HEAP_DESC desc{};
  WMT::Reference<WMT::CounterSampleBuffer> timestamp_samples;
  std::shared_ptr<owned_graphics::QueryTimestampProducers> timestamp_producers;
  WMT::Reference<WMT::Buffer> timestamp_dummy;
  WMT::Reference<WMT::Buffer> visibility_results;
  WMT::Reference<WMT::ComputePipelineState> binary_normalize_pipeline;
  WMT::Reference<WMT::ComputePipelineState> visibility_accumulate_pipeline;
};

struct ShaderImmutableConstants {
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t offset = 0;
  uint64_t gpu_address = 0;

  HRESULT Initialize(WMT::Device metal, const SM50_COMPILED_BITCODE &compiled);
};

class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  UINT IsComputePipelineState;

  static HRESULT
  InitializeShader(
      const char *Stage, D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader,
      struct MTL_SHADER_REFLECTION *pRefl
  );
};

class MTLD3D12GraphicsPipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::RenderPipelineState> pso;
  ShaderImmutableConstants vertex_constants;
  ShaderImmutableConstants fragment_constants;
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

  virtual WMT::DepthStencilState GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) = 0;

  /**
   * The Metal pipeline a recorded indirect command buffer inherits.
   *
   * Direct draws use `pso` without the ICB capability. The indirect variant is
   * built on first use from the same functions and immutable-buffer contract.
   * Null on failure; callers record an error instead of executing with a direct PSO.
   */
  virtual WMT::RenderPipelineState GetIndirectCommandBufferPipelineState() = 0;

  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

class MTLD3D12ComputePipelineState : public MTLD3D12PipelineState {
public:
  WMT::Reference<WMT::ComputePipelineState> pso;
  ShaderImmutableConstants compute_constants;
  WMTSize threadgroup_size;

  /* See MTLD3D12GraphicsPipelineState::GetIndirectCommandBufferPipelineState. */
  virtual WMT::ComputePipelineState GetIndirectCommandBufferPipelineState() = 0;

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

struct RTASComputePipelines {
  WMT::Reference<WMT::ComputePipelineState> prepass;
  WMT::Reference<WMT::ComputePipelineState> writeU64;
  WMT::Reference<WMT::ComputePipelineState> finalizeCompactedSize;
};

class MTLD3D12Device : public ID3D12Device5 {
public:
  virtual WMT::Device GetMTLDevice() = 0;

  virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;

  virtual HRESULT GetAdapter(REFIID riid, void **ppAdapter) = 0;

  virtual WMT::ResidencySet GetGlobalResidencySet() = 0;

  virtual HRESULT RegisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT UnregisterResidency(WMT::Allocation allocation) = 0;

  virtual HRESULT RegisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual HRESULT UnregisterResidencyAndVA(BufferAllocation *allocation) = 0;

  virtual BufferAllocation *LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) = 0;

  virtual InternalCommandLibrary& GetLib() = 0;

  /* Native sizes for one validated build description, so prebuild info and the actual build
   * agree on the same descriptor. */
  virtual HRESULT QueryRTASSizes(const raytracing::BuildInputs &inputs, raytracing::NativeSizes &sizes) = 0;

  /* Sidecar lookup/publish for the acceleration structure at a D3D12 address. */
  virtual bool FindRTAS(const raytracing::ArenaKey &key, RTASState &state) = 0;

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
   * Take a second lease on a published version: its recording has been handed to a submission, so
   * the version must survive the recording that published it.
   */
  virtual void HoldRTASLease(const raytracing::ArenaKey &key, uint32_t token) = 0;

  /**
   * Report the version the GPU header holds for a D3D12 address, read from the shared mirror
   * once the submission that wrote it has completed. This is the only authority on which native
   * structure a dispatch will read, and it is what allows an executed version to be superseded
   * and eventually released without ever guessing at GPU execution order.
   */
  virtual void ObserveRTAS(const raytracing::ArenaKey &key) = 0;

  /**
   * Submission lifetime. A queue marks a submission active before it is enqueued and retires it
   * after Metal reports completion; every submission in flight at an observation blocks the
   * release of the version it superseded, and retiring one releases whatever expired with it.
   */
  virtual uint64_t BeginRTASSubmission() = 0;

  virtual void RetireRTASSubmission(uint64_t id) = 0;

  /* Lazily compiled prepass/write/finalize kernels out of the internal command library. */
  virtual HRESULT GetRTASComputePipelines(RTASComputePipelines &pipelines) = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  EventListener event_listener;

  // Publish all feature-history dependencies of one temporal-containing Metal
  // command buffer atomically with respect to other queues on this device.
  // Queue workers acquire this only after reserving their submission-ring slot.
  std::mutex owned_temporal_submission_mutex;

  WMT::Reference<WMT::DepthStencilState> default_depth_stencil_state;
};

HRESULT CreateD3D12Device(IMTLDXGIAdapter *adapter, REFIID riid, void **ppDevice);

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

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap);

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

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
);

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain
);

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_STATIC_SAMPLER_DESC const &Desc);

void PopulateWMTSamplerInfo(WMT::Device Device, WMTSamplerInfo &InfoOut, D3D12_SAMPLER_DESC const &Desc);

inline std::tuple<MTLD3D12RenderTargetDescriptorHeap *, UINT>
GetRenderTargetHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12RenderTargetDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetRenderTargetDescriptor(MTLD3D12RenderTargetDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline std::tuple<MTLD3D12DescriptorHeap *, UINT>
GetShaderVisibleDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12DescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetShaderVisibleDescriptor(MTLD3D12DescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
  //
}

inline std::tuple<MTLD3D12SamplerDescriptorHeap *, UINT>
GetSamplerDescriptorHeap(MTLD3D12Device *pDevice, D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  EMBEDDED_DESCRIPTOR_HANDLE impl(Handle);
  return {impl.extract<MTLD3D12SamplerDescriptorHeap>(), (UINT)impl.Descriptor};
#else
  IMPLEMENT_ME
  return {};
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
GetSamplerDescriptor(MTLD3D12SamplerDescriptorHeap *pHeap, UINT Index) {
#ifdef DXMT_USE_EMBEDDED_HEAP_POINTER
  return EMBEDDED_DESCRIPTOR_HANDLE(pHeap, Index);
#else
  IMPLEMENT_ME
  return {};
#endif
}

template <typename VIEW_DESC>
HRESULT ExtractEntireResourceViewDescription(const D3D12_RESOURCE_DESC &ResourceDesc, VIEW_DESC *pViewDescOut);

constexpr auto kDefaultShader4Component = 0b1'011'010'001'000;

HRESULT ValidateResourceStates(D3D12_RESOURCE_STATES State, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateResourceDescs(const D3D12_RESOURCE_DESC *pDesc, const D3D12_HEAP_PROPERTIES *pHeapProps);

HRESULT ValidateHeapProperties(const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS Flags, bool AdapterIsNUMA);

D3D12_BOX GetResourceExtent(const D3D12_RESOURCE_DESC &Desc, UINT MipSlice);

UINT DecomposeSubresource(
    const D3D12_RESOURCE_DESC &Desc, UINT Subresource = 0, UINT *pMipSlice = NULL, UINT *pArraySlice = NULL,
    UINT *pPlaneSlice = NULL
);

bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps);

bool IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds);

} // namespace dxmt

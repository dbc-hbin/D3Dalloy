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

#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_dxil_subobjects.hpp"
#include "d3d12_enhanced_barrier.hpp"
#include "d3d12_format_support.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline_library.hpp"
#include "d3d12_resource_contract.hpp"
#include "d3d12_state_object.hpp"
#include "d3d12_temporal.hpp"
#include "Metal.hpp"
#include "DXBCParser/BlobContainer.h"
#include "com/com_pointer.hpp"
#include "com/com_object.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

/* Enhanced barriers and the CommandList8/9 dynamic state: GPU verified by owned-d3d12-enhanced-barriers. */
constexpr WINBOOL kReportLayoutFeatures = TRUE;

const GUID kD3D12DeviceDownlevelUUID = {0x74eaee3f, 0x2f4b, 0x476d, {0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10}};

static bool IsPacked422(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM;
}

/* Keep the compatibility decision shared by committed/placed resource creation
 * and allocation-info queries. Footprints are independent DXGI layout queries. */
static bool IsResourceDescriptorCompatible(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return desc.Width <= device->GetMTLDevice().maxBufferLength();
  if (desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE &&
      !MakeStandardSwizzleLayout(device, desc))
    return false;
  if (IsPacked422(desc.Format) && (desc.Width & 1))
    return false;
  MTL_DXGI_FORMAT_DESC mapped;
  return SUCCEEDED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, mapped)) &&
         IsD3D12TextureSampleSupported(device, desc.Format, mapped, desc.Flags, desc.SampleDesc.Count);
}

HRESULT PopulateWMTTextureInfo(WMT::Device Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc);

/*
 * Serialized root signature of a GlobalRootSignature/LocalRootSignature subobject declared in a
 * DXIL library (ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary), read through the
 * shared RDAT subobject reader. A null name selects the only root signature subobject, and is
 * ambiguous (E_INVALIDARG) when there are several.
 */
static HRESULT
FindLibraryRootSignature(const void *blob, SIZE_T size, LPCWSTR name, std::vector<uint8_t> &payload) {
  std::vector<DxilSubobject> subobjects;
  if (HRESULT hr = ReadDxilLibrarySubobjects(blob, size, subobjects); FAILED(hr))
    return hr == E_OUTOFMEMORY ? hr : E_INVALIDARG;
  const DxilSubobject *match = nullptr;
  for (const auto &subobject : subobjects) {
    if (subobject.kind != DxilSubobjectKind::GlobalRootSignature &&
        subobject.kind != DxilSubobjectKind::LocalRootSignature)
      continue;
    if (name) {
      if (subobject.name != name)
        continue;
      match = &subobject;
      break;
    }
    if (match)
      return E_INVALIDARG;
    match = &subobject;
  }
  if (!match)
    return E_INVALIDARG;
  payload = match->root_signature;
  return S_OK;
}

/* Resolve an application GPU address into the Metal buffer that backs it. D3D12 GPU virtual
 * addresses in this runtime are Metal device addresses, so the resolved offset is directly
 * usable as a device pointer in the shaders that read the acceleration-structure header. */
bool ResolveRTASGPUVA(void *context, D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t requiredBytes,
    raytracing::BufferLocation &location) {
  auto *device = static_cast<MTLD3D12Device *>(context);
  uint64_t offset = 0;
  auto *allocation = device->LookupBufferByVA(address, &offset);
  if (!allocation || offset > allocation->byteLength() || requiredBytes > allocation->byteLength() - offset)
    return false;
  location = {allocation->buffer(), offset, allocation->byteLength() - offset, allocation};
  return true;
}

class MTLD3D12DeviceImpl : public MTLD3D12Object<ComObject<MTLD3D12Device, IMTLD3D12DeviceExt>> {

  Com<IMTLDXGIAdapter> adapter_;
  bool center_sample_positions_supported_ = false;
  /* IOKit gpu-core-count of this adapter; 0 when the registry lookup fails. */
  uint32_t gpu_core_count_ = 0;

  std::atomic<HRESULT> removal_reason_{S_OK};
  std::mutex removal_mutex_;
  std::vector<std::weak_ptr<owned_graphics::FenceOrderEpoch>> fence_orders_;

  bool advertise_numa_ = false;

  dxmt::mutex residency_lock_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  std::mutex sparse_mapping_queue_mutex_;
  WMT::Reference<WMT::MTL4CommandQueue> sparse_mapping_queue_;
  /* SM 6.6 64 bit atomic emulation lock table (SM50_ATOMIC_LOCK_TABLE_ENTRIES words). */
  WMT::Reference<WMT::Buffer> atomic_lock_table_;
  uint64_t atomic_lock_table_address_ = 0;
  std::map<uint64_t, BufferAllocation *> interval_map_;
  dxmt::mutex swizzle_texture_lock_;
  std::unordered_map<Texture *, MTLD3D12Resource *> swizzle_textures_;
  std::atomic<uint32_t> swizzle_texture_count_{0};

  InternalCommandLibrary command_library;
  FormatCapabilityInspector format_inspector_;

  /* Acceleration-structure versions per D3D12 address, plus the GPU-published mirror that says
   * which one the header currently holds, and the lazily compiled prepass kernels. */
  RTASLifetime rtas_;
  owned_graphics::RTASMirror rtas_mirror_;
  std::vector<WMT::Reference<WMT::Buffer>> rtas_mirror_pages_;
  /* Stable mirror slot per D3D12 address, so the prepass always publishes into the same slot. */
  std::unordered_map<raytracing::ArenaKey, uint32_t, RTASKeyHash> rtas_slots_;
  std::mutex rtas_pipeline_lock_;
  RTASComputePipelines rtas_pipelines_;

  struct ResidencyBatchEntry {
    MTLD3D12PageableResidency *pageable;
    uint32_t occurrences;
  };

  HRESULT
  ResolveResidencyBatch(
      UINT object_count, ID3D12Pageable *const *objects, std::vector<ResidencyBatchEntry> &batch
  ) {
    if (object_count && !objects)
      return E_INVALIDARG;

    std::unordered_map<MTLD3D12PageableResidency *, size_t> indices;
    indices.reserve(object_count);
    batch.reserve(object_count);
    for (UINT i = 0; i < object_count; ++i) {
      if (!objects[i])
        return E_INVALIDARG;
      Com<ID3D12Device> object_device;
      if (FAILED(objects[i]->GetDevice(__uuidof(ID3D12Device),
                                       reinterpret_cast<void **>(&object_device))) ||
          object_device.ptr() != static_cast<ID3D12Device *>(this))
        return E_INVALIDARG;
      auto *pageable = dynamic_cast<MTLD3D12PageableResidency *>(objects[i]);
      if (!pageable || !pageable->SupportsResidency())
        return E_INVALIDARG;
      auto [entry, inserted] = indices.emplace(pageable, batch.size());
      if (inserted)
        batch.push_back({pageable, 1});
      else if (batch[entry->second].occurrences == std::numeric_limits<uint32_t>::max())
        return E_INVALIDARG;
      else
        ++batch[entry->second].occurrences;
    }
    return S_OK;
  }

  HRESULT
  MakeResidentLocked(const std::vector<ResidencyBatchEntry> &batch, bool deny_overbudget = false) {
    uint64_t addition_bytes = 0;
    for (const auto &entry : batch) {
      if (!entry.pageable->CanAddResidencyReference(entry.occurrences))
        return E_INVALIDARG;
      if (!entry.pageable->ResidencyReferenceCount()) {
        const uint64_t bytes = entry.pageable->ResidencyAllocationBytes();
        if (bytes > std::numeric_limits<uint64_t>::max() - addition_bytes)
          return E_OUTOFMEMORY;
        addition_bytes += bytes;
      }
    }
    if (deny_overbudget) {
      const uint64_t resident_bytes = residency_set_.allocatedSize();
      const uint64_t budget = GetMTLDevice().recommendedMaxWorkingSetSize();
      if (resident_bytes > budget || addition_bytes > budget - resident_bytes)
        return E_OUTOFMEMORY;
    }

    std::vector<WMT::Allocation> additions;
    for (const auto &entry : batch) {
      if (!entry.pageable->ResidencyReferenceCount()) {
        const auto &allocations = entry.pageable->ResidencyAllocations();
        additions.insert(additions.end(), allocations.begin(), allocations.end());
      }
    }
    if (!additions.empty()) {
      residency_set_.addAllocations(additions.data(), additions.size());
      residency_set_.commit();
    }
    for (const auto &entry : batch)
      entry.pageable->AddResidencyReference(entry.occurrences);
    return S_OK;
  }

  HRESULT
  EvictLocked(const std::vector<ResidencyBatchEntry> &batch) {
    std::vector<WMT::Allocation> removals;
    for (const auto &entry : batch) {
      if (!entry.pageable->CanRemoveResidencyReference(entry.occurrences))
        return E_INVALIDARG;
      if (entry.pageable->ResidencyReferenceCount() == entry.occurrences) {
        const auto &allocations = entry.pageable->ResidencyAllocations();
        removals.insert(removals.end(), allocations.begin(), allocations.end());
      }
    }
    if (!removals.empty()) {
      residency_set_.removeAllocations(removals.data(), removals.size());
      residency_set_.commit();
    }
    for (const auto &entry : batch)
      entry.pageable->RemoveResidencyReference(entry.occurrences);
    return S_OK;
  }

public:
  HRESULT RemovedReason() const override {
    return removal_reason_.load(std::memory_order_acquire);
  }

  void ReportRemoval(HRESULT reason) override {
    std::vector<std::weak_ptr<owned_graphics::FenceOrderEpoch>> orders;
    {
      std::lock_guard<std::mutex> lock(removal_mutex_);
      if (removal_reason_.load(std::memory_order_relaxed) != S_OK)
        return;
      removal_reason_.store(reason, std::memory_order_release);
      orders.swap(fence_orders_);
    }
    for (auto &weak : orders)
      if (auto order = weak.lock())
        order->remove();
  }

  bool RegisterFenceOrder(const std::shared_ptr<owned_graphics::FenceOrderEpoch> &order) override {
    {
      std::lock_guard<std::mutex> lock(removal_mutex_);
      if (removal_reason_.load(std::memory_order_relaxed) == S_OK) {
        try {
          if (fence_orders_.size() >= 64 && fence_orders_.size() == fence_orders_.capacity())
            std::erase_if(fence_orders_, [](const auto &weak) { return weak.expired(); });
          fence_orders_.push_back(order);
          return true;
        } catch (const std::bad_alloc &) {
          return false;
        }
      }
    }
    order->remove();
    return true;
  }

  MTLD3D12DeviceImpl(IMTLDXGIAdapter *adapter) :
      adapter_(adapter), command_library(adapter_->GetMTLDevice()),
      rtas_mirror_([this](uint64_t slot_count) { return CreateRTASMirrorPage(slot_count); }) {}

  ~MTLD3D12DeviceImpl() {}

  HRESULT
  Initialize() {
    WMT::Reference<WMT::Error> err;
    residency_set_ = adapter_->GetMTLDevice().newResidencySet(0, err);
    if (!residency_set_) {
      ERR("Failed to create MTLResidencySet: ", err.description().getUTF8String());
      return E_FAIL;
    }
    center_sample_positions_supported_ = GetMTLDevice().programmableSamplePositionsSupported();
    gpu_core_count_ = GetMTLDevice().gpuCoreCount();
    format_inspector_.Inspect(GetMTLDevice());

    {
      /* Zero-initialized (shared storage is zero filled by Metal) and resident for the
       * device lifetime, so every pipeline's 64 bit atomics can address it directly. */
      WMTBufferInfo lock_info{};
      lock_info.length = SM50_ATOMIC_LOCK_TABLE_ENTRIES * sizeof(uint32_t);
      lock_info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
      atomic_lock_table_ = GetMTLDevice().newBuffer(lock_info);
      if (!atomic_lock_table_)
        return E_OUTOFMEMORY;
      if (auto *words = lock_info.memory.get_accessible_or_null())
        memset(words, 0, lock_info.length);
      atomic_lock_table_address_ = lock_info.gpu_address;
      WMT::Buffer lock_buffer = atomic_lock_table_;
      residency_set_.addAllocations(&lock_buffer, 1);
      residency_set_.commit();
    }
    
    WMTDepthStencilInfo info{};
    info.depth_compare_function = WMTCompareFunctionAlways;
    default_depth_stencil_state = GetMTLDevice().newDepthStencilState(info);

    return S_OK;
  };

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  };

  bool
  SupportsCenterSamplePattern() const override {
    return center_sample_positions_supported_;
  }

  D3D_FEATURE_LEVEL
  GetFeatureLevel() {
    return kOwnedMaxFeatureLevel;
  };

  HRESULT
  GetAdapter(REFIID riid, void **ppAdapter) {
    return adapter_->QueryInterface(riid, ppAdapter);
  };

  UINT STDMETHODCALLTYPE
  GetNodeCount() {
    return 1; // FIXME
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IMTLD3D12DeviceExt)) {
      *ppvObject = static_cast<IMTLD3D12DeviceExt *>(this);
      AddRef();
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device) ||
        riid == __uuidof(ID3D12Device1) || riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5) || riid == __uuidof(ID3D12Device6) ||
        riid == __uuidof(ID3D12Device7) || riid == __uuidof(ID3D12Device8) || riid == __uuidof(ID3D12Device9) ||
        riid == __uuidof(ID3D12Device10) || riid == __uuidof(ID3D12Device11) || riid == __uuidof(ID3D12Device12) ||
        riid == __uuidof(ID3D12Device13) || riid == __uuidof(ID3D12Device14)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IDXGIDevice4))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12DeviceDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12Device1), riid)) {
      WARN("D3D12Device: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
    if (ppCommandQueue)
      *ppCommandQueue = nullptr;
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pDesc || pDesc->NodeMask > 1 ||
        (pDesc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
         pDesc->Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         pDesc->Type != D3D12_COMMAND_LIST_TYPE_COPY))
      return E_INVALIDARG;
    if (pDesc->Flags)
      WARN("CreateCommandQueue: flags ignored: ", pDesc->Flags);
    return dxmt::CreateCommandQueue(this, pDesc, riid, ppCommandQueue);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateCommandAllocator(this, Type, riid, ppCommandAllocator);
  };

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateGraphicsPipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateComputePipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateStateObject(const D3D12_STATE_OBJECT_DESC *pDesc, REFIID riid, void **ppStateObject) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateStateObject(this, pDesc, riid, ppStateObject);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator *pCommandAllocator,
      ID3D12PipelineState *pInitialPipelineState, REFIID riid, void **ppCommandList
  ) {
    if (ppCommandList)
      *ppCommandList = nullptr;
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pCommandAllocator || NodeMask > 1)
      return E_INVALIDARG;
    Com<ID3D12Device> allocator_device;
    if (FAILED(pCommandAllocator->GetDevice(__uuidof(ID3D12Device),
                                           reinterpret_cast<void **>(&allocator_device))) ||
        allocator_device.ptr() != static_cast<ID3D12Device *>(this))
      return E_INVALIDARG;
    auto allocator = static_cast<MTLD3D12CommandAllocator *>(pCommandAllocator);
    return allocator->CreateCommandList(NodeMask, Type, pInitialPipelineState, riid, ppCommandList);
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureData, UINT DataSize) {
    if (!pFeatureData)
      return E_INVALIDARG;
    auto metal = GetMTLDevice();
    switch (UINT(Feature)) {
    case kFeatureD3D12Options21: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS21)) return E_INVALIDARG;
      auto *out = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS21 *>(pFeatureData);
      *out = {D3D12_WORK_GRAPHS_TIER_1_0, D3D12_EXECUTE_INDIRECT_TIER_1_0, FALSE, FALSE};
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      out->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(pFeatureData);

      out->NumQualityLevels = 0;
      if (out->Flags != D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE &&
          out->Flags != D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return E_INVALIDARG;
      if (out->SampleCount == 0)
        return E_FAIL;
      if (out->Format == DXGI_FORMAT_UNKNOWN ||
          out->Flags == D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE ||
          !IsD3D12SampleCountSupported(this, out->SampleCount))
        return S_OK;

      MTL_DXGI_FORMAT_DESC format_desc;
      if (FAILED(MTLQueryDXGIFormat(metal, out->Format, format_desc)))
        return E_FAIL;
      out->NumQualityLevels = IsD3D12TextureSampleSupported(this, out->Format, format_desc,
          D3D12_RESOURCE_FLAG_NONE, out->SampleCount, true) ? 1 : 0;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(pFeatureData);
      switch (out->HighestVersion) {
      default:
        return E_INVALIDARG;
      case D3D_ROOT_SIGNATURE_VERSION_1:
        out->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1;
        break;
      case D3D_ROOT_SIGNATURE_VERSION_1_1:
        out->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        break;
      case kRootSignatureVersion_1_2:
        // Desc_1_2 serialize/deserialize/validation and D3D12_STATIC_SAMPLER_DESC1 flags
        // (UINT_BORDER_COLOR, NON_NORMALIZED_COORDINATES) are GPU verified by the bindless suite.
        out->HighestVersion = kRootSignatureVersion_1_2;
        break;
      }
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(pFeatureData);
      if (!out->NumFeatureLevels || !out->pFeatureLevelsRequested)
        return E_INVALIDARG;
      D3D_FEATURE_LEVEL max_level = {};
      for (unsigned i = 0; i < out->NumFeatureLevels; i++) {
        const auto requested = out->pFeatureLevelsRequested[i];
        if (requested >= D3D_FEATURE_LEVEL_11_0 && requested <= kOwnedMaxFeatureLevel)
          max_level = std::max(requested, max_level);
      }
      out->MaxSupportedFeatureLevel = max_level;
      return max_level ? S_OK : E_FAIL;
    }
    case D3D12_FEATURE_FORMAT_INFO:  {
       if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(pFeatureData);
      if (out->Format == DXGI_FORMAT_UNKNOWN || sampler_feedback::IsFeedbackFormat(out->Format)) {
        out->PlaneCount = 1;
        return S_OK;
      }
      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (FAILED(hr))
        return E_FAIL;

      out->PlaneCount = format_desc.PlanarCount;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(pFeatureData);
      out->MaxGPUVirtualAddressBitsPerProcess = 48;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return E_INVALIDARG;
      /* D3D contract: the caller passes the highest model it knows; the device answers
       * min(requested, supported), and rejects a request above anything it can name. */
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(pFeatureData);
      if (out->HighestShaderModel < D3D_SHADER_MODEL_5_1)
        return E_INVALIDARG;
      out->HighestShaderModel = std::min(out->HighestShaderModel, kOwnedReportedShaderModel);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(pFeatureData);
      out->DoublePrecisionFloatShaderOps = FALSE;
      // All 16 ops on every renderable UINT format (feature-level suite): native Metal logic op, or the exact
      // framebuffer-fetch PS epilogue when an RGB10A2_UINT target is bound (Metal saturates that format).
      out->OutputMergerLogicOp = TRUE;
      out->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
      /*
       * Tier 2 (tiled suite): reserved buffers and 2D textures over Metal 64KB placement sparse pages,
       * NULL tiles reading zero and dropping writes, residency status for every access, and min/max
       * filtering. Tier 3 is unreported: Metal's 3D sparse tile (e.g. 128x128x1 for RGBA8) is not the
       * D3D12 standard 3D tile (32x32x16), so volume tiles cannot be mapped individually.
       */
      out->TiledResourcesTier = GetMTLDevice().supportsPlacementSparse() ? D3D12_TILED_RESOURCES_TIER_2
                                                                         : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      // Tier 3: heap-sized unbounded CBV/SRV/UAV tables with no per-stage limits, unpopulated
      // table entries, and SM 6.6 ResourceDescriptorHeap/SamplerDescriptorHeap over
      // 1,000,000 / 2048 descriptor heaps (bindless suite).
      out->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
      out->PSSpecifiedStencilRefSupported = TRUE;
      out->TypedUAVLoadAdditionalFormats = TRUE;
      out->ROVsSupported = TRUE;
      out->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_1;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      out->StandardSwizzle64KBSupported = TRUE;
      out->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      // Row-major ALLOW_CROSS_ADAPTER 2D textures: linear Metal textures over shared section memory,
      // committed or placed in a SHARED_CROSS_ADAPTER heap (sharing suite).
      out->CrossAdapterRowMajorTextureSupported = TRUE;
      out->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
      out->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS13: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13 *>(pFeatureData);
      // None of these relaxations is implemented (copy pitch/alignment, inverted viewports,
      // cross-dimension copies, alpha blend factor).
      *out = {};
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS14: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS14))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS14 *>(pFeatureData);
      out->AdvancedTextureOpsSupported = FALSE;
      out->WriteableMSAATexturesSupported = FALSE;
      // OMSetFrontAndBackStencilRef and D3D12_DEPTH_STENCIL_DESC2 per-face masks.
      out->IndependentFrontAndBackStencilRefMaskSupported = kReportLayoutFeatures;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS15: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS15))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS15 *>(pFeatureData);
      out->TriangleFanSupported = FALSE;
      out->DynamicIndexBufferStripCutSupported = kReportLayoutFeatures;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS16: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(pFeatureData);
      // D3D12_HEAP_TYPE_GPU_UPLOAD: shared-storage, write-combined unified memory the GPU reads in place.
      out->GPUUploadHeapSupported = TRUE;
      out->DynamicDepthBiasSupported = kReportLayoutFeatures;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS17: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS17))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS17 *>(pFeatureData);
      // Heap samplers reject D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES (CreateSampler2).
      out->NonNormalizedCoordinateSamplersSupported = FALSE;
      out->ManualWriteTrackingResourceSupported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS18: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS18 *>(pFeatureData);
      // Begin/EndRenderPass are implemented (TIER_0 emulation, GPU verified by owned-d3d12-render-passes).
      out->RenderPassesValid = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(pFeatureData);
      out->DepthBoundsTestSupported = FALSE;
      out->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(pFeatureData);
      out->CastingFullyTypedFormatSupported = TRUE;
      // SV_Barycentrics / GetAttributeAtVertex lower to air.barycentric_coord and
      // vertex_value<T> (sm6 suite: barycentrics, barycentrics_indexed).
      out->BarycentricsSupported = TRUE;
      out->CopyQueueTimestampQueriesSupported = FALSE;
      // Draw-level looping with exact view locations (view-instancing suite); Tier 2/3 need instancing below
      // draw level.
      out->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_1;
      out->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAGS(
          D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY | D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(pFeatureData);
      // Apple simdgroups are always 32 lanes; [WaveSize(n != 32)] fails to compile.
      out->WaveOps = TRUE;
      out->WaveLaneCountMin = 32;
      out->WaveLaneCountMax = 32;
      // Each Apple GPU core runs 4 SIMD32 units (128 lanes); 0 when the IOKit core count is
      // unavailable, which D3D12 allows.
      out->TotalLaneCount = gpu_core_count_ * 128;
      // If CheckFeatureSupport succeeds this value will always be true.
      out->ExpandedComputeResourceStates = TRUE;
      // 64 bit integer arithmetic in every stage (sm6 suite: int64, graphics_int64). 64 bit
      // atomics are a separate SM 6.6 requirement and compile only in compute shaders.
      out->Int64ShaderOps = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(pFeatureData);
      // Mesh draws count AS/MS invocations, MS primitives and the clipper/PS stages exactly (PIPELINE_STATISTICS1);
      // an MS writes any SV_RenderTargetArrayIndex up to 2047. Both GPU-verified by the mesh-shader suite.
      out->MeshShaderPipelineStatsSupported = TRUE;
      out->MeshShaderSupportsFullRangeRenderTargetArrayIndex = TRUE;
      // Apple GPUs have no returning 64 bit texture atomics or threadgroup 64 bit atomics
      // (the Metal compiler rejects them); airconv refuses both at compile time.
      out->AtomicInt64OnTypedResourceSupported = FALSE;
      out->AtomicInt64OnGroupSharedSupported = FALSE;
      out->DerivativesInMeshAndAmplificationShadersSupported = FALSE;
      out->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11))
        return E_INVALIDARG;
      // Requires SM 6.6 64 bit atomics in every stage; they compile only in compute shaders.
      reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(pFeatureData)
          ->AtomicInt64OnDescriptorHeapResourceSupported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(pFeatureData);
      // Metal texture views cannot reinterpret block-compressed data as another BC or an
      // uncompressed format (MTLTexture newTextureViewWithPixelFormat: "not compatible"), which
      // the relaxed-casting contract requires. Format lists without those casts still work.
      out->RelaxedFormatCastingSupported = FALSE;
      out->EnhancedBarriersSupported = kReportLayoutFeatures;
      // MSPrimitives counts declared primitives minus those SV_CullPrimitive culls (dxil_mesh.cpp).
      out->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(pFeatureData);
      out->MSAA64KBAlignedTextureSupported = TRUE;
      // half/int16/uint16 arithmetic, buffer and stage I/O (sm6 suite: 16bit, packed, 16bit_io).
      out->Native16BitShaderOpsSupported = TRUE;
      // TODO(d3d12): revise when d3d12 shared resource is implemented
      out->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(pFeatureData);
      out->SRVOnlyTiledResourceTier3 = FALSE;
      // AS build/refit/copy/serialization, state-object extension, indirect dispatch and
      // RayQuery in every shader stage are verified by the raytracing GPU suite.
      out->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
      out->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(pFeatureData);
      // Wide outputs use captured replay to preserve all 128 scalars and clip/cull distances
      // beyond the native mesh varying limit; the mesh suite covers both execution paths.
      out->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
      out->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_1_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS6 *>(pFeatureData);
      *out = {};
      out->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_2;
      out->AdditionalShadingRatesSupported = TRUE;
      out->ShadingRateImageTileSize = 8;
      out->PerPrimitiveShadingRateSupportedWithViewportIndexing = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(pFeatureData);
      // CachedPSO/GetCachedBlob and ID3D12PipelineLibrary are implemented. Automatic OS-managed
      // in-process/disk caches are not.
      out->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY;
      return S_OK;
    }
    case D3D12_FEATURE_SERIALIZATION: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SERIALIZATION))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SERIALIZATION *>(pFeatureData);
      if (out->NodeIndex)
        return E_INVALIDARG;
      // OpenExistingHeapFromAddress/FileMapping heap serialization is not implemented.
      out->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureData);

      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        auto support = BuildD3D12FormatSupport(out->Format, {});
        out->Support1 = support.Support1;
        out->Support2 = support.Support2;
        return S_OK;
      }

      out->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      out->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

      if (sampler_feedback::IsFeedbackFormat(out->Format)) {
        // Opaque feedback maps: 2D, bound only through CreateSamplerFeedbackUnorderedAccessView.
        out->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        out->Support2 = D3D12_FORMAT_SUPPORT2_SAMPLER_FEEDBACK;
        return S_OK;
      }
      MTL_DXGI_FORMAT_DESC format_desc;
      if (FAILED(MTLQueryDXGIFormat(metal, out->Format, format_desc)))
        return E_INVALIDARG;

      const auto storage = resource_contract::GetEmulatedTextureStorage(out->Format);
      const auto physical = GetD3D12PhysicalPixelFormat(out->Format);
      auto capability = GetMTLPixelFormatCapability(
          physical == WMTPixelFormatInvalid ? format_desc.PixelFormat : physical);
      auto has_capability = [&](FormatCapability bit) {
        return any_bit_set(capability & bit);
      };

      const bool block_compressed = format_desc.Flag & MTL_DXGI_FORMAT_BC;
      const bool depth = format_desc.Flag & MTL_DXGI_FORMAT_DEPTH_PLANER;
      const bool stencil = format_desc.Flag & MTL_DXGI_FORMAT_STENCIL_PLANER;
      const auto native_texel_size = MTLGetTexelSize(format_desc.PixelFormat);
      const bool native_layout_compatible =
          block_compressed || depth || stencil ||
          (format_desc.BytesPerTexel && native_texel_size == format_desc.BytesPerTexel);
      const auto buffer_capability_mask = static_cast<FormatCapability>(
          FormatCapability::TextureBufferRead | FormatCapability::TextureBufferWrite |
          FormatCapability::TextureBufferReadWrite);
      const bool inspected_buffer_support = any_bit_set(capability & buffer_capability_mask);

      D3D12FormatSupportProperties properties{
          .mapped = format_desc.PixelFormat != WMTPixelFormatInvalid,
          .buffer_compatible = (storage == resource_contract::EmulatedTextureStorage::RGB32 &&
                                !(format_desc.Flag & MTL_DXGI_FORMAT_TYPELESS)) ||
              (storage == resource_contract::EmulatedTextureStorage::None &&
               !block_compressed && !depth && !stencil && native_layout_compatible && inspected_buffer_support),
          .texture_compatible = IsD3D12TextureFormatCompatible(out->Format, format_desc, capability, native_texel_size),
          .typeless = bool(format_desc.Flag & MTL_DXGI_FORMAT_TYPELESS),
          .block_compressed = block_compressed,
          .depth = depth,
          .stencil = stencil,
          .backbuffer = bool(format_desc.Flag & MTL_DXGI_FORMAT_BACKBUFFER),
          .vertex_attribute = format_desc.AttributeFormat != WMTAttributeFormatInvalid,
          .filter = IsPacked422(out->Format) ||
                    (out->Format == DXGI_FORMAT_R32G32B32_FLOAT && has_capability(FormatCapability::Filter)) ||
                    (storage == resource_contract::EmulatedTextureStorage::None && has_capability(FormatCapability::Filter)),
          // Metal does not permit shader writes through an sRGB texture view;
          // its filter/color capability does not imply a D3D12 typed UAV.
          .write = storage == resource_contract::EmulatedTextureStorage::None &&
                   has_capability(FormatCapability::Write) && !Is_sRGBVariant(format_desc.PixelFormat),
          .color = !IsPacked422(out->Format) && has_capability(FormatCapability::Color),
          .blend = (storage == resource_contract::EmulatedTextureStorage::None ||
                    out->Format == DXGI_FORMAT_R32G32B32_FLOAT) && has_capability(FormatCapability::Blend),
          .multisample = !IsPacked422(out->Format) &&
                         (IsD3D12MSAAPixelFormatSupported(this,
                              physical == WMTPixelFormatInvalid ? format_desc.PixelFormat : physical) ||
                          (IsD3D12DepthStencilPlaneView(out->Format) &&
                           IsD3D12MSAAPixelFormatSupported(this, WMTPixelFormatDepth32Float_Stencil8))) &&
                         (IsD3D12SampleCountSupported(this, 2) ||
                          IsD3D12SampleCountSupported(this, 4) ||
                          IsD3D12SampleCountSupported(this, 8)),
          .resolve = storage == resource_contract::EmulatedTextureStorage::RGB32 ?
                       out->Format == DXGI_FORMAT_R32G32B32_FLOAT : has_capability(FormatCapability::Resolve),
          .atomic = has_capability(FormatCapability::Atomic),
          .typed_uav_load = storage == resource_contract::EmulatedTextureStorage::None &&
                            (has_capability(FormatCapability::TextureBufferRead) ||
                             has_capability(FormatCapability::TextureBufferReadWrite)),
          .typed_uav_store = storage == resource_contract::EmulatedTextureStorage::None &&
                             (has_capability(FormatCapability::TextureBufferWrite) ||
                              has_capability(FormatCapability::TextureBufferReadWrite)),
      };

      auto support = BuildD3D12FormatSupport(out->Format, properties);
      out->Support1 = support.Support1;
      out->Support2 = support.Support2;
      /* TILED: the reserved 2D texture path accepts the format (MTLD3D12Texture::InitializeReserved). */
      if (properties.texture_compatible && !properties.typeless && format_desc.PlanarCount == 1 &&
          storage == resource_contract::EmulatedTextureStorage::None && metal.supportsPlacementSparse()) {
        const uint32_t unit_bytes = block_compressed ? format_desc.BlockSize : format_desc.BytesPerTexel;
        const auto tile =
            metal.sparseTileSize(WMTTextureType2D, format_desc.PixelFormat, 1, WMTSparsePageSize64);
        if (unit_bytes && (block_compressed || native_texel_size == unit_bytes) &&
            ReservedResource::MatchesStandardShape2D(tile, block_compressed ? 4 : 1, block_compressed ? 4 : 1,
                                                     unit_bytes))
          out->Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(out->Support2 | D3D12_FORMAT_SUPPORT2_TILED);
      }
      return S_OK;
    }
    default:
      break;
    }
    ERR("CheckFeatureSupport: unhandled feature ", Feature);
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap) {
    InitReturnPtr(ppDescriptorHeap);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateDescriptorHeap(this, pDesc, riid, ppDescriptorHeap);
  };

  UINT STDMETHODCALLTYPE
  GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) {
    switch (DescriptorHeapType) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return 32;
    default:
      break;
    }
    return 0;
  };

  HRESULT STDMETHODCALLTYPE
  CreateRootSignature(
      UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid, void **ppRootSignature
  ) {
    InitReturnPtr(ppRootSignature);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateRootSignature(this, NodeMask, pBytecode, BytecodeLength, riid, ppRootSignature);
  };

  void STDMETHODCALLTYPE
  CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
    if (pDesc)
      Heap->AddConstantBufferView(Index, pDesc->BufferLocation, pDesc->SizeInBytes);
    else
      Heap->AddConstantBufferView(Index, 0, 0);
  };

  void STDMETHODCALLTYPE
  CreateShaderResourceView(
      ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      /**
       * A raytracing acceleration structure view is the one SRV that names its
       * structure by GPU virtual address instead of by resource, so it is
       * registered as its own descriptor kind. The address is the whole payload:
       * the shader loads the structure's 64 byte header from it and takes the native
       * structure from the header's resource id.
       */
      if (pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
        Heap->AddAccelerationStructure(Index, pDesc->RaytracingAccelerationStructure.Location);
        return;
      }
      Heap->AddShaderResourceView(Index, pDesc);
      return;
    }
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
    Heap->AddShaderResourceView(Index, static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC *>(nullptr));
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateShaderResourceView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pResource, ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      Heap->AddUnorderedAccessView(Index, pDesc);
      return;
    }
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
    Heap->AddUnorderedAccessView(Index, static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC *>(nullptr));
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateUnorderedAccessView(pCounter, pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateRenderTargetView(
      ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
    Heap->AddRenderTarget(Index, nullptr);
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateRenderTargetView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateDepthStencilView(
      ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
    Heap->AddRenderTarget(Index, nullptr);
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateDepthStencilView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateSampler(const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetSamplerDescriptorHeap(this, Descriptor);
    Heap->AddSampler(Index, pDesc);
  };

  void STDMETHODCALLTYPE
  CopyDescriptors(
      UINT DstDescriptorRangeCount, const D3D12_CPU_DESCRIPTOR_HANDLE *DstDescriptorRangeOffsets,
      const UINT *DstDescriptorRangeSizes, UINT SrcDescriptorRangeCount,
      const D3D12_CPU_DESCRIPTOR_HANDLE *SrcDescriptorRangeOffsets, const UINT *SrcDescriptorRangeSizes,
      D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    unsigned int dst_range_idx, dst_idx, src_range_idx, src_idx;
    unsigned int dst_range_size, src_range_size, copy_count;

    dst_range_idx = dst_idx = 0;
    src_range_idx = src_idx = 0;
    while (dst_range_idx < DstDescriptorRangeCount && src_range_idx < SrcDescriptorRangeCount) {
      dst_range_size = DstDescriptorRangeSizes ? DstDescriptorRangeSizes[dst_range_idx] : 1;
      src_range_size = SrcDescriptorRangeSizes ? SrcDescriptorRangeSizes[src_range_idx] : 1;

      copy_count = std::min(dst_range_size - dst_idx, src_range_size - src_idx);

      switch (DescriptorHeapType) {
      case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
        auto [DstRangeHeap, DstRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }

      case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
        auto [DstRangeHeap, DstRangeIndex] = GetSamplerDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetSamplerDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
      case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
        auto [DstRangeHeap, DstRangeIndex] = GetRenderTargetHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetRenderTargetHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      default:
        return;
      }

      dst_idx += copy_count;
      src_idx += copy_count;

      if (dst_idx >= dst_range_size) {
        ++dst_range_idx;
        dst_idx = 0;
      }
      if (src_idx >= src_range_size) {
        ++src_range_idx;
        src_idx = 0;
      }
    }
  };

  void STDMETHODCALLTYPE
  CopyDescriptorsSimple(
      UINT DescriptorCount, const D3D12_CPU_DESCRIPTOR_HANDLE DstDescriptorRangeOffset,
      const D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeOffset, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    CopyDescriptors(
        1, &DstDescriptorRangeOffset, &DescriptorCount, 1, &SrcDescriptorRangeOffset, &DescriptorCount,
        DescriptorHeapType
    );
  };

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount, const D3D12_RESOURCE_DESC *pDescs
  ) {
    return GetResourceAllocationInfo1(__ret, VisibleMask, ResourceDestCount, pDescs, nullptr);
  };

  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT NodeMask, D3D12_HEAP_TYPE HeapType) {
    __ret->Type = D3D12_HEAP_TYPE_CUSTOM;
    __ret->CreationNodeMask = 1;
    __ret->VisibleNodeMask = 1;
    switch (HeapType) {
    case D3D12_HEAP_TYPE_DEFAULT:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
      __ret->MemoryPoolPreference = advertise_numa_ ? D3D12_MEMORY_POOL_L1 : D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_UPLOAD:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_READBACK:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    case kHeapTypeGpuUpload:
      // UMA (not cache-coherent): the same as UPLOAD, per the GPU upload heaps spec.
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    default:
      E_INVALIDARG;
    }

    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(pHeapProps, HeapFlags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(this, pDesc, pHeapProps);
    if (FAILED(hr))
      return hr;
    if (!IsResourceDescriptorCompatible(this, *pDesc))
      return E_INVALIDARG;
    hr = ValidateResourceStates(InitialState, pHeapProps, pDesc);
    if (FAILED(hr))
      return hr;
    hr = ValidateOptimizedClearValue(pDesc, OptimizedClearValue);
    if (FAILED(hr))
      return hr;
    // ALLOW_CROSS_ADAPTER resources live only in SHARED_CROSS_ADAPTER heaps.
    if ((pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) && !(HeapFlags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER))
      return E_INVALIDARG;
    if (!ppResource) {
      if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_RESOURCE_ALLOCATION_INFO info{};
        GetResourceAllocationInfo(&info, 0, 1, pDesc);
        if (info.SizeInBytes == UINT64_MAX)
          return E_INVALIDARG;
      }
      return S_FALSE;
    }
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return CreateCommittedTexture(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreateCommittedBuffer(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    default:
      break;
    }
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    InitReturnPtr(ppHeap);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pDesc)
      return E_INVALIDARG;
    if (pDesc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return E_INVALIDARG;
    HRESULT hr = ValidateHeapProperties(&pDesc->Properties, pDesc->Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    if (!pDesc->SizeInBytes ||
        (pDesc->Alignment && pDesc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
         pDesc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT))
      return E_INVALIDARG;
    if (!ppHeap)
      return S_FALSE;
    return dxmt::CreateHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource(
      ID3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pHeap)
      return E_INVALIDARG;
    Com<ID3D12Device> heap_device;
    if (FAILED(pHeap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&heap_device))) ||
        heap_device.ptr() != static_cast<ID3D12Device *>(this))
      return E_INVALIDARG;
    auto d3d12heap = dynamic_cast<MTLD3D12Heap *>(pHeap);
    if (!d3d12heap)
      return E_INVALIDARG;
    auto heap_desc = d3d12heap->GetDesc();
    HRESULT hr = S_OK;
    // Existing-memory heaps carry the D3D12-defined CUSTOM WRITE_BACK + SHARED_CROSS_ADAPTER
    // combination, which CreateHeap itself rejects; their properties were fixed at open.
    if (!d3d12heap->external_memory || d3d12heap->share)
      hr = ValidateHeapProperties(&heap_desc.Properties, heap_desc.Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(this, pDesc, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    if (!IsResourceDescriptorCompatible(this, *pDesc))
      return E_INVALIDARG;
    hr = ValidateResourceStates(InitialState, &heap_desc.Properties, pDesc);
    if (FAILED(hr))
      return hr;
    hr = ValidateOptimizedClearValue(pDesc, OptimizedClearValue);
    if (FAILED(hr))
      return hr;
    if (!ppResource) {
      if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)
          return E_INVALIDARG;
      } else if (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES)
          return E_INVALIDARG;
      } else if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) {
        return E_INVALIDARG;
      }
      D3D12_RESOURCE_ALLOCATION_INFO info{};
      GetResourceAllocationInfo(&info, 0, 1, pDesc);
      if (info.SizeInBytes == UINT64_MAX ||
          !resource_contract::PlacementFits(Offset, info.SizeInBytes, heap_desc.SizeInBytes, info.Alignment, 0))
        return E_INVALIDARG;
      return S_FALSE;
    }
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return CreatePlacedTexture(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreatePlacedBuffer(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    default:
      break;
    }
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **resource
  ) {
    return CreateReservedResource1(pDesc, InitialState, OptimizedClearValue, nullptr, riid, resource);
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle
  ) {
    if (!handle)
      return E_INVALIDARG;
    *handle = nullptr;
    if (!object)
      return E_INVALIDARG;
    std::shared_ptr<sharing::SharedObject> share;
    if (Com<ID3D12Heap> heap; SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap)))) {
      if (auto *native = dynamic_cast<MTLD3D12Heap *>(heap.ptr()))
        share = native->share;
    } else if (Com<ID3D12Resource> resource; SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
      if (auto *native = dynamic_cast<MTLD3D12Resource *>(resource.ptr()))
        share = native->share;
    } else if (Com<ID3D12Fence> fence; SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&fence)))) {
      if (auto *native = dynamic_cast<MTLD3D12Fence *>(fence.ptr()))
        share = native->share;
    }
    // Only objects created with a SHARED heap/fence flag own a share.
    if (!share)
      return E_INVALIDARG;
    try {
      return share->createHandle(attributes, access, name, handle);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandle(HANDLE handle, REFIID riid, void **object) {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    try {
      std::shared_ptr<sharing::SharedObject> share;
      HRESULT hr = sharing::Open(handle, share);
      if (FAILED(hr))
        return hr;
      // Reject the interface before building anything, so a wrong riid never registers residency.
      const bool heap = riid == __uuidof(ID3D12Heap) || riid == __uuidof(ID3D12Heap1);
      const bool resource = riid == __uuidof(ID3D12Resource) || riid == __uuidof(ID3D12Resource1) ||
                            riid == __uuidof(ID3D12Resource2);
      const bool fence = riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Fence1);
      const bool common = riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
                          riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable);
      switch (share->kind()) {
      case sharing::Kind::Heap:
        return heap || common ? OpenSharedHeap(this, std::move(share), riid, object) : E_NOINTERFACE;
      case sharing::Kind::Buffer:
        if (!(resource || common))
          return E_NOINTERFACE;
        return share->header().resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                   ? OpenSharedBuffer(this, std::move(share), riid, object)
                   : OpenSharedTexture(this, std::move(share), riid, object);
      case sharing::Kind::Texture:
        return resource || common ? OpenSharedTexture(this, std::move(share), riid, object) : E_NOINTERFACE;
      case sharing::Kind::Fence:
        return fence || common ? OpenSharedFence(this, std::move(share), riid, object) : E_NOINTERFACE;
      }
      return E_INVALIDARG;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
    return sharing::OpenByName(name, access, handle);
  };

  HRESULT STDMETHODCALLTYPE
  MakeResident(UINT ObjectCount, ID3D12Pageable *const *objects) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      if (RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      std::vector<ResidencyBatchEntry> batch;
      HRESULT hr = ResolveResidencyBatch(ObjectCount, objects, batch);
      return FAILED(hr) ? hr : MakeResidentLocked(batch);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  HRESULT STDMETHODCALLTYPE
  Evict(UINT ObjectCount, ID3D12Pageable *const *objects) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      if (RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      std::vector<ResidencyBatchEntry> batch;
      HRESULT hr = ResolveResidencyBatch(ObjectCount, objects, batch);
      return FAILED(hr) ? hr : EvictLocked(batch);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  HRESULT STDMETHODCALLTYPE
  CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

  HRESULT STDMETHODCALLTYPE
  GetDeviceRemovedReason() {
    return RemovedReason();
  };

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *pDesc, UINT FirstSubresource, UINT SubresourceCount, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    auto invalidate = [&] {
      for (UINT i = 0; i < SubresourceCount; i++) {
        if (pLayouts)
          pLayouts[i] = {UINT64_MAX, {static_cast<DXGI_FORMAT>(UINT_MAX), UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX}};
        if (pNumRows)
          pNumRows[i] = UINT_MAX;
        if (pRowSizeInBytes)
          pRowSizeInBytes[i] = UINT64_MAX;
      }
      if (pTotalBytes)
        *pTotalBytes = UINT64_MAX;
    };

    if (!SubresourceCount) {
      if (pTotalBytes)
        *pTotalBytes = 0;
      return;
    }
    if (!pDesc) {
      invalidate();
      return;
    }

    D3D12_RESOURCE_DESC desc = *pDesc;
    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    /* A reserved texture's copy footprints are those of the same texture in the default layout. */
    if (desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(ValidateResourceDescs(this, &desc, &default_heap))) {
      invalidate();
      return;
    }

    UINT plane_count = 1;
    DXGI_FORMAT plane_formats[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    UINT plane_bytes[2] = {1, 0};
    UINT block_width = 1, block_height = 1;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      /* DXGI requires even packed422 widths; never ask Metal about an invalid
       * native texture descriptor just to answer a layout query. */
      if (IsPacked422(desc.Format) && (desc.Width & 1)) {
        invalidate();
        return;
      }
      WMTTextureInfo texture_info{};
      MTL_DXGI_FORMAT_DESC format{};
      if (FAILED(PopulateWMTTextureInfo(GetMTLDevice(), texture_info, desc)) ||
          FAILED(MTLQueryDXGIFormat(GetMTLDevice(), desc.Format, format))) {
        invalidate();
        return;
      }
      if (!desc.MipLevels)
        desc.MipLevels = texture_info.mipmap_level_count;
      if (format.Flag & MTL_DXGI_FORMAT_BC)
        block_width = block_height = 4;
      else if (IsPacked422(desc.Format))
        block_width = 2;
      plane_count = format.PlanarCount;
      if (!plane_count || plane_count > 2) {
        invalidate();
        return;
      }
      if (plane_count == 2) {
        plane_formats[0] = DXGI_FORMAT_R32_TYPELESS;
        plane_formats[1] = DXGI_FORMAT_R8_TYPELESS;
        plane_bytes[0] = 4;
        plane_bytes[1] = 1;
      } else {
        plane_formats[0] = desc.Format;
        plane_bytes[0] = format.BytesPerTexel;
        if (!plane_bytes[0]) {
          invalidate();
          return;
        }
      }
    }

    uint64_t per_plane = desc.MipLevels;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        !resource_contract::CheckedMul(per_plane, desc.DepthOrArraySize, per_plane)) {
      invalidate();
      return;
    }
    uint64_t subresource_count = 0;
    if (!resource_contract::CheckedMul(per_plane, plane_count, subresource_count) ||
        FirstSubresource >= subresource_count || SubresourceCount > subresource_count - FirstSubresource) {
      invalidate();
      return;
    }

    uint64_t offset = 0;
    if (!resource_contract::CheckedAlign(BaseOffset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset)) {
      invalidate();
      return;
    }
    for (UINT i = 0; i < SubresourceCount; i++) {
      UINT mip = 0, plane = 0;
      DecomposeSubresource(desc, FirstSubresource + i, &mip, nullptr, &plane);
      const auto extent = GetResourceExtent(desc, mip);
      uint64_t padded_width = 0, padded_height = 0;
      if (!resource_contract::CheckedAlign(extent.right, block_width, padded_width) ||
          !resource_contract::CheckedAlign(extent.bottom, block_height, padded_height)) {
        invalidate();
        return;
      }
      const uint64_t rows = padded_height / block_height;
      uint64_t row_size = 0, row_pitch = 0;
      if (!resource_contract::CheckedMul(padded_width / block_width, plane_bytes[plane], row_size) ||
          !resource_contract::CheckedAlign(row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, row_pitch) ||
          row_pitch > UINT_MAX || padded_width > UINT_MAX || padded_height > UINT_MAX || rows > UINT_MAX) {
        invalidate();
        return;
      }
      uint64_t slice_pitch = 0, subresource_size = 0, next_offset = 0;
      if (!resource_contract::CheckedMul(row_pitch, rows, slice_pitch) ||
          !resource_contract::CheckedMul(slice_pitch, extent.back - 1, subresource_size) ||
          !resource_contract::CheckedMul(row_pitch, rows - 1, slice_pitch) ||
          !resource_contract::CheckedAdd(subresource_size, slice_pitch, subresource_size) ||
          !resource_contract::CheckedAdd(subresource_size, row_size, subresource_size) ||
          !resource_contract::CheckedAdd(offset, subresource_size, next_offset)) {
        invalidate();
        return;
      }
      if (pLayouts)
        pLayouts[i] = {offset, {plane_formats[plane], static_cast<UINT>(padded_width),
                               static_cast<UINT>(padded_height), extent.back, static_cast<UINT>(row_pitch)}};
      if (pNumRows)
        pNumRows[i] = static_cast<UINT>(rows);
      if (pRowSizeInBytes)
        pRowSizeInBytes[i] = row_size;
      if (i + 1 != SubresourceCount &&
          !resource_contract::CheckedAlign(next_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset)) {
        invalidate();
        return;
      }
      if (i + 1 == SubresourceCount) {
        if (next_offset < BaseOffset) {
          invalidate();
          return;
        }
        if (pTotalBytes)
          *pTotalBytes = next_offset - BaseOffset;
      }
    }
  };

  HRESULT STDMETHODCALLTYPE
  CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateQueryHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  SetStablePowerState(WINBOOL Enable) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandSignature(
      const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid,
      void **ppCommandSignature
  ) {
    InitReturnPtr(ppCommandSignature);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateCommandSignature(this, pDesc, pRootSignature, riid, ppCommandSignature);
  };

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *pResource, UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo,
      D3D12_TILE_SHAPE *StandardTileShape, UINT *SubresourceTilingCount, UINT FirstSubresourceTiling,
      D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (TotalTileCount)
      *TotalTileCount = 0;
    if (PackedMipInfo)
      *PackedMipInfo = {};
    if (StandardTileShape)
      *StandardTileShape = {};
    if (!pResource) {
      if (SubresourceTilingCount)
        *SubresourceTilingCount = 0;
      return;
    }
    Com<ID3D12Device> resource_device;
    if (FAILED(pResource->GetDevice(__uuidof(ID3D12Device),
                                    reinterpret_cast<void **>(&resource_device))) ||
        resource_device.ptr() != static_cast<ID3D12Device *>(this)) {
      if (SubresourceTilingCount)
        *SubresourceTilingCount = 0;
      return;
    }
    auto *resource = static_cast<MTLD3D12Resource *>(pResource);
    if (!resource->swizzle_layout && !resource->reserved) {
      if (SubresourceTilingCount)
        *SubresourceTilingCount = 0;
      return;
    }
    resource->GetResourceTiling(TotalTileCount, PackedMipInfo, StandardTileShape,
                                SubresourceTilingCount, FirstSubresourceTiling, SubresourceTilings);
  };

  LUID *STDMETHODCALLTYPE
  GetAdapterLuid(LUID *ret) {
    *ret = std::bit_cast<LUID>(__builtin_bswap64(adapter_->GetMTLDevice().registryID()));
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  CreatePipelineLibrary(const void *blob, SIZE_T blob_size, REFIID iid, void **lib) {
    return dxmt::CreatePipelineLibrary(this, blob, blob_size, iid, lib);
  };

  HRESULT STDMETHODCALLTYPE
  SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *pFences, const UINT64 *pValues, UINT FenceCount, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
      HANDLE hEvent
  ) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pFences || !pValues || !FenceCount ||
        (Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL && Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return E_INVALIDARG;
    try {
      std::vector<Rc<Fence>> fences;
      std::vector<FenceWaitValue> values;
      fences.reserve(FenceCount);
      values.reserve(FenceCount);
      for (UINT i = 0; i < FenceCount; ++i) {
        if (!pFences[i])
          return E_INVALIDARG;
        // Allow other owned devices as well. The private device interface
        // identifies our implementation before accessing its native fence.
        Com<IMTLD3D12DeviceExt> owner;
        if (FAILED(pFences[i]->GetDevice(__uuidof(IMTLD3D12DeviceExt), reinterpret_cast<void **>(&owner))))
          return E_INVALIDARG;
        fences.push_back(static_cast<MTLD3D12Fence *>(pFences[i])->fence);
      }
      for (UINT i = 0; i < FenceCount; ++i)
        values.push_back(fences[i]->prepareWaitValue(pValues[i]));
      HANDLE event = hEvent;
      if (!event) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
          return HRESULT_FROM_WIN32(GetLastError());
      }
      HRESULT hr = event_listener.setEventOnValues(
          values.data(), FenceCount, Flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event
      ) ? S_OK : E_OUTOFMEMORY;
      if (!hEvent) {
        if (SUCCEEDED(hr) && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
          hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(event);
        if (SUCCEEDED(hr) && RemovedReason() != S_OK)
          hr = DXGI_ERROR_DEVICE_REMOVED;
      }
      return hr;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  };

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(
      UINT ObjectCount, ID3D12Pageable *const *pObjects, const D3D12_RESIDENCY_PRIORITY *pPriorities
  ) {
    if (ObjectCount && !pPriorities)
      return E_INVALIDARG;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      std::vector<ResidencyBatchEntry> batch;
      HRESULT hr = ResolveResidencyBatch(ObjectCount, pObjects, batch);
      if (FAILED(hr))
        return hr;
      for (UINT i = 0; i < ObjectCount; ++i) {
        if (pPriorities[i] < D3D12_RESIDENCY_PRIORITY_MINIMUM ||
            pPriorities[i] > D3D12_RESIDENCY_PRIORITY_MAXIMUM)
          return E_INVALIDARG;
      }
      for (UINT i = 0; i < ObjectCount; ++i)
        dynamic_cast<MTLD3D12PageableResidency *>(pObjects[i])->SetResidencyPriorityHint(pPriorities[i]);
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    InitReturnPtr(ppPipelineState);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    PipelineStateStream stream;
    if (HRESULT hr = ParsePipelineStateStream(this, pDesc, stream); FAILED(hr))
      return hr;
    if (stream.compute)
      return CreateComputePipelineState(&stream.compute_desc, riid, ppPipelineState);
    return dxmt::CreateGraphicsPipelineState(this, &stream.graphics, riid, ppPipelineState,
                                             &stream.graphics_extras);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress(const void *pAddress, REFIID riid, void **ppHeap) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return CreateHeapFromHostMemory(this, const_cast<void *>(pAddress), 0, riid, ppHeap);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void **ppHeap) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return CreateHeapFromFileMapping(this, hFileMapping, riid, ppHeap);
  }

  HRESULT STDMETHODCALLTYPE
  EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFence,
      UINT64 FenceValue
  ) {
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if ((Flags != D3D12_RESIDENCY_FLAG_NONE && Flags != D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET) || !pFence)
      return E_INVALIDARG;
    Com<ID3D12Device> fence_device;
    if (FAILED(pFence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&fence_device))) ||
        fence_device.ptr() != static_cast<ID3D12Device *>(this))
      return E_INVALIDARG;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      if (RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      std::vector<ResidencyBatchEntry> batch;
      HRESULT hr = ResolveResidencyBatch(NumObjects, ppObjects, batch);
      if (FAILED(hr) || FAILED(hr = MakeResidentLocked(batch, Flags == D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET)))
        return hr;
      /* Metal residency-set mutation and commit are synchronous.  Signal only after the
       * completed operation, while still serialized with other enqueue calls. */
      return pFence->Signal(FenceValue);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList1(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void **ppCommandList
  ) {
    InitReturnPtr(ppCommandList);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (NodeMask > 1 || Flags != D3D12_COMMAND_LIST_FLAG_NONE)
      return E_INVALIDARG;
    return dxmt::CreateClosedCommandList(this, Type, riid, ppCommandList);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *pDesc, REFIID riid, void **ppSession) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource1(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppResource
  ) {
    if (pSession) {
      InitReturnPtr(ppResource);
      return E_NOTIMPL;
    }
    return CreateCommittedResource(
        pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
    );
  }

  HRESULT STDMETHODCALLTYPE
  CreateHeap1(const D3D12_HEAP_DESC *pDesc, ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppHeap) {
    if (pSession) {
      InitReturnPtr(ppHeap);
      return E_NOTIMPL;
    }
    return CreateHeap(pDesc, riid, ppHeap);
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource1(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, ID3D12ProtectedResourceSession *pSession, REFIID riid,
      void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    /* No protected sessions are created by this device, so none can be passed in. */
    if (!pDesc || pSession || !GetMTLDevice().supportsPlacementSparse())
      return E_INVALIDARG;
    /* Sampler feedback maps cannot be reserved resources (SamplerFeedback spec, resource properties). */
    if (sampler_feedback::IsFeedbackFormat(pDesc->Format)) {
      ERR("CreateReservedResource: a sampler feedback map cannot be a reserved resource");
      return E_INVALIDARG;
    }
    /*
     * Reserved resources live on no heap; they get the default heap's rules. Textures must use
     * 64KB_UNDEFINED_SWIZZLE, which the common validator does not know, so it sees UNKNOWN instead.
     */
    const D3D12_HEAP_PROPERTIES heap_props{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                           D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC common = *pDesc;
    if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (pDesc->Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE)
        return E_INVALIDARG;
      common.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    HRESULT hr = ValidateResourceDescs(this, &common, &heap_props);
    if (FAILED(hr))
      return hr;
    if (!IsResourceDescriptorCompatible(this, common))
      return E_INVALIDARG;
    hr = ValidateResourceStates(InitialState, &heap_props, &common);
    if (FAILED(hr))
      return hr;
    hr = ValidateOptimizedClearValue(&common, OptimizedClearValue);
    if (FAILED(hr))
      return hr;
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return CreateReservedBuffer(this, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    return CreateReservedTexture(this, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo1(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount,
      const D3D12_RESOURCE_DESC *pDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    bool has_msaa = false;
    auto invalid = [&] {
      __ret->SizeInBytes = UINT64_MAX;
      __ret->Alignment = has_msaa ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                  : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      return __ret;
    };
    __ret->SizeInBytes = 0;
    __ret->Alignment = 0;
    if (VisibleMask > 1)
      return invalid();
    if (!ResourceDestCount)
      return __ret;
    if (!pDescs)
      return invalid();
    __ret->Alignment = 1;

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    for (UINT i = 0; i < ResourceDestCount; i++) {
      const auto &desc = pDescs[i];
      has_msaa |= desc.SampleDesc.Count > 1;
      if (FAILED(ValidateResourceDescs(this, &desc, &default_heap)))
        return invalid();
      if (!IsResourceDescriptorCompatible(this, desc))
        return invalid();

      D3D12_RESOURCE_ALLOCATION_INFO1 info{};
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const auto native = GetMTLDevice().heapBufferSizeAndAlign(desc.Width, {});
        info.SizeInBytes = native.size;
        info.Alignment = std::max<uint64_t>(native.align, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
      } else {
        WMTTextureInfo texture_info{};
        if (FAILED(PopulateWMTTextureInfo(GetMTLDevice(), texture_info, desc)))
          return invalid();
        const uint64_t requested = desc.Alignment ? desc.Alignment
            : desc.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
          // Linear rows pitched to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (see InitializeRowMajor).
          MTL_DXGI_FORMAT_DESC format{};
          if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), desc.Format, format)) || !format.BytesPerTexel)
            return invalid();
          const uint64_t pitch = (desc.Width * format.BytesPerTexel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                                 ~uint64_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
          info.SizeInBytes = pitch * desc.Height;
          info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        } else if (desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
          auto layout = MakeStandardSwizzleLayout(this, desc);
          if (!layout)
            return invalid();
          info.SizeInBytes = layout->size_bytes;
          info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        } else {
          const auto native = GetMTLDevice().heapTextureSizeAndAlign(texture_info);
          info.SizeInBytes = native.size;
          info.Alignment = std::max<uint64_t>(native.align, requested);
        }
      }
      if (!info.Alignment || !resource_contract::CheckedAlign(info.SizeInBytes, info.Alignment, info.SizeInBytes) ||
          !resource_contract::CheckedAlign(__ret->SizeInBytes, info.Alignment, info.Offset) ||
          !resource_contract::CheckedAdd(info.Offset, info.SizeInBytes, __ret->SizeInBytes))
        return invalid();
      __ret->Alignment = std::max(__ret->Alignment, info.Alignment);
      if (pAllocationInfos)
        pAllocationInfos[i] = info;
    }
    if (!resource_contract::CheckedAlign(__ret->SizeInBytes, __ret->Alignment, __ret->SizeInBytes))
      return invalid();
    return __ret;
  }

  /* ---- ID3D12Device6 ---- */

  /*
   * This device runs no background optimization or profiling work, so every mode is already in
   * effect and there are never outstanding measurements: commits complete immediately.
   */
  HRESULT STDMETHODCALLTYPE
  SetBackgroundProcessingMode(
      D3D12_BACKGROUND_PROCESSING_MODE Mode, D3D12_MEASUREMENTS_ACTION MeasurementsAction, HANDLE hEventToSignalUponCompletion,
      WINBOOL *pbFurtherMeasurementsDesired
  ) {
    if (pbFurtherMeasurementsDesired)
      *pbFurtherMeasurementsDesired = FALSE;
    if (UINT(Mode) > D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM ||
        UINT(MeasurementsAction) > D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS)
      return E_INVALIDARG;
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (hEventToSignalUponCompletion && !SetEvent(hEventToSignalUponCompletion))
      return E_INVALIDARG;
    return S_OK;
  }

  /* ---- ID3D12Device7 ---- */

  HRESULT STDMETHODCALLTYPE
  AddToStateObject(
      const D3D12_STATE_OBJECT_DESC *pAddition, ID3D12StateObject *pStateObjectToGrowFrom, REFIID riid,
      void **ppNewStateObject
  ) {
    InitReturnPtr(ppNewStateObject);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::AddToStateObject(this, pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession1(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *pDesc, REFIID riid, void **ppSession
  ) {
    InitReturnPtr(ppSession);
    return E_NOTIMPL;
  }

  /* ---- ID3D12Device8 ---- */

  /* D3D12_RESOURCE_DESC1 of a non-feedback resource (a zero mip region) is a D3D12_RESOURCE_DESC. */
  static HRESULT
  NarrowResourceDesc(const D3D12_RESOURCE_DESC1 *in, D3D12_RESOURCE_DESC &out) {
    if (!in)
      return E_INVALIDARG;
    if (sampler_feedback::IsFeedbackFormat(in->Format) || in->SamplerFeedbackMipRegion.Width ||
        in->SamplerFeedbackMipRegion.Height || in->SamplerFeedbackMipRegion.Depth)
      return E_INVALIDARG;
    out = {in->Dimension, in->Alignment, in->Width,  in->Height,     in->DepthOrArraySize,
           in->MipLevels, in->Format,    in->SampleDesc, in->Layout, in->Flags};
    return S_OK;
  }

  /*
   * Validates the common creation contract of a sampler feedback map (MakeLayout validates the
   * description): a GPU-only heap, no clear value, a state from the spec's permitted set.
   */
  static HRESULT
  ValidateSamplerFeedbackMap(const D3D12_RESOURCE_DESC1 &desc, const D3D12_HEAP_PROPERTIES &heap,
                             D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                             sampler_feedback::Layout &layout, D3D12_RESOURCE_DESC &narrowed) {
    constexpr D3D12_RESOURCE_STATES permitted =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
        D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
    if (!sampler_feedback::MakeLayout(desc, layout) || clear || IsCpuVisibleHeap(&heap) ||
        (heap.Type != D3D12_HEAP_TYPE_DEFAULT && heap.Type != D3D12_HEAP_TYPE_CUSTOM) ||
        (state & ~permitted) ||
        ((state & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RESOLVE_DEST |
                   D3D12_RESOURCE_STATE_COPY_DEST)) &&
         std::popcount(uint32_t(state)) != 1) ||
        (desc.Alignment && desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)) {
      ERR("CreateResource: invalid sampler feedback map");
      return E_INVALIDARG;
    }
    narrowed = {desc.Dimension, desc.Alignment, desc.Width,      desc.Height, desc.DepthOrArraySize,
                desc.MipLevels, desc.Format,    desc.SampleDesc, desc.Layout, desc.Flags};
    return S_OK;
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo2(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT NumResourceDescs,
      const D3D12_RESOURCE_DESC1 *pResourceDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1
  ) {
    return GetResourceAllocationInfo3(__ret, VisibleMask, NumResourceDescs, pResourceDescs, nullptr, nullptr,
                                      pResourceAllocationInfo1);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource2(
      const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
      D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue,
      ID3D12ProtectedResourceSession *pProtectedSession, REFIID riidResource, void **ppvResource
  ) {
    InitReturnPtr(ppvResource);
    if (pDesc && sampler_feedback::IsFeedbackFormat(pDesc->Format)) {
      if (RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      if (pProtectedSession)
        return E_NOTIMPL;
      sampler_feedback::Layout layout;
      D3D12_RESOURCE_DESC desc;
      HRESULT hr = ValidateHeapProperties(pHeapProperties, HeapFlags, advertise_numa_);
      if (FAILED(hr) || FAILED(hr = ValidateSamplerFeedbackMap(*pDesc, *pHeapProperties, InitialResourceState,
                                                               pOptimizedClearValue, layout, desc)))
        return hr;
      if (HeapFlags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER))
        return E_INVALIDARG;
      return CreateSamplerFeedbackMap(this, *pHeapProperties, HeapFlags, desc, layout, nullptr, 0,
                                      InitialResourceState, riidResource, ppvResource);
    }
    D3D12_RESOURCE_DESC desc;
    if (HRESULT hr = NarrowResourceDesc(pDesc, desc); FAILED(hr))
      return hr;
    return CreateCommittedResource1(pHeapProperties, HeapFlags, &desc, InitialResourceState, pOptimizedClearValue,
                                    pProtectedSession, riidResource, ppvResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource1(
      ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource
  ) {
    InitReturnPtr(ppvResource);
    if (pDesc && sampler_feedback::IsFeedbackFormat(pDesc->Format)) {
      if (RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      auto *heap = pHeap ? dynamic_cast<MTLD3D12Heap *>(pHeap) : nullptr;
      Com<ID3D12Device> heap_device;
      if (!heap || FAILED(pHeap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&heap_device))) ||
          heap_device.ptr() != static_cast<ID3D12Device *>(this))
        return E_INVALIDARG;
      const auto heap_desc = heap->GetDesc();
      sampler_feedback::Layout layout;
      D3D12_RESOURCE_DESC desc;
      if (HRESULT hr = ValidateSamplerFeedbackMap(*pDesc, heap_desc.Properties, InitialState, pOptimizedClearValue,
                                                  layout, desc);
          FAILED(hr))
        return hr;
      if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)
        return E_INVALIDARG;
      D3D12_RESOURCE_ALLOCATION_INFO1 info{};
      if (FAILED(SamplerFeedbackAllocationInfo(this, layout, info)) ||
          !resource_contract::PlacementFits(HeapOffset, info.SizeInBytes, heap_desc.SizeInBytes, info.Alignment, 0))
        return E_INVALIDARG;
      return CreateSamplerFeedbackMap(this, heap_desc.Properties, heap_desc.Flags, desc, layout, heap, HeapOffset,
                                      InitialState, riid, ppvResource);
    }
    D3D12_RESOURCE_DESC desc;
    if (HRESULT hr = NarrowResourceDesc(pDesc, desc); FAILED(hr))
      return hr;
    return CreatePlacedResource(pHeap, HeapOffset, &desc, InitialState, pOptimizedClearValue, riid, ppvResource);
  }

  /*
   * The feedback UAV (d3d12_sampler_feedback.hpp): q0 = cell array, q1 = layout word. A null paired
   * texture makes feedback writes no-ops (q0 = 0); a null feedback map is a null descriptor. The
   * paired texture must be a single-sampled TEXTURE2D matching the map's size, mips and array size.
   */
  void STDMETHODCALLTYPE
  CreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *pTargetedResource, ID3D12Resource *pFeedbackResource, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
  ) {
    auto [heap, index] = GetShaderVisibleDescriptorHeap(this, DestDescriptor);
    auto *map = static_cast<MTLD3D12Resource *>(pFeedbackResource);
    if (!map) {
      heap->AddUnorderedAccessView(index, static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC *>(nullptr));
      return;
    }
    if (!map->feedback || !map->buffer) {
      ERR("CreateSamplerFeedbackUnorderedAccessView: not a sampler feedback map");
      return;
    }
    const auto &layout = *map->feedback;
    if (pTargetedResource) {
      const auto paired = pTargetedResource->GetDesc();
      const UINT paired_mips = paired.MipLevels
          ? paired.MipLevels : UINT(std::bit_width(std::max<uint64_t>(paired.Width, paired.Height)));
      if (paired.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || paired.SampleDesc.Count != 1 ||
          paired.Width != layout.width || paired.Height != layout.height || paired_mips != layout.mips ||
          paired.DepthOrArraySize != layout.slices) {
        ERR("CreateSamplerFeedbackUnorderedAccessView: the paired texture does not match the feedback map");
        return;
      }
    }
    heap->AddSamplerFeedbackView(index, map->buffer.ptr(), layout.totalCells() * 4, pTargetedResource != nullptr,
                                 layout.word());
  }

  void STDMETHODCALLTYPE
  GetCopyableFootprints1(
      const D3D12_RESOURCE_DESC1 *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    D3D12_RESOURCE_DESC desc;
    const bool valid = SUCCEEDED(NarrowResourceDesc(pResourceDesc, desc));
    // An invalid description takes the null-desc path, which fills the documented sentinels.
    GetCopyableFootprints(valid ? &desc : nullptr, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows,
                          pRowSizeInBytes, pTotalBytes);
  }

  /* ---- ID3D12Device9 ---- */

  HRESULT STDMETHODCALLTYPE
  CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC *pDesc, REFIID riid, void **ppvSession) {
    InitReturnPtr(ppvSession);
    // D3D12_FEATURE_SHADER_CACHE reports no AUTOMATIC_*/DRIVER_MANAGED/SHADER_CONTROL_CLEAR support.
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS Control) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID CreatorID, REFIID riid, void **ppCommandQueue) {
    // CreatorID only groups queues for the driver's scheduling heuristics; Metal has no equivalent.
    return CreateCommandQueue(pDesc, riid, ppCommandQueue);
  }

  /* ---- ID3D12Device10 ---- */

  /*
   * Format list casting (VulkanOn12 spec): same unit size, no planar formats, an uncompressed
   * resource only lists uncompressed formats. Metal views are stricter for block formats and
   * depth: a BC texture may only change sRGB-ness, and depth/stencil or emulated-storage formats
   * cannot be reinterpreted at all, so those casts fail creation instead of failing later.
   */
  HRESULT
  ValidateCastableFormats(const D3D12_RESOURCE_DESC &desc, UINT32 count, const DXGI_FORMAT *formats) {
    if (!count)
      return S_OK;
    if (!formats)
      return E_INVALIDARG;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      for (UINT32 i = 0; i < count; ++i)
        if (formats[i] != DXGI_FORMAT_UNKNOWN)
          return E_INVALIDARG;
      return S_OK;
    }
    MTL_DXGI_FORMAT_DESC base;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), desc.Format, base)))
      return E_INVALIDARG;
    constexpr uint32_t planar_flags = MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER;
    const bool base_exact_only = (base.Flag & (planar_flags | MTL_DXGI_FORMAT_BC)) || base.PlanarCount != 1 ||
                                 resource_contract::GetEmulatedTextureStorage(desc.Format) !=
                                     resource_contract::EmulatedTextureStorage::None;
    for (UINT32 i = 0; i < count; ++i) {
      const auto format = formats[i];
      MTL_DXGI_FORMAT_DESC cast;
      if (format == DXGI_FORMAT_UNKNOWN || FAILED(MTLQueryDXGIFormat(GetMTLDevice(), format, cast)))
        return E_INVALIDARG;
      if (resource_contract::AreCopyCompatibleFormats(desc.Format, format))
        continue;
      if (base_exact_only) {
        ERR("CreateResource: Metal cannot view format ", desc.Format, " as ", format);
        return E_INVALIDARG;
      }
      if ((cast.Flag & (planar_flags | MTL_DXGI_FORMAT_BC)) || cast.PlanarCount != 1 ||
          resource_contract::GetEmulatedTextureStorage(format) != resource_contract::EmulatedTextureStorage::None ||
          cast.BytesPerTexel != base.BytesPerTexel)
        return E_INVALIDARG;
    }
    return S_OK;
  }

  /*
   * InitialLayout rules: buffers have no layout (UNDEFINED); simultaneous-access textures only
   * use COMMON; other textures take any layout of a queue this device creates whose access the
   * resource flags allow. Returns the legacy creation state used for the shared validation.
   */
  HRESULT
  ValidateInitialLayout(
      const D3D12_RESOURCE_DESC &desc, const D3D12_HEAP_PROPERTIES &heap, D3D12_BARRIER_LAYOUT layout,
      D3D12_RESOURCE_STATES &legacy_state
  ) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (layout != D3D12_BARRIER_LAYOUT_UNDEFINED)
        return E_INVALIDARG;
      legacy_state = heap.Type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                     : heap.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                             : D3D12_RESOURCE_STATE_COMMON;
      return S_OK;
    }
    if (layout != D3D12_BARRIER_LAYOUT_UNDEFINED &&
        !enhanced_barrier::LayoutAllowedOnQueue(layout, D3D12_COMMAND_LIST_TYPE_DIRECT) &&
        !enhanced_barrier::LayoutAllowedOnQueue(layout, D3D12_COMMAND_LIST_TYPE_COMPUTE))
      return E_INVALIDARG;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) && layout != D3D12_BARRIER_LAYOUT_COMMON)
      return E_INVALIDARG;

    legacy_state = enhanced_barrier::LegacyStateFromLayout(layout);
    return ValidateResourceStates(legacy_state, &heap, &desc);
  }

  /* Record the enhanced-barrier creation contract on a freshly created resource and return it as riid. */
  static HRESULT
  FinishLayoutResource(
      Com<ID3D12Resource> &resource, const D3D12_RESOURCE_DESC &desc, D3D12_BARRIER_LAYOUT layout, UINT32 count,
      const DXGI_FORMAT *formats, REFIID riid, void **ppResource
  ) {
    auto *impl = static_cast<MTLD3D12Resource *>(resource.ptr());
    try {
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        impl->owned_initial_layout = layout;
        impl->owned_castable_formats.assign(formats, formats + count);
      }
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return resource->QueryInterface(riid, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource3(
      const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
      D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue,
      ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats,
      REFIID riidResource, void **ppvResource
  ) {
    InitReturnPtr(ppvResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (pProtectedSession)
      return E_NOTIMPL;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES legacy_state;
    HRESULT hr;
    if (!pHeapProperties || FAILED(hr = NarrowResourceDesc(pDesc, desc)))
      return E_INVALIDARG;
    if (FAILED(hr = ValidateInitialLayout(desc, *pHeapProperties, InitialLayout, legacy_state)) ||
        FAILED(hr = ValidateCastableFormats(desc, NumCastableFormats, pCastableFormats)))
      return hr;
    if (!ppvResource)
      return CreateCommittedResource(pHeapProperties, HeapFlags, &desc, legacy_state, pOptimizedClearValue,
                                     riidResource, nullptr);
    Com<ID3D12Resource> resource;
    if (FAILED(hr = CreateCommittedResource(pHeapProperties, HeapFlags, &desc, legacy_state, pOptimizedClearValue,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void **>(&resource))))
      return hr;
    return FinishLayoutResource(resource, desc, InitialLayout, NumCastableFormats, pCastableFormats, riidResource,
                                ppvResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource2(
      ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
      const D3D12_CLEAR_VALUE *pOptimizedClearValue, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats,
      REFIID riid, void **ppvResource
  ) {
    InitReturnPtr(ppvResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    auto *heap = pHeap ? dynamic_cast<MTLD3D12Heap *>(pHeap) : nullptr;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES legacy_state;
    HRESULT hr;
    if (!heap || FAILED(hr = NarrowResourceDesc(pDesc, desc)))
      return E_INVALIDARG;
    const auto heap_desc = heap->GetDesc();
    if (FAILED(hr = ValidateInitialLayout(desc, heap_desc.Properties, InitialLayout, legacy_state)) ||
        FAILED(hr = ValidateCastableFormats(desc, NumCastableFormats, pCastableFormats)))
      return hr;
    if (!ppvResource)
      return CreatePlacedResource(pHeap, HeapOffset, &desc, legacy_state, pOptimizedClearValue, riid, nullptr);
    Com<ID3D12Resource> resource;
    if (FAILED(hr = CreatePlacedResource(pHeap, HeapOffset, &desc, legacy_state, pOptimizedClearValue,
                                         __uuidof(ID3D12Resource), reinterpret_cast<void **>(&resource))))
      return hr;
    return FinishLayoutResource(resource, desc, InitialLayout, NumCastableFormats, pCastableFormats, riid,
                                ppvResource);
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource2(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue,
      ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, DXGI_FORMAT *pCastableFormats,
      REFIID riid, void **ppvResource
  ) {
    InitReturnPtr(ppvResource);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pDesc || pProtectedSession)
      return E_INVALIDARG;
    // Reserved resources get the default heap's rules (see CreateReservedResource1).
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_STATES legacy_state;
    HRESULT hr;
    if (FAILED(hr = ValidateInitialLayout(*pDesc, heap, InitialLayout, legacy_state)) ||
        FAILED(hr = ValidateCastableFormats(*pDesc, NumCastableFormats, pCastableFormats)))
      return hr;
    Com<ID3D12Resource> resource;
    if (FAILED(hr = CreateReservedResource1(pDesc, legacy_state, pOptimizedClearValue, nullptr,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void **>(&resource))))
      return hr;
    if (!ppvResource)
      return S_FALSE;
    return FinishLayoutResource(resource, *pDesc, InitialLayout, NumCastableFormats, pCastableFormats, riid,
                                ppvResource);
  }

  /* ---- ID3D12Device11 ---- */

  void STDMETHODCALLTYPE
  CreateSampler2(const D3D12_SAMPLER_DESC2 *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!pDesc)
      return;
    D3D12_SAMPLER_DESC desc{pDesc->Filter,        pDesc->AddressU, pDesc->AddressV, pDesc->AddressW,
                            pDesc->MipLODBias,    pDesc->MaxAnisotropy, pDesc->ComparisonFunc, {},
                            pDesc->MinLOD,        pDesc->MaxLOD};
    if (pDesc->Flags & ~D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) {
      // Heap samplers with unnormalized coordinates need NonNormalizedCoordinateSamplersSupported.
      ERR("CreateSampler2: unsupported sampler flags ", uint32_t(pDesc->Flags));
      return;
    }
    if (pDesc->Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) {
      // Metal's three predefined border colors return integer (0,0,0,0), (0,0,0,1) and (1,1,1,1)
      // on integer textures; only those UINT borders are representable.
      const auto *c = pDesc->UintBorderColor;
      const bool rgb_zero = !c[0] && !c[1] && !c[2], rgb_one = c[0] == 1 && c[1] == 1 && c[2] == 1;
      if (!((rgb_zero && c[3] <= 1) || (rgb_one && c[3] == 1))) {
        ERR("CreateSampler2: uint border color (", c[0], ",", c[1], ",", c[2], ",", c[3], ") is not representable");
        return;
      }
      for (UINT i = 0; i < 4; ++i)
        desc.BorderColor[i] = float(c[i]);
    } else {
      std::copy_n(pDesc->FloatBorderColor, 4, desc.BorderColor);
    }
    CreateSampler(&desc, DestDescriptor);
  }

  /* ---- ID3D12Device13 ---- */

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress1(const void *pAddress, SIZE_T size, REFIID riid, void **ppvHeap) {
    InitReturnPtr(ppvHeap);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    // Unlike OpenExistingHeapFromAddress, the range is explicit; a zero size is invalid here.
    if (!pAddress || !size)
      return E_INVALIDARG;
    return CreateHeapFromHostMemory(this, const_cast<void *>(pAddress), size, riid, ppvHeap);
  }

  /* ---- ID3D12Device14 ---- */

  HRESULT STDMETHODCALLTYPE
  CreateRootSignatureFromSubobjectInLibrary(
      UINT nodeMask, const void *pLibraryBlob, SIZE_T blobLengthInBytes, LPCWSTR subobjectName, REFIID riid,
      void **ppvRootSignature
  ) {
    InitReturnPtr(ppvRootSignature);
    if (RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    std::vector<uint8_t> payload;
    if (HRESULT hr = FindLibraryRootSignature(pLibraryBlob, blobLengthInBytes, subobjectName, payload);
        FAILED(hr)) {
      ERR("CreateRootSignatureFromSubobjectInLibrary: no matching root signature subobject");
      return hr;
    }
    // RDAT stores the bare RTS0 payload; CreateRootSignature takes a container.
    std::vector<uint8_t> container;
    if (HRESULT hr = WrapRootSignaturePayload(payload.data(), payload.size(), container); FAILED(hr))
      return hr;
    return dxmt::CreateRootSignature(this, nodeMask, container.data(), container.size(), riid, ppvRootSignature);
  }

  /* ---- ID3D12Device12 ---- */

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT NumResourceDescs,
      const D3D12_RESOURCE_DESC1 *pResourceDescs, const UINT32 *pNumCastableFormats,
      const DXGI_FORMAT *const *ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1
  ) {
    auto invalid = [&] {
      __ret->SizeInBytes = UINT64_MAX;
      __ret->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      return __ret;
    };
    if (NumResourceDescs && !pResourceDescs)
      return invalid();
    std::vector<D3D12_RESOURCE_DESC> descs;
    try {
      descs.resize(NumResourceDescs);
    } catch (const std::bad_alloc &) {
      return invalid();
    }
    bool any_feedback = false;
    for (UINT i = 0; i < NumResourceDescs; ++i) {
      if (sampler_feedback::IsFeedbackFormat(pResourceDescs[i].Format)) {
        any_feedback = true;
        continue;
      }
      if (FAILED(NarrowResourceDesc(&pResourceDescs[i], descs[i])))
        return invalid();
      const UINT32 count = pNumCastableFormats ? pNumCastableFormats[i] : 0;
      if (count && (!ppCastableFormats || FAILED(ValidateCastableFormats(descs[i], count, ppCastableFormats[i]))))
        return invalid();
    }
    // Castable formats only widen view usage; Metal texture sizes already include PixelFormatView.
    if (!any_feedback)
      return GetResourceAllocationInfo1(__ret, VisibleMask, NumResourceDescs, descs.data(), pResourceAllocationInfo1);
    // Sampler feedback maps are buffers of cells; lay every description out one after another.
    if (VisibleMask > 1)
      return invalid();
    __ret->SizeInBytes = 0;
    __ret->Alignment = 1;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    for (UINT i = 0; i < NumResourceDescs; ++i) {
      D3D12_RESOURCE_ALLOCATION_INFO1 info{};
      if (sampler_feedback::IsFeedbackFormat(pResourceDescs[i].Format)) {
        sampler_feedback::Layout layout;
        D3D12_RESOURCE_DESC unused;
        if ((pNumCastableFormats && pNumCastableFormats[i]) ||
            FAILED(ValidateSamplerFeedbackMap(pResourceDescs[i], default_heap, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              layout, unused)) ||
            FAILED(SamplerFeedbackAllocationInfo(this, layout, info)))
          return invalid();
      } else {
        D3D12_RESOURCE_ALLOCATION_INFO single;
        GetResourceAllocationInfo1(&single, VisibleMask, 1, &descs[i], &info);
        if (single.SizeInBytes == UINT64_MAX)
          return invalid();
        info.SizeInBytes = single.SizeInBytes;
        info.Alignment = single.Alignment;
      }
      if (!resource_contract::CheckedAlign(__ret->SizeInBytes, info.Alignment, info.Offset) ||
          !resource_contract::CheckedAdd(info.Offset, info.SizeInBytes, __ret->SizeInBytes))
        return invalid();
      __ret->Alignment = std::max(__ret->Alignment, info.Alignment);
      if (pResourceAllocationInfo1)
        pResourceAllocationInfo1[i] = info;
    }
    if (!resource_contract::CheckedAlign(__ret->SizeInBytes, __ret->Alignment, __ret->SizeInBytes))
      return invalid();
    return __ret;
  }

  WMT::ResidencySet
  GetGlobalResidencySet() {
    return residency_set_;
  };

  WMT::MTL4CommandQueue
  GetSparseMappingQueue() {
    std::lock_guard<std::mutex> lock(sparse_mapping_queue_mutex_);
    if (!sparse_mapping_queue_) {
      sparse_mapping_queue_ = GetMTLDevice().newMTL4CommandQueue();
      if (sparse_mapping_queue_)
        sparse_mapping_queue_.addResidencySet(residency_set_);
    }
    return sparse_mapping_queue_;
  }

  uint64_t
  GetAtomicLockTableAddress() override {
    return atomic_lock_table_address_;
  }

  HRESULT
  RegisterResidency(
      WMT::Allocation allocation, MTLD3D12PageableResidency *owner, bool initially_resident
  ) {
    if (!allocation)
      return E_OUTOFMEMORY;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      bool owner_registered = false;
      if (owner) {
        if (!owner->AddResidencyAllocation(allocation))
          return E_OUTOFMEMORY;
        owner_registered = true;
        if (!owner->SupportsResidency())
          owner->InitializeResidency(initially_resident);
      }
      try {
        if (!owner || owner->ResidencyReferenceCount()) {
          residency_set_.addAllocations(&allocation, 1);
          residency_set_.commit();
        }
      } catch (...) {
        if (owner_registered)
          owner->RemoveResidencyAllocation(allocation);
        throw;
      }
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT
  UnregisterSwizzleShadowResidency(WMT::Allocation allocation, MTLD3D12PageableResidency *owner) {
    if (!allocation || !owner)
      return E_INVALIDARG;
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    const auto &allocations = owner->ResidencyAllocations();
    if (std::find(allocations.begin(), allocations.end(), allocation) == allocations.end())
      return E_INVALIDARG;
    HRESULT hr = S_OK;
    if (owner->ResidencyReferenceCount()) {
      try {
        residency_set_.removeAllocations(&allocation, 1);
        residency_set_.commit();
      } catch (...) {
        hr = E_FAIL;
      }
    }
    owner->RemoveResidencyAllocation(allocation);
    return hr;
  }

  HRESULT
  RegisterStandardSwizzleTexture(Texture *texture, MTLD3D12Resource *resource) {
    if (!texture || !resource)
      return E_INVALIDARG;
    try {
      std::unique_lock<dxmt::mutex> lock(swizzle_texture_lock_);
      if (!swizzle_textures_.emplace(texture, resource).second)
        return E_INVALIDARG;
      swizzle_texture_count_.fetch_add(1, std::memory_order_release);
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  void
  UnregisterStandardSwizzleTexture(Texture *texture) {
    std::unique_lock<dxmt::mutex> lock(swizzle_texture_lock_);
    if (swizzle_textures_.erase(texture))
      swizzle_texture_count_.fetch_sub(1, std::memory_order_release);
  }

  Com<MTLD3D12Resource, false>
  LookupStandardSwizzleTexture(Texture *texture) {
    if (!HasStandardSwizzleTextures())
      return nullptr;
    std::unique_lock<dxmt::mutex> lock(swizzle_texture_lock_);
    auto entry = swizzle_textures_.find(texture);
    if (entry == swizzle_textures_.end() || !entry->second->TryAddRefPrivate())
      return nullptr;
    return Com<MTLD3D12Resource, false>::transfer(entry->second);
  }

  bool
  HasStandardSwizzleTextures() const {
    return swizzle_texture_count_.load(std::memory_order_acquire) != 0;
  }

  HRESULT STDMETHODCALLTYPE SupportsTemporalUpscale(BOOL *supported) final {
    if (!supported)
      return E_POINTER;
    *supported = GetMTLDevice().supportsFXTemporalScaler();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateTemporalUpscaleState(
      const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, IMTLD3D12TemporalUpscaleState **state) final {
    return CreateOwnedTemporalState(this, desc, state);
  }

  HRESULT
  UnregisterResidency(WMT::Allocation allocation, MTLD3D12PageableResidency *owner) {
    if (!allocation)
      return E_INVALIDARG;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      if (!owner || owner->ResidencyReferenceCount()) {
        residency_set_.removeAllocations(&allocation, 1);
        residency_set_.commit();
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT
  RegisterResidencyAndVA(
      BufferAllocation *allocation, MTLD3D12PageableResidency *owner, bool initially_resident
  ) {
    if (!allocation || !allocation->buffer())
      return E_OUTOFMEMORY;
    try {
      std::unique_lock<dxmt::mutex> lock(residency_lock_);
      auto [entry, inserted] = interval_map_.emplace(allocation->gpuAddress(), allocation);
      if (!inserted)
        return E_INVALIDARG;
      if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
        return S_OK;
      auto buffer = allocation->buffer();
      try {
        if (owner) {
          if (!owner->AddResidencyAllocation(buffer)) {
            interval_map_.erase(entry);
            return E_OUTOFMEMORY;
          }
          if (!owner->SupportsResidency())
            owner->InitializeResidency(initially_resident);
        }
      } catch (...) {
        interval_map_.erase(entry);
        throw;
      }
      if (!owner || initially_resident) {
        residency_set_.addAllocations(&buffer, 1);
        residency_set_.commit();
      }
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  /**
   * Drop the residency an acceleration structure's sidecar holds.
   *
   * A Metal acceleration structure and its per-instance translation buffers are
   * resources but not the buffer a D3D12 allocation is registered through, so each
   * one was registered individually when the build published it and has to be
   * removed individually here. Removing an allocation the residency set does not
   * hold is a no-op, so the null sidecar of a structure that was never built costs
   * nothing.
   */
  void
  UnregisterRTASResidency(RTASState &state) {
    if (!state.sidecar)
      return;
    // In-place refits publish a new version of the same native object. Sweeping the
    // previous version must not make the current one nonresident.
    bool stillReferenced = false;
    rtas_.ForEachKey([&](const raytracing::ArenaKey &key) {
      const auto *current = rtas_.FindRecorded(key);
      stillReferenced |= current && current->sidecar == state.sidecar;
    });
    if (stillReferenced)
      return;
    WMT::Allocation structure = state.sidecar.accelerationStructure();
    if (structure)
      residency_set_.removeAllocations(&structure, 1);
    WMT::Allocation instances = state.sidecar.instanceBuffer();
    if (instances)
      residency_set_.removeAllocations(&instances, 1);
    WMT::Allocation contributions = state.sidecar.contributionBuffer();
    if (contributions)
      residency_set_.removeAllocations(&contributions, 1);
    const auto generations = state.sidecar.generations();
    for (uint64_t i = 0; i < generations.count(); ++i) {
      auto allocation = generations.object(i);
      residency_set_.removeAllocations(&allocation, 1);
    }
  }

  HRESULT
  UnregisterResidencyAndVA(BufferAllocation *allocation, MTLD3D12PageableResidency *owner) {
    if (!allocation)
      return E_INVALIDARG;
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    interval_map_.erase(allocation->gpuAddress());
    /*
     * Every acceleration structure that lived in this allocation dies with it, so each of its
     * versions is released here: this is D3D12's own lifetime rule (the application may not free
     * a resource a submission still reads), not a release decision of ours.
     */
    bool released_sidecars = false;
    std::vector<raytracing::ArenaKey> dying;
    rtas_.ForEachKey([&](const raytracing::ArenaKey &key) {
      if (key.allocationIdentity == allocation)
        dying.push_back(key);
    });
    for (const auto &key : dying) {
      for (auto &state : rtas_.ReleaseAll(key)) {
        UnregisterRTASResidency(state);
        released_sidecars = true;
      }
      FreeRTASSlot(key);
    }
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap)) {
      if (released_sidecars)
        residency_set_.commit();
      return S_OK;
    }
    auto buffer = allocation->buffer();
    if (!owner || owner->ResidencyReferenceCount()) {
      residency_set_.removeAllocations(&buffer, 1);
      residency_set_.commit();
    } else if (released_sidecars) {
      residency_set_.commit();
    }
    return S_OK;
  }

  BufferAllocation *
  LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t *pOffset) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    auto iter = interval_map_.upper_bound(VA);
    if (iter == interval_map_.begin()) {
      *pOffset = 0;
      return {};
    }
    --iter;
    *pOffset = VA - iter->first;
    if (*pOffset >= iter->second->byteLength()) {
      *pOffset = 0;
      return {};
    }
    return iter->second;
  }

  InternalCommandLibrary &
  GetLib() {
    return command_library;
  }

  HRESULT
  QueryRTASSizes(const raytracing::BuildInputs &inputs, raytracing::NativeSizes &sizes) {
    raytracing::NativeDescriptor descriptor;
    const HRESULT hr = raytracing::CreateNativeDescriptor(GetMTLDevice(), inputs, {}, descriptor);
    if (FAILED(hr))
      return hr;
    sizes = descriptor.sizes;
    return S_OK;
  }

  bool
  FindRTAS(const raytracing::ArenaKey &key, RTASState &state) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    const RTASState *recorded = rtas_.FindRecorded(key);
    if (!recorded)
      return false;
    state = *recorded;
    return true;
  }

  bool
  StoreRTAS(const raytracing::ArenaKey &key, RTASState state, uint32_t *token,
      WMT::Reference<WMT::Buffer> *mirror, uint64_t *mirror_offset) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    /*
     * The mirror slot is reserved before anything is published, because a version whose token has
     * no slot to be published into could never be identified by the GPU and would be retained
     * forever. An address keeps one slot for its whole life, so this only allocates the first time;
     * a slot of a released address is handed back by FreeRTASSlot, so the fixed page budget bounds
     * how many addresses are alive at once and not how many were ever created.
     */
    uint32_t slot = 0;
    auto found = rtas_slots_.find(key);
    if (found == rtas_slots_.end()) {
      const size_t page_count = rtas_mirror_pages_.size();
      if (!rtas_mirror_.Allocate(&slot))
        return false;
      rtas_slots_.emplace(key, slot);
      if (rtas_mirror_pages_.size() != page_count) {
        /* A page was created: it must be resident for the prepass that writes it. */
        WMT::Allocation page = rtas_mirror_pages_.back();
        residency_set_.addAllocations(&page, 1);
        residency_set_.commit();
      }
    } else {
      slot = found->second;
    }
    const size_t page_index = slot / owned_graphics::RTASMirror::kSlotsPerPage;
    if (page_index >= rtas_mirror_pages_.size())
      return false;

    /*
     * A Metal acceleration structure is an MTLResource but not a buffer, so it is never covered
     * by the buffer residency registration a ray dispatch performs. Each published version is
     * therefore registered here, before the submission that may read it is ever encoded: a
     * residency set is consulted when a command buffer executes, so declaring residency at
     * completion time would already be too late for the dispatch that runs the build.
     *
     * Publishing only records the version. Nothing is released, because a build that has not
     * executed yet has not superseded anything: only an observation of the mirror can retire the
     * version the header used to hold.
     */
    const uint32_t published = rtas_.Publish(key, std::move(state));
    const RTASState *recorded = rtas_.FindRecorded(key);
    if (recorded) {
      AddRTASResidency(*recorded);
      residency_set_.commit();
    }

    *token = published;
    *mirror = rtas_mirror_pages_[page_index];
    *mirror_offset = uint64_t(slot % owned_graphics::RTASMirror::kSlotsPerPage) * sizeof(uint32_t);
    return true;
  }

  void
  ReleaseRTASLease(const raytracing::ArenaKey &key, uint32_t token) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    auto released = rtas_.ReleaseLease(key, token);
    if (released.empty())
      return;
    for (auto &state : released)
      UnregisterRTASResidency(state);
    residency_set_.commit();
  }

  void
  HoldRTASLease(const raytracing::ArenaKey &key, uint32_t token) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    rtas_.HoldLease(key, token);
  }

  void
  ObserveRTAS(const raytracing::ArenaKey &key) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    auto found = rtas_slots_.find(key);
    if (found == rtas_slots_.end())
      return;
    /*
     * The token the GPU published for this address is the only authority on which native
     * structure a dispatch will read. A zero slot means no build has executed yet, so nothing has
     * been superseded. Residency needs no work here: every published version is registered at
     * publication, so the live version is already resident by the time any dispatch can run.
     */
    const uint32_t token = rtas_mirror_.Load(found->second);
    if (!token)
      return;
    if (!rtas_.Observe(key, token))
      return;
    ReleaseSupersededRTASLocked();
  }

  /** Release every version no in-flight submission can still read. Caller holds residency_lock_. */
  void
  ReleaseSupersededRTASLocked() {
    auto released = rtas_.Sweep();
    if (released.empty())
      return;
    for (auto &state : released)
      UnregisterRTASResidency(state);
    residency_set_.commit();
  }

  /** Add one version's native structure and translation buffers to the residency set. */
  bool AllocateRTAS(WMT::AccelerationStructureSidecar sidecar, uint64_t size, uint64_t count) final {
    std::lock_guard<dxmt::mutex> lock(residency_lock_);
    if (!sidecar.allocate(GetMTLDevice(), size, count))
      return false;
    AddRTASResidency({sidecar, size, uint32_t(count), {}});
    residency_set_.commit();
    return true;
  }

  /** Add one version's native structure and translation buffers to the residency set. */
  void
  AddRTASResidency(const RTASState &state) {
    if (!state.sidecar)
      return;
    WMT::Allocation structure = state.sidecar.accelerationStructure();
    if (structure)
      residency_set_.addAllocations(&structure, 1);
    WMT::Allocation instances = state.sidecar.instanceBuffer();
    if (instances)
      residency_set_.addAllocations(&instances, 1);
    WMT::Allocation contributions = state.sidecar.contributionBuffer();
    if (contributions)
      residency_set_.addAllocations(&contributions, 1);
  }

  /**
   * Return one address's mirror slot for reuse. Caller holds residency_lock_.
   *
   * The address is gone with its allocation, so no GPU work can still publish into this slot:
   * D3D12 only lets an application release a resource no in-flight submission reads. Without
   * this, every acceleration structure the application ever creates would consume a slot forever
   * and the fixed page budget would become a cap on total AS creations rather than on live ones.
   */
  void
  FreeRTASSlot(const raytracing::ArenaKey &key) {
    auto found = rtas_slots_.find(key);
    if (found == rtas_slots_.end())
      return;
    rtas_mirror_.Free(found->second);
    rtas_slots_.erase(found);
  }

  uint64_t
  BeginRTASSubmission() {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    return rtas_.BeginSubmission();
  }

  void
  RetireRTASSubmission(uint64_t id) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    rtas_.RetireSubmission(id);
    ReleaseSupersededRTASLocked();
  }

  /*
   * A page of the shared version mirror. Pages are never resized, copied or freed while the
   * device lives, so a GPU store into a slot always targets a buffer that still exists.
   *
   * Called from inside StoreRTAS, which already holds residency_lock_, so this must not take it:
   * dxmt::mutex is not recursive. The caller registers the new page's residency.
   */
  std::atomic<uint32_t> *
  CreateRTASMirrorPage(uint64_t slot_count) {
    WMTBufferInfo info{};
    info.length = slot_count * sizeof(uint32_t);
    /* Default (Shared) storage and no private mapping: the queue reads the published token back
     * through the CPU pointer once the writing submission completes. */
    info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    info.memory.set(0);
    auto buffer = GetMTLDevice().newBuffer(info);
    if (!buffer)
      return nullptr;
    auto *cpu = static_cast<std::atomic<uint32_t> *>(info.memory.get_accessible_or_null());
    if (!cpu)
      return nullptr;
    /*
     * Construct the slots rather than memset them: the host reads a slot with an atomic load while
     * the device may be storing it, so each slot has to be a real atomic object from the start.
     * Metal reports this buffer zeroed by no contract, and a zero slot must mean "no build has
     * executed", so every slot is value-initialized here.
     */
    for (uint64_t i = 0; i < slot_count; ++i)
      new (&cpu[i]) std::atomic<uint32_t>(0);
    rtas_mirror_pages_.push_back(buffer);
    return cpu;
  }

  HRESULT
  GetRTASComputePipelines(RTASComputePipelines &pipelines) {
    std::unique_lock<std::mutex> lock(rtas_pipeline_lock_);
    if (!rtas_pipelines_.prepass) {
      auto library = command_library.getLibrary();
      WMT::Reference<WMT::Error> error;
      auto prepass_function = library.newFunction("dxmt_as_prepass");
      auto prepass = GetMTLDevice().newComputePipelineState(prepass_function, error);
      if (error || !prepass)
        return E_FAIL;
      rtas_pipelines_.prepass = std::move(prepass);
      const char *names[] = {"dxmt_as_shadow", "dxmt_as_postbuild", "dxmt_as_serialize",
          "dxmt_as_deserialize", "dxmt_as_visualize", "dxmt_as_visualize_descriptors"};
      WMT::Reference<WMT::ComputePipelineState> *states[] = {&rtas_pipelines_.shadow, &rtas_pipelines_.postbuild,
          &rtas_pipelines_.serialize, &rtas_pipelines_.deserialize, &rtas_pipelines_.visualize,
          &rtas_pipelines_.visualizeDescriptors};
      for (uint32_t i = 0; i < 6; ++i) {
        error = {};
        *states[i] = GetMTLDevice().newComputePipelineState(library.newFunction(names[i]), error);
        if (error || !*states[i]) {
          rtas_pipelines_ = {};
          return E_FAIL;
        }
      }
    }
    pipelines = rtas_pipelines_;
    return S_OK;
  }

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };

  /* ID3D12Device5 surface. The raytracing feature tier stays NOT_SUPPORTED until ray dispatch
   * works end to end, so the state objects and meta commands that would only be reachable
   * through it report E_NOTIMPL instead of pretending to exist. */
  HRESULT STDMETHODCALLTYPE
  CreateLifetimeTracker(ID3D12LifetimeOwner *, REFIID, void **tracker) final {
    InitReturnPtr(tracker);
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  RemoveDevice() final {}

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommands(UINT *meta_command_count, D3D12_META_COMMAND_DESC *descs) final {
    if (!meta_command_count)
      return E_INVALIDARG;
    if (descs && *meta_command_count)
      return E_INVALIDARG;
    *meta_command_count = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommandParameters(
      REFGUID, D3D12_META_COMMAND_PARAMETER_STAGE, UINT *total_size, UINT *parameter_count,
      D3D12_META_COMMAND_PARAMETER_DESC *
  ) final {
    if (!total_size || !parameter_count)
      return E_INVALIDARG;
    *total_size = 0;
    *parameter_count = 0;
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateMetaCommand(REFGUID, UINT, const void *, SIZE_T, REFIID, void **meta_command) final {
    InitReturnPtr(meta_command);
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE
  GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info
  ) final {
    if (!info)
      return;
    *info = {};
    if (!desc)
      return;
    raytracing::BuildInputs inputs;
    if (FAILED(raytracing::ConvertBuildInputs(*desc, nullptr, nullptr, inputs)))
      return;
    raytracing::NativeSizes native;
    if (FAILED(QueryRTASSizes(inputs, native)))
      return;
    raytracing::ShadowPlan plan;
    if (!raytracing::PlanShadow(inputs, plan))
      return;
    if (!raytracing::CalculatePrebuildInfo(native, plan.size, &info->ResultDataMaxSizeInBytes,
            &info->ScratchDataSizeInBytes, &info->UpdateScratchDataSizeInBytes))
      *info = {};
  }

  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
  CheckDriverMatchingIdentifier(
      D3D12_SERIALIZED_DATA_TYPE type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier
  ) final {
    return raytracing::CheckDriverMatchingIdentifier(GetMTLDevice().registryID(), type, identifier);
  }
};

HRESULT
CreateD3D12Device(IMTLDXGIAdapter *adapter, const IID &riid, void **ppDevice) {
  auto device = Com(new MTLD3D12DeviceImpl(adapter));
  HRESULT hr = device->Initialize();
  if (FAILED(hr))
    return hr;
  return device->QueryInterface(riid, ppDevice);
};

} // namespace dxmt

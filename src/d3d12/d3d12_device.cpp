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
#include "d3d12_format_support.hpp"
#include "d3d12_state_object.hpp"
#include "d3d12_temporal.hpp"
#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "com/com_object.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include <map>
#include <new>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

const GUID kD3D12DeviceDownlevelUUID = {0x74eaee3f, 0x2f4b, 0x476d, {0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10}};

HRESULT PopulateWMTTextureInfo(WMT::Device Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc);

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

  bool advertise_numa_ = false;

  dxmt::mutex residency_lock_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  std::map<uint64_t, BufferAllocation *> interval_map_;

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

public:
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
    format_inspector_.Inspect(GetMTLDevice());
    
    WMTDepthStencilInfo info{};
    info.depth_compare_function = WMTCompareFunctionAlways;
    default_depth_stencil_state = GetMTLDevice().newDepthStencilState(info);

    return S_OK;
  };

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  };

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
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5)) {
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
    return dxmt::CreateCommandAllocator(this, Type, riid, ppCommandAllocator);
  };

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    return dxmt::CreateGraphicsPipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    return dxmt::CreateComputePipelineState(this, pDesc, riid, ppPipelineState);
  };

  HRESULT STDMETHODCALLTYPE
  CreateStateObject(const D3D12_STATE_OBJECT_DESC *pDesc, REFIID riid, void **ppStateObject) {
    return dxmt::CreateStateObject(this, pDesc, riid, ppStateObject);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator *pCommandAllocator,
      ID3D12PipelineState *pInitialPipelineState, REFIID riid, void **ppCommandList
  ) {
    if (ppCommandList)
      *ppCommandList = nullptr;
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
    switch (Feature) {
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

      if (out->SampleCount == 0) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }

      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->NumQualityLevels = out->SampleCount == 0 ? 1 : 0;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (SUCCEEDED(hr) && out->SampleCount) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = metal.supportsTextureSampleCount(out->SampleCount) ? 1 : 0;
      } else {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }
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
      if (out->Format == DXGI_FORMAT_UNKNOWN) {
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
      reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(pFeatureData)->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(pFeatureData);
      out->DoublePrecisionFloatShaderOps = FALSE;
      out->OutputMergerLogicOp = FALSE;
      out->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
      out->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      out->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2;
      out->PSSpecifiedStencilRefSupported = TRUE;
      out->TypedUAVLoadAdditionalFormats = TRUE;
      out->ROVsSupported = TRUE;
      out->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      out->StandardSwizzle64KBSupported = TRUE;
      out->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      out->CrossAdapterRowMajorTextureSupported = FALSE;
      out->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
      out->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS16: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(pFeatureData);
      out->GPUUploadHeapSupported = FALSE;    // TODO(d3d12): gpu upload heap
      out->DynamicDepthBiasSupported = FALSE; // TODO(d3d12): ID3D12GraphicsCommandList9::RSSetDepthBias
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
      out->BarycentricsSupported = FALSE;
      out->CopyQueueTimestampQueriesSupported = FALSE;
      out->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      out->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAGS(
          D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY | D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(pFeatureData);
      out->WaveOps = 0;
      out->WaveLaneCountMin = 0;
      out->WaveLaneCountMax = 0;
      out->TotalLaneCount = 0;
      // If CheckFeatureSupport succeeds this value will always be true.
      out->ExpandedComputeResourceStates = TRUE;
      out->Int64ShaderOps = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(pFeatureData);
      out->RelaxedFormatCastingSupported = FALSE;
      out->EnhancedBarriersSupported = FALSE;
      out->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(pFeatureData);
      out->MSAA64KBAlignedTextureSupported = TRUE;
      out->Native16BitShaderOpsSupported = FALSE; // TODO(d3d12): should be true
      // TODO(d3d12): revise when d3d12 shared resource is implemented
      out->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(pFeatureData);
      out->SRVOnlyTiledResourceTier3 = FALSE;
      // Advertised only once the whole acceleration-structure path (build/refit/copy plus
      // ray dispatch) is complete; the AS commands themselves already validate and execute.
      out->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      out->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(pFeatureData);
      out->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      out->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(pFeatureData);
      out->SupportFlags =
          D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_INPROC_CACHE | D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE;
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

      MTL_DXGI_FORMAT_DESC format_desc;
      if (FAILED(MTLQueryDXGIFormat(metal, out->Format, format_desc)))
        return E_INVALIDARG;

      auto capability = GetMTLPixelFormatCapability(format_desc.PixelFormat);
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
      const auto texture_capability_mask = static_cast<FormatCapability>(
          FormatCapability::Atomic | FormatCapability::Filter | FormatCapability::Write |
          FormatCapability::Color | FormatCapability::Blend | FormatCapability::MSAA |
          FormatCapability::Sparse | FormatCapability::Resolve | FormatCapability::DepthStencil);
      const auto buffer_capability_mask = static_cast<FormatCapability>(
          FormatCapability::TextureBufferRead | FormatCapability::TextureBufferWrite |
          FormatCapability::TextureBufferReadWrite);
      const bool inspected_texture_support = any_bit_set(capability & texture_capability_mask);
      const bool inspected_buffer_support = any_bit_set(capability & buffer_capability_mask);

      D3D12FormatSupportProperties properties{
          .mapped = format_desc.PixelFormat != WMTPixelFormatInvalid,
          .buffer_compatible =
              !block_compressed && !depth && !stencil && native_layout_compatible && inspected_buffer_support,
          .texture_compatible = native_layout_compatible && inspected_texture_support,
          .typeless = bool(format_desc.Flag & MTL_DXGI_FORMAT_TYPELESS),
          .block_compressed = block_compressed,
          .depth = depth,
          .stencil = stencil,
          .backbuffer = bool(format_desc.Flag & MTL_DXGI_FORMAT_BACKBUFFER),
          .vertex_attribute = format_desc.AttributeFormat != WMTAttributeFormatInvalid,
          .filter = has_capability(FormatCapability::Filter),
          .write = has_capability(FormatCapability::Write),
          .color = has_capability(FormatCapability::Color),
          .blend = has_capability(FormatCapability::Blend),
          .multisample = has_capability(FormatCapability::MSAA),
          .resolve = has_capability(FormatCapability::Resolve),
          .atomic = has_capability(FormatCapability::Atomic),
          .typed_uav_load = has_capability(FormatCapability::TextureBufferRead) ||
                            has_capability(FormatCapability::TextureBufferReadWrite),
          .typed_uav_store = has_capability(FormatCapability::TextureBufferWrite) ||
                             has_capability(FormatCapability::TextureBufferReadWrite),
      };

      auto support = BuildD3D12FormatSupport(out->Format, properties);
      out->Support1 = support.Support1;
      out->Support2 = support.Support2;
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
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(pHeapProps, HeapFlags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, pHeapProps);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, pHeapProps);
    if (FAILED(hr))
      return hr;
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
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&pDesc->Properties, pDesc->Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    return dxmt::CreateHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource(
      ID3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (!pHeap)
      return E_INVALIDARG;
    auto d3d12heap = static_cast<MTLD3D12Heap *>(pHeap);
    auto heap_desc = d3d12heap->GetDesc();
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&heap_desc.Properties, heap_desc.Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
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
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle
  ) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandle(HANDLE handle, REFIID riid, void **object) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  MakeResident(UINT ObjectCount, ID3D12Pageable *const *objects) {
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Evict(UINT ObjectCount, ID3D12Pageable *const *objects) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

  HRESULT STDMETHODCALLTYPE
  GetDeviceRemovedReason() {
    return S_OK;
  };

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *pDesc, UINT FirstSubresource, UINT SubresourceCount, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    UINT64 TotalBytes = 0;
    UINT64 Offset = 0;
    UINT BlockWidth = 1;
    do {
      if (!pDesc)
        break;

      UINT PlaneCount = 1;
      UINT PerPlaneSubresources = DecomposeSubresource(*pDesc, 0);
      DXGI_FORMAT PlaneFormats[2] = {};
      UINT PlaneBytesPerTexel[2] = {};

      MTL_DXGI_FORMAT_DESC FormatDesc;

      if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (pDesc->Format != DXGI_FORMAT_UNKNOWN)
          break;
        PlaneFormats[0] = DXGI_FORMAT_UNKNOWN;
        PlaneBytesPerTexel[0] = 1;
      } else {
        if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), pDesc->Format, FormatDesc)))
          break;

        if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && pDesc->Height != 1)
          break;

        if (FormatDesc.Flag & MTL_DXGI_FORMAT_BC)
          BlockWidth = 4;

        if (FormatDesc.PlanarCount > 1) {
          assert(FormatDesc.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER));
          PlaneCount = FormatDesc.PlanarCount;
          PlaneFormats[0] = DXGI_FORMAT_R32_TYPELESS;
          PlaneBytesPerTexel[0] = 4;
          PlaneFormats[1] = DXGI_FORMAT_R8_TYPELESS;
          PlaneBytesPerTexel[1] = 1;
        } else {
          PlaneFormats[0] = pDesc->Format;
          PlaneBytesPerTexel[0] = FormatDesc.BytesPerTexel;
          if (PlaneBytesPerTexel[0] == 0)
            IMPLEMENT_ME
        }
      }

      if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        PerPlaneSubresources *= pDesc->DepthOrArraySize;

      if (FirstSubresource >= PerPlaneSubresources * PlaneCount ||
          SubresourceCount > PerPlaneSubresources * PlaneCount - FirstSubresource) {
        WARN("GetCopyableFootprints: subresource is out of range");
        break;
      }

      for (unsigned i = 0; i < SubresourceCount; i++) {
        auto Subresource = FirstSubresource + i;
        auto Plane = 0u, MipLevel = 0u;
        DecomposeSubresource(*pDesc, Subresource, &MipLevel, NULL, &Plane);
        auto Extent = GetResourceExtent(*pDesc, MipLevel);
        auto Width = align(Extent.right, BlockWidth);
        auto Height = align(Extent.bottom, BlockWidth);
        auto RowCount = Height / BlockWidth;
        auto Depth = Extent.back;
        auto RowSize = (Width / BlockWidth) * PlaneBytesPerTexel[Plane];
        auto RowPitch = align(RowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount);
        if (pLayouts) {
          pLayouts[i].Offset = BaseOffset + Offset;
          pLayouts[i].Footprint.Format = PlaneFormats[Plane];
          pLayouts[i].Footprint.Width = Width;
          pLayouts[i].Footprint.Height = Height;
          pLayouts[i].Footprint.Depth = Depth;
          pLayouts[i].Footprint.RowPitch = RowPitch;
        }
        if (pNumRows)
          pNumRows[i] = RowCount;
        if (pRowSizeInBytes)
          pRowSizeInBytes[i] = RowSize;

        auto SubresourceSize = RowPitch * (RowCount - 1) + RowSize;
        SubresourceSize =
            align(SubresourceSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount) * (Depth - 1) + SubresourceSize;

        TotalBytes = Offset + SubresourceSize;
        Offset = align(TotalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
      if (pTotalBytes)
        *pTotalBytes = TotalBytes;
      return;
    } while (0);
    for (unsigned i = 0; i < SubresourceCount; i++) {
      if (pLayouts) {
        pLayouts[i].Offset = ~0ull;
        pLayouts[i].Footprint.Format = ~(DXGI_FORMAT)0u;
        pLayouts[i].Footprint.Width = ~0u;
        pLayouts[i].Footprint.Height = ~0u;
        pLayouts[i].Footprint.Depth = ~0u;
        pLayouts[i].Footprint.RowPitch = ~0u;
      }
      if (pNumRows)
        pNumRows[i] = ~0u;
      if (pRowSizeInBytes)
        pRowSizeInBytes[i] = ~0ull;
    }
    if (pTotalBytes)
      *pTotalBytes = UINT64_MAX;
  };

  HRESULT STDMETHODCALLTYPE
  CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
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
    return dxmt::CreateCommandSignature(this, pDesc, pRootSignature, riid, ppCommandSignature);
  };

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *pResource, UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo,
      D3D12_TILE_SHAPE *StandardTileShape, UINT *SubresourceTilingCount, UINT FirstSubresourceTiling,
      D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    IMPLEMENT_ME
  };

  LUID *STDMETHODCALLTYPE
  GetAdapterLuid(LUID *ret) {
    *ret = std::bit_cast<LUID>(__builtin_bswap64(adapter_->GetMTLDevice().registryID()));
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  CreatePipelineLibrary(const void *blob, SIZE_T blob_size, REFIID iid, void **lib) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *pFences, const UINT64 *pValues, UINT FenceCount, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
      HANDLE hEvent
  ) {
    if (!pFences || !pValues || !FenceCount ||
        (Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL && Flags != D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return E_INVALIDARG;
    try {
      std::vector<WMT::Reference<WMT::SharedEvent>> events;
      std::vector<WMTSharedEventValue> values;
      events.reserve(FenceCount);
      values.reserve(FenceCount);
      for (UINT i = 0; i < FenceCount; ++i) {
        if (!pFences[i])
          return E_INVALIDARG;
        // Allow other owned devices as well. The private device interface
        // identifies our implementation before accessing its native fence.
        Com<IMTLD3D12DeviceExt> owner;
        if (FAILED(pFences[i]->GetDevice(__uuidof(IMTLD3D12DeviceExt), reinterpret_cast<void **>(&owner))))
          return E_INVALIDARG;
        events.push_back(static_cast<MTLD3D12Fence *>(pFences[i])->fence->sharedEvent());
        values.push_back({events.back().handle, pValues[i]});
      }
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
      }
      return hr;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  };

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT ObjectCount, ID3D12Pageable *const *pObjects, const D3D12_RESIDENCY_PRIORITY *pPriorities) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    const char *stream_start = reinterpret_cast<const char *>(pDesc->pPipelineStateSubobjectStream);
    const char *stream_end = stream_start + pDesc->SizeInBytes;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc_cs{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_graphics{};
    {
      desc_graphics.DepthStencilState.DepthEnable = TRUE;
      desc_graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc_graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      desc_graphics.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      desc_graphics.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.BackFace = desc_graphics.DepthStencilState.FrontFace;
      desc_graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc_graphics.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      desc_graphics.RasterizerState.DepthClipEnable = TRUE;
      desc_graphics.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
      desc_graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc_graphics.SampleDesc.Count = 1;
      desc_graphics.SampleDesc.Quality = 0;
      desc_graphics.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    }

    uint32_t defined_type = 0;

    while (stream_start < stream_end) {
      if (stream_start + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream_end) {
        ERR("CreatePipelineState: invalid stream");
        return E_INVALIDARG;
      }
      auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(stream_start);

      if (defined_type & (1 << type)) {
        ERR("CreatePipelineState: duplicated subobejct type ", type);
        return E_INVALIDARG;
      }
      defined_type |= (1 << type);

#define GET_STREAM_DATA(data_type)                                                                                     \
  using subobject_t = struct {                                                                                         \
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                                                                          \
    data_type data;                                                                                                    \
  };                                                                                                                   \
  auto subobject = reinterpret_cast<subobject_t const *>(stream_start);                                                \
  if (stream_start + sizeof(*subobject) > stream_end) {                                                                \
    ERR("CreatePipelineState: invalid stream");                                                                        \
    return E_INVALIDARG;                                                                                               \
  }                                                                                                                    \
  stream_start += align(sizeof(*subobject), sizeof(void *));

      switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
        GET_STREAM_DATA(ID3D12RootSignature *);
        desc_cs.pRootSignature = subobject->data;
        desc_graphics.pRootSignature = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.VS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.PS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.DS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.HS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.GS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_cs.CS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
        GET_STREAM_DATA(D3D12_STREAM_OUTPUT_DESC);
        desc_graphics.StreamOutput = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: {
        GET_STREAM_DATA(D3D12_BLEND_DESC);
        desc_graphics.BlendState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: {
        GET_STREAM_DATA(UINT);
        desc_graphics.SampleMask = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: {
        GET_STREAM_DATA(D3D12_RASTERIZER_DESC);
        desc_graphics.RasterizerState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
        GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC);
        desc_graphics.DepthStencilState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
        GET_STREAM_DATA(D3D12_INPUT_LAYOUT_DESC);
        desc_graphics.InputLayout = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: {
        GET_STREAM_DATA(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        desc_graphics.IBStripCutValue = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: {
        GET_STREAM_DATA(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        desc_graphics.PrimitiveTopologyType = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
        GET_STREAM_DATA(D3D12_RT_FORMAT_ARRAY);
        memcpy(desc_graphics.RTVFormats, subobject->data.RTFormats, sizeof(desc_graphics.RTVFormats));
        desc_graphics.NumRenderTargets = subobject->data.NumRenderTargets;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: {
        GET_STREAM_DATA(DXGI_FORMAT);
        desc_graphics.DSVFormat = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
        GET_STREAM_DATA(DXGI_SAMPLE_DESC);
        desc_graphics.SampleDesc = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: {
        GET_STREAM_DATA(UINT);
        desc_graphics.NodeMask = subobject->data;
        desc_cs.NodeMask = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
        GET_STREAM_DATA(D3D12_CACHED_PIPELINE_STATE);
        desc_graphics.CachedPSO = subobject->data;
        desc_cs.CachedPSO = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: {
        GET_STREAM_DATA(D3D12_PIPELINE_STATE_FLAGS);
        desc_graphics.Flags = subobject->data;
        desc_cs.Flags = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
        GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC1);
        desc_graphics.DepthStencilState.StencilEnable = subobject->data.StencilEnable;
        desc_graphics.DepthStencilState.DepthEnable = subobject->data.DepthEnable;
        desc_graphics.DepthStencilState.DepthFunc = subobject->data.DepthFunc;
        desc_graphics.DepthStencilState.DepthWriteMask = subobject->data.DepthWriteMask;
        desc_graphics.DepthStencilState.StencilWriteMask = subobject->data.StencilWriteMask;
        desc_graphics.DepthStencilState.StencilReadMask = subobject->data.StencilReadMask;
        desc_graphics.DepthStencilState.BackFace = subobject->data.BackFace;
        desc_graphics.DepthStencilState.FrontFace = subobject->data.FrontFace;
        if (subobject->data.DepthBoundsTestEnable) {
          WARN("CreatePipelineState: ignore DepthBoundsTestEnable");
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
        GET_STREAM_DATA(D3D12_VIEW_INSTANCING_DESC);
        if (subobject->data.Flags) {
          WARN("CreatePipelineState: ignore ViewInstancing");
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        if (subobject->data.pShaderBytecode) {
          ERR("CreatePipelineState: unsupported AS");
          return E_NOTIMPL;
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        if (subobject->data.pShaderBytecode) {
          ERR("CreatePipelineState: unsupported MS");
          return E_NOTIMPL;
        }
        break;
      }
      default:
        ERR("CreatePipelineState: unhandled subobject type ", type);
        return E_INVALIDARG;
      }
    }

    if (desc_cs.CS.pShaderBytecode) {
      uint32_t incompatible_type =
          (1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS | 1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS |
           1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS);
      if (defined_type & incompatible_type) {
        ERR("CreatePipelineState: invalid compute pipeline state stream");
        return E_INVALIDARG;
      }
      return CreateComputePipelineState(&desc_cs, riid, ppPipelineState);
    }

    return CreateGraphicsPipelineState(&desc_graphics, riid, ppPipelineState);
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress(const void *pAddress, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFence,
      UINT64 FenceValue
  ) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList1(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void **ppCommandList
  ) {
    InitReturnPtr(ppCommandList);
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
    return E_NOTIMPL;
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo1(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount,
      const D3D12_RESOURCE_DESC *pDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    D3D12_RESOURCE_ALLOCATION_INFO1 resource_info;
    bool has_msaa_resource = false;

    __ret->SizeInBytes = 0;
    __ret->Alignment = 1;

    for (unsigned i = 0; i < ResourceDestCount; i++) {
      const D3D12_RESOURCE_DESC *desc = &pDescs[i];
      has_msaa_resource |= desc->SampleDesc.Count > 1;

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
          DEBUG("GetResourceAllocationInfo: invalid alignment ", desc->Alignment, " for buffer resource.\n");
          goto invalid;
        }
        resource_info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto size_and_align = GetMTLDevice().heapBufferSizeAndAlign(desc->Width, {});
        resource_info.SizeInBytes = size_and_align.size;
      } else {
        WMTTextureInfo texture_info;
        if (FAILED(PopulateWMTTextureInfo(GetMTLDevice(), texture_info, *desc))) {
          DEBUG("GetResourceAllocationInfo: invalid texture descriptor\n");
          goto invalid;
        }
        auto size_and_align = GetMTLDevice().heapTextureSizeAndAlign(texture_info);
        resource_info.SizeInBytes = size_and_align.size;
        resource_info.Alignment = size_and_align.align;
        auto requested_alignment = desc->Alignment              ? desc->Alignment
                                   : desc->SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                                : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resource_info.Alignment = std::max(resource_info.Alignment, requested_alignment);
      }

      resource_info.SizeInBytes = align(resource_info.SizeInBytes, resource_info.Alignment);
      resource_info.Offset = align(__ret->SizeInBytes, resource_info.Alignment);

      if (pAllocationInfos)
        pAllocationInfos[i] = resource_info;

      __ret->SizeInBytes = resource_info.Offset + resource_info.SizeInBytes;
      __ret->Alignment = std::max(__ret->Alignment, resource_info.Alignment);
    }

    __ret->SizeInBytes = align(__ret->SizeInBytes, __ret->Alignment);
    return __ret;

  invalid:

    __ret->SizeInBytes = ~(uint64_t)0;
    __ret->Alignment = has_msaa_resource ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                         : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return __ret;
  }

  WMT::ResidencySet
  GetGlobalResidencySet() {
    return residency_set_;
  };

  HRESULT
  RegisterResidency(WMT::Allocation allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    residency_set_.addAllocations(&allocation, 1);
    residency_set_.commit();
    return S_OK;
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
  UnregisterResidency(WMT::Allocation allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    residency_set_.removeAllocations(&allocation, 1);
    residency_set_.commit();
    return S_OK;
  }

  HRESULT
  RegisterResidencyAndVA(BufferAllocation *allocation) {
    std::unique_lock<dxmt::mutex> lock(residency_lock_);
    interval_map_.emplace(allocation->gpuAddress(), allocation);
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    auto buffer = allocation->buffer();
    residency_set_.addAllocations(&buffer, 1);
    residency_set_.commit();
    return S_OK;
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
    WMT::Allocation structure = state.sidecar.accelerationStructure();
    if (structure)
      residency_set_.removeAllocations(&structure, 1);
    WMT::Allocation instances = state.sidecar.instanceBuffer();
    if (instances)
      residency_set_.removeAllocations(&instances, 1);
    WMT::Allocation contributions = state.sidecar.contributionBuffer();
    if (contributions)
      residency_set_.removeAllocations(&contributions, 1);
  }

  HRESULT
  UnregisterResidencyAndVA(BufferAllocation *allocation) {
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
    residency_set_.removeAllocations(&buffer, 1);
    residency_set_.commit();
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
    info.options = WMTResourceHazardTrackingModeUntracked;
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
      auto write_function = library.newFunction("dxmt_as_write_u64");
      auto finalize_function = library.newFunction("dxmt_as_finalize_compacted_size");
      auto prepass = GetMTLDevice().newComputePipelineState(prepass_function, error);
      if (error || !prepass)
        return E_FAIL;
      error = {};
      auto write_u64 = GetMTLDevice().newComputePipelineState(write_function, error);
      if (error || !write_u64)
        return E_FAIL;
      error = {};
      auto finalize = GetMTLDevice().newComputePipelineState(finalize_function, error);
      if (error || !finalize)
        return E_FAIL;
      rtas_pipelines_.prepass = std::move(prepass);
      rtas_pipelines_.writeU64 = std::move(write_u64);
      rtas_pipelines_.finalizeCompactedSize = std::move(finalize);
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
    if (FAILED(raytracing::ConvertBuildInputs(*desc, ResolveRTASGPUVA, this, inputs)))
      return;
    raytracing::NativeSizes native;
    if (FAILED(QueryRTASSizes(inputs, native)))
      return;
    if (!raytracing::CalculatePrebuildInfo(native, &info->ResultDataMaxSizeInBytes,
            &info->ScratchDataSizeInBytes, &info->UpdateScratchDataSizeInBytes))
      *info = {};
  }

  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
  CheckDriverMatchingIdentifier(
      D3D12_SERIALIZED_DATA_TYPE, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier
  ) final {
    return identifier ? D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION
                      : D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
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

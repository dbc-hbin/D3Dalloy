#include "d3d12_pipeline_library.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "DXBCParser/PipelineLibrary.h"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "util_math.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt {
namespace {

// Stream subobjects newer than the build SDK headers; layouts follow Microsoft's d3d12.h.
constexpr uint32_t kSubobjectDepthStencil2 = 26;
constexpr uint32_t kSubobjectRasterizer1 = 27;
constexpr uint32_t kSubobjectRasterizer2 = 28;
constexpr uint32_t kSubobjectSerializedRootSignature = 29;

struct DepthStencilOpDesc1 {
  D3D12_STENCIL_OP StencilFailOp;
  D3D12_STENCIL_OP StencilDepthFailOp;
  D3D12_STENCIL_OP StencilPassOp;
  D3D12_COMPARISON_FUNC StencilFunc;
  UINT8 StencilReadMask;
  UINT8 StencilWriteMask;
};

struct DepthStencilDesc2 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  DepthStencilOpDesc1 FrontFace;
  DepthStencilOpDesc1 BackFace;
  BOOL DepthBoundsTestEnable;
};

struct RasterizerDesc1 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias;
  FLOAT DepthBiasClamp;
  FLOAT SlopeScaledDepthBias;
  BOOL DepthClipEnable;
  BOOL MultisampleEnable;
  BOOL AntialiasedLineEnable;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

enum LineRasterizationMode : UINT { LineAliased, LineAlphaAntialiased, LineQuadrilateralWide, LineQuadrilateralNarrow };

struct RasterizerDesc2 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias;
  FLOAT DepthBiasClamp;
  FLOAT SlopeScaledDepthBias;
  BOOL DepthClipEnable;
  LineRasterizationMode LineRasterizationMode;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

struct SerializedRootSignatureDesc {
  const void *pSerializedBlob;
  SIZE_T SerializedBlobSizeInBytes;
};

// D3D12_RASTERIZER_DESC1/2 DepthBias is FLOAT. The classic desc keeps the nearest INT (for callers
// that read it back); a fractional value is carried exactly in GraphicsPipelineExtras.
bool StoreDepthBias(FLOAT bias, INT &out, GraphicsPipelineExtras &extras) {
  if (!std::isfinite(bias))
    return false;
  const float clamped = std::clamp(bias, float(std::numeric_limits<INT>::min()), 2147483520.0f);
  out = INT(clamped);
  if (bias != float(out))
    extras.depth_bias = bias;
  else
    extras.depth_bias.reset();
  return true;
}

void CopyStencilFace(D3D12_DEPTH_STENCILOP_DESC &out, const DepthStencilOpDesc1 &in) {
  out = {in.StencilFailOp, in.StencilDepthFailOp, in.StencilPassOp, in.StencilFunc};
}

} // namespace

HRESULT ParsePipelineStateStream(MTLD3D12Device *device, const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc,
                                 PipelineStateStream &stream) {
  if (!pDesc || (pDesc->SizeInBytes && !pDesc->pPipelineStateSubobjectStream))
    return E_INVALIDARG;
  const char *stream_start = reinterpret_cast<const char *>(pDesc->pPipelineStateSubobjectStream);
  const char *stream_end = stream_start + pDesc->SizeInBytes;

  auto &desc_cs = stream.compute_desc;
  auto &desc_graphics = stream.graphics;
  desc_cs = {};
  desc_graphics = {};
  desc_graphics.DepthStencilState.DepthEnable = TRUE;
  desc_graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc_graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  desc_graphics.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  desc_graphics.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
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

  uint64_t defined_type = 0;
  constexpr uint64_t kDepthStencilTypes = 1ull << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL |
                                          1ull << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 |
                                          1ull << kSubobjectDepthStencil2;
  constexpr uint64_t kRasterizerTypes = 1ull << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER |
                                        1ull << kSubobjectRasterizer1 | 1ull << kSubobjectRasterizer2;
  constexpr uint64_t kRootTypes =
      1ull << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE | 1ull << kSubobjectSerializedRootSignature;

  while (stream_start < stream_end) {
    if (stream_start + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream_end) {
      ERR("CreatePipelineState: invalid stream");
      return E_INVALIDARG;
    }
    const uint32_t type = *reinterpret_cast<const uint32_t *>(stream_start);
    if (type >= 64) {
      ERR("CreatePipelineState: unhandled subobject type ", type);
      return E_INVALIDARG;
    }
    const uint64_t bit = 1ull << type;
    // Each state has one definition, whichever revision of its subobject carries it.
    const uint64_t group = bit & kDepthStencilTypes ? kDepthStencilTypes
                         : bit & kRasterizerTypes   ? kRasterizerTypes
                         : bit & kRootTypes         ? kRootTypes
                                                    : bit;
    if (defined_type & group) {
      ERR("CreatePipelineState: duplicated subobject type ", type);
      return E_INVALIDARG;
    }
    defined_type |= bit;

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
    case kSubobjectSerializedRootSignature: {
      GET_STREAM_DATA(SerializedRootSignatureDesc);
      if (!subobject->data.pSerializedBlob || !subobject->data.SerializedBlobSizeInBytes)
        return E_INVALIDARG;
      if (HRESULT hr = CreateRootSignature(device, 0, subobject->data.pSerializedBlob,
                                           subobject->data.SerializedBlobSizeInBytes, __uuidof(ID3D12RootSignature),
                                           reinterpret_cast<void **>(&stream.serialized_root));
          FAILED(hr))
        return hr;
      desc_cs.pRootSignature = stream.serialized_root.ptr();
      desc_graphics.pRootSignature = stream.serialized_root.ptr();
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
      stream.graphics_extras.depth_bias.reset();
      break;
    }
    case kSubobjectRasterizer1: {
      GET_STREAM_DATA(RasterizerDesc1);
      const auto &in = subobject->data;
      auto &out = desc_graphics.RasterizerState;
      if (!StoreDepthBias(in.DepthBias, out.DepthBias, stream.graphics_extras)) {
        ERR("CreatePipelineState: non-finite DepthBias");
        return E_INVALIDARG;
      }
      out.FillMode = in.FillMode;
      out.CullMode = in.CullMode;
      out.FrontCounterClockwise = in.FrontCounterClockwise;
      out.DepthBiasClamp = in.DepthBiasClamp;
      out.SlopeScaledDepthBias = in.SlopeScaledDepthBias;
      out.DepthClipEnable = in.DepthClipEnable;
      out.MultisampleEnable = in.MultisampleEnable;
      out.AntialiasedLineEnable = in.AntialiasedLineEnable;
      out.ForcedSampleCount = in.ForcedSampleCount;
      out.ConservativeRaster = in.ConservativeRaster;
      break;
    }
    case kSubobjectRasterizer2: {
      GET_STREAM_DATA(RasterizerDesc2);
      const auto &in = subobject->data;
      auto &out = desc_graphics.RasterizerState;
      if (!StoreDepthBias(in.DepthBias, out.DepthBias, stream.graphics_extras)) {
        ERR("CreatePipelineState: non-finite DepthBias");
        return E_INVALIDARG;
      }
      // Narrow quadrilateral lines need NarrowQuadrilateralLinesSupported, which is not reported.
      switch (in.LineRasterizationMode) {
      case LineAliased:
        out.MultisampleEnable = FALSE;
        out.AntialiasedLineEnable = FALSE;
        break;
      case LineAlphaAntialiased:
        out.MultisampleEnable = FALSE;
        out.AntialiasedLineEnable = TRUE;
        break;
      case LineQuadrilateralWide:
        out.MultisampleEnable = TRUE;
        out.AntialiasedLineEnable = FALSE;
        break;
      default:
        ERR("CreatePipelineState: unsupported LineRasterizationMode ", uint32_t(in.LineRasterizationMode));
        return E_INVALIDARG;
      }
      out.FillMode = in.FillMode;
      out.CullMode = in.CullMode;
      out.FrontCounterClockwise = in.FrontCounterClockwise;
      out.DepthBiasClamp = in.DepthBiasClamp;
      out.SlopeScaledDepthBias = in.SlopeScaledDepthBias;
      out.DepthClipEnable = in.DepthClipEnable;
      out.ForcedSampleCount = in.ForcedSampleCount;
      out.ConservativeRaster = in.ConservativeRaster;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
      GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC);
      desc_graphics.DepthStencilState = subobject->data;
      stream.graphics_extras.back_stencil_read_write_mask.reset();
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
      GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC1);
      stream.graphics_extras.depth_bounds = bool(subobject->data.DepthBoundsTestEnable);
      auto &out = desc_graphics.DepthStencilState;
      out.StencilEnable = subobject->data.StencilEnable;
      out.DepthEnable = subobject->data.DepthEnable;
      out.DepthFunc = subobject->data.DepthFunc;
      out.DepthWriteMask = subobject->data.DepthWriteMask;
      out.StencilWriteMask = subobject->data.StencilWriteMask;
      out.StencilReadMask = subobject->data.StencilReadMask;
      out.BackFace = subobject->data.BackFace;
      out.FrontFace = subobject->data.FrontFace;
      stream.graphics_extras.back_stencil_read_write_mask.reset();
      break;
    }
    case kSubobjectDepthStencil2: {
      GET_STREAM_DATA(DepthStencilDesc2);
      const auto &in = subobject->data;
      stream.graphics_extras.depth_bounds = bool(in.DepthBoundsTestEnable);
      // Distinct front/back masks (IndependentFrontAndBackStencilRefMaskSupported) are exact in
      // Metal's per-face stencil descriptors.
      if (in.FrontFace.StencilReadMask != in.BackFace.StencilReadMask ||
          in.FrontFace.StencilWriteMask != in.BackFace.StencilWriteMask)
        stream.graphics_extras.back_stencil_read_write_mask =
            std::pair<UINT8, UINT8>{in.BackFace.StencilReadMask, in.BackFace.StencilWriteMask};
      else
        stream.graphics_extras.back_stencil_read_write_mask.reset();
      auto &out = desc_graphics.DepthStencilState;
      out.DepthEnable = in.DepthEnable;
      out.DepthWriteMask = in.DepthWriteMask;
      out.DepthFunc = in.DepthFunc;
      out.StencilEnable = in.StencilEnable;
      out.StencilReadMask = in.FrontFace.StencilReadMask;
      out.StencilWriteMask = in.FrontFace.StencilWriteMask;
      CopyStencilFace(out.FrontFace, in.FrontFace);
      CopyStencilFace(out.BackFace, in.BackFace);
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
      if (subobject->data.NumRenderTargets > 8)
        return E_INVALIDARG;
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
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
      GET_STREAM_DATA(D3D12_VIEW_INSTANCING_DESC);
      // ViewInstancingTier 1. ViewInstanceCount 0 disables view instancing (SV_ViewID reads 0).
      const auto &in = subobject->data;
      if (in.ViewInstanceCount > D3D12_MAX_VIEW_INSTANCE_COUNT ||
          (in.Flags & ~D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING) ||
          (in.ViewInstanceCount && !in.pViewInstanceLocations)) {
        ERR("CreatePipelineState: invalid view instancing description");
        return E_INVALIDARG;
      }
      stream.graphics_extras.view_instancing.reset();
      if (!in.ViewInstanceCount)
        break;
      ViewInstancingState state;
      state.count = in.ViewInstanceCount;
      state.masking = in.Flags & D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING;
      for (UINT i = 0; i < in.ViewInstanceCount; ++i) {
        const auto &location = in.pViewInstanceLocations[i];
        if (location.ViewportArrayIndex >= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ||
            location.RenderTargetArrayIndex >= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
          ERR("CreatePipelineState: view instance location out of range");
          return E_INVALIDARG;
        }
        state.locations[i] = location;
      }
      stream.graphics_extras.view_instancing = state;
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
      GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
      // MeshShaderTier 1. An empty bytecode means the stage is absent.
      if (!subobject->data.pShaderBytecode != !subobject->data.BytecodeLength) {
        ERR("CreatePipelineState: invalid amplification/mesh shader bytecode");
        return E_INVALIDARG;
      }
      (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ? stream.graphics_extras.as : stream.graphics_extras.ms) =
          subobject->data;
      break;
    }
    default:
      ERR("CreatePipelineState: unhandled subobject type ", type);
      return E_INVALIDARG;
    }
#undef GET_STREAM_DATA
  }

  stream.compute = desc_cs.CS.pShaderBytecode != nullptr;
  if (stream.compute && (defined_type & (1ull << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS))) {
    ERR("CreatePipelineState: invalid compute pipeline state stream");
    return E_INVALIDARG;
  }
  // A mesh pipeline replaces the whole vertex front end: no VS/HS/DS/GS, input layout or stream
  // output, and an AS needs an MS.
  const auto &extras = stream.graphics_extras;
  if (extras.depth_bounds && !device->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7)) {
    ERR("CreatePipelineState: depth bounds requires native bounds or Apple7 per-sample capture");
    return E_INVALIDARG;
  }
  if (extras.as.pShaderBytecode && !extras.ms.pShaderBytecode) {
    ERR("CreatePipelineState: amplification shader without a mesh shader");
    return E_INVALIDARG;
  }
  if (extras.ms.pShaderBytecode &&
      (stream.compute || desc_graphics.VS.pShaderBytecode || desc_graphics.HS.pShaderBytecode ||
       desc_graphics.DS.pShaderBytecode || desc_graphics.GS.pShaderBytecode ||
       desc_graphics.InputLayout.NumElements || desc_graphics.StreamOutput.NumEntries)) {
    ERR("CreatePipelineState: mesh shader combined with vertex-pipeline state");
    return E_INVALIDARG;
  }
  return S_OK;
}

namespace {

enum PipelineKind : uint32_t { KindGraphics, KindCompute };

static_assert(sizeof(WMTPipelineCacheIdentity) == sizeof(DxmtPipelineLibraryHeader::identity));
static_assert(sizeof(Sha1Digest) == sizeof(DxmtPipelineLibraryHeader::digest));

Sha1Digest LibraryDigest(const uint8_t *bytes, size_t size) {
  Sha1HashState hash;
  hash.update(bytes, DXMT_PIPELINE_LIBRARY_DIGEST_BEGIN);
  hash.update(bytes + DXMT_PIPELINE_LIBRARY_DIGEST_END, size - DXMT_PIPELINE_LIBRARY_DIGEST_END);
  return hash.final();
}

/*
 * Named PSO payloads. Every entry owns a copy of its bytes, so the application
 * may release the blob it created the library from (D3D12 requires it to keep
 * the blob alive; copying makes that requirement unnecessary here). An entry
 * also keeps the PSO it produced, so later loads return that same object.
 */
class MTLD3D12PipelineLibrary : public MTLD3D12DeviceChild<ID3D12PipelineLibrary1> {
  struct Entry {
    PipelineKind kind;
    Sha1Digest key;
    std::vector<uint8_t> blob;
    std::mutex create_mutex;
    Com<ID3D12PipelineState> pipeline;
  };

  // Entries are never erased, so an Entry reference stays valid after the map lock is released.
  std::shared_mutex mutex_;
  std::unordered_map<std::wstring, Entry> entries_;
  // Deterministic order for Serialize: names in insertion order.
  std::vector<const std::wstring *> order_;
  // Serialized size and entry count of order_, checked by Rust on every insert.
  DxmtPipelineLibraryExtent extent_{DXMT_PIPELINE_LIBRARY_HEADER_SIZE, 0};

  HRESULT Insert(std::wstring name, PipelineKind kind, const Sha1Digest &key, const uint8_t *blob, size_t size,
                 ID3D12PipelineState *pipeline) {
    if (entries_.count(name))
      return E_INVALIDARG;
    DxmtPipelineLibraryExtent extent;
    switch (dxmt_pipeline_library_extend(&extent_, uint64_t(name.size()) * sizeof(WCHAR), size, &extent)) {
    case DXMT_PIPELINE_LIBRARY_SUCCESS:
      break;
    case DXMT_PIPELINE_LIBRARY_OVERFLOW:
      return E_OUTOFMEMORY;
    default:
      return E_INVALIDARG;
    }
    // Allocate before publishing, so extent_, order_ and entries_ stay consistent on bad_alloc.
    std::vector<uint8_t> copy(blob, blob + size);
    if (order_.size() == order_.capacity())
      order_.reserve(std::max<size_t>(1, order_.size() * 2));
    auto [it, inserted] = entries_.try_emplace(std::move(name));
    auto &entry = it->second;
    entry.kind = kind;
    entry.key = key;
    entry.blob = std::move(copy);
    entry.pipeline = pipeline;
    order_.push_back(&it->first);
    extent_ = extent;
    return S_OK;
  }

  template <typename Desc>
  HRESULT Load(const WCHAR *name, const Desc *desc, PipelineKind kind, REFIID riid, void **ppPipelineState,
               const GraphicsPipelineExtras *extras = nullptr) {
    InitReturnPtr(ppPipelineState);
    if (!name || !desc)
      return E_INVALIDARG;
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    Sha1Digest key;
    HRESULT hr;
    if constexpr (std::is_same_v<Desc, D3D12_COMPUTE_PIPELINE_STATE_DESC>)
      hr = PipelineCacheKey(*desc, key);
    else
      hr = PipelineCacheKey(*desc, key, extras);
    if (FAILED(hr))
      return hr;
    Entry *entry = nullptr;
    try {
      std::shared_lock lock(mutex_);
      auto it = entries_.find(name);
      if (it == entries_.end())
        return E_INVALIDARG;
      entry = &it->second;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    // The stored key is the whole-description key: any change of root signature,
    // shader or state makes it differ, which D3D12 reports as E_INVALIDARG.
    if (entry->kind != kind || entry->key != key)
      return E_INVALIDARG;
    std::lock_guard create_lock(entry->create_mutex);
    if (!entry->pipeline) {
      Desc restored = *desc;
      restored.CachedPSO = {entry->blob.data(), entry->blob.size()};
      Com<ID3D12PipelineState> pipeline;
      HRESULT hr;
      if constexpr (std::is_same_v<Desc, D3D12_COMPUTE_PIPELINE_STATE_DESC>)
        hr = CreateComputePipelineState(device_, &restored, __uuidof(ID3D12PipelineState),
                                        reinterpret_cast<void **>(&pipeline));
      else
        hr = CreateGraphicsPipelineState(device_, &restored, __uuidof(ID3D12PipelineState),
                                         reinterpret_cast<void **>(&pipeline), extras);
      if (FAILED(hr))
        return hr;
      entry->pipeline = std::move(pipeline);
    }
    return entry->pipeline->QueryInterface(riid, ppPipelineState);
  }

public:
  MTLD3D12PipelineLibrary(MTLD3D12Device *device) : MTLD3D12DeviceChild<ID3D12PipelineLibrary1>(device) {}

  HRESULT Initialize(const void *data, size_t size) {
    if (!size)
      return S_OK;
    const auto *bytes = static_cast<const uint8_t *>(data);
    DxmtPipelineLibraryHeader header;
    switch (dxmt_pipeline_library_header(bytes, size, &header)) {
    case DXMT_PIPELINE_LIBRARY_SUCCESS:
      break;
    case DXMT_PIPELINE_LIBRARY_VERSION_MISMATCH:
      return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    default:
      return E_INVALIDARG;
    }
    // Identity is checked before the digest so that a library from another GPU or
    // driver build reports the error an application acts on (rebuild the library).
    if (header.adapter != device_->GetMTLDevice().registryID())
      return D3D12_ERROR_ADAPTER_NOT_FOUND;
    WMTPipelineCacheIdentity identity;
    if (!PipelineCacheIdentity(identity))
      return E_FAIL;
    if (memcmp(&identity, header.identity, sizeof(identity)))
      return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    if (memcmp(LibraryDigest(bytes, size).data, header.digest, sizeof(header.digest)))
      return E_INVALIDARG;
    uint64_t offset = DXMT_PIPELINE_LIBRARY_HEADER_SIZE;
    try {
      for (uint32_t i = 0; i < header.count; ++i) {
        DxmtPipelineLibraryEntry record;
        if (dxmt_pipeline_library_entry(bytes, size, offset, &record) != DXMT_PIPELINE_LIBRARY_SUCCESS)
          return E_INVALIDARG;
        std::wstring name(record.name_bytes / sizeof(WCHAR), L'\0');
        memcpy(name.data(), bytes + record.name_offset, record.name_bytes);
        const uint8_t *blob = bytes + record.blob_offset;
        Sha1Digest key, blob_key;
        memcpy(key.data, record.key, sizeof(key.data));
        if (HRESULT hr = PipelineCacheBlobKey(blob, record.blob_size, blob_key); FAILED(hr))
          return hr;
        if (blob_key != key)
          return E_INVALIDARG;
        offset = record.next_offset;
        if (HRESULT hr = Insert(std::move(name), PipelineKind(record.kind), key, blob, record.blob_size, nullptr);
            FAILED(hr))
          return hr == E_OUTOFMEMORY ? hr : E_INVALIDARG;
      }
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    return offset == size ? S_OK : E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12PipelineLibrary) || riid == __uuidof(ID3D12PipelineLibrary1)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    if (logQueryInterfaceError(__uuidof(ID3D12PipelineLibrary1), riid))
      WARN("D3D12PipelineLibrary: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  StorePipeline(const WCHAR *name, ID3D12PipelineState *pipeline) override {
    if (!name || !pipeline)
      return E_INVALIDARG;
    Com<ID3D12Device> owner;
    if (FAILED(pipeline->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(device_))
      return E_INVALIDARG;
    Com<ID3DBlob> blob;
    if (HRESULT hr = pipeline->GetCachedBlob(&blob); FAILED(hr))
      return hr;
    const auto *bytes = static_cast<const uint8_t *>(blob->GetBufferPointer());
    Sha1Digest key;
    if (HRESULT hr = PipelineCacheBlobKey(bytes, blob->GetBufferSize(), key); FAILED(hr))
      return hr;
    const auto kind = static_cast<MTLD3D12PipelineState *>(pipeline)->IsComputePipelineState ? KindCompute
                                                                                              : KindGraphics;
    try {
      std::unique_lock lock(mutex_);
      return Insert(name, kind, key, bytes, blob->GetBufferSize(), pipeline);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  HRESULT STDMETHODCALLTYPE
  LoadGraphicsPipeline(const WCHAR *name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
                       void **ppPipelineState) override {
    return Load(name, desc, KindGraphics, riid, ppPipelineState);
  }

  HRESULT STDMETHODCALLTYPE
  LoadComputePipeline(const WCHAR *name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
                      void **ppPipelineState) override {
    return Load(name, desc, KindCompute, riid, ppPipelineState);
  }

  HRESULT STDMETHODCALLTYPE
  LoadPipeline(const WCHAR *name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid,
               void **ppPipelineState) override {
    InitReturnPtr(ppPipelineState);
    if (!name || !desc)
      return E_INVALIDARG;
    PipelineStateStream stream;
    if (HRESULT hr = ParsePipelineStateStream(device_, desc, stream); FAILED(hr))
      return hr;
    return stream.compute ? Load(name, &stream.compute_desc, KindCompute, riid, ppPipelineState)
                          : Load(name, &stream.graphics, KindGraphics, riid, ppPipelineState,
                                 &stream.graphics_extras);
  }

  SIZE_T STDMETHODCALLTYPE
  GetSerializedSize() override {
    std::shared_lock lock(mutex_);
    return extent_.size;
  }

  HRESULT STDMETHODCALLTYPE
  Serialize(void *data, SIZE_T size) override {
    if (!data)
      return E_INVALIDARG;
    WMTPipelineCacheIdentity identity;
    if (!PipelineCacheIdentity(identity))
      return E_FAIL;
    std::shared_lock lock(mutex_);
    if (size < extent_.size)
      return E_INVALIDARG;
    auto *bytes = static_cast<uint8_t *>(data);
    DxmtPipelineLibraryHeader header{};
    header.adapter = device_->GetMTLDevice().registryID();
    header.size = extent_.size;
    header.count = uint32_t(extent_.count);
    memcpy(header.identity, &identity, sizeof(identity));
    if (dxmt_pipeline_library_begin(&header, bytes, extent_.size) != DXMT_PIPELINE_LIBRARY_SUCCESS)
      return E_FAIL;
    uint64_t offset = DXMT_PIPELINE_LIBRARY_HEADER_SIZE;
    for (const auto *name : order_) {
      const auto &entry = entries_.at(*name);
      DxmtPipelineLibraryRecord record{};
      record.name_bytes = name->size() * sizeof(WCHAR);
      record.blob_size = entry.blob.size();
      record.kind = entry.kind;
      memcpy(record.key, entry.key.data, sizeof(record.key));
      DxmtPipelineLibraryEntry placed;
      if (dxmt_pipeline_library_put_entry(bytes, extent_.size, offset, &record, &placed) !=
          DXMT_PIPELINE_LIBRARY_SUCCESS)
        return E_FAIL;
      memcpy(bytes + placed.name_offset, name->data(), placed.name_bytes);
      memcpy(bytes + placed.blob_offset, entry.blob.data(), placed.blob_size);
      offset = placed.next_offset;
    }
    const auto digest = LibraryDigest(bytes, extent_.size);
    if (offset != extent_.size ||
        dxmt_pipeline_library_set_digest(bytes, extent_.size, digest.data) != DXMT_PIPELINE_LIBRARY_SUCCESS)
      return E_FAIL;
    return S_OK;
  }
};

} // namespace

HRESULT CreatePipelineLibrary(MTLD3D12Device *device, const void *blob, SIZE_T blob_size, REFIID riid,
                              void **library) {
  InitReturnPtr(library);
  if (device->RemovedReason() != S_OK)
    return DXGI_ERROR_DEVICE_REMOVED;
  if (!blob != !blob_size)
    return E_INVALIDARG;
  try {
    auto object = Com(new MTLD3D12PipelineLibrary(device));
    if (HRESULT hr = object->Initialize(blob, blob_size); FAILED(hr))
      return hr;
    if (!library)
      return S_FALSE;
    return object->QueryInterface(riid, library);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

} // namespace dxmt

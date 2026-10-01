#include "d3d12_device.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "../d3d10/d3d10_blob.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace dxmt {
namespace {

constexpr uint64_t kMagic = 0x004f5350544d5844; // "DXMTPSO\0", little endian.
// 2: graphics key covers the stream-output declaration. 3: it covers GS/HS/DS bytecode.
// 4: P3 hidden-resource/signature AIR ABI changed even for unchanged shader/PSO inputs.
// Serialized pipeline libraries validate this revision for every embedded PSO blob.
constexpr uint32_t kVersion = 4;

struct StageRecord {
  uint64_t library_size;
  uint64_t constant_size;
  uint64_t constant_alignment;
  uint64_t relocation_count;
};

struct CacheHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t stages;
  uint64_t size;
  uint64_t adapter;
  WMTPipelineCacheIdentity identity;
  Sha1Digest key;
  Sha1Digest digest;
  std::array<StageRecord, PipelineCache::StageCount> records;
};
static_assert(sizeof(CacheHeader) == 216);
static_assert(sizeof(SM50_CONSTANT_RELOCATION) == 16);

struct StageOffsets { size_t library, constants, relocations; };

struct NativeIdentity {
  WMTPipelineCacheIdentity value{};
  bool valid = WMTGetPipelineCacheIdentity(&value);
};

const NativeIdentity &Identity() {
  static const NativeIdentity identity;
  return identity;
}

Sha1Digest BlobDigest(const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  constexpr size_t begin = offsetof(CacheHeader, digest);
  constexpr size_t end = begin + sizeof(Sha1Digest);
  Sha1HashState hash;
  hash.update(bytes, begin);
  hash.update(bytes + end, size - end);
  return hash.final();
}

// Offsets are relative to an owned allocation, not to a possibly unaligned
// application pointer. Every region is bounded before its bytes are examined.
bool Layout(const CacheHeader &header, size_t limit,
            std::array<StageOffsets, PipelineCache::StageCount> &offsets, size_t &size) {
  size = sizeof(CacheHeader);
  if (limit < size)
    return false;
  auto region = [&](uint64_t length, size_t &offset) {
    if (length > limit - size)
      return false;
    offset = size;
    size += length;
    const size_t padding = (-size) & 7;
    if (padding > limit - size)
      return false;
    size += padding;
    return true;
  };
  for (uint32_t i = 0; i < PipelineCache::StageCount; ++i) {
    const auto &record = header.records[i];
    if (!(header.stages & (1u << i))) {
      if (record.library_size || record.constant_size || record.constant_alignment || record.relocation_count)
        return false;
      continue;
    }
    if (record.library_size < 4 || !record.constant_alignment ||
        (record.constant_alignment & (record.constant_alignment - 1)) ||
        record.constant_alignment > UINT32_MAX || record.constant_size > UINT32_MAX ||
        record.relocation_count > record.constant_size / sizeof(uint64_t) ||
        (!record.constant_size && record.constant_alignment != 1))
      return false;
    if (!region(record.library_size, offsets[i].library) ||
        !region(record.constant_size, offsets[i].constants) ||
        !region(record.relocation_count * sizeof(SM50_CONSTANT_RELOCATION), offsets[i].relocations))
      return false;
  }
  return true;
}

class KeyBuilder {
  Sha1HashState hash_;
  bool valid_ = true;
public:
  template <typename... T> void add(const T &...values) { (hash_.update(values), ...); }
  void bytes(const void *data, size_t size) {
    add(uint64_t(size));
    if (size && !data)
      valid_ = false;
    else if (size)
      hash_.update(data, size);
  }
  void shader(D3D12_SHADER_BYTECODE bytecode) { bytes(bytecode.pShaderBytecode, bytecode.BytecodeLength); }
  void root(ID3D12RootSignature *signature) {
    add(bool(signature));
    if (signature) {
      const void *data = nullptr;
      const auto size = static_cast<MTLD3D12RootSignature *>(signature)->GetBlob(&data);
      bytes(data, size);
    }
  }
  HRESULT finish(Sha1Digest &key) {
    if (!valid_)
      return E_INVALIDARG;
    key = hash_.final();
    return S_OK;
  }
};

} // namespace

HRESULT PipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Sha1Digest &key) {
  KeyBuilder hash;
  hash.add(uint32_t(2), kD3D12ShaderMetalVersion, desc.NodeMask, desc.Flags);
  hash.root(desc.pRootSignature);
  hash.shader(desc.CS);
  return hash.finish(key);
}

HRESULT PipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Sha1Digest &key,
                         const GraphicsPipelineExtras *extras) {
  if (desc.NumRenderTargets > 8 || (desc.InputLayout.NumElements && !desc.InputLayout.pInputElementDescs))
    return E_INVALIDARG;
  KeyBuilder hash;
  // Shader-visible ROV buffer ordering now has a hidden native resource. Old
  // cached AIR lacks its access and cannot be paired with the runtime marker.
  // 5: DXIL array and packed semantic rows map to metadata element IDs, not chunk ordinals.
  // Old cached AIR can read the wrong signature lanes after this correction.
  hash.add(uint32_t(5), kD3D12ShaderMetalVersion, desc.NodeMask, desc.Flags);
  hash.root(desc.pRootSignature);
  hash.shader(desc.VS);
  hash.shader(desc.PS);
  hash.shader(desc.DS);
  hash.shader(desc.HS);
  hash.shader(desc.GS);
  hash.add(desc.SampleMask, desc.IBStripCutValue, desc.PrimitiveTopologyType,
           desc.NumRenderTargets, desc.DSVFormat, desc.SampleDesc.Count, desc.SampleDesc.Quality);
  for (UINT i = 0; i < desc.NumRenderTargets; ++i)
    hash.add(desc.RTVFormats[i]);
  const auto &blend = desc.BlendState;
  hash.add(bool(blend.AlphaToCoverageEnable), bool(blend.IndependentBlendEnable));
  const UINT blend_count = blend.IndependentBlendEnable ? desc.NumRenderTargets : std::min(desc.NumRenderTargets, 1u);
  for (UINT i = 0; i < blend_count; ++i) {
    const auto &rt = blend.RenderTarget[i];
    hash.add(bool(rt.BlendEnable), bool(rt.LogicOpEnable), rt.SrcBlend, rt.DestBlend, rt.BlendOp,
             rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.BlendOpAlpha, rt.LogicOp, rt.RenderTargetWriteMask);
  }
  const auto &raster = desc.RasterizerState;
  hash.add(raster.FillMode, raster.CullMode, bool(raster.FrontCounterClockwise), raster.DepthBias,
           raster.DepthBiasClamp, raster.SlopeScaledDepthBias, bool(raster.DepthClipEnable),
           bool(raster.MultisampleEnable), bool(raster.AntialiasedLineEnable), raster.ForcedSampleCount,
           raster.ConservativeRaster);
  const auto &depth = desc.DepthStencilState;
  hash.add(bool(depth.DepthEnable), depth.DepthWriteMask, depth.DepthFunc, bool(depth.StencilEnable),
           depth.StencilReadMask, depth.StencilWriteMask);
  for (const auto *face : {&depth.FrontFace, &depth.BackFace})
    hash.add(face->StencilFailOp, face->StencilDepthFailOp, face->StencilPassOp, face->StencilFunc);
  // Stream-only state (fractional depth bias, back-face stencil masks); absent for classic descs.
  if (extras && !extras->empty()) {
    hash.add(uint32_t(0x45585452), bool(extras->depth_bias), extras->depth_bias.value_or(0.0f),
             bool(extras->back_stencil_read_write_mask));
    if (extras->back_stencil_read_write_mask)
      hash.add(extras->back_stencil_read_write_mask->first, extras->back_stencil_read_write_mask->second);
    if (extras->view_instancing) {
      hash.add(uint32_t(0x56494e53), extras->view_instancing->count, extras->view_instancing->masking);
      for (UINT i = 0; i < extras->view_instancing->count; ++i)
        hash.add(extras->view_instancing->locations[i].ViewportArrayIndex,
                 extras->view_instancing->locations[i].RenderTargetArrayIndex);
    }
    if (extras->ms.pShaderBytecode) {
      hash.add(uint32_t(0x4d455348));
      hash.shader(extras->as);
      hash.shader(extras->ms);
    }
  }
  hash.add(desc.InputLayout.NumElements);
  for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
    const auto &element = desc.InputLayout.pInputElementDescs[i];
    if (!element.SemanticName)
      return E_INVALIDARG;
    hash.bytes(element.SemanticName, std::strlen(element.SemanticName));
    hash.add(element.SemanticIndex, element.Format, element.InputSlot, element.AlignedByteOffset,
             element.InputSlotClass, element.InstanceDataStepRate);
  }
  // Stream output selects the VS capture epilogue, and RasterizedStream selects the
  // rasterization-disabled (void) VS variant without a fragment stage.
  const auto &so = desc.StreamOutput;
  if ((so.NumEntries && !so.pSODeclaration) || (so.NumStrides && !so.pBufferStrides))
    return E_INVALIDARG;
  hash.add(so.NumEntries, so.NumStrides, so.RasterizedStream);
  for (UINT i = 0; i < so.NumEntries; ++i) {
    const auto &entry = so.pSODeclaration[i];
    hash.add(entry.Stream, bool(entry.SemanticName));
    if (entry.SemanticName)
      hash.bytes(entry.SemanticName, std::strlen(entry.SemanticName));
    hash.add(entry.SemanticIndex, entry.StartComponent, entry.ComponentCount, entry.OutputSlot);
  }
  for (UINT i = 0; i < so.NumStrides; ++i)
    hash.add(so.pBufferStrides[i]);
  return hash.finish(key);
}

bool PipelineCacheIdentity(WMTPipelineCacheIdentity &identity) {
  const auto &native = Identity();
  identity = native.value;
  return native.valid;
}

HRESULT PipelineCacheBlobKey(const void *data, size_t size, Sha1Digest &key) {
  if (!data || size < sizeof(CacheHeader))
    return E_INVALIDARG;
  CacheHeader header;
  memcpy(&header, data, sizeof(header));
  if (header.magic != kMagic)
    return E_INVALIDARG;
  if (header.version != kVersion)
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  if (header.size != size)
    return E_INVALIDARG;
  key = header.key;
  return S_OK;
}

HRESULT PipelineCache::Initialize(const D3D12_CACHED_PIPELINE_STATE &cached, const Sha1Digest &key,
                                  uint32_t stages, uint64_t adapter) {
  key_ = key;
  stages_ = stages;
  adapter_ = adapter;
  if (!cached.CachedBlobSizeInBytes) {
    // A cold PSO compiles every stage; a restored one (below) compiles none.
    owned_graphics::Trace("pipeline_cache_cold", ",\"stages\":", stages);
    return S_OK;
  }
  if (!cached.pCachedBlob || cached.CachedBlobSizeInBytes < sizeof(CacheHeader))
    return E_INVALIDARG;
  CacheHeader header;
  memcpy(&header, cached.pCachedBlob, sizeof(header));
  if (header.magic != kMagic)
    return E_INVALIDARG;
  if (header.version != kVersion)
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  if (header.size != cached.CachedBlobSizeInBytes || header.stages != stages || header.key != key)
    return E_INVALIDARG;
  std::array<StageOffsets, StageCount> offsets{};
  size_t size = 0;
  if (!Layout(header, cached.CachedBlobSizeInBytes, offsets, size) || size != header.size ||
      BlobDigest(cached.pCachedBlob, size) != header.digest)
    return E_INVALIDARG;
  if (header.adapter != adapter)
    return D3D12_ERROR_ADAPTER_NOT_FOUND;
  const auto &identity = Identity();
  if (!identity.valid)
    return E_FAIL;
  if (memcmp(&identity.value, &header.identity, sizeof(header.identity)))
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  const auto *input = static_cast<const uint8_t *>(cached.pCachedBlob);
  for (uint32_t i = 0; i < StageCount; ++i) {
    if (!(stages & (1u << i)))
      continue;
    const auto &record = header.records[i];
    if (memcmp(input + offsets[i].library, "MTLB", 4))
      return E_INVALIDARG;
    for (uint64_t j = 0; j < record.relocation_count; ++j) {
      SM50_CONSTANT_RELOCATION relocation;
      memcpy(&relocation, input + offsets[i].relocations + j * sizeof(relocation), sizeof(relocation));
      if (relocation.Offset > record.constant_size - sizeof(uint64_t) ||
          (relocation.TargetOffset > record.constant_size &&
           relocation.TargetOffset != SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE))
        return E_INVALIDARG;
    }
  }
  try {
    blob_.assign(input, input + size);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  for (uint32_t i = 0; i < StageCount; ++i) {
    if (!(stages & (1u << i)))
      continue;
    const auto &record = header.records[i];
    auto &view = views_[i];
    view.Data = blob_.data() + offsets[i].library;
    view.Size = record.library_size;
    view.ConstantData = record.constant_size ? blob_.data() + offsets[i].constants : nullptr;
    view.ConstantDataSize = record.constant_size;
    view.ConstantDataAlignment = record.constant_alignment;
    view.ConstantRelocations = record.relocation_count ? blob_.data() + offsets[i].relocations : nullptr;
    view.ConstantRelocationCount = record.relocation_count;
  }
  owned_graphics::Trace("pipeline_cache_restore", ",\"stages\":", stages, ",\"bytes\":", uint64_t(size));
  return S_OK;
}

sm50_bitcode_t PipelineCache::bitcode(Stage stage) {
  return loaded() ? sm50_bitcode_t{} : sm50_bitcode_t(compiled_[stage]);
}

SM50_COMPILED_BITCODE PipelineCache::view(Stage stage) {
  if (loaded())
    return views_[stage];
  SM50_COMPILED_BITCODE result{};
  if (sm50_bitcode_t bitcode = compiled_[stage])
    SM50GetCompiledBitcode(bitcode, &result);
  return result;
}

HRESULT PipelineCache::GetBlob(ID3DBlob **blob) {
  if (!blob)
    return E_POINTER;
  *blob = nullptr;
  try {
    if (loaded()) {
      const HRESULT hr = CreateBlobFromMalloc(blob_.size(), blob);
      if (FAILED(hr))
        return hr;
      memcpy((*blob)->GetBufferPointer(), blob_.data(), blob_.size());
      return S_OK;
    }
    const auto &identity = Identity();
    if (!identity.valid)
      return E_FAIL;
    CacheHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.stages = stages_;
    header.adapter = adapter_;
    header.identity = identity.value;
    header.key = key_;
    std::array<SM50_COMPILED_BITCODE, StageCount> views{};
    for (uint32_t i = 0; i < StageCount; ++i) {
      if (!(stages_ & (1u << i)))
        continue;
      auto &compiled = views[i] = view(Stage(i));
      header.records[i] = {compiled.Size, compiled.ConstantDataSize, compiled.ConstantDataAlignment,
                           compiled.ConstantRelocationCount};
    }
    std::array<StageOffsets, StageCount> offsets{};
    size_t size = 0;
    if (!Layout(header, std::numeric_limits<size_t>::max(), offsets, size))
      return E_FAIL;
    header.size = size;
    const HRESULT hr = CreateBlobFromMalloc(size, blob);
    if (FAILED(hr))
      return hr;
    auto *bytes = static_cast<uint8_t *>((*blob)->GetBufferPointer());
    memset(bytes, 0, size);
    memcpy(bytes, &header, sizeof(header));
    for (uint32_t i = 0; i < StageCount; ++i) {
      if (!(stages_ & (1u << i)))
        continue;
      const auto &compiled = views[i];
      const sm50_bitcode_t source = bitcode(Stage(i));
      if (!SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_LIBRARY, compiled.Data,
                                   bytes + offsets[i].library, compiled.Size) ||
          !SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_CONSTANTS, compiled.ConstantData,
                                   bytes + offsets[i].constants, compiled.ConstantDataSize) ||
          !SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_RELOCATIONS, compiled.ConstantRelocations,
                                   bytes + offsets[i].relocations,
                                   compiled.ConstantRelocationCount * sizeof(SM50_CONSTANT_RELOCATION))) {
        (*blob)->Release();
        *blob = nullptr;
        return E_FAIL;
      }
    }
    const auto digest = BlobDigest(bytes, size);
    memcpy(bytes + offsetof(CacheHeader, digest), &digest, sizeof(digest));
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

} // namespace dxmt

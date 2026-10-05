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

#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_sampler_feedback.hpp"
#include "dxmt_sampler.hpp"
#include "util_math.hpp"
#include "util_md5.hpp"
#include <cstring>
#include <set>
#include <cmath>
#include <new>
#include <type_traits>
#include <vector>
#include "../d3d10/d3d10_blob.hpp"
#include "../airconv/dxbc_root_signature.hpp"

namespace dxmt {

/* Every global/local root used by one program shares this register-keyed immutable table. */
HRESULT AppendRootStaticSamplers(
    MTLD3D12RootSignature *root, std::vector<D3D12_STATIC_SAMPLER_DESC> &table, UINT *appended
) {
  *appended = 0;
  if (!root) return S_OK;
  const void *blob = nullptr, *raw = nullptr;
  const UINT blob_size = root->GetBlob(&blob); UINT raw_size = 0;
  if (!blob_size || !blob || FAILED(microsoft::DXBCGetRootSignature(blob, &raw, &raw_size))) return E_INVALIDARG;
  RootSignatureDeserializer deserializer;
  if (FAILED(deserializer.Deserialize(raw, raw_size))) return E_INVALIDARG;
  const auto &desc = deserializer.desc_1_1_.Desc_1_1;
  for (UINT i = 0; i < desc.NumStaticSamplers; ++i) {
    const auto &state = desc.pStaticSamplers[i];
    bool known = false;
    for (const auto &existing : table) {
      if (existing.RegisterSpace != state.RegisterSpace || existing.ShaderRegister != state.ShaderRegister) continue;
      if (std::memcmp(&existing, &state, sizeof(state))) {
        ERR("Root signature static sampler definitions conflict"); return E_INVALIDARG;
      }
      known = true; break;
    }
    if (known) continue;
    table.push_back(state); ++*appended;
  }
  return S_OK;
}

namespace {

struct RootBinding {
  D3D12_DESCRIPTOR_RANGE_TYPE type;
  UINT shader_register;
  UINT register_space;
  UINT64 count;
  D3D12_SHADER_VISIBILITY visibility;
};

inline D3D12_DESCRIPTOR_RANGE_FLAGS RangeFlags(const D3D12_DESCRIPTOR_RANGE &) {
  return D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
}
inline D3D12_DESCRIPTOR_RANGE_FLAGS RangeFlags(const D3D12_DESCRIPTOR_RANGE1 &range) {
  return range.Flags;
}
inline D3D12_ROOT_DESCRIPTOR_FLAGS DescriptorFlags(const D3D12_ROOT_DESCRIPTOR &) {
  return D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
}
inline D3D12_ROOT_DESCRIPTOR_FLAGS DescriptorFlags(const D3D12_ROOT_DESCRIPTOR1 &descriptor) {
  return descriptor.Flags;
}

bool ValidVisibility(D3D12_SHADER_VISIBILITY visibility) {
  // AMPLIFICATION (6) and MESH (7) exist with MeshShaderTier 1.
  return visibility >= D3D12_SHADER_VISIBILITY_ALL && visibility <= D3D12_SHADER_VISIBILITY_MESH;
}
bool VisibilitiesOverlap(D3D12_SHADER_VISIBILITY a, D3D12_SHADER_VISIBILITY b) {
  return a == D3D12_SHADER_VISIBILITY_ALL || b == D3D12_SHADER_VISIBILITY_ALL || a == b;
}

bool ValidFilter(D3D12_FILTER filter) {
  const UINT value = static_cast<UINT>(filter);
  const UINT reduction = (value >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) & D3D12_FILTER_REDUCTION_TYPE_MASK;
  const UINT min = (value >> D3D12_MIN_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
  const UINT mag = (value >> D3D12_MAG_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
  const UINT mip = (value >> D3D12_MIP_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
  if ((value & ~0x1d5u) || reduction > D3D12_FILTER_REDUCTION_TYPE_MAXIMUM || min > 1 || mag > 1 || mip > 1)
    return false;
  return !(value & D3D12_ANISOTROPIC_FILTERING_BIT) || (min == 1 && mag == 1 && mip == 1);
}

inline UINT SamplerFlags(const D3D12_STATIC_SAMPLER_DESC &) { return D3D12_SAMPLER_FLAG_NONE; }
inline UINT SamplerFlags(const D3D12_STATIC_SAMPLER_DESC1 &sampler) { return sampler.Flags; }

/*
 * A static sampler of any root signature version. 1.0/1.1 samplers carry no flags and
 * only the three float border colors; 1.2 samplers (D3D12_STATIC_SAMPLER_DESC1) add
 * the two *_UINT border colors and D3D12_SAMPLER_FLAGS, validated per the
 * VulkanOn12 spec: UINT_BORDER_COLOR must not be combined with a float border color,
 * and NON_NORMALIZED_COORDINATES restricts the sampler to point/linear min=mag with
 * point mips, no comparison/anisotropy, MinLOD = MaxLOD = 0 and CLAMP/BORDER U,V.
 */
template <typename STATIC_SAMPLER>
bool ValidStaticSampler(const STATIC_SAMPLER &sampler) {
  const auto valid_address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
    return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
  };
  constexpr bool v12 = std::is_same_v<STATIC_SAMPLER, D3D12_STATIC_SAMPLER_DESC1>;
  const UINT border = static_cast<UINT>(sampler.BorderColor);
  const UINT max_border = v12 ? kStaticBorderColorOpaqueWhiteUint
                              : static_cast<UINT>(D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);
  if (!(ValidFilter(sampler.Filter) && valid_address(sampler.AddressU) && valid_address(sampler.AddressV) &&
        valid_address(sampler.AddressW) && std::isfinite(sampler.MipLODBias) && sampler.MipLODBias >= -16.0f &&
        sampler.MipLODBias <= 15.99f && sampler.MaxAnisotropy >= 1 && sampler.MaxAnisotropy <= 16 &&
        sampler.ComparisonFunc >= D3D12_COMPARISON_FUNC_NEVER &&
        sampler.ComparisonFunc <= D3D12_COMPARISON_FUNC_ALWAYS && border <= max_border &&
        !std::isnan(sampler.MinLOD) && !std::isnan(sampler.MaxLOD) && sampler.MinLOD <= sampler.MaxLOD &&
        ValidVisibility(sampler.ShaderVisibility)))
    return false;
  const UINT flags = SamplerFlags(sampler);
  if (flags & ~UINT(D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR | D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES))
    return false;
  if ((flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) && border <= D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE)
    return false;
  if (flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES) {
    if (!sampler_feedback::ValidNonNormalizedSampler(sampler))
      return false;
  }
  return true;
}

template <typename ROOT_SIGNATURE_DESC>
HRESULT ValidateRootSignatureDesc(const ROOT_SIGNATURE_DESC &desc) {
  using ROOT_PARAMETER = typename GetType<ROOT_SIGNATURE_DESC>::ROOT_PARAMETER;
  using DESCRIPTOR_RANGE = typename GetType<ROOT_SIGNATURE_DESC>::DESCRIPTOR_RANGE;
  const UINT signature_flags = static_cast<UINT>(desc.Flags);
  // 0x400/0x800 are the SM 6.6 heap-directly-indexed flags; like every other flag they
  // are rejected on a local root signature, which must carry exactly 0x80.
  if ((desc.NumParameters && !desc.pParameters) || (desc.NumStaticSamplers && !desc.pStaticSamplers) ||
      (signature_flags & ~0xfffu) || ((signature_flags & 0x80u) && signature_flags != 0x80u))
    return E_INVALIDARG;

  const bool local = (signature_flags & 0x80u) != 0;
  UINT root_cost = 0;
  UINT local_bytes = 0;
  // The 4 KiB record cap includes the 32-byte shader identifier.
  constexpr UINT local_payload_limit = static_cast<UINT>(
      raytracing::kMaxShaderRecordSize - raytracing::kShaderIdentifierSize);
  const UINT root_cost_limit = local ? local_payload_limit / 4u : D3D12_MAX_ROOT_COST;
  std::vector<RootBinding> bindings;
  try {
    bindings.reserve(desc.NumParameters + desc.NumStaticSamplers);
    for (UINT i = 0; i < desc.NumParameters; ++i) {
      const ROOT_PARAMETER &parameter = desc.pParameters[i];
      if (!ValidVisibility(parameter.ShaderVisibility))
        return E_INVALIDARG;
      switch (parameter.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        const auto &table = parameter.DescriptorTable;
        if (!table.NumDescriptorRanges || !table.pDescriptorRanges || root_cost == root_cost_limit)
          return E_INVALIDARG;
        ++root_cost;
        if (local) {
          local_bytes = (local_bytes + 7u) & ~7u;
          if (local_bytes > local_payload_limit - 8u)
            return E_INVALIDARG;
          local_bytes += 8u;
        }
        const bool sampler_heap = table.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        UINT64 previous_end = 0;
        std::vector<std::pair<UINT64, UINT64>> spans;
        spans.reserve(table.NumDescriptorRanges);
        for (UINT j = 0; j < table.NumDescriptorRanges; ++j) {
          const DESCRIPTOR_RANGE &range = table.pDescriptorRanges[j];
          if (range.RangeType < D3D12_DESCRIPTOR_RANGE_TYPE_SRV || range.RangeType > D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ||
              !range.NumDescriptors || ((range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) != sampler_heap))
            return E_INVALIDARG;
          const UINT flags = static_cast<UINT>(RangeFlags(range));
          const UINT data_flags = flags & 0xeu;
          constexpr UINT keep_bounds = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS;
          if ((flags & ~(0xfu | keep_bounds)) ||
              (data_flags && data_flags != 0x2u && data_flags != 0x4u && data_flags != 0x8u) ||
              ((flags & 0x1u) && ((flags & keep_bounds) || data_flags == 0x8u)) ||
              (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && (flags & ~0x1u)))
            return E_INVALIDARG;
          const UINT64 offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                  ? previous_end : range.OffsetInDescriptorsFromTableStart;
          const UINT64 end = offset + range.NumDescriptors;
          if (end > UINT64(UINT32_MAX) + 1 || (range.NumDescriptors == UINT_MAX && j + 1 != table.NumDescriptorRanges))
            return E_INVALIDARG;
          for (const auto &span : spans)
            if (offset < span.second && span.first < end)
              return E_INVALIDARG;
          spans.emplace_back(offset, end);
          previous_end = end;
          if (UINT64(range.BaseShaderRegister) + range.NumDescriptors > UINT64(UINT32_MAX) + 1)
            return E_INVALIDARG;
          bindings.push_back({range.RangeType, range.BaseShaderRegister, range.RegisterSpace,
                              range.NumDescriptors, parameter.ShaderVisibility});
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        if (!parameter.Constants.Num32BitValues || parameter.Constants.Num32BitValues > D3D12_MAX_ROOT_COST - root_cost)
          return E_INVALIDARG;
        root_cost += parameter.Constants.Num32BitValues;
        if (local) {
          if (parameter.Constants.Num32BitValues > (local_payload_limit - local_bytes) / 4u)
            return E_INVALIDARG;
          local_bytes += parameter.Constants.Num32BitValues * 4u;
        }
        bindings.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_CBV, parameter.Constants.ShaderRegister,
                            parameter.Constants.RegisterSpace, 1, parameter.ShaderVisibility});
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV: {
        if (root_cost > D3D12_MAX_ROOT_COST - 2)
          return E_INVALIDARG;
        root_cost += 2;
        if (local) {
          local_bytes = (local_bytes + 7u) & ~7u;
          if (local_bytes > local_payload_limit - 8u)
            return E_INVALIDARG;
          local_bytes += 8u;
        }
        const UINT flags = static_cast<UINT>(DescriptorFlags(parameter.Descriptor));
        const UINT data_flags = flags & 0xeu;
        if ((flags & ~0xeu) || (data_flags && data_flags != 0x2u && data_flags != 0x4u && data_flags != 0x8u))
          return E_INVALIDARG;
        const auto type = parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV :
                          parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV :
                                                                                    D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        bindings.push_back({type, parameter.Descriptor.ShaderRegister, parameter.Descriptor.RegisterSpace, 1,
                            parameter.ShaderVisibility});
        break;
      }
      default:
        return E_INVALIDARG;
      }
    }
    for (UINT i = 0; i < desc.NumStaticSamplers; ++i) {
      const auto &sampler = desc.pStaticSamplers[i];
      if (!ValidStaticSampler(sampler))
        return E_INVALIDARG;
      bindings.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, sampler.ShaderRegister, sampler.RegisterSpace, 1,
                          sampler.ShaderVisibility});
    }
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }

  for (size_t i = 0; i < bindings.size(); ++i) {
    const auto &a = bindings[i];
    const UINT64 a_end = UINT64(a.shader_register) + a.count;
    for (size_t j = i + 1; j < bindings.size(); ++j) {
      const auto &b = bindings[j];
      if (a.type != b.type || a.register_space != b.register_space || !VisibilitiesOverlap(a.visibility, b.visibility))
        continue;
      const UINT64 b_end = UINT64(b.shader_register) + b.count;
      if (UINT64(a.shader_register) < b_end && UINT64(b.shader_register) < a_end)
        return E_INVALIDARG;
    }
  }
  return S_OK;
}

} // namespace

#pragma region Serialization

template <typename ROOT_SIGNATURE_DESC>
HRESULT
SerializeVersionedRootSignatureImpl(
    const ROOT_SIGNATURE_DESC *pDesc, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob
) {
  if (!pDesc)
    return E_INVALIDARG;
  if (HRESULT hr = ValidateRootSignatureDesc(*pDesc); FAILED(hr))
    return hr;

  using ROOT_PARAMETER = GetType<ROOT_SIGNATURE_DESC>::ROOT_PARAMETER;
  using ROOT_DESCRIPTOR = GetType<ROOT_SIGNATURE_DESC>::ROOT_DESCRIPTOR;
  using DESCRIPTOR_RANGE = GetType<ROOT_SIGNATURE_DESC>::DESCRIPTOR_RANGE;

  struct segment {
    size_t offset;
    size_t size_aligned;
    void *ptr;
    segment(size_t offset, size_t size) {
      this->offset = offset;
      size_aligned = align(size, 4);
      ptr = malloc(size_aligned);
      if (ptr)
        memset(ptr, 0, size_aligned);
    }
    ~segment() {
      free(ptr);
    };
    segment(const segment &copy) = delete;
    segment(segment &&move) {
      ptr = move.ptr;
      size_aligned = move.size_aligned;
      offset = move.offset;
      move.ptr = nullptr;
      move.size_aligned = 0;
      move.offset = 0;
    };
  };
  std::vector<segment> segments;
  size_t total_offset = 0;

  auto add_segment = [&]<typename TYPE>(size_t count, TYPE **ptr) -> size_t {
    if (count == 0) {
      *ptr = nullptr;
      return total_offset;
    }
    size_t size = sizeof(TYPE) * count;
    segments.emplace_back(total_offset, size);
    total_offset += size;
    *ptr = reinterpret_cast<TYPE *>(segments.back().ptr);
    return segments.back().offset;
  };

  RawRootSignatureDesc *RootSig = nullptr;

  add_segment(1, &RootSig);
  if (!RootSig)
    return E_OUTOFMEMORY;

  RootSig->Version = (uint32_t)Version;
  RootSig->Flags = (uint32_t)pDesc->Flags;
  RootSig->NumParameters = pDesc->NumParameters;
  RootSig->NumStaticSamplers = pDesc->NumStaticSamplers;

  /* Root Parameters */

  RawRootParameter *Parameters = nullptr;
  RootSig->RootParametersOffset = add_segment(pDesc->NumParameters, &Parameters);
  if (pDesc->NumParameters && !Parameters)
    return E_OUTOFMEMORY;

  for (UINT i = 0; i < pDesc->NumParameters; i++) {
    const ROOT_PARAMETER *RootParameter = &pDesc->pParameters[i];
    RawRootParameter *Out = &Parameters[i];
    Out->ParameterType = (uint32_t)RootParameter->ParameterType;
    Out->ShaderVisibility = (uint32_t)RootParameter->ShaderVisibility;
    switch (RootParameter->ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      RawRootDescriptorTable *OutTable = nullptr;
      Out->PayloadOffset = add_segment(1, &OutTable);
      if (!OutTable)
        return E_OUTOFMEMORY;
      OutTable->NumDescriptorRanges = RootParameter->DescriptorTable.NumDescriptorRanges;
      DESCRIPTOR_RANGE *OutRanges = nullptr;
      OutTable->DescriptorRangesOffset = add_segment(RootParameter->DescriptorTable.NumDescriptorRanges, &OutRanges);
      if (OutTable->NumDescriptorRanges && !OutRanges)
        return E_OUTOFMEMORY;
      for (UINT j = 0; j < RootParameter->DescriptorTable.NumDescriptorRanges; j++) {
        const DESCRIPTOR_RANGE *Range = &RootParameter->DescriptorTable.pDescriptorRanges[j];
        DESCRIPTOR_RANGE *OutRange = &OutRanges[j];
        OutRange->RangeType = Range->RangeType;
        OutRange->NumDescriptors = Range->NumDescriptors;
        OutRange->BaseShaderRegister = Range->BaseShaderRegister;
        OutRange->RegisterSpace = Range->RegisterSpace;
        OutRange->OffsetInDescriptorsFromTableStart = Range->OffsetInDescriptorsFromTableStart;
        SetFlag(OutRange, Range);
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
      D3D12_ROOT_CONSTANTS *OutConstants = nullptr;
      Out->PayloadOffset = add_segment(1, &OutConstants);
      if (!OutConstants)
        return E_OUTOFMEMORY;
      *OutConstants = RootParameter->Constants;
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV: {
      ROOT_DESCRIPTOR *OutDescriptor = nullptr;
      Out->PayloadOffset = add_segment(1, &OutDescriptor);
      if (!OutDescriptor)
        return E_OUTOFMEMORY;
      OutDescriptor->ShaderRegister = RootParameter->Descriptor.ShaderRegister;
      OutDescriptor->RegisterSpace = RootParameter->Descriptor.RegisterSpace;
      SetFlag(OutDescriptor, &RootParameter->Descriptor);
      break;
    }
    }
  }

  /* Static Samplers */

  /* 1.2 stores D3D12_STATIC_SAMPLER_DESC1 (with its flags word), every other version the flagless layout. */
  using STATIC_SAMPLER = std::remove_const_t<std::remove_pointer_t<decltype(pDesc->pStaticSamplers)>>;
  STATIC_SAMPLER *StaticSamplers = nullptr;
  RootSig->StaticSamplersOffset = add_segment(pDesc->NumStaticSamplers, &StaticSamplers);
  if (pDesc->NumStaticSamplers) {
    if (!StaticSamplers)
      return E_OUTOFMEMORY;
    memcpy(StaticSamplers, pDesc->pStaticSamplers, sizeof(STATIC_SAMPLER) * pDesc->NumStaticSamplers);
  }

  struct DXBC_ROOT_SIGNATURE_BLOB_HEADER {
    microsoft::DXBCHeader DXBCHeader;
    UINT RTSOBlobOffset;
    microsoft::DXBCBlobHeader RTSOBlobHeader;
  };

  auto TotalSize = sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) + total_offset;

  ID3D10Blob *blob = nullptr;
  HRESULT hr = CreateBlobFromMalloc(TotalSize, &blob);
  if (FAILED(hr))
    return hr;
  char *OutBlob = reinterpret_cast<char *>(blob->GetBufferPointer());

  DXBC_ROOT_SIGNATURE_BLOB_HEADER *pHeader = (DXBC_ROOT_SIGNATURE_BLOB_HEADER *)OutBlob;
  pHeader->DXBCHeader.DXBCHeaderFourCC = microsoft::DXBC_FOURCC_NAME;
  pHeader->DXBCHeader.BlobCount = 1;
  pHeader->DXBCHeader.Version = {1, 0};
  pHeader->DXBCHeader.ContainerSizeInBytes = TotalSize;
  pHeader->DXBCHeader.Hash = {};

  pHeader->RTSOBlobOffset = sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) - sizeof(microsoft::DXBCBlobHeader);
  pHeader->RTSOBlobHeader.BlobFourCC = microsoft::DXBC_RootSignature;
  pHeader->RTSOBlobHeader.BlobSize = total_offset;

  for (auto &seg : segments) {
    memcpy(OutBlob + sizeof(DXBC_ROOT_SIGNATURE_BLOB_HEADER) + seg.offset, seg.ptr, seg.size_aligned);
  }

  auto hash = md5::hashDxbcBinary(OutBlob, TotalSize);
  memcpy(pHeader->DXBCHeader.Hash.Digest, hash.data.data(), sizeof(md5::Digest));

  *ppBlob = blob;
  return hr;
};

extern "C" HRESULT WINAPI
D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignatureDesc, ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob
) {
  // InitReturnPtr(ppBlob); // deliberately, don't touch it
  InitReturnPtr(ppErrorBlob);

  if (!pRootSignatureDesc || !ppBlob)
    return E_INVALIDARG;

  switch (pRootSignatureDesc->Version) {
  default:
    break;
  case D3D_ROOT_SIGNATURE_VERSION_1:
    return SerializeVersionedRootSignatureImpl<D3D12_ROOT_SIGNATURE_DESC>(
        &pRootSignatureDesc->Desc_1_0, D3D_ROOT_SIGNATURE_VERSION_1, ppBlob, ppErrorBlob
    );
  case D3D_ROOT_SIGNATURE_VERSION_1_1:
    return SerializeVersionedRootSignatureImpl<D3D12_ROOT_SIGNATURE_DESC1>(
        &pRootSignatureDesc->Desc_1_1, D3D_ROOT_SIGNATURE_VERSION_1_1, ppBlob, ppErrorBlob
    );
  case kRootSignatureVersion_1_2:
    return SerializeVersionedRootSignatureImpl<D3D12_ROOT_SIGNATURE_DESC2>(
        &reinterpret_cast<const VersionedRootSignatureDesc *>(pRootSignatureDesc)->Desc_1_2,
        kRootSignatureVersion_1_2, ppBlob, ppErrorBlob
    );
  }

  return E_INVALIDARG;
}

extern "C" HRESULT WINAPI
D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob **ppBlob,
    ID3DBlob **ppErrorBlob
) {
  // InitReturnPtr(ppBlob); // deliberately, don't touch it
  InitReturnPtr(ppErrorBlob);

  if (!pRootSignatureDesc || !ppBlob || Version != D3D_ROOT_SIGNATURE_VERSION_1_0)
    return E_INVALIDARG;

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_desc;
  versioned_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  versioned_desc.Desc_1_0 = *pRootSignatureDesc;
  return D3D12SerializeVersionedRootSignature(&versioned_desc, ppBlob, ppErrorBlob);
}

#pragma endregion
#pragma region Deserialization

template <typename Base> class MTLD3D12RootSignatureDeserializer : public ComObject<Base> {
public:
  virtual HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    if (riid == __uuidof(Base)) {
      *ppvObject = ref_and_cast<Base>(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  virtual const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetRootSignatureDesc() {
    return &impl_.desc_1_0_.Desc_1_0;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetRootSignatureDescAtVersion(
      D3D_ROOT_SIGNATURE_VERSION Version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **ppDesc
  ) {
    switch (Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
      *ppDesc = &impl_.desc_1_0_;
      break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
      *ppDesc = &impl_.desc_1_1_;
      break;
    case kRootSignatureVersion_1_2:
      *ppDesc = reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&impl_.desc_1_2_);
      break;
    default:
      return E_INVALIDARG;
    }
    return S_OK;
  }

  virtual const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE
  GetUnconvertedRootSignatureDesc() {
    switch (impl_.raw_root_sig_->Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
      return &impl_.desc_1_0_;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
      return &impl_.desc_1_1_;
    default:
      return reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&impl_.desc_1_2_);
    }
  }

  MTLD3D12RootSignatureDeserializer() {}

  HRESULT
  Deserialize(const void *pBytecode, SIZE_T DataSize) {

    return impl_.Deserialize(pBytecode, DataSize);
  }

private:
  RootSignatureDeserializer impl_;
};

extern "C" HRESULT WINAPI
D3D12CreateVersionedRootSignatureDeserializer(
    const void *pBytecode, SIZE_T DataSize, REFIID iid, void **ppDeserializer
) {
  InitReturnPtr(ppDeserializer);

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = microsoft::DXBCGetRootSignature(pBytecode, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return hr;

  auto deserializer = Com(new MTLD3D12RootSignatureDeserializer<ID3D12VersionedRootSignatureDeserializer>());
  hr = deserializer->Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return hr;
  return deserializer->QueryInterface(iid, ppDeserializer);
}

extern "C" HRESULT WINAPI
D3D12CreateRootSignatureDeserializer(const void *pBytecode, SIZE_T DataSize, REFIID iid, void **ppDeserializer) {
  InitReturnPtr(ppDeserializer);

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = microsoft::DXBCGetRootSignature(pBytecode, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return hr;

  auto deserializer = Com(new MTLD3D12RootSignatureDeserializer<ID3D12RootSignatureDeserializer>());
  hr = deserializer->Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return hr;
  return deserializer->QueryInterface(iid, ppDeserializer);
}

#pragma endregion

inline std::pair<std::vector<uint32_t>, uint32_t>
CalculateRootSignatureQwordOffsets(const D3D12_ROOT_SIGNATURE_DESC1 &Desc) {
  uint32_t current_offset = 0;
  std::vector<uint32_t> out;

  for (unsigned i = 0; i < Desc.NumParameters; i++) {
    auto &Parameter = Desc.pParameters[i];

    switch (Parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      out.push_back(current_offset++);
      break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      out.push_back(current_offset);
      current_offset += (Parameter.Constants.Num32BitValues / 2) + (Parameter.Constants.Num32BitValues & 1);
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      out.push_back(current_offset++);
      break;
    }
  }

  return {out, current_offset};
}

class MTLD3D12RootSignatureImpl : public MTLD3D12DeviceChild<MTLD3D12RootSignature> {

  std::vector<uint8_t> blob_;
  std::vector<uint32_t> qword_offsets_;
  std::vector<uint32_t> status_qword_offsets_;
  std::vector<D3D12_ROOT_PARAMETER_TYPE> parameter_types_;
  std::vector<UINT> parameter_constant_counts_;
  std::vector<TableRange> table_ranges_;
  std::vector<std::pair<uint32_t, uint32_t>> table_range_spans_;

  std::vector<Rc<Sampler>> static_samplers_; // which is not really "static"
  /**
  It's supposed to be implemented by `constexpr sampler` in Metal shader, but
  for unknown reason it lacks of `mirrored_clamp_to_edge` address mode.
  We may lower that with an extra `abs()` to sample coord though...
  Let's do it in the simple way
  */
  std::vector<uint64_t> static_samplers_encoded_;
  std::vector<MTLD3D12Device::StaticSamplerKey> registered_sampler_keys_;
  bool sampler_keys_registered_ = false;

public:
  MTLD3D12RootSignatureImpl(MTLD3D12Device *pDevice, const void *pBytecode, SIZE_T BytecodeLength) :
      MTLD3D12DeviceChild<MTLD3D12RootSignature>(pDevice) {
    blob_.resize(BytecodeLength);
    memcpy(blob_.data(), pBytecode, BytecodeLength);
  }

  ~MTLD3D12RootSignatureImpl() override {
    if (!sampler_keys_registered_)
      return;
    std::lock_guard lock(device_->static_sampler_mutex);
    for (const auto &key : registered_sampler_keys_) {
      auto state = device_->static_sampler_uses.find(key);
      if (--state->second == 0)
        device_->static_sampler_uses.erase(state);
    }
  }

  HRESULT
  Initialize() {
    const void *pRawRootSig;
    UINT RawRootSigSize;
    HRESULT hr = microsoft::DXBCGetRootSignature(blob_.data(), &pRawRootSig, &RawRootSigSize);
    if (FAILED(hr))
      return hr;

    RootSignatureDeserializer deserializer;
    hr = deserializer.Deserialize(pRawRootSig, RawRootSigSize);
    if (FAILED(hr))
      return hr;

    auto &desc = deserializer.desc_1_1_.Desc_1_1;
    /* A 1.2 blob is validated with its sampler flags and UINT border colors; older
     * blobs keep the flagless 1.0/1.1 sampler contract. */
    const auto &desc_1_2 = deserializer.desc_1_2_.Desc_1_2;
    hr = deserializer.raw_root_sig_->Version == kRootSignatureVersion_1_2 ? ValidateRootSignatureDesc(desc_1_2)
                                                                           : ValidateRootSignatureDesc(desc);
    if (FAILED(hr))
      return hr;

    IsLocal = (static_cast<UINT>(desc.Flags) & 0x80u) != 0;
    Flags = static_cast<uint32_t>(desc.Flags);
    NumStaticSamplers = desc.NumStaticSamplers;
    if (NumStaticSamplers) {
      try {
        std::set<MTLD3D12Device::StaticSamplerKey> unique;
        for (UINT i = 0; i < desc.NumStaticSamplers; ++i) {
          const auto &s = desc_1_2.pStaticSamplers[i];
          // Sampler flags distinguish otherwise identical states; they share the border word.
          unique.insert({static_cast<uint32_t>(s.Filter), static_cast<uint32_t>(s.AddressU),
                         static_cast<uint32_t>(s.AddressV), static_cast<uint32_t>(s.AddressW),
                         std::bit_cast<uint32_t>(s.MipLODBias), s.MaxAnisotropy,
                         static_cast<uint32_t>(s.ComparisonFunc),
                         static_cast<uint32_t>(s.BorderColor) | (static_cast<uint32_t>(s.Flags) << 8),
                         std::bit_cast<uint32_t>(s.MinLOD), std::bit_cast<uint32_t>(s.MaxLOD)});
        }
        registered_sampler_keys_.assign(unique.begin(), unique.end());
        std::lock_guard lock(device_->static_sampler_mutex);
        size_t added = 0;
        for (const auto &key : registered_sampler_keys_)
          added += !device_->static_sampler_uses.contains(key);
        if (device_->static_sampler_uses.size() + added > 2032)
          return E_INVALIDARG;
        size_t committed = 0;
        try {
          for (const auto &key : registered_sampler_keys_) {
            ++device_->static_sampler_uses[key];
            ++committed;
          }
        } catch (const std::bad_alloc &) {
          for (size_t i = 0; i < committed; ++i) {
            auto state = device_->static_sampler_uses.find(registered_sampler_keys_[i]);
            if (--state->second == 0)
              device_->static_sampler_uses.erase(state);
          }
          return E_OUTOFMEMORY;
        }
        sampler_keys_registered_ = true;
      } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
      }
      try {
        static_samplers_encoded_.reserve(size_t(desc.NumStaticSamplers) * 8);
        static_samplers_.reserve(desc.NumStaticSamplers);
        for (unsigned i = 0; i < desc.NumStaticSamplers; i++) {
          WMTSamplerInfo info;
          PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, desc_1_2.pStaticSamplers[i]);

          auto sampler = Sampler::createSampler(device_->GetMTLDevice(), info, desc_1_2.pStaticSamplers[i].MipLODBias,
                                                SamplerReductionMode(desc_1_2.pStaticSamplers[i].Filter));
          if (!sampler)
            return E_OUTOFMEMORY;
          const auto &static_desc = desc_1_2.pStaticSamplers[i];
          static_samplers_encoded_.push_back(sampler->sampler_state_handle);
          static_samplers_encoded_.push_back(sampler->sampler_state_cube_handle);
          static_samplers_encoded_.push_back(sampler_feedback::SamplerMetadata(
              sampler->lod_bias, sampler_feedback::SamplerFlags(static_desc.Filter, static_desc.AddressU,
                  static_desc.AddressV, static_desc.MaxAnisotropy,
                  static_desc.Flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES, static_desc.AddressW,
                  static_cast<UINT>(static_desc.BorderColor) >= kStaticBorderColorOpaqueBlackUint, static_desc.ComparisonFunc)));
          static_samplers_encoded_.push_back(sampler_feedback::SamplerLodRange(static_desc.MinLOD, static_desc.MaxLOD));
          const bool white = static_cast<UINT>(static_desc.BorderColor) == kStaticBorderColorOpaqueWhiteUint;
          static_samplers_encoded_.push_back(white ? uint64_t(1) | uint64_t(1) << 32 : 0);
          static_samplers_encoded_.push_back(uint64_t(white ? 1 : 0) | uint64_t(1) << 32);
          const uint32_t float_rgb = static_desc.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ? 0x3f800000u : 0;
          const uint32_t float_alpha = static_desc.BorderColor == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK ? 0 : 0x3f800000u;
          static_samplers_encoded_.push_back(uint64_t(float_rgb) | uint64_t(float_rgb) << 32);
          static_samplers_encoded_.push_back(uint64_t(float_rgb) | uint64_t(float_alpha) << 32);

          static_samplers_.emplace_back(std::move(sampler));
        }
      } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
      }
      EncodedStaticSamplers = static_samplers_encoded_.data();
    }

    auto [offsets, total_qwords] = CalculateRootSignatureQwordOffsets(desc);
    qword_offsets_ = std::move(offsets);
    parameter_types_.reserve(desc.NumParameters);
    parameter_constant_counts_.reserve(desc.NumParameters);
    table_range_spans_.reserve(desc.NumParameters);
    for (UINT i = 0; i < desc.NumParameters; ++i) {
      const auto &parameter = desc.pParameters[i];
      parameter_types_.push_back(parameter.ParameterType);
      parameter_constant_counts_.push_back(parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                                                ? parameter.Constants.Num32BitValues : 0);
      const uint32_t begin = table_ranges_.size();
      if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        uint64_t previous_end = 0;
        for (UINT j = 0; j < parameter.DescriptorTable.NumDescriptorRanges; ++j) {
          const auto &range = parameter.DescriptorTable.pDescriptorRanges[j];
          const uint64_t offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                      ? previous_end : range.OffsetInDescriptorsFromTableStart;
          table_ranges_.push_back({offset, range.NumDescriptors, range.RangeType,
              range.RegisterSpace, range.BaseShaderRegister, parameter.ShaderVisibility});
          HasTextureDescriptorTables |= range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
                                        range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
          previous_end = offset + range.NumDescriptors;
        }
      } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                 parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                 parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        const auto type = parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV :
            parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        table_ranges_.push_back({0, 1, type, parameter.Descriptor.RegisterSpace,
            parameter.Descriptor.ShaderRegister, parameter.ShaderVisibility});
      }
      table_range_spans_.emplace_back(begin, table_ranges_.size() - begin);
    }
    ParameterQwords = total_qwords;
    if (Flags & kRootSignatureFlagsDirectlyIndexed) {
      HeapQwordOffset = total_qwords;
      total_qwords += kHeapQwords;
    }
    status_qword_offsets_.assign(desc.NumParameters, ~0u);
    if (!IsLocal) {
      for (UINT i = 0; i < desc.NumParameters; ++i)
        if (desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
            desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV)
          status_qword_offsets_[i] = total_qwords++;
    }
    StatusQwordOffsets = status_qword_offsets_.data();
    if (!IsLocal)
      ViewIdQwordOffset = total_qwords++;
    UploadQwords = total_qwords;
    ParameterSlots = qword_offsets_.size();
    SlotQwordOffsets = qword_offsets_.data();
    ParameterTypes = parameter_types_.data();
    ParameterConstantCounts = parameter_constant_counts_.data();

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12RootSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12RootSignature), riid)) {
      WARN("D3D12RootSignature: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  UINT
  GetBlob(const void **ppBlob) {
    *ppBlob = blob_.data();
    return blob_.size();
  }

  std::span<const TableRange> GetTableRanges(UINT root_parameter) const override {
    if (root_parameter >= table_range_spans_.size()) return {};
    if (parameter_types_[root_parameter] != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) return {};
    const auto [begin, count] = table_range_spans_[root_parameter];
    if (!count) return {};
    return {table_ranges_.data() + begin, count};
  }

  const TableRange *GetRootDescriptorRange(UINT root_parameter) const override {
    if (root_parameter >= table_range_spans_.size() ||
        parameter_types_[root_parameter] == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) return nullptr;
    const auto [begin, count] = table_range_spans_[root_parameter];
    return count ? &table_ranges_[begin] : nullptr;
  }
};

HRESULT
CreateRootSignature(
    MTLD3D12Device *pDevice, UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid,
    void **ppRootSignature
) {
  InitReturnPtr(ppRootSignature);
  if (!ppRootSignature || NodeMask > 1 || !pBytecode || !BytecodeLength)
    return E_INVALIDARG;

  auto root_sig = Com(new MTLD3D12RootSignatureImpl(pDevice, pBytecode, BytecodeLength));
  HRESULT hr = root_sig->Initialize();
  if (FAILED(hr))
    return hr;
  return root_sig->QueryInterface(riid, ppRootSignature);
}

} // namespace dxmt
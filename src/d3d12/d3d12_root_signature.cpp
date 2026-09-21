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
#include "dxmt_sampler.hpp"
#include "util_math.hpp"
#include "util_md5.hpp"
#include <cstring>
#include <cmath>
#include <new>
#include <vector>
#include "../d3d10/d3d10_blob.hpp"
#include "../airconv/dxbc_root_signature.hpp"

namespace dxmt {

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
  return visibility >= D3D12_SHADER_VISIBILITY_ALL && visibility <= D3D12_SHADER_VISIBILITY_PIXEL;
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

bool ValidStaticSampler(const D3D12_STATIC_SAMPLER_DESC &sampler) {
  const auto valid_address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
    return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
  };
  return ValidFilter(sampler.Filter) && valid_address(sampler.AddressU) && valid_address(sampler.AddressV) &&
         valid_address(sampler.AddressW) && std::isfinite(sampler.MipLODBias) && sampler.MipLODBias >= -16.0f &&
         sampler.MipLODBias <= 15.99f && sampler.MaxAnisotropy >= 1 && sampler.MaxAnisotropy <= 16 &&
         sampler.ComparisonFunc >= D3D12_COMPARISON_FUNC_NEVER &&
         sampler.ComparisonFunc <= D3D12_COMPARISON_FUNC_ALWAYS &&
         sampler.BorderColor >= D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK &&
         sampler.BorderColor <= D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE && !std::isnan(sampler.MinLOD) &&
         !std::isnan(sampler.MaxLOD) && sampler.MinLOD <= sampler.MaxLOD && ValidVisibility(sampler.ShaderVisibility);
}

template <typename ROOT_SIGNATURE_DESC>
HRESULT ValidateRootSignatureDesc(const ROOT_SIGNATURE_DESC &desc) {
  using ROOT_PARAMETER = typename GetType<ROOT_SIGNATURE_DESC>::ROOT_PARAMETER;
  using DESCRIPTOR_RANGE = typename GetType<ROOT_SIGNATURE_DESC>::DESCRIPTOR_RANGE;
  const UINT signature_flags = static_cast<UINT>(desc.Flags);
  if ((desc.NumParameters && !desc.pParameters) || (desc.NumStaticSamplers && !desc.pStaticSamplers) ||
      (signature_flags & ~0x3ffu) || ((signature_flags & 0x80u) && signature_flags != 0x80u) ||
      ((signature_flags & 0x80u) && desc.NumStaticSamplers))
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
          if ((flags & ~0xfu) || (data_flags && data_flags != 0x2u && data_flags != 0x4u && data_flags != 0x8u) ||
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

  D3D12_STATIC_SAMPLER_DESC *StaticSamplers = nullptr;
  RootSig->StaticSamplersOffset = add_segment(pDesc->NumStaticSamplers, &StaticSamplers);
  if (pDesc->NumStaticSamplers) {
    if (!StaticSamplers)
      return E_OUTOFMEMORY;
    memcpy(StaticSamplers, pDesc->pStaticSamplers, sizeof(D3D12_STATIC_SAMPLER_DESC) * pDesc->NumStaticSamplers);
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
    default:
      return &impl_.desc_1_1_;
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
  std::vector<D3D12_ROOT_PARAMETER_TYPE> parameter_types_;
  std::vector<UINT> parameter_constant_counts_;
  std::vector<raytracing::LocalRootCopy> local_root_copies_;

  std::vector<Rc<Sampler>> static_samplers_; // which is not really "static"
  /**
  It's supposed to be implemented by `constexpr sampler` in Metal shader, but
  for unknown reason it lacks of `mirrored_clamp_to_edge` address mode.
  We may lower that with an extra `abs()` to sample coord though...
  Let's do it in the simple way
  */
  std::vector<uint64_t> static_samplers_encoded_;

public:
  MTLD3D12RootSignatureImpl(MTLD3D12Device *pDevice, const void *pBytecode, SIZE_T BytecodeLength) :
      MTLD3D12DeviceChild<MTLD3D12RootSignature>(pDevice) {
    blob_.resize(BytecodeLength);
    memcpy(blob_.data(), pBytecode, BytecodeLength);
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
    if (FAILED(hr = ValidateRootSignatureDesc(desc)))
      return hr;

    IsLocal = (static_cast<UINT>(desc.Flags) & 0x80u) != 0;
    NumStaticSamplers = desc.NumStaticSamplers;
    if (NumStaticSamplers) {
      for (unsigned i = 0; i < desc.NumStaticSamplers; i++) {
        WMTSamplerInfo info;
        PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, desc.pStaticSamplers[i]);

        auto sampler = Sampler::createSampler(device_->GetMTLDevice(), info, desc.pStaticSamplers[i].MipLODBias);
        static_samplers_encoded_.push_back(sampler->sampler_state_handle);
        static_samplers_encoded_.push_back(sampler->sampler_state_cube_handle);
        static_samplers_encoded_.push_back((uint64_t)std::bit_cast<uint32_t>(sampler->lod_bias));
        static_samplers_encoded_.push_back(0 /* padding */);

        static_samplers_.emplace_back(std::move(sampler));
      }
      EncodedStaticSamplers = static_samplers_encoded_.data();
    }

    auto [offsets, total_qwords] = CalculateRootSignatureQwordOffsets(desc);
    qword_offsets_ = std::move(offsets);
    parameter_types_.reserve(desc.NumParameters);
    parameter_constant_counts_.reserve(desc.NumParameters);
    for (UINT i = 0; i < desc.NumParameters; ++i) {
      parameter_types_.push_back(desc.pParameters[i].ParameterType);
      parameter_constant_counts_.push_back(desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                                                ? desc.pParameters[i].Constants.Num32BitValues : 0);
    }
    UploadQwords = total_qwords;
    ParameterSlots = qword_offsets_.size();
    SlotQwordOffsets = qword_offsets_.data();
    ParameterTypes = parameter_types_.data();
    ParameterConstantCounts = parameter_constant_counts_.data();

    /*
     * Local root argument translation for raytracing shader records. The
     * application writes the parameters after the 32 byte shader identifier in the
     * D3D12 local root argument layout: root constants 4 byte packed, root
     * descriptors and descriptor tables 8 byte aligned. Every range that has to
     * move is recorded here so the pipeline can materialize the qword layout.
     */
    uint32_t source_offset = 0;
    for (unsigned i = 0; i < desc.NumParameters; i++) {
      auto &parameter = desc.pParameters[i];
      uint32_t size = 0;
      switch (parameter.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        size = parameter.Constants.Num32BitValues * 4;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        size = sizeof(uint64_t);
        source_offset = (source_offset + 7) & ~7u;
        break;
      default:
        return E_INVALIDARG;
      }
      if (!size)
        continue;
      local_root_copies_.push_back(
          {source_offset, size, qword_offsets_[i] * static_cast<uint32_t>(sizeof(uint64_t)), 0}
      );
      source_offset += size;
    }
    LocalRootDataSize = (source_offset + 7) & ~7u;
    LocalRootCopyCount = local_root_copies_.size();
    LocalRootCopies = local_root_copies_.data();

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
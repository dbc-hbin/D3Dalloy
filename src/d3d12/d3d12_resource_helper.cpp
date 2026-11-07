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
#include "d3d12_resource_contract.hpp"
#include "d3d12_enhanced_barrier.hpp"
#include "util_bit.hpp"
#include <bit>

namespace d3dalloy {

HRESULT
ValidateCastableFormats(MTLD3D12Device *device,const D3D12_RESOURCE_DESC &desc,UINT32 count,
                       const DXGI_FORMAT *formats) {
  if (!count) return S_OK;
  if (!formats) return E_INVALIDARG;
  if (desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
    for (UINT32 i=0;i<count;++i) if (formats[i]!=DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
    return S_OK;
  }
  MTL_DXGI_FORMAT_DESC base{};
  if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(),desc.Format,base))) return E_INVALIDARG;
  const bool exact_only=base.PlanarCount!=1 ||
      resource_contract::GetEmulatedTextureStorage(desc.Format)!=resource_contract::EmulatedTextureStorage::None;
  for (UINT32 i=0;i<count;++i) {
    const auto format=formats[i]; MTL_DXGI_FORMAT_DESC cast{};
    if (format==DXGI_FORMAT_UNKNOWN || FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(),format,cast)))
      return E_INVALIDARG;
    if (resource_contract::AreCopyCompatibleFormats(desc.Format,format)) continue;
    if (exact_only || (cast.Flag&MTL_DXGI_FORMAT_BC) || cast.PlanarCount!=1 ||
        resource_contract::GetEmulatedTextureStorage(format)!=resource_contract::EmulatedTextureStorage::None ||
        cast.BytesPerTexel!=((base.Flag&MTL_DXGI_FORMAT_BC) ? base.BlockSize : base.BytesPerTexel))
      return E_INVALIDARG;
  }
  return S_OK;
}

HRESULT
MTLD3D12Resource::PublishOwnedFormatCasting() {
  if (!share) return S_OK;
  auto &header=share->header();
  header.format_list_explicit=owned_initial_layout.has_value();
  header.initial_layout=owned_initial_layout.value_or(D3D12_BARRIER_LAYOUT_COMMON);
  std::fill(std::begin(header.castable_formats),std::end(header.castable_formats),uint64_t(0));
  for (DXGI_FORMAT format:owned_castable_formats) {
    const auto index=uint32_t(format);
    if (index>=sharing::kCastFormatWordCount*64 ||
        (!index && header.resource_desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)) return E_INVALIDARG;
    header.castable_formats[index/64] |= uint64_t(1)<<(index%64);
  }
  return S_OK;
}

HRESULT
MTLD3D12Resource::ImportOwnedFormatCasting(MTLD3D12Device *device,const D3D12_RESOURCE_DESC &desc) {
  if (!share || !share->header().format_list_explicit) return S_OK;
  const auto &header=share->header();
  if (desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (header.initial_layout!=D3D12_BARRIER_LAYOUT_UNDEFINED) return E_INVALIDARG;
  } else {
    if (header.initial_layout!=D3D12_BARRIER_LAYOUT_UNDEFINED &&
        !enhanced_barrier::LayoutAllowedOnQueue(header.initial_layout,D3D12_COMMAND_LIST_TYPE_DIRECT) &&
        !enhanced_barrier::LayoutAllowedOnQueue(header.initial_layout,D3D12_COMMAND_LIST_TYPE_COMPUTE))
      return E_INVALIDARG;
    if ((desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) &&
        header.initial_layout!=D3D12_BARRIER_LAYOUT_COMMON) return E_INVALIDARG;
    if (FAILED(ValidateResourceStates(enhanced_barrier::LegacyStateFromLayout(header.initial_layout),
                                   &header.heap_props,&desc))) return E_INVALIDARG;
  }
  try {
    uint32_t count=0; for (uint64_t word:header.castable_formats) count+=std::popcount(word);
    owned_castable_formats.reserve(count);
    for (uint32_t index=0;index<sharing::kCastFormatWordCount*64;++index)
      if (header.castable_formats[index/64] & (uint64_t(1)<<(index%64)))
        owned_castable_formats.push_back(DXGI_FORMAT(index));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  HRESULT hr=ValidateCastableFormats(device,desc,owned_castable_formats.size(),owned_castable_formats.data());
  if (FAILED(hr)) return hr;
  owned_initial_layout=header.initial_layout;
  return S_OK;
}

WMTPixelFormat GetD3D12PhysicalPixelFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_UINT:
    return WMTPixelFormatRGBA32Uint;
  case DXGI_FORMAT_R32G32B32_SINT:
    return WMTPixelFormatRGBA32Sint;
  case DXGI_FORMAT_R32G32B32_FLOAT:
    return WMTPixelFormatRGBA32Float;
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
    return WMTPixelFormatRGBA8Unorm;
  default:
    return WMTPixelFormatInvalid;
  }
}

WMTPixelFormat
GetD3D12ResourcePixelFormat(const MTL_DXGI_FORMAT_DESC &mapped, D3D12_RESOURCE_FLAGS flags) {
  WMTPixelFormat pixel_format = mapped.PixelFormat;
  if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    return pixel_format;
  switch (pixel_format) {
  case WMTPixelFormatR32Uint:
  case WMTPixelFormatR32Sint:
  case WMTPixelFormatR32Float:
    pixel_format = WMTPixelFormatDepth32Float;
    break;
  case WMTPixelFormatR16Uint:
  case WMTPixelFormatR16Sint:
  case WMTPixelFormatR16Float:
  case WMTPixelFormatR16Unorm:
  case WMTPixelFormatR16Snorm:
    pixel_format = WMTPixelFormatDepth16Unorm;
    break;
  default:
    break;
  }
  return pixel_format;
}

bool
IsD3D12SampleCountSupported(MTLD3D12Device *device, UINT count) {
  // The native count alone is not sufficient: software sample-frequency
  // replay and depth gathering have fixed 2/4/8 sample layouts.
  return count == 1 || ((count == 2 || count == 4 || count == 8) &&
                         device->GetMTLDevice().supportsTextureSampleCount(count));
}

bool
IsD3D12SampleQualitySupported(MTLD3D12Device *device, UINT count, UINT quality) {
  // The fixed standard and center patterns are outside the ordinary quality
  // range. The center pattern needs Metal programmable sample positions;
  // query that capability once at device initialization.
  return count == 1 ? quality == 0 : count > 1 &&
      (quality == 0 || quality == 0xffffffffu ||
       (quality == 0xfffffffeu && device->SupportsCenterSamplePattern()));
}

bool
IsD3D12MSAAPixelFormatSupported(MTLD3D12Device *device, WMTPixelFormat format) {
  if (any_bit_set(device->GetMTLPixelFormatCapability(format) & FormatCapability::MSAA))
    return true;
  // Apple9 R32Uint and RGBA32Uint/Sint pass real 2/4/8x native allocations,
  // clears and per-sample texture2d_ms reads. The shared capability table
  // predates that hardware; keep the correction D3D12-local.
  return (format == WMTPixelFormatR32Uint || format == WMTPixelFormatRGBA32Uint ||
          format == WMTPixelFormatRGBA32Sint) &&
         device->GetMTLDevice().supportsFamily(WMTGPUFamilyApple9);
}

bool
IsD3D12TextureFormatCompatible(DXGI_FORMAT format, const MTL_DXGI_FORMAT_DESC &mapped,
                               FormatCapability capability, uint32_t native_texel_size) {
  if (resource_contract::GetEmulatedTextureStorage(format) != resource_contract::EmulatedTextureStorage::None)
    return true;
  const bool block_compressed = mapped.Flag & MTL_DXGI_FORMAT_BC;
  const bool depth_stencil = mapped.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER);
  const bool native_layout_compatible = block_compressed || depth_stencil ||
      (mapped.BytesPerTexel && native_texel_size == mapped.BytesPerTexel);
  const auto texture_caps = static_cast<FormatCapability>(
      FormatCapability::Atomic | FormatCapability::Filter | FormatCapability::Write |
      FormatCapability::Color | FormatCapability::Blend | FormatCapability::MSAA |
      FormatCapability::Sparse | FormatCapability::Resolve | FormatCapability::DepthStencil);
  return mapped.PixelFormat != WMTPixelFormatInvalid && native_layout_compatible &&
         any_bit_set(capability & texture_caps);
}

bool
IsD3D12TextureSampleSupported(MTLD3D12Device *device, DXGI_FORMAT format, const MTL_DXGI_FORMAT_DESC &mapped,
                               D3D12_RESOURCE_FLAGS flags, UINT count,
                               bool query_typeless_interpretations) {
  const auto storage = resource_contract::GetEmulatedTextureStorage(format);
  if (storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
      storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8) {
    if (count != 1 || (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
      return false;
  } else if (storage == resource_contract::EmulatedTextureStorage::RGB32 &&
             (flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
    return false;
  }
  if (!IsD3D12SampleCountSupported(device, count) ||
      !IsD3D12TextureFormatCompatible(format, mapped,
          device->GetMTLPixelFormatCapability(mapped.PixelFormat), MTLGetTexelSize(mapped.PixelFormat)))
    return false;
  const auto physical = GetD3D12PhysicalPixelFormat(format);
  const auto backing = physical != WMTPixelFormatInvalid ? physical : GetD3D12ResourcePixelFormat(mapped, flags);
  if (count == 1)
    return true;
  if (mapped.Flag & MTL_DXGI_FORMAT_BC)
    return false;
  if (IsD3D12MSAAPixelFormatSupported(device, backing))
    return true;
  // A typeless quality query has no resource flags: it can still name a
  // depth/stencil-compatible backing even if the default color mapping lacks
  // MSAA. Actual resource creation supplies its own flags and uses one backing.
  if (query_typeless_interpretations && (mapped.Flag & MTL_DXGI_FORMAT_TYPELESS)) {
    return IsD3D12MSAAPixelFormatSupported(
        device, GetD3D12ResourcePixelFormat(mapped, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
  }
  return false;
}

template <>
HRESULT
ExtractEntireResourceViewDescription<D3D12_DEPTH_STENCIL_VIEW_DESC>(
    const D3D12_RESOURCE_DESC &ResourceDesc, D3D12_DEPTH_STENCIL_VIEW_DESC *pViewDescOut
) {
  pViewDescOut->Flags = D3D12_DSV_FLAG_NONE;
  pViewDescOut->Format = ResourceDesc.Format;
  switch (ResourceDesc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    ERR("Unsupported buffer DSV");
    return E_FAIL;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (ResourceDesc.DepthOrArraySize > 1) {
      pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
      pViewDescOut->Texture1DArray.MipSlice = 0;
      pViewDescOut->Texture1DArray.FirstArraySlice = 0;
      pViewDescOut->Texture1DArray.ArraySize = ResourceDesc.DepthOrArraySize;
    } else {
      pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
      pViewDescOut->Texture1D.MipSlice = 0;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (ResourceDesc.SampleDesc.Count > 1) {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DMSArray.ArraySize = ResourceDesc.DepthOrArraySize;
      } else {
        pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      }
    } else {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        pViewDescOut->Texture2DArray.MipSlice = 0;
        pViewDescOut->Texture2DArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DArray.ArraySize = ResourceDesc.DepthOrArraySize;
      } else {
        pViewDescOut->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        pViewDescOut->Texture2D.MipSlice = 0;
      }
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    ERR("Unsupported 3d DSV");
    return E_FAIL;
  }
  default:
    return E_INVALIDARG;
  }

  return S_OK;
}

template <>
HRESULT
ExtractEntireResourceViewDescription<D3D12_RENDER_TARGET_VIEW_DESC>(
    const D3D12_RESOURCE_DESC &ResourceDesc, D3D12_RENDER_TARGET_VIEW_DESC *pViewDescOut
) {
  pViewDescOut->Format = ResourceDesc.Format;
  switch (ResourceDesc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    ERR("Unsupported buffer RTV");
    return E_FAIL;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (ResourceDesc.DepthOrArraySize > 1) {
      pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
      pViewDescOut->Texture1DArray.MipSlice = 0;
      pViewDescOut->Texture1DArray.FirstArraySlice = 0;
      pViewDescOut->Texture1DArray.ArraySize = ResourceDesc.DepthOrArraySize;
    } else {
      pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
      pViewDescOut->Texture1D.MipSlice = 0;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (ResourceDesc.SampleDesc.Count > 1) {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DMSArray.ArraySize = ResourceDesc.DepthOrArraySize;
      } else {
        pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
      }
    } else {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        pViewDescOut->Texture2DArray.MipSlice = 0;
        pViewDescOut->Texture2DArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DArray.ArraySize = ResourceDesc.DepthOrArraySize;
        pViewDescOut->Texture2DArray.PlaneSlice = 0; // FIXME(resource-planar)
      } else {
        pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        pViewDescOut->Texture2D.MipSlice = 0;
        pViewDescOut->Texture2D.PlaneSlice = 0; // FIXME(resource-planar)
      }
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    pViewDescOut->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
    pViewDescOut->Texture3D.FirstWSlice = 0;
    pViewDescOut->Texture3D.WSize = ResourceDesc.DepthOrArraySize;
    pViewDescOut->Texture3D.MipSlice = 0;
    break;
  }
  default:
    return E_INVALIDARG;
  }

  return S_OK;
}

template <>
HRESULT
ExtractEntireResourceViewDescription<D3D12_SHADER_RESOURCE_VIEW_DESC>(
    const D3D12_RESOURCE_DESC &ResourceDesc, D3D12_SHADER_RESOURCE_VIEW_DESC *pViewDescOut
) {
  pViewDescOut->Format = ResourceDesc.Format;
  switch (ResourceDesc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    ERR("Unsupported buffer SRV");
    return E_FAIL;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (ResourceDesc.DepthOrArraySize > 1) {
      pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
      pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
      pViewDescOut->Texture1DArray.MostDetailedMip = 0;
      pViewDescOut->Texture1DArray.MipLevels = ResourceDesc.MipLevels;
      pViewDescOut->Texture1DArray.FirstArraySlice = 0;
      pViewDescOut->Texture1DArray.ArraySize = ResourceDesc.DepthOrArraySize;
      pViewDescOut->Texture1DArray.ResourceMinLODClamp = 0;
    } else {
      pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
      pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
      pViewDescOut->Texture1D.MostDetailedMip = 0;
      pViewDescOut->Texture1D.MipLevels = ResourceDesc.MipLevels;
      pViewDescOut->Texture1D.ResourceMinLODClamp = 0;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (ResourceDesc.SampleDesc.Count > 1) {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
        pViewDescOut->Texture2DMSArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DMSArray.ArraySize = ResourceDesc.DepthOrArraySize;
      } else {
        pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
      }
    } else {
      if (ResourceDesc.DepthOrArraySize > 1) {
        pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
        pViewDescOut->Texture2DArray.MostDetailedMip = 0;
        pViewDescOut->Texture2DArray.MipLevels = ResourceDesc.MipLevels;
        pViewDescOut->Texture2DArray.FirstArraySlice = 0;
        pViewDescOut->Texture2DArray.ArraySize = ResourceDesc.DepthOrArraySize;
        pViewDescOut->Texture2DArray.PlaneSlice = 0; // FIXME(resource-planar)
        pViewDescOut->Texture2DArray.ResourceMinLODClamp = 0;
      } else {
        pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
        pViewDescOut->Texture2D.MostDetailedMip = 0;
        pViewDescOut->Texture2D.MipLevels = ResourceDesc.MipLevels;
        pViewDescOut->Texture2D.PlaneSlice = 0; // FIXME(resource-planar)
        pViewDescOut->Texture2D.ResourceMinLODClamp = 0;
      }
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    pViewDescOut->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    pViewDescOut->Shader4ComponentMapping = kDefaultShader4Component;
    pViewDescOut->Texture3D.MostDetailedMip = 0;
    pViewDescOut->Texture3D.MipLevels = ResourceDesc.MipLevels;
    pViewDescOut->Texture3D.ResourceMinLODClamp = 0;
    break;
  }
  default:
    return E_INVALIDARG;
  }

  return S_OK;
}

template <>
HRESULT
ExtractEntireResourceViewDescription<D3D12_UNORDERED_ACCESS_VIEW_DESC>(
    const D3D12_RESOURCE_DESC &ResourceDesc, D3D12_UNORDERED_ACCESS_VIEW_DESC *pViewDescOut
) {
  pViewDescOut->Format = ResourceDesc.Format;
  switch (ResourceDesc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    ERR("Unsupported buffer UAV");
    return E_FAIL;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (ResourceDesc.DepthOrArraySize > 1) {
      pViewDescOut->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
      pViewDescOut->Texture1DArray.MipSlice = 0;
      pViewDescOut->Texture1DArray.FirstArraySlice = 0;
      pViewDescOut->Texture1DArray.ArraySize = ResourceDesc.DepthOrArraySize;
    } else {
      pViewDescOut->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
      pViewDescOut->Texture1D.MipSlice = 0;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (ResourceDesc.DepthOrArraySize > 1) {
      pViewDescOut->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
      pViewDescOut->Texture2DArray.MipSlice = 0;
      pViewDescOut->Texture2DArray.FirstArraySlice = 0;
      pViewDescOut->Texture2DArray.ArraySize = ResourceDesc.DepthOrArraySize;
      pViewDescOut->Texture2DArray.PlaneSlice = 0; // FIXME(resource-planar)
    } else {
      pViewDescOut->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      pViewDescOut->Texture2D.MipSlice = 0;
      pViewDescOut->Texture2D.PlaneSlice = 0; // FIXME(resource-planar)
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    pViewDescOut->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    pViewDescOut->Texture3D.FirstWSlice = 0;
    pViewDescOut->Texture3D.WSize = ResourceDesc.DepthOrArraySize;
    pViewDescOut->Texture3D.MipSlice = 0;
    break;
  }
  default:
    return E_INVALIDARG;
  }

  return S_OK;
}

constexpr D3D12_RESOURCE_STATES kExclusiveWrite =
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
    D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
    D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE | D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;

bool IsShadingRateImageDesc(const D3D12_RESOURCE_DESC &desc) {
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R8_UINT &&
         desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
         desc.SampleDesc.Quality == 0 && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
         !(desc.Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
}

HRESULT
ValidateResourceStates(D3D12_RESOURCE_STATES State, const D3D12_HEAP_PROPERTIES *pHeapProps,
                       const D3D12_RESOURCE_DESC *pDesc) {
  if (!pHeapProps || !pDesc)
    return E_INVALIDARG;
  if ((State & D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE) &&
      (State != D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE || !IsShadingRateImageDesc(*pDesc)))
    return E_INVALIDARG;
  if (State & kExclusiveWrite) {
    if (State & ~kExclusiveWrite)
      return E_INVALIDARG;
    if (bit::popcnt(State) != 1)
      return E_INVALIDARG;
  }
  if (((State & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
       !(pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) ||
      ((State & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE)) &&
       (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        !(pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) ||
      ((State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
       !(pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
      ((State & D3D12_RESOURCE_STATE_STREAM_OUT) &&
       pDesc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER))
    return E_INVALIDARG;

  switch (pHeapProps->Type) {
  case D3D12_HEAP_TYPE_UPLOAD:
    if (State != D3D12_RESOURCE_STATE_GENERIC_READ)
      return E_INVALIDARG;
    break;
  case D3D12_HEAP_TYPE_READBACK:
    if (State != D3D12_RESOURCE_STATE_COPY_DEST)
      return E_INVALIDARG;
    break;
  default:
    break;
  }

  return S_OK;
}

bool
IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES *pHeapProps) {
  switch (pHeapProps->Type) {
  case D3D12_HEAP_TYPE_UPLOAD:
  case D3D12_HEAP_TYPE_READBACK:
  case kHeapTypeGpuUpload:
    return true;
  case D3D12_HEAP_TYPE_CUSTOM:
    return pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
  default:
    return false;
  }
}

HRESULT
ValidateResourceDescs(MTLD3D12Device *device, const D3D12_RESOURCE_DESC *pDesc,
                      const D3D12_HEAP_PROPERTIES *pHeapProps) {
  if (!pDesc || !pHeapProps || !pDesc->Width || !pDesc->Height || !pDesc->DepthOrArraySize ||
      !pDesc->SampleDesc.Count ||
      !IsD3D12SampleQualitySupported(device, pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality))
    return E_INVALIDARG;

  if ((pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
      (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    return E_INVALIDARG;
  if ((pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
      (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    return E_INVALIDARG;

  auto HeapType = pHeapProps->Type;
  switch (HeapType) {
  case D3D12_HEAP_TYPE_UPLOAD:
  case D3D12_HEAP_TYPE_READBACK: {
    if (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
      return E_INVALIDARG;
    if (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
      return E_INVALIDARG;
    if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
      return E_INVALIDARG;
    break;
  }
  case D3D12_HEAP_TYPE_DEFAULT:
  case D3D12_HEAP_TYPE_CUSTOM:
    break;
  case kHeapTypeGpuUpload:
    // GPU upload heaps are GPU-local and GPU-writable. On UMA, textures are allowed (CPU access to
    // UNKNOWN-layout textures goes through WriteToSubresource/ReadFromSubresource); render targets
    // and depth are not CPU-accessible surface kinds.
    if (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return E_INVALIDARG;
    break;
  default:
    return E_INVALIDARG;
  }

  switch (pDesc->Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    if (pDesc->Height != 1 || pDesc->DepthOrArraySize != 1 || pDesc->MipLevels != 1 ||
        pDesc->Format != DXGI_FORMAT_UNKNOWN || pDesc->SampleDesc.Count != 1 ||
        pDesc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
        (pDesc->Alignment && pDesc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) ||
        (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)))
      return E_INVALIDARG;
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    if ((pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) && pDesc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
      return E_INVALIDARG;
    if ((pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
         (pDesc->Width > D3D12_REQ_TEXTURE1D_U_DIMENSION ||
          pDesc->DepthOrArraySize > D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION)) ||
        (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         (pDesc->Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
          pDesc->Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
          pDesc->DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)) ||
        (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
         (pDesc->Width > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
          pDesc->Height > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
          pDesc->DepthOrArraySize > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)))
      return E_INVALIDARG;
    uint64_t longest_dimension = pDesc->Width;
    if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D)
      longest_dimension = std::max(longest_dimension, uint64_t(pDesc->Height));
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      longest_dimension = std::max(longest_dimension, uint64_t(pDesc->DepthOrArraySize));
    if (pDesc->MipLevels && pDesc->MipLevels > std::bit_width(longest_dimension))
      return E_INVALIDARG;
    if (pDesc->Format == DXGI_FORMAT_UNKNOWN ||
        (pDesc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN &&
         pDesc->Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE &&
         pDesc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR))
      return E_INVALIDARG;
    // Row-major textures are cross-adapter 2D surfaces: one mip, one slice, single-sample,
    // color only (Metal linear textures over shared section memory).
    if (pDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
        (!(pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) ||
         pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || pDesc->MipLevels != 1 ||
         pDesc->DepthOrArraySize != 1 || pDesc->SampleDesc.Count != 1 ||
         (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return E_INVALIDARG;
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        (pDesc->Height != 1 || pDesc->SampleDesc.Count != 1))
      return E_INVALIDARG;
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && pDesc->SampleDesc.Count != 1)
      return E_INVALIDARG;
    if (pDesc->SampleDesc.Count > 1 &&
        (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || pDesc->MipLevels != 1 ||
         pDesc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN))
      return E_INVALIDARG;
    break;
  }
  default:
    return E_INVALIDARG;
  }

  return S_OK;
}

std::optional<standard_swizzle::Layout>
MakeStandardSwizzleLayout(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc) {
  const auto storage = resource_contract::GetEmulatedTextureStorage(desc.Format);
  const bool paired = storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
                      storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8;
  if (desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE ||
      desc.Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
      desc.SampleDesc.Count != 1 ||
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
      (storage != resource_contract::EmulatedTextureStorage::None && !paired))
    return std::nullopt;

  MTL_DXGI_FORMAT_DESC format{};
  if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, format)) ||
      format.PlanarCount != 1 ||
      (format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)))
    return std::nullopt;
  const bool bc = format.Flag & MTL_DXGI_FORMAT_BC;
  const uint32_t unit_bytes = bc ? format.BlockSize : paired ? 4 : format.BytesPerTexel;
  if (!bc && !paired && MTLGetTexelSize(format.PixelFormat) != unit_bytes)
    return std::nullopt;

  const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  auto layout = standard_swizzle::Layout::make(
      desc.Width, desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : desc.Height,
      volume ? desc.DepthOrArraySize : 1, volume ? 1 : desc.DepthOrArraySize,
      desc.MipLevels, unit_bytes, bc ? 4 : paired ? 2 : 1, bc ? 4 : 1, volume
  );
  if (!layout || layout->total_tiles > UINT32_MAX ||
      layout->size_bytes > device->max_buffer_length)
    return std::nullopt;
  for (uint32_t i = 0; i < layout->mip_count; ++i) {
    if (layout->mips[i].tiles_x > UINT32_MAX || layout->mips[i].tiles_y > UINT16_MAX ||
        layout->mips[i].tiles_z > UINT16_MAX)
      return std::nullopt;
  }
  return layout;
}

HRESULT
ValidateOptimizedClearValue(const D3D12_RESOURCE_DESC *pDesc, const D3D12_CLEAR_VALUE *optimized_clear_value) {
  if (!pDesc)
    return E_INVALIDARG;
  if (!optimized_clear_value)
    return S_OK;
  if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
      !(pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ||
      optimized_clear_value->Format == DXGI_FORMAT_UNKNOWN ||
      !resource_contract::AreCopyCompatibleFormats(pDesc->Format, optimized_clear_value->Format))
    return E_INVALIDARG;
  return S_OK;
}

HRESULT
ValidateHeapProperties(const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS Flags, bool AdapterIsNUMA) {
  if (!pHeapProps || pHeapProps->CreationNodeMask > 1 || pHeapProps->VisibleNodeMask > 1)
    return E_INVALIDARG;
  // The runtime reports ManualWriteTrackingResourceSupported=FALSE. Only tools
  // may override it and implement TrackWrite (D3D12GPUUploadHeaps spec).
  // The bundled Wine header predates this public heap flag.
  constexpr UINT tools_manual_write_tracking = 0x2000;
  if (UINT(Flags) & tools_manual_write_tracking)
    return E_INVALIDARG;
  switch (pHeapProps->Type) {
  case D3D12_HEAP_TYPE_DEFAULT:
  case D3D12_HEAP_TYPE_READBACK:
  case kHeapTypeGpuUpload: {
    if (pHeapProps->Type != kHeapTypeGpuUpload && (Flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH))
      return E_INVALIDARG;
    // CPU-accessible GPU upload heaps cannot be shared (D3D12GPUUploadHeaps spec, Restrictions).
    if (pHeapProps->Type == kHeapTypeGpuUpload &&
        (Flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_DISPLAY)))
      return E_INVALIDARG;
    [[fallthrough]];
  }
  case D3D12_HEAP_TYPE_UPLOAD: {
    if (pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN)
      return E_INVALIDARG;
    if (pHeapProps->MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN)
      return E_INVALIDARG;
    break;
  }
  case D3D12_HEAP_TYPE_CUSTOM: {
    if (pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
        pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE &&
        pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)
      return E_INVALIDARG;
    if (pHeapProps->MemoryPoolPreference != D3D12_MEMORY_POOL_L0 &&
        pHeapProps->MemoryPoolPreference != D3D12_MEMORY_POOL_L1)
      return E_INVALIDARG;
    if (pHeapProps->MemoryPoolPreference == D3D12_MEMORY_POOL_L1 && AdapterIsNUMA) {
      if (pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
        return E_INVALIDARG;
    }
    break;
  }
  default:
    return E_INVALIDARG;
  }

  if (pHeapProps->MemoryPoolPreference == D3D12_MEMORY_POOL_L1) {
    if (!AdapterIsNUMA)
      return E_INVALIDARG;
    if (pHeapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
      return E_INVALIDARG;
  }

  if ((Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) && IsCpuVisibleHeap(pHeapProps))
    return E_INVALIDARG;

  return S_OK;
}

D3D12_BOX
GetResourceExtent(const D3D12_RESOURCE_DESC &Desc, UINT MipSlice) {
  D3D12_BOX box{0, 0, 0, 1, 1, 1};
  switch (Desc.Dimension) {
  default:
    break;
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
    box.back = std::max<uint32_t>(1u, Desc.DepthOrArraySize >> MipSlice);
    [[fallthrough]];
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    box.bottom = std::max<uint32_t>(1u, Desc.Height >> MipSlice);
    [[fallthrough]];
  case D3D12_RESOURCE_DIMENSION_BUFFER:
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    box.right = std::max<uint32_t>(1u, Desc.Width >> MipSlice);
    break;
  }
  return box;
}

UINT
DecomposeSubresource(
    const D3D12_RESOURCE_DESC &Desc, UINT Subresource, UINT *pMipSlice, UINT *pArraySlice, UINT *pPlaneSlice
) {
  uint32_t ExtentOr = Desc.Width;
  uint32_t ArraySize = 1;
  switch (Desc.Dimension) {
  default:
    return 1;
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    ArraySize = Desc.DepthOrArraySize;
    break;
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    ExtentOr |= Desc.Height;
    ArraySize = Desc.DepthOrArraySize;
    break;
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
    ExtentOr |= Desc.Height | Desc.DepthOrArraySize;
    break;
  }
  auto MaxMipLevels = 32 - bit::lzcnt(ExtentOr);
  auto MipLevels = Desc.MipLevels ? Desc.MipLevels : MaxMipLevels;

  if (pMipSlice)
    *pMipSlice = Subresource % MipLevels;
  if (pArraySlice)
    *pArraySlice = ((Subresource / MipLevels) % ArraySize);
  if (pPlaneSlice)
    *pPlaneSlice = (Subresource / (MipLevels * ArraySize));
  return MipLevels;
}

bool
IsD3D12BoxInBounds(D3D12_BOX &box, D3D12_BOX &bounds) {
  if (box.left < bounds.left)
    return false;
  if (box.top < bounds.top)
    return false;
  if (box.front < bounds.front)
    return false;
  if (box.right > bounds.right)
    return false;
  if (box.bottom > bounds.bottom)
    return false;
  if (box.back > bounds.back)
    return false;

  return true;
}

} // namespace d3dalloy

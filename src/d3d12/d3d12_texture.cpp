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
#include "d3d12_format_support.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_resource_contract.hpp"
#include "d3d12_sampler_feedback.hpp"
#include "dxmt_format.hpp"
#include "com/com_pointer.hpp"
#include <algorithm>
#include <new>
#include <vector>

namespace dxmt {

namespace {

bool
ResolveViewRange(UINT first, UINT requested, UINT total, UINT &resolved) {
  return resource_contract::ResolveRange(first, requested, total, resolved);
}

bool
IsTypelessTextureFormat(WMT::Device device, DXGI_FORMAT format) {
  MTL_DXGI_FORMAT_DESC metal_format;
  return SUCCEEDED(MTLQueryDXGIFormat(device, format, metal_format)) &&
         (metal_format.Flag & MTL_DXGI_FORMAT_TYPELESS);
}

HRESULT
ValidateCpuTextureTransfer(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc, UINT subresource,
    const D3D12_BOX *requested_box, const void *data, UINT row_pitch, UINT slice_pitch,
    D3D12_BOX &box, UINT &level, UINT &slice, uint64_t &row_bytes, uint64_t &rows,
    uint64_t &bytes_per_unit
) {
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    return E_INVALIDARG;
  MTL_DXGI_FORMAT_DESC format{};
  if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, format)))
    return E_INVALIDARG;
  const uint64_t array_size = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
  const uint64_t count = uint64_t(desc.MipLevels) * array_size * format.PlanarCount;
  if (subresource >= count || !format.PlanarCount)
    return E_INVALIDARG;
  UINT plane = 0;
  DecomposeSubresource(desc, subresource, &level, &slice, &plane);
  if (plane)
    return E_INVALIDARG;
  D3D12_BOX extent = GetResourceExtent(desc, level);
  box = requested_box ? *requested_box : extent;
  // D3D12 defines an inverted box as empty, not invalid.
  if (box.left >= box.right || box.top >= box.bottom || box.front >= box.back)
    return S_OK;
  if (!IsD3D12BoxInBounds(box, extent))
    return E_INVALIDARG;
  if (!data)
    return E_INVALIDARG;

  const bool bc = format.Flag & MTL_DXGI_FORMAT_BC;
  const auto storage = resource_contract::GetEmulatedTextureStorage(desc.Format);
  const bool packed = storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
                      storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8;
  if (bc && ((box.left | box.top) & 3 ||
             ((box.right & 3) && box.right != extent.right) ||
             ((box.bottom & 3) && box.bottom != extent.bottom)))
    return E_INVALIDARG;
  if (packed && ((box.left & 1) || ((box.right & 1) && box.right != extent.right)))
    return E_INVALIDARG;
  const uint64_t columns = bc ? (uint64_t(box.right - box.left) + 3) / 4 :
      packed ? (uint64_t(box.right - box.left) + 1) / 2 : box.right - box.left;
  rows = bc ? (uint64_t(box.bottom - box.top) + 3) / 4 : box.bottom - box.top;
  bytes_per_unit = bc ? format.BlockSize : format.BytesPerTexel;
  uint64_t min_slice_pitch = 0;
  if (!bytes_per_unit || !resource_contract::CheckedMul(columns, bytes_per_unit, row_bytes) ||
      row_pitch < row_bytes ||
      !resource_contract::CheckedMul(row_pitch, rows, min_slice_pitch))
    return E_INVALIDARG;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && box.back - box.front > 1 &&
      slice_pitch < min_slice_pitch)
    return E_INVALIDARG;
  uint64_t last_row = 0, last_slice = 0, required = 0;
  if (!resource_contract::CheckedMul(row_pitch, rows - 1, last_row) ||
      !resource_contract::CheckedMul(slice_pitch, box.back - box.front - 1, last_slice) ||
      !resource_contract::CheckedAdd(last_slice, last_row, required) ||
      !resource_contract::CheckedAdd(required, row_bytes, required) || required > SIZE_MAX)
    return E_INVALIDARG;
  return S_OK;
}

void
ConvertEmulatedCpuRow(resource_contract::EmulatedTextureStorage storage, bool upload,
                      bool floating_point, const uint8_t *source, uint8_t *destination, UINT width) {
  if (storage == resource_contract::EmulatedTextureStorage::RGB32) {
    const uint32_t alpha = floating_point ? 0x3f800000u : 1u;
    for (UINT pixel = 0; pixel < width; ++pixel) {
      if (upload) {
        memcpy(destination + uint64_t(pixel) * 16, source + uint64_t(pixel) * 12, 12);
        memcpy(destination + uint64_t(pixel) * 16 + 12, &alpha, sizeof(alpha));
      } else {
        memcpy(destination + uint64_t(pixel) * 12, source + uint64_t(pixel) * 16, 12);
      }
    }
    return;
  }
  const bool rgbg = storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8;
  for (UINT pixel = 0; pixel < width; pixel += 2) {
    const uint8_t *s = source + (upload ? uint64_t(pixel / 2) : uint64_t(pixel)) * 4;
    uint8_t *d = destination + (upload ? uint64_t(pixel) : uint64_t(pixel / 2)) * 4;
    if (upload) {
      const uint8_t red = rgbg ? s[0] : s[1];
      const uint8_t blue = rgbg ? s[2] : s[3];
      d[0] = red;
      d[1] = rgbg ? s[1] : s[0];
      d[2] = blue;
      d[3] = 255;
      if (pixel + 1 < width) {
        d[4] = red;
        d[5] = rgbg ? s[3] : s[2];
        d[6] = blue;
        d[7] = 255;
      }
    } else {
      if (rgbg) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        d[3] = pixel + 1 < width ? s[5] : 0;
      } else {
        d[0] = s[1]; d[1] = s[0];
        d[2] = pixel + 1 < width ? s[5] : 0;
        d[3] = s[2];
      }
    }
  }
}

HRESULT
TransferStandardSwizzleCpu(const standard_swizzle::Layout &layout,
                           UINT level, UINT array_slice, const D3D12_BOX &box,
                           const uint8_t *source, uint8_t *destination,
                           UINT row_pitch, UINT slice_pitch, bool from_linear) {
  const uint64_t columns = (uint64_t(box.right - box.left) + layout.block_width - 1) / layout.block_width;
  const uint64_t rows = (uint64_t(box.bottom - box.top) + layout.block_height - 1) / layout.block_height;
  const uint64_t unit_x = box.left / layout.block_width;
  const uint64_t unit_y = box.top / layout.block_height;
  for (UINT z = box.front; z < box.back; ++z)
    for (uint64_t row = 0; row < rows; ++row)
      for (uint64_t column = 0; column < columns; ++column) {
        const auto offset = layout.offset(level, array_slice, unit_x + column, unit_y + row, z);
        if (!offset || !resource_contract::RangeFits(*offset, layout.bytes_per_unit, layout.size_bytes))
          return E_INVALIDARG;
        const uint64_t linear_offset = uint64_t(z - box.front) * slice_pitch + row * row_pitch +
                                       column * layout.bytes_per_unit;
        if (from_linear)
          memcpy(destination + *offset, source + linear_offset, layout.bytes_per_unit);
        else
          memcpy(destination + linear_offset, source + *offset, layout.bytes_per_unit);
      }
  return S_OK;
}

bool
IsSrvDimensionCompatible(const D3D12_RESOURCE_DESC &desc, D3D12_SRV_DIMENSION dimension) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return dimension == D3D12_SRV_DIMENSION_TEXTURE1D || dimension == D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return dimension == D3D12_SRV_DIMENSION_TEXTURE3D;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return false;
  if (desc.SampleDesc.Count > 1)
    return dimension == D3D12_SRV_DIMENSION_TEXTURE2DMS || dimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
  return dimension == D3D12_SRV_DIMENSION_TEXTURE2D || dimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY ||
         dimension == D3D12_SRV_DIMENSION_TEXTURECUBE || dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
}

bool
IsUavDimensionCompatible(const D3D12_RESOURCE_DESC &desc, D3D12_UAV_DIMENSION dimension) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return dimension == D3D12_UAV_DIMENSION_TEXTURE1D || dimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return dimension == D3D12_UAV_DIMENSION_TEXTURE2D || dimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && dimension == D3D12_UAV_DIMENSION_TEXTURE3D;
}

bool
IsRtvDimensionCompatible(const D3D12_RESOURCE_DESC &desc, D3D12_RTV_DIMENSION dimension) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return dimension == D3D12_RTV_DIMENSION_TEXTURE1D || dimension == D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return dimension == D3D12_RTV_DIMENSION_TEXTURE3D;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return false;
  if (desc.SampleDesc.Count > 1)
    return dimension == D3D12_RTV_DIMENSION_TEXTURE2DMS || dimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
  return dimension == D3D12_RTV_DIMENSION_TEXTURE2D || dimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
}

bool
IsDsvDimensionCompatible(const D3D12_RESOURCE_DESC &desc, D3D12_DSV_DIMENSION dimension) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return dimension == D3D12_DSV_DIMENSION_TEXTURE1D || dimension == D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return false;
  if (desc.SampleDesc.Count > 1)
    return dimension == D3D12_DSV_DIMENSION_TEXTURE2DMS || dimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
  return dimension == D3D12_DSV_DIMENSION_TEXTURE2D || dimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
}

/* An explicit list replaces (rather than widens) the legacy family contract. */
HRESULT
ValidateTextureViewFormat(DXGI_FORMAT resource_format, DXGI_FORMAT view_format,
                          const std::vector<DXGI_FORMAT> &castable_formats, bool format_list, WMT::Device device) {
  if (resource_format == view_format)
    return S_OK;
  if (!format_list) {
    if (!resource_contract::AreCopyCompatibleFormats(resource_format, view_format))
      return E_INVALIDARG;
    // RelaxedCasting v1.3 (2018) keeps these exclusions for fully typed
    // resources. A typeless resource still allows either interpretation.
    MTL_DXGI_FORMAT_DESC source_format{};
    if (FAILED(MTLQueryDXGIFormat(device, resource_format, source_format)))
      return E_INVALIDARG;
    // Depth/stencil plane views retain their established family contract; a
    // stencil SRV is not a color FLOAT-to-UINT reinterpretation.
    if (!(source_format.Flag & MTL_DXGI_FORMAT_TYPELESS) && source_format.PlanarCount == 1) {
      const auto source = resource_contract::GetTextureCastClass(resource_format);
      const auto target = resource_contract::GetTextureCastClass(view_format);
      using Class = resource_contract::TextureCastClass;
      if ((source == Class::Float) != (target == Class::Float) ||
          (source == Class::Snorm && target == Class::Unorm) ||
          (source == Class::Unorm && target == Class::Snorm))
        return E_INVALIDARG;
    }
    return S_OK;
  }
  return std::find(castable_formats.begin(), castable_formats.end(), view_format) != castable_formats.end()
             ? S_OK
             : E_INVALIDARG;
}

HRESULT
ValidateTexturePlane(
    WMT::Device device, DXGI_FORMAT resource_format, DXGI_FORMAT view_format, UINT plane_slice
) {
  MTL_DXGI_FORMAT_DESC resource_metal_format;
  MTL_DXGI_FORMAT_DESC view_metal_format;
  if (FAILED(MTLQueryDXGIFormat(device, resource_format, resource_metal_format)) ||
      FAILED(MTLQueryDXGIFormat(device, view_format, view_metal_format)))
    return E_INVALIDARG;
  if (plane_slice >= resource_metal_format.PlanarCount)
    return E_INVALIDARG;
  if (resource_metal_format.PlanarCount == 1)
    return plane_slice == 0 ? S_OK : E_INVALIDARG;

  const bool depth = view_metal_format.Flag & MTL_DXGI_FORMAT_DEPTH_PLANER;
  const bool stencil = view_metal_format.Flag & MTL_DXGI_FORMAT_STENCIL_PLANER;
  if (depth == stencil)
    return E_INVALIDARG;
  return plane_slice == (stencil ? 1u : 0u) ? S_OK : E_INVALIDARG;
}

bool
FitsSmallTextureAlignment(const D3D12_RESOURCE_DESC &desc, const MTL_DXGI_FORMAT_DESC &format) {
  const bool msaa = desc.SampleDesc.Count > 1;
  const auto storage = resource_contract::GetEmulatedTextureStorage(desc.Format);
  const bool paired = storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
                      storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8;
  if (!desc.Width || !desc.Height || !desc.DepthOrArraySize || !desc.SampleDesc.Count ||
      desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
      (format.PlanarCount != 1 &&
       !(msaa && (format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)))) ||
      (storage != resource_contract::EmulatedTextureStorage::None && !paired) ||
      (!msaa && (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))))
    return false;

  const bool bc = format.Flag & MTL_DXGI_FORMAT_BC;
  if ((bc && (msaa || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)) ||
      (paired && msaa) ||
      (!bc && !format.BytesPerTexel))
    return false;
  const uint32_t unit_bits = (bc ? format.BlockSize : paired ? 4 : format.BytesPerTexel) *
                             8 * desc.SampleDesc.Count;
  struct Shape { uint32_t width, height, depth; } tile{};
  if (msaa) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
      return false;
    switch (unit_bits) {
    case 8: tile = {256, 256, 1}; break;
    case 16: tile = {256, 128, 1}; break;
    case 32: tile = {128, 128, 1}; break;
    case 64: tile = {128, 64, 1}; break;
    case 128: tile = {64, 64, 1}; break;
    case 256: tile = {64, 32, 1}; break;
    case 512: tile = {32, 32, 1}; break;
    case 1024: tile = {32, 16, 1}; break;
    case 2048: tile = {16, 16, 1}; break;
    default: return false;
    }
  } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
    switch (unit_bits) {
    case 8: tile = {4096, 1, 1}; break;
    case 16: tile = {2048, 1, 1}; break;
    case 32: tile = {1024, 1, 1}; break;
    case 64: tile = {512, 1, 1}; break;
    case 128: tile = {256, 1, 1}; break;
    default: return false;
    }
  } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    switch (unit_bits) {
    case 8: tile = {64, 64, 1}; break;
    case 16: tile = {64, 32, 1}; break;
    case 32: tile = {32, 32, 1}; break;
    case 64: tile = {32, 16, 1}; break;
    case 128: tile = {16, 16, 1}; break;
    default: return false;
    }
  } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    switch (unit_bits) {
    case 8: tile = {16, 16, 16}; break;
    case 16: tile = {16, 16, 8}; break;
    case 32: tile = {16, 8, 8}; break;
    case 64: tile = {8, 8, 8}; break;
    case 128: tile = {8, 8, 4}; break;
    default: return false;
    }
  } else {
    return false;
  }

  const uint64_t x = bc ? (desc.Width - 1) / 4 + 1 :
      paired ? (desc.Width - 1) / 2 + 1 : desc.Width;
  const uint64_t y = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 :
      bc ? (uint64_t(desc.Height) + 3) / 4 : desc.Height;
  const uint64_t z = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
  const uint64_t max_tiles = msaa ? 64 : 16;
  const uint64_t x_tiles = (x + tile.width - 1) / tile.width;
  const uint64_t y_tiles = (y + tile.height - 1) / tile.height;
  const uint64_t z_tiles = (z + tile.depth - 1) / tile.depth;
  return x_tiles <= max_tiles && y_tiles <= max_tiles / x_tiles &&
         z_tiles <= max_tiles / (x_tiles * y_tiles);
}

} // namespace

HRESULT
PopulateWMTTextureInfo(MTLD3D12Device *device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc) {
  const auto Device = device->GetMTLDevice();
  MTL_DXGI_FORMAT_DESC Format;
  HRESULT hr = MTLQueryDXGIFormat(Device, Desc.Format, Format);
  if (FAILED(hr))
    return hr;

  const auto physical_format = GetD3D12PhysicalPixelFormat(Desc.Format);
  InfoOut.pixel_format = physical_format != WMTPixelFormatInvalid
                             ? physical_format : GetD3D12ResourcePixelFormat(Format, Desc.Flags);

  switch (Desc.Dimension) {
  default:
    return E_INVALIDARG;
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (Format.Flag & MTL_DXGI_FORMAT_BC)
      return E_INVALIDARG;
    InfoOut.width = Desc.Width;
    InfoOut.height = 1;
    InfoOut.depth = 1;
    InfoOut.array_length = Desc.DepthOrArraySize;
    if (Desc.DepthOrArraySize > 1)
      InfoOut.type = WMTTextureType2DArray;
    else
      InfoOut.type = WMTTextureType2D;
    InfoOut.sample_count = 1;
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    InfoOut.width = Desc.Width;
    InfoOut.height = Desc.Height;
    InfoOut.depth = 1;
    InfoOut.array_length = Desc.DepthOrArraySize;
    if (Desc.SampleDesc.Count == 0)
      return E_INVALIDARG;
    if (Desc.SampleDesc.Count > 1) {
      if (Desc.DepthOrArraySize > 1)
        InfoOut.type = WMTTextureType2DMultisampleArray;
      else
        InfoOut.type = WMTTextureType2DMultisample;
      InfoOut.sample_count = Desc.SampleDesc.Count;
    } else {
      if (Desc.DepthOrArraySize > 1)
        InfoOut.type = WMTTextureType2DArray;
      else
        InfoOut.type = WMTTextureType2D;
      InfoOut.sample_count = 1;
    }
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    InfoOut.width = Desc.Width;
    InfoOut.height = Desc.Height;
    InfoOut.depth = Desc.DepthOrArraySize;
    InfoOut.array_length = 1;
    InfoOut.type = WMTTextureType3D;
    InfoOut.sample_count = 1;
    break;
  }
  }

  InfoOut.mipmap_level_count = 32 - __builtin_clz(InfoOut.width | InfoOut.height | InfoOut.depth);
  if (Desc.MipLevels)
    InfoOut.mipmap_level_count = std::min<uint32_t>(InfoOut.mipmap_level_count, Desc.MipLevels);

  WMTTextureUsage Usage = WMTTextureUsagePixelFormatView;
  // Internal resolves/decompression read and rasterize even without public SRV/RTV/DSV flags.
  if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      any_bit_set(device->GetMTLPixelFormatCapability(InfoOut.pixel_format) &
                  (FormatCapability::Color | FormatCapability::DepthStencil)))
    Usage |= WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead;
  if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    Usage |= WMTTextureUsageRenderTarget;
  if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    Usage |= WMTTextureUsageRenderTarget;
  if (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    Usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
  // R8_UINT can be a shading-rate source even with DENY_SHADER_RESOURCE (VRS explicitly permits it).
  if (!(Desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || Desc.SampleDesc.Count > 1 ||
      IsShadingRateImageDesc(Desc))
    Usage |= WMTTextureUsageShaderRead;
  // BC storage is never rasterized or written by Metal. Color cast views own
  // writable shadows and publish their raw blocks through ordered GPU blits.
  if (Format.Flag & MTL_DXGI_FORMAT_BC)
    Usage = WMTTextureUsage(Usage & ~(WMTTextureUsageRenderTarget | WMTTextureUsageShaderWrite));
  InfoOut.usage = Usage;

  // TODO: decide storage mode
  InfoOut.options = WMTResourceHazardTrackingModeUntracked;

  if (Desc.Alignment) {
    if (Desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        Desc.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
        (Desc.SampleDesc.Count == 1 || Desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT))
      return E_INVALIDARG;

    const bool msaa = Desc.SampleDesc.Count > 1;
    const bool small_alignment = Desc.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT ||
                                 (msaa && Desc.Alignment == D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT);
    if (small_alignment) {
      if (msaa != (Desc.Alignment == D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT))
        return E_INVALIDARG;
      // Only mip 0 of one array slice participates in the architecture-independent
      // 4KB/64KB tile count; other mips and slices do not affect eligibility.
      if (!FitsSmallTextureAlignment(Desc, Format))
        return E_INVALIDARG;
    }
  }

  return S_OK;
};

WMTPixelFormat
EncodeComponentMapping(UINT Shader4ComponentMapping, WMTPixelFormat Format) {

#define DX2MTL_SWIZZLE(x) ((x) + 2) % 6

  auto MappingR = DX2MTL_SWIZZLE(Shader4ComponentMapping & 0x7);
  auto MappingG = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 3) & 0x7);
  auto MappingB = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 6) & 0x7);
  auto MappingA = DX2MTL_SWIZZLE((Shader4ComponentMapping >> 9) & 0x7);
  auto DecodedR = GET_SWIZZLE_RED(Format);
  auto DecodedG = GET_SWIZZLE_GREEN(Format);
  auto DecodedB = GET_SWIZZLE_BLUE(Format);
  auto DecodedA = GET_SWIZZLE_ALPHA(Format);
  uint32_t Map[6] = {0, 1, DecodedR, DecodedG, DecodedB, DecodedA};
  return WMTPixelFormat(ENCODE_FORMAT_SWIZZLE(Format, Map[MappingR], Map[MappingG], Map[MappingB], Map[MappingA]));
}

class MTLD3D12Texture : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;
  Com<MTLD3D12Heap> placed_heap_;
  WMT::Reference<WMT::Heap> volume_heap_;
  bool volume_heap_registered_ = false;
  bool volume_initially_resident_ = true;
  MTLD3D12PageableResidency *volume_residency_owner_ = nullptr;
  bool owns_registration_ = false;
  bool swizzle_shadow_registered_ = false;
  bool swizzle_canonical_registered_ = false;

public:
  MTLD3D12Texture(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  /* OpenSharedTexture: send right of the resolved IOSurface texture port, consumed by Initialize. */
  mach_port_t shared_import_port_ = 0;


  /*
   * Creator: records the descriptors and the IOSurfaceID identity in the share header.
   * Every holder (creator or importer): verifies identity and publishes this process' rendezvous.
   */
  HRESULT
  PublishSharedTexture() {
    auto allocation = texture->current();
    const uint64_t identity = WMTShareObjectIdentity(allocation->texture().handle, 1);
    if (!identity)
      return E_FAIL;
    if (!share) {
      HRESULT hr = sharing::Create(sharing::Kind::Texture, 0, share);
      if (FAILED(hr))
        return hr;
      share->header().heap_props = heap_props_;
      share->header().heap_flags = heap_flags_;
      share->header().resource_desc = desc_;
      share->header().identity = identity;
    } else if (share->header().identity != identity) {
      // A stale or reused registration resolved to a different surface.
      return E_INVALIDARG;
    }
    mach_port_t port = allocation->machPort;
    if (!port)
      return E_FAIL;
    // The rendezvous keeps its own send right; drop the allocation's so the IOSurface is freed
    // once the last holder releases the texture.
    HRESULT hr = share->publishPort(port);
    WMTMachPortDeallocate(port);
    allocation->machPort = 0;
    return hr;
  }

  /*
   * ROW_MAJOR (cross-adapter) textures: a Metal linear 2D texture over shared section memory, either
   * the committed resource's own share or a placement in a shared heap. Rows are pitched to
   * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT so another adapter or process can address the same bytes
   * with the standard copyable footprint.
   */
  HRESULT
  InitializeRowMajor(const WMTTextureInfo &texture_info, MTLD3D12Heap *pHeap, UINT64 heap_offset) {
    const bool committed_shared =
        !pHeap && (heap_flags_ & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
    if ((!pHeap || !pHeap->external_memory) && !committed_shared)
      return E_INVALIDARG;
    MTL_DXGI_FORMAT_DESC format{};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, format)) || format.PlanarCount != 1 ||
        (format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)) ||
        !format.BytesPerTexel || MTLGetTexelSize(format.PixelFormat) != format.BytesPerTexel ||
        texture_info.type != WMTTextureType2D || texture_info.mipmap_level_count != 1 ||
        texture_info.array_length != 1 || texture_info.sample_count != 1)
      return E_INVALIDARG;
    const uint64_t pitch = (desc_.Width * format.BytesPerTexel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~uint64_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint64_t bytes = pitch * desc_.Height;
    if (bytes > UINT32_MAX)
      return E_INVALIDARG;

    uint8_t *memory = nullptr;
    if (pHeap) {
      const UINT64 heap_size = pHeap->GetDesc().SizeInBytes;
      if (!resource_contract::PlacementFits(heap_offset, bytes, heap_size, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                            D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))
        return E_INVALIDARG;
      memory = pHeap->external_memory + heap_offset;
    } else {
      heap_flags_ |= D3D12_HEAP_FLAG_SHARED;
      if (!share) {
        HRESULT hr = sharing::Create(sharing::Kind::Buffer, bytes, share);
        if (FAILED(hr))
          return hr;
        share->header().heap_props = heap_props_;
        share->header().heap_flags = heap_flags_;
        share->header().resource_desc = desc_;
      } else if (share->payloadSize() < bytes) {
        return E_INVALIDARG;
      }
      memory = share->payload();
    }

    try {
      texture = new Texture(unsigned(bytes), unsigned(pitch), texture_info, device_->GetMTLDevice());
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    Flags<TextureAllocationFlag> flags = {};
    texture->rename(texture->allocate(flags, memory));
    if (!texture->current() || !texture->current()->texture())
      return E_OUTOFMEMORY;
    owns_registration_ = !pHeap;
    if (pHeap)
      placed_heap_ = pHeap;
    return device_->RegisterResidency(texture->current()->texture(), owns_registration_ ? this : nullptr,
                                      pHeap || !(heap_flags_ & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT));
  }

  HRESULT
  InitializeStandardSwizzle(const WMTTextureInfo &texture_info, MTLD3D12Heap *pHeap, UINT64 heap_offset) {
    const bool volume=reserved && reserved->volumeLayout();
    auto canonical_desc=desc_;
    if (volume) canonical_desc.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
    auto layout = MakeStandardSwizzleLayout(device_, canonical_desc);
    if (!layout)
      return E_INVALIDARG;
    const auto native_buffer = device_->GetMTLDevice().heapBufferSizeAndAlign(layout->size_bytes, {});
    if (pHeap && !resource_contract::PlacementFits(
            heap_offset, native_buffer.size, pHeap->GetDesc().SizeInBytes,
            D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, native_buffer.align))
      return E_INVALIDARG;

    try {
      swizzle_layout = std::make_unique<standard_swizzle::Layout>(*layout);
      buffer = new Buffer(layout->size_bytes, device_->GetMTLDevice());
      if (!volume) texture = new Texture(texture_info, device_->GetMTLDevice());
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    placed_heap_ = pHeap;
    if (volume && !pHeap && (heap_flags_&D3D12_HEAP_FLAG_SHARED)) {
      if (!share) {
        HRESULT hr=sharing::Create(sharing::Kind::Buffer,layout->size_bytes,share);
        if (FAILED(hr)) return hr;
        share->header().heap_props=heap_props_;
        share->header().heap_flags=heap_flags_;
        share->header().resource_desc=desc_;
        share->header().texture_storage=sharing::kTextureStorageSwizzledVolume;
      } else if (share->kind()!=sharing::Kind::Buffer || share->payloadSize()<layout->size_bytes ||
                 share->header().texture_storage!=sharing::kTextureStorageSwizzledVolume) return E_INVALIDARG;
    }
    standard_swizzle_may_alias = pHeap != nullptr || share != nullptr;
    Flags<BufferAllocationFlag> buffer_flags;
#ifdef __i386__
    /* Map() of a standard-swizzle texture reads this buffer's CPU mapping in a 32-bit caller. */
    if (!pHeap && IsCpuVisibleHeap(&heap_props_))
      buffer_flags.set(BufferAllocationFlag::CpuPlaced);
    if (pHeap && pHeap->host_memory)
      buffer->rename(buffer->allocate(buffer_flags, pHeap->host_memory + heap_offset));
    else
#endif
    if (pHeap && pHeap->external_memory)
      buffer->rename(buffer->allocate(buffer_flags,pHeap->external_memory+heap_offset));
    else if (volume && share)
      buffer->rename(buffer->allocate(buffer_flags,share->payload()));
    else if (pHeap)
      buffer->rename(buffer->allocate(buffer_flags, pHeap->heap, heap_offset));
    else
      buffer->rename(buffer->allocate(buffer_flags));
    if (!buffer->current() || !buffer->current()->buffer())
      return E_OUTOFMEMORY;

    Flags<TextureAllocationFlag> texture_flags;
    if (!volume) texture->rename(texture->allocate(texture_flags));
    if (!texture->current() || !texture->current()->texture())
      return E_OUTOFMEMORY;

    const bool initially_resident = volume ? volume_initially_resident_
                                          : !(heap_flags_ & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT);
    if (!pHeap) {
      HRESULT hr = device_->RegisterResidency(buffer->current()->buffer(), this, initially_resident);
      if (FAILED(hr))
        return hr;
      swizzle_canonical_registered_ = true;
    }
    HRESULT hr = device_->RegisterResidency(
        texture->current()->texture(), pHeap ? dynamic_cast<MTLD3D12PageableResidency *>(pHeap) : this,
        initially_resident);
    if (FAILED(hr))
      return hr;
    swizzle_shadow_registered_ = true;

    return S_OK;
  }

  HRESULT
  InitializeOrdinaryBCVolume(const D3D12_HEAP_PROPERTIES &properties, D3D12_HEAP_FLAGS flags,
                             const D3D12_RESOURCE_DESC &description, MTLD3D12Heap *heap, uint64_t offset) {
    const auto original=description;
    if (original.Alignment!=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ||
        (flags&D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)) return E_INVALIDARG;
    auto internal=original;
    internal.Flags=D3D12_RESOURCE_FLAGS(internal.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    internal.Layout=D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    internal.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    volume_initially_resident_=heap || !(flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT);
    volume_residency_owner_=heap ? dynamic_cast<MTLD3D12PageableResidency *>(heap) : this;
    if (heap) placed_heap_=heap;
    HRESULT hr=InitializeReserved(&internal);
    if (FAILED(hr)) return hr;
    desc_=original;
    heap_props_=properties; heap_flags_=flags; owned_is_reserved=false;
    const uint64_t bytes=uint64_t(reserved->totalTiles())*kReservedTileBytes;
    WMTHeapInfo info{}; info.type=WMTHeapTypePlacement; info.options=WMTResourceStorageModePrivate; info.size=bytes;
    volume_heap_=device_->GetMTLDevice().newPlacementHeap(info,WMTSparsePageSize64);
    if (!volume_heap_) return E_OUTOFMEMORY;
    hr=device_->RegisterResidency(volume_heap_,volume_residency_owner_,volume_initially_resident_);
    if (FAILED(hr)) return hr;
    volume_heap_registered_=true;
    auto completed=device_->GetMTLDevice().newSharedEvent();
    auto queue=device_->GetSparseMappingQueue();
    if (!completed || !queue) return E_OUTOFMEMORY;
    std::vector<std::pair<uint32_t,ReservedTileTarget>> assignments;
    assignments.reserve(reserved->totalTiles());
    for (uint32_t tile=0;tile<reserved->totalTiles();++tile)
      assignments.push_back({tile,{volume_heap_,tile}});
    {
      auto execution=device_->gpu_execution_gate.AcquireShared();
      if (!execution) return DXGI_ERROR_DEVICE_REMOVED;
      std::lock_guard<std::mutex> mapping_lock(ReservedResource::MappingQueueMutex);
      reserved->Apply(queue,{},texture->current()->texture(),assignments);
      queue.signalEvent(completed,1);
      if (!completed.waitUntilSignaledValue(1,10000)) return E_FAIL;
    }
    WMTTextureInfo original_info{};
    if (FAILED(hr=PopulateWMTTextureInfo(device_,original_info,desc_))) return hr;
    return InitializeStandardSwizzle(original_info,heap,offset);
  }

  HRESULT
  Initialize(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, MTLD3D12Heap *pHeap, UINT64 HeapOffset
  ) {
    desc_ = *pDesc;
    heap_props_ = *pHeapProps;
    heap_flags_ = HeapFlags;

    WMTTextureInfo texture_info;
    HRESULT hr = PopulateWMTTextureInfo(device_, texture_info, desc_);
    if (FAILED(hr))
      return hr;

    if (!desc_.MipLevels)
      desc_.MipLevels = texture_info.mipmap_level_count;

    if (!desc_.Alignment) {
      desc_.Alignment = desc_.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                   : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    MTL_DXGI_FORMAT_DESC mapped{};
    if (desc_.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(),desc_.Format,mapped)) &&
        (mapped.Flag&MTL_DXGI_FORMAT_BC))
      return InitializeOrdinaryBCVolume(*pHeapProps,HeapFlags,desc_,pHeap,HeapOffset);

    if (desc_.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)
      return InitializeStandardSwizzle(texture_info, pHeap, HeapOffset);

    if (desc_.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
      return InitializeRowMajor(texture_info, pHeap, HeapOffset);

    if (pHeap) {
      auto size_and_align = device_->GetMTLDevice().heapTextureSizeAndAlign(texture_info);
      const UINT64 heap_size = pHeap->GetDesc().SizeInBytes;
      if (!resource_contract::PlacementFits(
              HeapOffset, size_and_align.size, heap_size, desc_.Alignment, size_and_align.align
          ))
        return E_INVALIDARG;
    }

    if (pHeap && pHeap->external_memory) {
      // A shared heap is plain section memory without an MTLHeap; Metal can only alias it with
      // linear textures, which D3D12 textures of UNKNOWN layout are not.
      return E_INVALIDARG;
    }

    const bool shared = !pHeap && (HeapFlags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
    if (shared) {
      if (desc_.SampleDesc.Count > 1)
        return E_INVALIDARG;
      heap_flags_ |= D3D12_HEAP_FLAG_SHARED;
    }

    try {
      texture = new Texture(texture_info, device_->GetMTLDevice());
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    Flags<TextureAllocationFlag> flags = {};
    if (pHeap)
      texture->rename(texture->allocate(flags, pHeap->heap, HeapOffset));
    else if (shared && share)
      texture->rename(texture->import(shared_import_port_));
    else if (shared) {
      // IOSurface-backed shared textures must be MTLStorageModePrivate.
      flags.set(TextureAllocationFlag::Shared);
      flags.set(TextureAllocationFlag::GpuPrivate);
      texture->rename(texture->allocate(flags));
    } else
      texture->rename(texture->allocate(flags));
    if (!texture->current() || !texture->current()->texture())
      return shared && share ? E_INVALIDARG : E_OUTOFMEMORY;
    if (shared) {
      HRESULT hr = PublishSharedTexture();
      if (FAILED(hr))
        return hr;
    }
    owns_registration_ = !pHeap;
    if (!owns_registration_) {
      placed_heap_ = pHeap;
      return S_OK;
    }
    return device_->RegisterResidency(
        texture->current()->texture(), this, !(HeapFlags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)
    );
  };

  /*
   * Reserved 2D/array textures use native 64KB pages. Volumes flatten each
   * standard-shape 3D tile into one sparse 2D-array page: native 3D pages cannot
   * represent independent D3D12 64KB mappings. All pages start unmapped.
   */
  HRESULT PrepareReservedVolumeCastLayout(uint32_t mip) {
    if (!reserved || !reserved->volumeLayout() || reserved->blockWidth() != 4 || reserved->blockHeight() != 4)
      return S_OK;
    std::lock_guard<dxmt::mutex> lock(reserved->mutex);
    if (reserved->volume_cast_layouts.contains(mip)) return S_OK;
    auto layout = *reserved->volumeLayout();
    layout.width = (std::max(layout.width >> mip, 1u) + 3) / 4;
    layout.height = (std::max(layout.height >> mip, 1u) + 3) / 4;
    layout.depth = std::max(layout.depth >> mip, 1u);
    layout.mips = 1;
    layout.shape_w /= 4; layout.shape_h /= 4;
    layout.page_w /= 4; layout.page_h /= 4;
    layout.block_w = layout.block_h = 1;
    WMTBufferInfo info{};
    info.length = sizeof(layout) + 4 * sizeof(uint32_t) + sizeof(uint64_t);
    info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    auto buffer = device_->GetMTLDevice().newBuffer(info);
    auto *memory = static_cast<uint32_t *>(info.memory.get_accessible_or_null());
    if (!buffer || !memory) return E_OUTOFMEMORY;
    memcpy(memory, &layout, sizeof(layout));
    const auto &sub = reserved->subresources()[mip];
    memory[14] = sub.first_tile; memory[15] = sub.tiles_x; memory[16] = sub.tiles_y; memory[17] = sub.tiles_z;
    memcpy(memory + 18, &reserved->volume_status_address, sizeof(uint64_t));
    HRESULT hr = device_->RegisterResidency(buffer);
    if (FAILED(hr)) return hr;
    reserved->volume_cast_layouts.emplace(mip, ReservedResource::VolumeCastLayout{std::move(buffer), info.gpu_address});
    return S_OK;
  }

  HRESULT
  InitializeReserved(const D3D12_RESOURCE_DESC *pDesc) {
    owned_is_reserved=true;
    desc_ = *pDesc;
    heap_props_ = {};
    heap_flags_ = D3D12_HEAP_FLAG_NONE;
    if ((desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) || desc_.SampleDesc.Count != 1 ||
        desc_.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ||
        (desc_.Alignment && desc_.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) ||
        (desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER)))
      return E_INVALIDARG;
    desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    auto metal = device_->GetMTLDevice();
    MTL_DXGI_FORMAT_DESC format{};
    if (FAILED(MTLQueryDXGIFormat(metal, desc_.Format, format)) || format.PlanarCount != 1 ||
        resource_contract::GetEmulatedTextureStorage(desc_.Format) != resource_contract::EmulatedTextureStorage::None)
      return E_INVALIDARG;
    const bool bc = format.Flag & MTL_DXGI_FORMAT_BC;
    const uint32_t unit_bytes = bc ? format.BlockSize : format.BytesPerTexel;
    if (!unit_bytes || (!bc && MTLGetTexelSize(format.PixelFormat) != unit_bytes))
      return E_INVALIDARG;

    WMTTextureInfo texture_info;
    HRESULT hr = PopulateWMTTextureInfo(device_, texture_info, desc_);
    if (FAILED(hr))
      return hr;
    if (!desc_.MipLevels)
      desc_.MipLevels = texture_info.mipmap_level_count;
    const bool volume = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const uint32_t *volume_words = nullptr;
    const auto tile = metal.sparseTileSize(volume ? WMTTextureType2DArray : texture_info.type,
                                          texture_info.pixel_format, 1, WMTSparsePageSize64);
    if (volume) {
      hr = ReservedResource::CreateVolumeTiling(desc_, tile, bc ? 4 : 1, bc ? 4 : 1, unit_bytes, reserved);
      if (FAILED(hr))
        return hr;
      const auto &layout = *reserved->volumeLayout();
      texture_info.type = WMTTextureType2DArray;
      texture_info.width = layout.atlas_columns * layout.page_w;
      texture_info.height = layout.atlas_rows * layout.page_h;
      texture_info.depth = 1;
      texture_info.array_length = (layout.total_tiles + layout.atlas_columns * layout.atlas_rows - 1) /
          (layout.atlas_columns * layout.atlas_rows);
      texture_info.mipmap_level_count = 1;
      if (!bc) {
        // RTV publication writes exact bits through an integer view. Allocate
        // non-sRGB storage; SRVs retain their requested sRGB decode in views.
        texture_info.pixel_format = Forget_sRGB(texture_info.pixel_format);
        texture_info.usage = WMTTextureUsage(texture_info.usage | WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
      }
      if (!volume_residency_owner_) {
        // Only the initial NULL state is CPU-written, before this resource can be exposed.
        // Later changes come from Apply on the sparse mapping queue. Ordinary BC volumes
        // are immutable fully backed and carry a zero address instead.
        WMTBufferInfo status_info{};
        status_info.length = uint64_t(reserved->totalTiles()) * sizeof(uint32_t);
        status_info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
        reserved->status_buffer = metal.newBuffer(status_info);
        auto *status = status_info.memory.get_accessible_or_null();
        if (!reserved->status_buffer || !status) return E_OUTOFMEMORY;
        memset(status, 0, status_info.length);
        hr = device_->RegisterResidency(reserved->status_buffer);
        if (FAILED(hr)) { reserved->status_buffer = {}; return hr; }
        reserved->volume_status_address = status_info.gpu_address;
        reserved->volume_device = device_;
      }
      WMTBufferInfo info{};
      info.length = sizeof(ReservedVolumeLayout) + uint64_t(desc_.MipLevels) * 4 * sizeof(uint32_t) + sizeof(uint64_t);
      info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
      reserved->volume_layout_buffer = metal.newBuffer(info);
      auto *memory = static_cast<uint32_t *>(info.memory.get_accessible_or_null());
      if (!reserved->volume_layout_buffer || !memory)
        return E_OUTOFMEMORY;
      memcpy(memory, &layout, sizeof(layout));
      for (const auto &sub : reserved->subresources()) {
        auto *mip = memory + 14 + sub.mip * 4;
        mip[0] = sub.first_tile; mip[1] = sub.tiles_x; mip[2] = sub.tiles_y; mip[3] = sub.tiles_z;
      }
      memcpy(memory + 14 + 4 * desc_.MipLevels, &reserved->volume_status_address, sizeof(uint64_t));
      hr = device_->RegisterResidency(reserved->volume_layout_buffer);
      if (FAILED(hr)) {
        reserved->volume_layout_buffer = {};
        return hr;
      }
      reserved->volume_layout_address = info.gpu_address;
      reserved->volume_mapping_queue = device_->GetSparseMappingQueue();
      volume_words = memory;
    }

    try {
      texture = new Texture(texture_info, metal);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    texture->rename(texture->allocateSparse(WMTSparsePageSize64));
    if (!texture->current() || !texture->current()->texture())
      return E_OUTOFMEMORY;
    if (!volume) {
      hr = ReservedResource::CreateTextureTiling(
          desc_, texture->current()->texture(), tile, bc ? 4 : 1, bc ? 4 : 1, unit_bytes, reserved
      );
      if (FAILED(hr))
        return hr;
    } else {
      texture->reservedVolumeLayoutAddress = reserved->volume_layout_address;
      texture->reservedVolumeLayoutWords = volume_words;
      texture->reservedVolumeCastContext = this;
      texture->reservedVolumeCastBinding = [](void *context, uint32_t mip, WMTPixelFormat format, bool compressed) {
        auto *resource = static_cast<MTLD3D12Texture *>(context);
        auto atlas = resource->reserved->GetVolumeCastAtlas(resource->device_, format, compressed);
        uint64_t address = 0;
        if (!compressed) {
          std::lock_guard<dxmt::mutex> lock(resource->reserved->mutex);
          const auto metadata = resource->reserved->volume_cast_layouts.find(mip);
          if (metadata != resource->reserved->volume_cast_layouts.end()) address = metadata->second.address;
        }
        return Texture::ReservedVolumeCastBinding{std::move(atlas), address};
      };
    }
    texture->reservedResource = reserved.get();
    owns_registration_ = true;
    return device_->RegisterResidency(texture->current()->texture(),
        volume_residency_owner_ ? volume_residency_owner_ : this,volume_initially_resident_);
  }

  ~MTLD3D12Texture() {
    device_->UnregisterDescriptorResource(this);
    if (volume_heap_registered_) device_->UnregisterResidency(volume_heap_,volume_residency_owner_);
    if (reserved && reserved->status_buffer) device_->UnregisterResidency(reserved->status_buffer);
    if (reserved)
      for (const auto &[mip, layout] : reserved->volume_cast_layouts)
        device_->UnregisterResidency(layout.buffer);
    if (reserved && reserved->volume_layout_buffer)
      device_->UnregisterResidency(reserved->volume_layout_buffer);
    if (swizzle_layout) {
      if (swizzle_shadow_registered_)
        device_->UnregisterSwizzleShadowResidency(
            texture->current()->texture(), placed_heap_ ? dynamic_cast<MTLD3D12PageableResidency *>(placed_heap_.ptr()) : this);
      else if (volume_residency_owner_ && texture && texture->current())
        device_->UnregisterResidency(texture->current()->texture(),volume_residency_owner_);
      if (swizzle_canonical_registered_)
        device_->UnregisterResidency(buffer->current()->buffer(), this);
      return;
    }
    if (texture && texture->current() && !texture->current()->flags().test(TextureAllocationFlag::AllocatedOnHeap))
      device_->UnregisterResidency(texture->current()->texture(),
          volume_residency_owner_ ? volume_residency_owner_ : owns_registration_ ? this : nullptr);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) ||
        riid == __uuidof(ID3D12Resource1) || riid == __uuidof(ID3D12Resource2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Texture: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!IsCpuVisibleHeap(&heap_props_))
      return E_INVALIDARG;
    UINT subresource_count = desc_.MipLevels;
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      subresource_count *= desc_.DepthOrArraySize;
    if (Subresource >= subresource_count)
      return E_INVALIDARG;
    if (swizzle_layout) {
      if (ppData) {
        auto *bytes = static_cast<uint8_t *>(buffer->current()->mappedMemory(0));
        const auto offset = swizzle_layout->offset(
            Subresource % swizzle_layout->mip_count, Subresource / swizzle_layout->mip_count, 0, 0);
        if (!bytes || !offset)
          return E_INVALIDARG;
        *ppData = bytes + *offset;
      }
      return S_OK;
    }
    if (ppData || (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc_.MipLevels > 1))
      return E_INVALIDARG;
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) {};

  virtual D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  virtual D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return 0;
  };

  virtual HRESULT STDMETHODCALLTYPE
  WriteToSubresource(
      UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcSlicePitch
  ) {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      SrcSlicePitch = 0;
    UINT Level = 0, Slice = 0;
    D3D12_BOX box{};
    uint64_t row_bytes = 0, rows = 0, bytes_per_unit = 0;
    HRESULT hr = ValidateCpuTextureTransfer(
        device_, desc_, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcSlicePitch,
        box, Level, Slice, row_bytes, rows, bytes_per_unit
    );
    if (FAILED(hr))
      return hr;
    if (box.left >= box.right || box.top >= box.bottom || box.front >= box.back)
      return S_OK;

    if (swizzle_layout) {
      if (!IsCpuVisibleHeap(&heap_props_) || !buffer || !buffer->current() ||
          !buffer->current()->mappedMemory(0) || bytes_per_unit != swizzle_layout->bytes_per_unit)
        return E_INVALIDARG;
      return TransferStandardSwizzleCpu(*swizzle_layout, Level, Slice, box,
          static_cast<const uint8_t *>(pSrcData), static_cast<uint8_t *>(buffer->current()->mappedMemory(0)),
          SrcRowPitch, SrcSlicePitch, true);
    }

    const auto storage = resource_contract::GetEmulatedTextureStorage(desc_.Format);
    if (storage != resource_contract::EmulatedTextureStorage::None) {
      const UINT width = box.right - box.left;
      const UINT native_row_bytes = width * (storage == resource_contract::EmulatedTextureStorage::RGB32 ? 16u : 4u);
      std::vector<uint8_t> native_row(native_row_bytes);
      const auto *source = static_cast<const uint8_t *>(pSrcData);
      for (UINT z = box.front; z < box.back; ++z)
        for (UINT row = box.top; row < box.bottom; ++row) {
          ConvertEmulatedCpuRow(storage, true, desc_.Format == DXGI_FORMAT_R32G32B32_FLOAT,
              source + uint64_t(z - box.front) * SrcSlicePitch + uint64_t(row - box.top) * SrcRowPitch,
              native_row.data(), width);
          texture->current()->texture().replaceRegion(
              {box.left, row, z}, {width, 1, 1}, Level, Slice, native_row.data(), native_row_bytes, 0);
        }
      return S_OK;
    }

    if (SrcRowPitch % bytes_per_unit ||
        (box.back - box.front > 1 && SrcSlicePitch % SrcRowPitch)) {
      const UINT block_height = rows == box.bottom - box.top ? 1 : 4;
      auto *source = static_cast<const uint8_t *>(pSrcData);
      for (UINT z = box.front; z < box.back; ++z) {
        for (uint64_t row = 0; row < rows; ++row) {
          const UINT y = box.top + static_cast<UINT>(row * block_height);
          const UINT height = std::min<UINT>(block_height, box.bottom - y);
          const uint64_t offset = uint64_t(z - box.front) * SrcSlicePitch + row * SrcRowPitch;
          texture->current()->texture().replaceRegion(
              {box.left, y, z}, {box.right - box.left, height, 1}, Level, Slice,
              source + offset, static_cast<UINT>(row_bytes), 0
          );
        }
      }
      return S_OK;
    }
    texture->current()->texture().replaceRegion(
        {box.left, box.top, box.front}, {box.right - box.left, box.bottom - box.top, box.back - box.front}, Level,
        Slice, pSrcData, SrcRowPitch, SrcSlicePitch
    );
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstSlicePitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox
  ) {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      DstSlicePitch = 0;
    UINT Level = 0, Slice = 0;
    D3D12_BOX box{};
    uint64_t row_bytes = 0, rows = 0, bytes_per_unit = 0;
    HRESULT hr = ValidateCpuTextureTransfer(
        device_, desc_, SrcSubresource, pSrcBox, pDstData, DstRowPitch, DstSlicePitch,
        box, Level, Slice, row_bytes, rows, bytes_per_unit
    );
    if (FAILED(hr))
      return hr;
    if (box.left >= box.right || box.top >= box.bottom || box.front >= box.back)
      return S_OK;

    if (swizzle_layout) {
      if (!IsCpuVisibleHeap(&heap_props_) || !buffer || !buffer->current() ||
          !buffer->current()->mappedMemory(0) || bytes_per_unit != swizzle_layout->bytes_per_unit)
        return E_INVALIDARG;
      return TransferStandardSwizzleCpu(*swizzle_layout, Level, Slice, box,
          static_cast<const uint8_t *>(buffer->current()->mappedMemory(0)), static_cast<uint8_t *>(pDstData),
          DstRowPitch, DstSlicePitch, false);
    }

    const auto storage = resource_contract::GetEmulatedTextureStorage(desc_.Format);
    if (storage != resource_contract::EmulatedTextureStorage::None) {
      const UINT width = box.right - box.left;
      const UINT native_row_bytes = width * (storage == resource_contract::EmulatedTextureStorage::RGB32 ? 16u : 4u);
      std::vector<uint8_t> native_row(native_row_bytes);
      auto *destination = static_cast<uint8_t *>(pDstData);
      for (UINT z = box.front; z < box.back; ++z)
        for (UINT row = box.top; row < box.bottom; ++row) {
          texture->current()->texture().getBytes(
              {box.left, row, z}, {width, 1, 1}, Level, Slice, native_row.data(), native_row_bytes, 0);
          ConvertEmulatedCpuRow(storage, false, desc_.Format == DXGI_FORMAT_R32G32B32_FLOAT,
              native_row.data(),
              destination + uint64_t(z - box.front) * DstSlicePitch + uint64_t(row - box.top) * DstRowPitch,
              width);
        }
      return S_OK;
    }

    if (DstRowPitch % bytes_per_unit ||
        (box.back - box.front > 1 && DstSlicePitch % DstRowPitch)) {
      const UINT block_height = rows == box.bottom - box.top ? 1 : 4;
      auto *destination = static_cast<uint8_t *>(pDstData);
      for (UINT z = box.front; z < box.back; ++z) {
        for (uint64_t row = 0; row < rows; ++row) {
          const UINT y = box.top + static_cast<UINT>(row * block_height);
          const UINT height = std::min<UINT>(block_height, box.bottom - y);
          const uint64_t offset = uint64_t(z - box.front) * DstSlicePitch + row * DstRowPitch;
          texture->current()->texture().getBytes(
              {box.left, y, z}, {box.right - box.left, height, 1}, Level, Slice,
              destination + offset, static_cast<UINT>(row_bytes), 0
          );
        }
      }
      return S_OK;
    }
    texture->current()->texture().getBytes(
        {box.left, box.top, box.front}, {box.right - box.left, box.bottom - box.top, box.back - box.front}, Level,
        Slice, pDstData, DstRowPitch, DstSlicePitch
    );
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (owned_is_reserved)
      return E_INVALIDARG; // a reserved resource has no heap
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC ViewDesc;
    if (!pDesc) {
      if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
        return E_INVALIDARG;
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN) {
        if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
          return E_INVALIDARG;
        ViewDesc.Format = desc_.Format;
      }
    }

    if ((desc_.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        !IsSrvDimensionCompatible(desc_, ViewDesc.ViewDimension) ||
        FAILED(ValidateTextureViewFormat(desc_.Format, ViewDesc.Format, owned_castable_formats, owned_initial_layout.has_value(), device_->GetMTLDevice())))
      return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr))
      return hr;
    if (!IsD3D12ShaderViewFormat(ViewDesc.Format, bool(metal_format.Flag & MTL_DXGI_FORMAT_TYPELESS)))
      return E_INVALIDARG;
    MTL_DXGI_FORMAT_DESC resource_format{};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, resource_format)))
      return E_INVALIDARG;
    // Same-family BC interpretations share the native parent and its sparse mappings.
    // A separate formatted atlas duplicates that storage unnecessarily; only block-to-texel
    // or incompatible-format casts need the canonical transport path.
    if ((resource_format.Flag & MTL_DXGI_FORMAT_BC) &&
        Forget_sRGB(resource_format.PixelFormat) != Forget_sRGB(metal_format.PixelFormat) &&
        (!(metal_format.Flag & MTL_DXGI_FORMAT_BC) ||
         !resource_contract::AreCopyCompatibleFormats(desc_.Format, ViewDesc.Format))) {
      view_descriptor.castBlockSize = resource_format.BlockSize;
      view_descriptor.castCompressed = (metal_format.Flag & MTL_DXGI_FORMAT_BC) != 0;
    }

    auto view_format = GetD3D12PhysicalPixelFormat(ViewDesc.Format);
    if (view_format == WMTPixelFormatInvalid)
      view_format = metal_format.PixelFormat;
    if (resource_contract::GetEmulatedTextureStorage(ViewDesc.Format) ==
        resource_contract::EmulatedTextureStorage::RGB32) {
      // RGB32 is stored as RGBA32; the fourth channel is not part of the
      // logical resource even after RTV writes. Compose alpha=ONE with the
      // application's component mapping.
      view_format = WMTPixelFormat(ENCODE_FORMAT_SWIZZLE(
          view_format, GET_SWIZZLE_RED(view_format), GET_SWIZZLE_GREEN(view_format),
          GET_SWIZZLE_BLUE(view_format), 1));
    }
    view_descriptor.format = EncodeComponentMapping(ViewDesc.Shader4ComponentMapping, view_format);

    if (texture->pixelFormat() == WMTPixelFormatDepth32Float || texture->pixelFormat() == WMTPixelFormatDepth16Unorm) {
      const bool depth_access =
          (texture->pixelFormat() == WMTPixelFormatDepth32Float && ViewDesc.Format == DXGI_FORMAT_R32_FLOAT) ||
          (texture->pixelFormat() == WMTPixelFormatDepth16Unorm && ViewDesc.Format == DXGI_FORMAT_R16_UNORM);
      if (depth_access) {
        view_descriptor.format = EncodeComponentMapping(ViewDesc.Shader4ComponentMapping, texture->pixelFormat());
      } else {
        view_descriptor.castBlockSize = resource_format.BytesPerTexel;
        view_descriptor.castSourceBlockWidth = 1;
      }
    }

    FLOAT ResourceMinLODClamp = 0;

    switch (ViewDesc.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: {
      UINT mip_count = 0;
      if (!ResolveViewRange(ViewDesc.Texture1D.MostDetailedMip, ViewDesc.Texture1D.MipLevels, desc_.MipLevels, mip_count))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      ResourceMinLODClamp = ViewDesc.Texture1D.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: {
      UINT mip_count = 0, array_size = 0;
      if (!ResolveViewRange(
              ViewDesc.Texture1DArray.MostDetailedMip, ViewDesc.Texture1DArray.MipLevels, desc_.MipLevels, mip_count
          ) ||
          !ResolveViewRange(
              ViewDesc.Texture1DArray.FirstArraySlice, ViewDesc.Texture1DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      ResourceMinLODClamp = ViewDesc.Texture1DArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2D: {
      UINT mip_count = 0;
      if (!ResolveViewRange(ViewDesc.Texture2D.MostDetailedMip, ViewDesc.Texture2D.MipLevels, desc_.MipLevels, mip_count) ||
          FAILED(ValidateTexturePlane(device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2D.PlaneSlice)))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      ResourceMinLODClamp = ViewDesc.Texture2D.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: {
      UINT mip_count = 0, array_size = 0;
      if (!ResolveViewRange(
              ViewDesc.Texture2DArray.MostDetailedMip, ViewDesc.Texture2DArray.MipLevels, desc_.MipLevels, mip_count
          ) ||
          !ResolveViewRange(
              ViewDesc.Texture2DArray.FirstArraySlice, ViewDesc.Texture2DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ) ||
          FAILED(ValidateTexturePlane(
              device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2DArray.PlaneSlice
          )))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      ResourceMinLODClamp = ViewDesc.Texture2DArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURECUBE: {
      UINT mip_count = 0;
      if (desc_.DepthOrArraySize < 6 ||
          !ResolveViewRange(ViewDesc.TextureCube.MostDetailedMip, ViewDesc.TextureCube.MipLevels, desc_.MipLevels, mip_count))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureTypeCube; // FIXME: lowering to cube array
      view_descriptor.firstMiplevel = ViewDesc.TextureCube.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 6;
      ResourceMinLODClamp = ViewDesc.TextureCube.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: {
      UINT mip_count = 0;
      if (!ResolveViewRange(
              ViewDesc.TextureCubeArray.MostDetailedMip, ViewDesc.TextureCubeArray.MipLevels, desc_.MipLevels, mip_count
          ) ||
          (ViewDesc.TextureCubeArray.First2DArrayFace % 6) ||
          ViewDesc.TextureCubeArray.First2DArrayFace >= desc_.DepthOrArraySize)
        return E_INVALIDARG;
      const UINT remaining_faces = desc_.DepthOrArraySize - ViewDesc.TextureCubeArray.First2DArrayFace;
      UINT cube_count = ViewDesc.TextureCubeArray.NumCubes == UINT_MAX
                            ? remaining_faces / 6
                            : ViewDesc.TextureCubeArray.NumCubes;
      if (!cube_count || cube_count > remaining_faces / 6)
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureTypeCubeArray;
      view_descriptor.firstMiplevel = ViewDesc.TextureCubeArray.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = ViewDesc.TextureCubeArray.First2DArrayFace;
      view_descriptor.arraySize = cube_count * 6;
      ResourceMinLODClamp = ViewDesc.TextureCubeArray.ResourceMinLODClamp;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.msaaShadow = (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
      view_descriptor.type = WMTTextureType2DMultisample; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: {
      UINT array_size = 0;
      if (!ResolveViewRange(
              ViewDesc.Texture2DMSArray.FirstArraySlice, ViewDesc.Texture2DMSArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      view_descriptor.msaaShadow = (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
      break;
    }
    case D3D12_SRV_DIMENSION_TEXTURE3D: {
      UINT mip_count = 0;
      if (!ResolveViewRange(ViewDesc.Texture3D.MostDetailedMip, ViewDesc.Texture3D.MipLevels, desc_.MipLevels, mip_count))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MostDetailedMip;
      view_descriptor.miplevelCount = mip_count;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      ResourceMinLODClamp = ViewDesc.Texture3D.ResourceMinLODClamp;
      break;
    }
    default:
      return E_INVALIDARG;
    }

    if (view_descriptor.castBlockSize && view_descriptor.castSourceBlockWidth == 4 && !view_descriptor.castCompressed &&
        (view_descriptor.miplevelCount != 1 || view_descriptor.arraySize != 1))
      return E_INVALIDARG;
    if (view_descriptor.castBlockSize && view_descriptor.castSourceBlockWidth == 1 && desc_.SampleDesc.Count > 1) {
      view_descriptor.msaaShadow = true;
      view_descriptor.type = WMTTextureType2DArray;
    }
    ResourceMinLODClamp -= view_descriptor.firstMiplevel;
    const UINT logical_first_mip = view_descriptor.firstMiplevel;
    const UINT logical_mip_count = view_descriptor.miplevelCount;
    if (reserved && reserved->volumeLayout()) {
      if (view_descriptor.castBlockSize && !view_descriptor.castCompressed) {
        hr = PrepareReservedVolumeCastLayout(logical_first_mip);
        if (FAILED(hr)) return hr;
        view_descriptor.volumeCastMip = logical_first_mip;
      }
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = texture->arrayLength();
    }
    View = texture->createView(view_descriptor);
    if (!texture->view(View).texture)
      return E_OUTOFMEMORY;
    if (reserved && reserved->volumeLayout()) {
      View.mip_start = logical_first_mip;
      View.mip_end = logical_first_mip + logical_mip_count;
      View.mip_count = desc_.MipLevels;
    }

    const bool cube_view = ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE ||
                           ViewDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    return Heap->AddShaderResourceView(Index, texture.ptr(), View, ResourceMinLODClamp, cube_view,
                                      desc_.SampleDesc.Count > 1 && desc_.SampleDesc.Quality == 0xfffffffeu,
                                      sampler_feedback::EmptyViewDefaults(ViewDesc.Format, ViewDesc.Shader4ComponentMapping) |
                                      (texture->reservedVolumeLayoutAddress ?
                                          uint64_t(ViewDesc.Shader4ComponentMapping & 0xfff) << 48 : 0));
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (pCounter)
      return E_INVALIDARG;
    HRESULT hr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ViewDesc;
    if (!pDesc) {
      if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
        return E_INVALIDARG;
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN) {
        if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
          return E_INVALIDARG;
        ViewDesc.Format = desc_.Format;
      }
    }

    if (!(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        !IsUavDimensionCompatible(desc_, ViewDesc.ViewDimension) ||
        FAILED(ValidateTextureViewFormat(desc_.Format, ViewDesc.Format, owned_castable_formats, owned_initial_layout.has_value(), device_->GetMTLDevice())))
      return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    TextureViewKey View = texture->fullView;
    uint16_t first_w_slice = 0, w_size = 0;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr))
      return hr;
    if (metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))
      return E_INVALIDARG;
    if (metal_format.Flag & (MTL_DXGI_FORMAT_TYPELESS | MTL_DXGI_FORMAT_BC) ||
        Is_sRGBVariant(metal_format.PixelFormat) ||
        !any_bit_set(device_->GetMTLPixelFormatCapability(metal_format.PixelFormat) & FormatCapability::Write))
      return E_INVALIDARG;

    view_descriptor.format = metal_format.PixelFormat;
    MTL_DXGI_FORMAT_DESC resource_format{};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, resource_format)))
      return E_INVALIDARG;
    if ((resource_format.Flag & MTL_DXGI_FORMAT_BC) && !(metal_format.Flag & MTL_DXGI_FORMAT_BC))
      view_descriptor.castBlockSize = resource_format.BlockSize;

    switch (ViewDesc.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D: {
      if (ViewDesc.Texture1D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE2D: {
      if (ViewDesc.Texture2D.MipSlice >= desc_.MipLevels ||
          FAILED(ValidateTexturePlane(device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2D.PlaneSlice)))
        return E_INVALIDARG;
      view_descriptor.msaaShadow = desc_.SampleDesc.Count > 1;
      view_descriptor.type = view_descriptor.msaaShadow ? WMTTextureType2DArray : WMTTextureType2D;
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: {
      UINT array_size = 0;
      if (ViewDesc.Texture1DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture1DArray.FirstArraySlice, ViewDesc.Texture1DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: {
      view_descriptor.msaaShadow = desc_.SampleDesc.Count > 1;
      UINT array_size = 0;
      if (ViewDesc.Texture2DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture2DArray.FirstArraySlice, ViewDesc.Texture2DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ) ||
          FAILED(ValidateTexturePlane(
              device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2DArray.PlaneSlice
          )))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      break;
    }
    case D3D12_UAV_DIMENSION_TEXTURE3D: {
      if (ViewDesc.Texture3D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      const UINT mip_depth = std::max<UINT>(1u, static_cast<UINT>(desc_.DepthOrArraySize) >> ViewDesc.Texture3D.MipSlice);
      UINT resolved_w_size = 0;
      if (!ResolveViewRange(ViewDesc.Texture3D.FirstWSlice, ViewDesc.Texture3D.WSize, mip_depth, resolved_w_size))
        return E_INVALIDARG;
      first_w_slice = static_cast<uint16_t>(ViewDesc.Texture3D.FirstWSlice);
      w_size = static_cast<uint16_t>(resolved_w_size);
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    default:
      return E_INVALIDARG;
    }

    if (view_descriptor.castBlockSize &&
        (view_descriptor.miplevelCount != 1 || view_descriptor.arraySize != 1))
      return E_INVALIDARG;
    const UINT logical_uav_mip = view_descriptor.firstMiplevel;
    if (reserved && reserved->volumeLayout()) {
      if (view_descriptor.castBlockSize && !view_descriptor.castCompressed) {
        hr = PrepareReservedVolumeCastLayout(logical_uav_mip);
        if (FAILED(hr)) return hr;
        view_descriptor.volumeCastMip = logical_uav_mip;
      }
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = texture->arrayLength();
    }
    View = texture->createView(view_descriptor);
    if (!texture->view(View).texture)
      return E_OUTOFMEMORY;
    if (reserved && reserved->volumeLayout()) {
      View.mip_start = logical_uav_mip;
      View.mip_end = logical_uav_mip + 1;
      View.mip_count = desc_.MipLevels;
    }
    return Heap->AddUnorderedAccessView(Index, texture.ptr(), View, first_w_slice, w_size,
        sampler_feedback::EmptyViewDefaults(ViewDesc.Format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_RENDER_TARGET_VIEW_DESC ViewDesc;
    if (!pDesc) {
      if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
        return E_INVALIDARG;
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN) {
        if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
          return E_INVALIDARG;
        ViewDesc.Format = desc_.Format;
      }
    }

    if (!(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        !IsRtvDimensionCompatible(desc_, ViewDesc.ViewDimension) ||
        FAILED(ValidateTextureViewFormat(desc_.Format, ViewDesc.Format, owned_castable_formats, owned_initial_layout.has_value(), device_->GetMTLDevice())))
      return E_INVALIDARG;

    auto [Heap, Index] = GetRenderTargetHeap(device_, Descriptor);
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr))
      return hr;
    if (metal_format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER | MTL_DXGI_FORMAT_BC))
      return E_INVALIDARG;

    const auto physical_format = GetD3D12PhysicalPixelFormat(ViewDesc.Format);
    view_descriptor.format = physical_format != WMTPixelFormatInvalid ? physical_format : metal_format.PixelFormat;
    MTL_DXGI_FORMAT_DESC resource_format{};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc_.Format, resource_format)))
      return E_INVALIDARG;
    if ((resource_format.Flag & MTL_DXGI_FORMAT_BC) && !(metal_format.Flag & MTL_DXGI_FORMAT_BC))
      view_descriptor.castBlockSize = resource_format.BlockSize;

    MTL_RENDER_TARGET_DESC RenderTargetDesc;
    RenderTargetDesc.DepthPlane = 0;
    RenderTargetDesc.RenderTargetArrayLength = 0;
    RenderTargetDesc.CenterSamplePattern = desc_.SampleDesc.Count > 1 && desc_.SampleDesc.Quality == 0xfffffffeu;
    RenderTargetDesc.Flags = 0;

    switch (ViewDesc.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D: {
      if (ViewDesc.Texture1D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY: {
      UINT array_size = 0;
      if (ViewDesc.Texture1DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture1DArray.FirstArraySlice, ViewDesc.Texture1DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2D: {
      if (ViewDesc.Texture2D.MipSlice >= desc_.MipLevels ||
          FAILED(ValidateTexturePlane(device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2D.PlaneSlice)))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: {
      UINT array_size = 0;
      if (ViewDesc.Texture2DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture2DArray.FirstArraySlice, ViewDesc.Texture2DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ) ||
          FAILED(ValidateTexturePlane(
              device_->GetMTLDevice(), desc_.Format, ViewDesc.Format, ViewDesc.Texture2DArray.PlaneSlice
          )))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.type = WMTTextureType2DMultisample; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: {
      UINT array_size = 0;
      if (!ResolveViewRange(
              ViewDesc.Texture2DMSArray.FirstArraySlice, ViewDesc.Texture2DMSArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      break;
    }
    case D3D12_RTV_DIMENSION_TEXTURE3D: {
      if (ViewDesc.Texture3D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      const UINT mip_depth = std::max<UINT>(1u, static_cast<UINT>(desc_.DepthOrArraySize) >> ViewDesc.Texture3D.MipSlice);
      UINT w_size = 0;
      if (!ResolveViewRange(ViewDesc.Texture3D.FirstWSlice, ViewDesc.Texture3D.WSize, mip_depth, w_size))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType3D;
      view_descriptor.firstMiplevel = ViewDesc.Texture3D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      RenderTargetDesc.DepthPlane = ViewDesc.Texture3D.FirstWSlice;
      RenderTargetDesc.RenderTargetArrayLength = w_size;
      break;
    }
    default:
      return E_INVALIDARG;
    }

    if (view_descriptor.castBlockSize &&
        (view_descriptor.miplevelCount != 1 || view_descriptor.arraySize != 1))
      return E_INVALIDARG;
    if (reserved && reserved->volumeLayout()) {
      view_descriptor.reservedVolumeRender = true;
      if (view_descriptor.castBlockSize && !view_descriptor.castCompressed) {
        hr = PrepareReservedVolumeCastLayout(view_descriptor.firstMiplevel);
        if (FAILED(hr)) return hr;
        view_descriptor.volumeCastMip = view_descriptor.firstMiplevel;
      }
      view_descriptor.volumeFirstW = RenderTargetDesc.DepthPlane;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.arraySize = RenderTargetDesc.RenderTargetArrayLength;
      RenderTargetDesc.DepthPlane = 0;
    }
    View = texture->createView(view_descriptor);
    if (!texture->view(View).texture)
      return E_OUTOFMEMORY;
    RenderTargetDesc.Texture = texture.ptr();
    RenderTargetDesc.View = View;
    RenderTargetDesc.Width = texture->width(View);
    RenderTargetDesc.Height = texture->height(View);

    return Heap->AddRenderTarget(Index, &RenderTargetDesc);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc;
    if (!pDesc) {
      if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
        return E_INVALIDARG;
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN) {
        if (IsTypelessTextureFormat(device_->GetMTLDevice(), desc_.Format))
          return E_INVALIDARG;
        ViewDesc.Format = desc_.Format;
      }
    }

    if (!(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        !IsDsvDimensionCompatible(desc_, ViewDesc.ViewDimension) ||
        FAILED(ValidateTextureViewFormat(desc_.Format, ViewDesc.Format, owned_castable_formats, owned_initial_layout.has_value(), device_->GetMTLDevice())))
      return E_INVALIDARG;

    auto [Heap, Index] = GetRenderTargetHeap(device_, Descriptor);
    TextureViewKey View = texture->fullView;

    TextureViewDescriptor view_descriptor;
    MTL_DXGI_FORMAT_DESC metal_format;
    hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, metal_format);
    if (FAILED(hr))
      return hr;
    if (!(metal_format.Flag & MTL_DXGI_FORMAT_DEPTH_PLANER))
      return E_INVALIDARG;
    if (ViewDesc.Flags & ~(D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL))
      return E_INVALIDARG;
    view_descriptor.format = metal_format.PixelFormat;

    MTL_RENDER_TARGET_DESC RenderTargetDesc;
    RenderTargetDesc.DepthPlane = 0;
    RenderTargetDesc.RenderTargetArrayLength = 0;
    RenderTargetDesc.CenterSamplePattern = desc_.SampleDesc.Count > 1 && desc_.SampleDesc.Quality == 0xfffffffeu;
    RenderTargetDesc.Flags = ViewDesc.Flags;

    switch (ViewDesc.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D: {
      if (ViewDesc.Texture1D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture1D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY: {
      UINT array_size = 0;
      if (ViewDesc.Texture1DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture1DArray.FirstArraySlice, ViewDesc.Texture1DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture1DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture1DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2D: {
      if (ViewDesc.Texture2D.MipSlice >= desc_.MipLevels)
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2D; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = ViewDesc.Texture2D.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY: {
      UINT array_size = 0;
      if (ViewDesc.Texture2DArray.MipSlice >= desc_.MipLevels ||
          !ResolveViewRange(
              ViewDesc.Texture2DArray.FirstArraySlice, ViewDesc.Texture2DArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DArray;
      view_descriptor.firstMiplevel = ViewDesc.Texture2DArray.MipSlice;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DMS: {
      view_descriptor.type = WMTTextureType2DMultisample; // FIXME: lowering to 2d array
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = 0;
      view_descriptor.arraySize = 1;
      View = texture->createView(view_descriptor);
      break;
    }
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY: {
      UINT array_size = 0;
      if (!ResolveViewRange(
              ViewDesc.Texture2DMSArray.FirstArraySlice, ViewDesc.Texture2DMSArray.ArraySize, desc_.DepthOrArraySize,
              array_size
          ))
        return E_INVALIDARG;
      view_descriptor.type = WMTTextureType2DMultisampleArray;
      view_descriptor.firstMiplevel = 0;
      view_descriptor.miplevelCount = 1;
      view_descriptor.firstArraySlice = ViewDesc.Texture2DMSArray.FirstArraySlice;
      view_descriptor.arraySize = array_size;
      RenderTargetDesc.RenderTargetArrayLength = view_descriptor.arraySize;
      View = texture->createView(view_descriptor);
      break;
    }
    default:
      return E_INVALIDARG;
    }

    if (!texture->view(View).texture)
      return E_OUTOFMEMORY;
    RenderTargetDesc.Texture = texture.ptr();
    RenderTargetDesc.View = View;
    RenderTargetDesc.Width = std::max<uint32_t>(1u, texture->width() >> view_descriptor.firstMiplevel);
    RenderTargetDesc.Height = std::max<uint32_t>(1u, texture->height() >> view_descriptor.firstMiplevel);

    return Heap->AddRenderTarget(Index, &RenderTargetDesc);
  };

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTitleShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (reserved) {
      reserved->GetResourceTiling(TotalTileCount, PackedMipInfo, StandardTitleShape, SubresourceTilingCount,
                                  FirstSubresourceTiling, SubresourceTilings);
      return;
    }
    if (!swizzle_layout)
      return;
    const auto &layout = *swizzle_layout;
    if (TotalTileCount)
      *TotalTileCount = static_cast<UINT>(layout.total_tiles);
    if (PackedMipInfo)
      *PackedMipInfo = {static_cast<UINT8>(layout.mip_count), 0, 0, 0};
    if (StandardTitleShape)
      *StandardTitleShape = {
          UINT(layout.interleave.tile_width * layout.block_width),
          UINT(layout.interleave.tile_height * layout.block_height),
          UINT(layout.interleave.tile_depth)
      };
    if (!SubresourceTilingCount)
      return;
    const UINT total_subresources = layout.mip_count * layout.array_count;
    const UINT count = FirstSubresourceTiling < total_subresources
        ? std::min(*SubresourceTilingCount, total_subresources - FirstSubresourceTiling) : 0;
    *SubresourceTilingCount = count;
    if (!SubresourceTilings)
      return;
    for (UINT i = 0; i < count; ++i) {
      const UINT subresource = FirstSubresourceTiling + i;
      const UINT array = subresource / layout.mip_count;
      const auto &mip = layout.mips[subresource % layout.mip_count];
      SubresourceTilings[i] = {
          static_cast<UINT>(mip.tiles_x), static_cast<UINT16>(mip.tiles_y),
          static_cast<UINT16>(mip.tiles_z),
          static_cast<UINT>(array * layout.tiles_per_slice + mip.first_tile_in_slice)
      };
    }
  };
};

HRESULT
CreateCommittedTexture(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
) {
  auto *impl = new (std::nothrow) MTLD3D12Texture(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto texture = Com(impl);
  HRESULT hr = texture->Initialize(pHeapProps, HeapFlags, pDesc, InitialState, nullptr, 0);
  if (FAILED(hr))
    return hr;
  if (FAILED(hr = pDevice->RegisterDescriptorResource(texture.ptr())))
    return hr;
  texture->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

HRESULT
CreateReservedTexture(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto *impl = new (std::nothrow) MTLD3D12Texture(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto texture = Com(impl);
  HRESULT hr = texture->InitializeReserved(pDesc);
  if (FAILED(hr))
    return hr;
  if (FAILED(hr = pDevice->RegisterDescriptorResource(texture.ptr())))
    return hr;
  texture->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

HRESULT
OpenSharedTexture(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppResource) {
  InitReturnPtr(ppResource);
  const auto props = share->header().heap_props;
  const auto flags = share->header().heap_flags;
  const auto desc = share->header().resource_desc;
  if (desc.Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE1D || desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
      !(flags & D3D12_HEAP_FLAG_SHARED))
    return E_INVALIDARG;
  const bool row_major = desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const bool volume_storage=share->header().texture_storage==sharing::kTextureStorageSwizzledVolume;
  if (volume_storage) {
    MTL_DXGI_FORMAT_DESC format{};
    if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(),desc.Format,format)) ||
        !(format.Flag&MTL_DXGI_FORMAT_BC)) return E_INVALIDARG;
  }
  const bool memory_backed=row_major || volume_storage;
  if (share->kind() != (memory_backed ? sharing::Kind::Buffer : sharing::Kind::Texture))
    return E_INVALIDARG;
  auto *impl = new (std::nothrow) MTLD3D12Texture(pDevice);
  if (!impl) return E_OUTOFMEMORY;
  auto texture = Com(impl);
  texture->share = std::move(share);
  HRESULT hr=texture->ImportOwnedFormatCasting(pDevice,desc);
  if (FAILED(hr)) return hr;
  mach_port_t port=memory_backed ? 0 : texture->share->lookUpPort();
  if (!memory_backed && !port) return E_INVALIDARG;
  texture->shared_import_port_ = port;
  hr = texture->Initialize(&props, flags, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, 0);
  // On success PublishSharedTexture consumed the port (allocation machPort reset to 0).
  if (port && (!texture->texture || !texture->texture->current() || texture->texture->current()->machPort == port))
    WMTMachPortDeallocate(port);
  if (FAILED(hr))
    return hr;
  if (FAILED(hr = pDevice->RegisterDescriptorResource(texture.ptr())))
    return hr;
  return texture->QueryInterface(riid, ppResource);
}

HRESULT
CreatePlacedTexture(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto *impl = new (std::nothrow) MTLD3D12Texture(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto texture = Com(impl);
  D3D12_HEAP_DESC heap_desc = pHeap->GetDesc();

  if (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
    if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES)
      return E_INVALIDARG;
  } else {
    if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)
      return E_INVALIDARG;
  }

  HRESULT hr = texture->Initialize(&heap_desc.Properties, heap_desc.Flags, pDesc, InitialState, pHeap, HeapOffset);
  if (FAILED(hr))
    return hr;
  if (FAILED(hr = pDevice->RegisterDescriptorResource(texture.ptr())))
    return hr;
  texture->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return texture->QueryInterface(riid, ppResource);
}

} // namespace dxmt

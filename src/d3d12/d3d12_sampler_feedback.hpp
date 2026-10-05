/*
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

/*
 * Sampler feedback (SamplerFeedbackTier 1.0).
 *
 * A feedback map (DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE / _MIP_REGION_USED_OPAQUE) is an
 * owned buffer of 32 bit cells, one per decoded R8_UINT texel; 0 is "cleared" for both kinds.
 *   MIN_MIP: per slice a grid0 = ceil(W / region_w) x ceil(H / region_h) grid; a cell holds
 *     255 - (most detailed requested mip), updated with atomic max. Decoded: 255 - cell, so a
 *     cleared cell decodes to 0xFF.
 *   MIP_REGION_USED: per slice, for each mip m, a max(1, grid0 >> m) grid back to back; a cell
 *     holds 1 once its region was requested (atomic or). Decoded: 0xFF or 0.
 *
 * The feedback UAV is a buffer-shaped descriptor: q0 = cell array GPU address (0 for a null
 * paired texture: writes are dropped), q1 = layout word (FeedbackLayoutWord), q2/q3 = 0. The
 * shader side is src/airconv/shaders/air_sampler_feedback.metal; it also reads the paired SRV
 * view range (SRV q2) and the sampler words below.
 *
 * Sampler descriptor q2 = lod bias (low 32, f32) | flags << 32, q3 = MinLOD | MaxLOD << 32 (f32):
 *   flags bits 0-2 AddressU, 3-5 AddressV (D3D12_TEXTURE_ADDRESS_MODE), 6 min linear,
 *   7 mag linear, 8 mip linear, 9 anisotropic, 10-14 MaxAnisotropy, 15 non-normalized,
 *   16 UINT border, 17-19 AddressW, 20-21 reduction, 22-25 comparison function, 31 valid.
 * q4/q5 retain the exact uint4 border; q6/q7 retain float4 border bits (64-byte sampler stride).
 */

#include "d3d12.h"
#include "d3d12_resource_contract.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace dxmt::sampler_feedback {

/* VulkanOn12 / DDI 0100: W and bias are unrestricted; sampled views are single-mip 1D/2D. */
template <typename DESC>
bool ValidNonNormalizedSampler(const DESC &desc) {
  const UINT filter = UINT(desc.Filter);
  const UINT reduction = D3D12_DECODE_FILTER_REDUCTION(desc.Filter);
  const auto address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
    return mode == D3D12_TEXTURE_ADDRESS_MODE_CLAMP || mode == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  };
  return !(filter & ~0x194u) && reduction != D3D12_FILTER_REDUCTION_TYPE_COMPARISON &&
         D3D12_DECODE_MIN_FILTER(desc.Filter) == D3D12_DECODE_MAG_FILTER(desc.Filter) &&
         desc.MinLOD == 0.0f && desc.MaxLOD == 0.0f && address(desc.AddressU) && address(desc.AddressV);
}

/* SRV q3 low4: empty-view ones; bits8..31: four 6-bit integer widths (0 means32). */
inline uint32_t EmptyViewDefaults(DXGI_FORMAT format, UINT mapping) {
  using resource_contract::FormatFamily;
  uint32_t defaults = 0;
  switch (resource_contract::GetFormatFamily(format)) {
  case FormatFamily::R32G32B32: case FormatFamily::R32G32: case FormatFamily::R32G8X24:
  case FormatFamily::R16G16: case FormatFamily::R32: case FormatFamily::R24G8:
  case FormatFamily::R8G8: case FormatFamily::R16: case FormatFamily::R8:
  case FormatFamily::B8G8R8X8: case FormatFamily::BC4: case FormatFamily::BC5: case FormatFamily::BC6H:
    defaults = 8;
    break;
  default:
    if (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP ||
        format == DXGI_FORMAT_R1_UNORM || format == DXGI_FORMAT_B5G6R5_UNORM ||
        format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM)
      defaults = 8;
    break;
  }
  uint32_t widths[4] = {};
  switch (resource_contract::GetFormatFamily(format)) {
  case FormatFamily::R8: widths[0] = 8; break;
  case FormatFamily::R8G8: widths[0] = widths[1] = 8; break;
  case FormatFamily::R8G8B8A8: widths[0] = widths[1] = widths[2] = widths[3] = 8; break;
  case FormatFamily::R16: widths[0] = 16; break;
  case FormatFamily::R16G16: widths[0] = widths[1] = 16; break;
  case FormatFamily::R16G16B16A16: widths[0] = widths[1] = widths[2] = widths[3] = 16; break;
  default: break;
  }
  if (format == DXGI_FORMAT_R10G10B10A2_UINT) {
    widths[0] = widths[1] = widths[2] = 10;
    widths[3] = 2;
  }
  if (format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT)
    widths[0] = widths[1] = 8;
  uint32_t result = 0;
  for (unsigned channel = 0; channel < 4; ++channel) {
    const unsigned selector = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(channel, mapping);
    if (selector == 5 || (selector < 4 && (defaults & (1u << selector))))
      result |= 1u << channel;
    result |= widths[channel] << (8 + channel * 6);
  }
  return result;
}

/* DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE / _MIP_REGION_USED_OPAQUE (absent from the mingw headers). */
inline constexpr DXGI_FORMAT kMinMipOpaque = DXGI_FORMAT(189);
inline constexpr DXGI_FORMAT kMipRegionUsedOpaque = DXGI_FORMAT(190);

inline bool
IsFeedbackFormat(DXGI_FORMAT format) {
  return format == kMinMipOpaque || format == kMipRegionUsedOpaque;
}

struct Layout {
  uint32_t width, height, mips, slices;
  uint32_t region_w_log2, region_h_log2;
  bool region_used;

  uint32_t gridWidth(uint32_t mip = 0) const {
    return std::max(1u, ((width + (1u << region_w_log2) - 1) >> region_w_log2) >> mip);
  }
  uint32_t gridHeight(uint32_t mip = 0) const {
    return std::max(1u, ((height + (1u << region_h_log2) - 1) >> region_h_log2) >> mip);
  }
  /* Cells of mip `mip` (MIN_MIP: the single flattened grid) before it within one slice. */
  uint64_t mipOffset(uint32_t mip) const {
    uint64_t offset = 0;
    for (uint32_t m = 0; m < mip; ++m)
      offset += uint64_t(gridWidth(m)) * gridHeight(m);
    return offset;
  }
  uint64_t sliceCells() const {
    return region_used ? mipOffset(mips) : uint64_t(gridWidth()) * gridHeight();
  }
  uint64_t totalCells() const { return sliceCells() * slices; }
  /* Decoded subresources: MIN_MIP maps have one mip per slice. */
  uint32_t decodedMips() const { return region_used ? mips : 1; }

  uint64_t word() const {
    return uint64_t(width) | uint64_t(height) << 16 | uint64_t(mips) << 32 | uint64_t(slices) << 37 |
           uint64_t(region_w_log2) << 49 | uint64_t(region_h_log2) << 53 | uint64_t(region_used) << 57;
  }
};

/*
 * Validates a feedback map description (D3D12_RESOURCE_DESC1 with a feedback format): TEXTURE2D,
 * single sampled, UNKNOWN layout, ALLOW_UNORDERED_ACCESS, and a mip region whose width and
 * height are powers of two, >= 4 and <= half of the matching mip-0 dimension, depth 0 or 1.
 */
inline bool
MakeLayout(const D3D12_RESOURCE_DESC1 &desc, Layout &out) {
  if (!IsFeedbackFormat(desc.Format) || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 || desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
      !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
      (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER)) ||
      !desc.Width || desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !desc.Height ||
      desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !desc.DepthOrArraySize ||
      desc.DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
    return false;
  const auto &region = desc.SamplerFeedbackMipRegion;
  if (region.Depth > 1 || region.Width < 4 || region.Height < 4 || !std::has_single_bit(region.Width) ||
      !std::has_single_bit(region.Height) || region.Width > desc.Width / 2 || region.Height > desc.Height / 2)
    return false;
  const uint32_t full_mips = std::bit_width(std::max<uint64_t>(desc.Width, desc.Height));
  const uint32_t mips = desc.MipLevels ? desc.MipLevels : full_mips;
  if (mips > full_mips)
    return false;
  out = {uint32_t(desc.Width),
         desc.Height,
         mips,
         desc.DepthOrArraySize,
         uint32_t(std::countr_zero(region.Width)),
         uint32_t(std::countr_zero(region.Height)),
         desc.Format == kMipRegionUsedOpaque};
  return true;
}

/* Sampler descriptor flags (q2 high 32 bits). */
inline uint32_t
SamplerFlags(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE u, D3D12_TEXTURE_ADDRESS_MODE v, UINT max_anisotropy,
             bool non_normalized = false, D3D12_TEXTURE_ADDRESS_MODE w = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
             bool uint_border = false, D3D12_COMPARISON_FUNC comparison = D3D12_COMPARISON_FUNC_NEVER) {
  const bool anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(filter);
  return (uint32_t(u) & 7u) | (uint32_t(v) & 7u) << 3 | uint32_t(D3D12_DECODE_MIN_FILTER(filter) != 0) << 6 |
         uint32_t(D3D12_DECODE_MAG_FILTER(filter) != 0) << 7 | uint32_t(D3D12_DECODE_MIP_FILTER(filter) != 0) << 8 |
         uint32_t(anisotropic) << 9 | (anisotropic ? std::clamp(max_anisotropy, 1u, 16u) : 1u) << 10 |
         uint32_t(non_normalized) << 15 | uint32_t(uint_border) << 16 | (uint32_t(w) & 7u) << 17 |
         uint32_t(D3D12_DECODE_FILTER_REDUCTION(filter)) << 20 | (uint32_t(comparison) & 15u) << 22 | 1u << 31;
}

inline uint64_t
SamplerMetadata(float lod_bias, uint32_t flags) {
  return uint64_t(std::bit_cast<uint32_t>(lod_bias)) | uint64_t(flags) << 32;
}

inline uint64_t
SamplerLodRange(float min_lod, float max_lod) {
  return uint64_t(std::bit_cast<uint32_t>(min_lod)) | uint64_t(std::bit_cast<uint32_t>(max_lod)) << 32;
}

} // namespace dxmt::sampler_feedback

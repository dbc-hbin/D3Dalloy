#pragma once

#include "d3d12.h"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace d3dalloy::sampler_feedback {

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

} // namespace d3dalloy::sampler_feedback

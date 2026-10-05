// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxmt {

// Compatibility advertising, not physical dedicated VRAM or a Windows carveout.
// R is OS-accessible RAM; Metal's dynamic budget remains independent of this.
template <typename Size>
constexpr Size advertisedDedicatedVideoMemory(bool unified, uint64_t recommended, uint64_t ram) {
  static_assert(std::is_unsigned_v<Size> && sizeof(Size) <= sizeof(uint64_t));
  if (!unified)
    return static_cast<Size>(recommended);

  constexpr uint64_t reserve = uint64_t{16} << 30;
  const uint64_t above_reserve = ram > reserve ? ram - reserve : 0;
  // floor(4R/5), without overflowing the multiplication for large R.
  const uint64_t eighty_percent = (ram / 5) * 4 + ((ram % 5) * 4) / 5;
  const uint64_t graphics = std::min(eighty_percent, std::max(above_reserve, ram / 2));
  // DXGI descriptors use SIZE_T: saturate UMA advertising for 32-bit callers.
  return static_cast<Size>(std::min({recommended, graphics, uint64_t{std::numeric_limits<Size>::max()}}));
}

} // namespace dxmt

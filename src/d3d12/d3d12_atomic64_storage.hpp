#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace dxmt::atomic64 {

enum class CanonicalKind : uint32_t { Buffer, HeapBytes, HostBytes, TextureUnits, HeapTextureUnits };

// TextureUnits is a logical native-texture unit domain, never an assertion
// that an opaque Metal heap stores those units in row-major byte order.
struct CanonicalOwner {
  CanonicalKind kind;
  uint64_t identity;
  uint64_t base = 0;
  std::array<uint32_t, 8> layout{};
};

struct CanonicalCell {
  CanonicalOwner owner;
  uint64_t offset;
  bool operator<(const CanonicalCell &other) const {
    return std::tie(owner.kind, owner.identity, owner.layout, offset) <
           std::tie(other.owner.kind, other.owner.identity, other.owner.layout, other.offset);
  }
};

class CanonicalCellMap {
public:
  uint32_t Resolve(CanonicalOwner owner, uint64_t byte_offset) {
    if (byte_offset > std::numeric_limits<uint64_t>::max() - owner.base)
      throw std::length_error("atomic64 canonical offset overflow");
    CanonicalCell key{owner, (owner.base + byte_offset) & ~uint64_t(7)};
    key.owner.base = 0;
    const auto existing = cells_.find(key);
    if (existing != cells_.end())
      return existing->second;
    if (cells_.size() == UINT32_MAX)
      throw std::length_error("atomic64 root index overflow");
    const uint32_t index = static_cast<uint32_t>(cells_.size());
    cells_.emplace(std::move(key), index);
    return index;
  }

  uint32_t Count() const { return static_cast<uint32_t>(cells_.size()); }

private:
  std::map<CanonicalCell, uint32_t> cells_;
};

struct ArenaLayout {
  uint32_t node_count;
  uint32_t bitmap_words;
  uint64_t roots_bytes;
  uint64_t nodes_bytes;
  uint64_t bitmap_bytes;

  static std::optional<ArenaLayout> Make(uint64_t cells, uint64_t participants) {
    if (!participants || participants >= (uint64_t(1) << 30) || cells > UINT32_MAX)
      return std::nullopt;
    const uint64_t nodes = cells + 3 * participants + 1;
    if (nodes > UINT32_MAX)
      return std::nullopt;
    const uint64_t words = (nodes + 31) / 32;
    return ArenaLayout{static_cast<uint32_t>(nodes), static_cast<uint32_t>(words),
                       cells * 4, nodes * 16, words * 4};
  }
};

} // namespace dxmt::atomic64

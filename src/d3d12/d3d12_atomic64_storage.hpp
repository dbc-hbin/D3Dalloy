#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace d3dalloy::atomic64 {

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
    const auto after = runs_.upper_bound(key);
    if (after != runs_.begin()) {
      const auto previous = std::prev(after);
      const auto &owner = previous->first.owner;
      if (std::tie(owner.kind, owner.identity, owner.layout) ==
          std::tie(key.owner.kind, key.owner.identity, key.owner.layout)) {
        const uint64_t distance = (key.offset - previous->first.offset) / 8;
        auto &run = previous->second;
        if (distance < run.count) return run.first + uint32_t(distance);
        // Preserve first-seen IDs exactly. Adjacent physical cells are merged
        // only when their canonical IDs are adjacent too; interleaved aliases
        // and sparse mappings can therefore never silently change identity.
        if (distance == run.count && uint64_t(run.first) + run.count == count_) {
          if (count_ == UINT32_MAX) throw std::length_error("atomic64 root index overflow");
          ++run.count; return count_++;
        }
      }
    }
    if (count_ == UINT32_MAX)
      throw std::length_error("atomic64 root index overflow");
    runs_.emplace_hint(after, std::move(key), Run{count_, 1});
    return count_++;
  }

  uint32_t Count() const { return count_; }
  size_t RunCount() const { return runs_.size(); }

private:
  struct Run { uint32_t first; uint64_t count; };
  std::map<CanonicalCell, Run> runs_;
  uint32_t count_ = 0;
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

} // namespace d3dalloy::atomic64

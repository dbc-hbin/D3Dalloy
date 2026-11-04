// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace owned_graphics {

struct RTASSizes {
  uint64_t accelerationStructureSize;
  uint64_t buildScratchSize;
  uint64_t refitScratchSize;

  bool operator==(const RTASSizes &) const = default;
};

struct RTASGeometrySizeKey {
  uint32_t type, flags;
  uint64_t primitiveCount, stride;
  uint32_t vertexFormat, indexFormat;
  bool hasTransform, hasVertexBuffer, hasIndexBuffer, hasTransformBuffer;

  bool operator==(const RTASGeometrySizeKey &) const = default;
};

struct RTASSizeKey {
  uint32_t type = 0, flags = 0, metalUsage = 0, instanceCount = 0;
  std::vector<RTASGeometrySizeKey> geometries;

  bool operator==(const RTASSizeKey &) const = default;
};

// Device-local, bounded immutable size results. The caller serializes access;
// native queries run outside that lock. Keys hold no buffers or descriptors.
class RTASSizeCache {
public:
  static constexpr size_t kCapacity = 64;
  static constexpr size_t kMaxGeometries = 128;

  bool Find(const RTASSizeKey &key, RTASSizes &sizes) const {
    if (key.geometries.size() > kMaxGeometries)
      return false;
    for (const auto &entry : entries_) {
      if (entry.valid && entry.key == key) {
        sizes = entry.sizes;
        return true;
      }
    }
    return false;
  }

  bool Insert(RTASSizeKey key, RTASSizes sizes) {
    // Zero refit scratch is valid. Zero storage/build scratch is a failed native
    // query, so it must remain retryable instead of becoming a cached failure.
    if (!sizes.accelerationStructureSize || !sizes.buildScratchSize ||
        key.geometries.size() > kMaxGeometries || key.geometries.capacity() > kMaxGeometries)
      return false;
    for (auto &entry : entries_) {
      if (entry.valid && entry.key == key) {
        entry.sizes = sizes;
        return true;
      }
    }
    entries_[next_] = {std::move(key), sizes, true};
    next_ = (next_ + 1) % kCapacity;
    return true;
  }

private:
  struct Entry {
    RTASSizeKey key;
    RTASSizes sizes{};
    bool valid = false;
  };

  std::array<Entry, kCapacity> entries_{};
  size_t next_ = 0;
};

} // namespace owned_graphics

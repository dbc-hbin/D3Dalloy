#pragma once

#include "sha1/sha1_util.hpp"
#include <memory>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace d3dalloy {

// Read-only CPU input, never a compiler/LLVM handle. PSOs and root signatures
// own their references; cache membership cannot extend their byte storage lifetime.
class SharedShaderBytecode {
  struct Storage {
    std::vector<uint8_t> bytes;
    Sha1Digest digest;
    Storage(const void *source, size_t length, const Sha1Digest &hash)
        : bytes(static_cast<const uint8_t *>(source), static_cast<const uint8_t *>(source) + length), digest(hash) {}
  };
  std::shared_ptr<const Storage> storage_;
  friend class ShaderBytecodePool;
  explicit SharedShaderBytecode(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
public:
  SharedShaderBytecode() = default;
  const uint8_t *data() const { return storage_ ? storage_->bytes.data() : nullptr; }
  size_t size() const { return storage_ ? storage_->bytes.size() : 0; }
  bool empty() const { return !size(); }
  Sha1Digest digest() const { return storage_ ? storage_->digest : Sha1HashState::compute(nullptr, 0); }
};

class ShaderBytecodePool {
  using Storage = SharedShaderBytecode::Storage;
  std::mutex mutex_;
  std::unordered_multimap<Sha1Digest, std::weak_ptr<const Storage>> entries_;
  // A digest is an index, not identity. Compare every byte even on collisions,
  // and never index caller pointers whose contents/lifetime are unowned.
  std::shared_ptr<const Storage> Find(const Sha1Digest &hash, const void *source, size_t length) {
    const auto [begin, end] = entries_.equal_range(hash);
    for (auto it = begin; it != end;) {
      if (auto stored = it->second.lock()) {
        if (stored->bytes.size() == length && !std::memcmp(stored->bytes.data(), source, length)) return stored;
        ++it;
      } else it = entries_.erase(it);
    }
    return {};
  }
public:
  SharedShaderBytecode Capture(const void *source, size_t length) {
    if (!source || !length) return {};
    const auto hash = Sha1HashState::compute(source, length);
    {
      std::lock_guard lock(mutex_);
      if (auto stored = Find(hash, source, length)) return SharedShaderBytecode(std::move(stored));
    }
    // Keep the mandatory owning copy outside the pool lock. Concurrent misses
    // recheck before publishing; only immutable storage can escape this method.
    auto owned = std::make_shared<const Storage>(source, length, hash);
    try {
      std::lock_guard lock(mutex_);
      if (auto stored = Find(hash, owned->bytes.data(), length)) return SharedShaderBytecode(std::move(stored));
      constexpr size_t limit = 256;
      if (entries_.size() >= limit)
        for (auto it = entries_.begin(); it != entries_.end();)
          it = it->second.expired() ? entries_.erase(it) : std::next(it);
      // Bounded weak metadata. A full pool skips admission, not the owned copy.
      if (entries_.size() < limit) entries_.emplace(hash, owned);
    } catch (const std::bad_alloc &) {
      // Optional sharing must not fail an already successful input snapshot.
    }
    return SharedShaderBytecode(std::move(owned));
  }
};

} // namespace d3dalloy

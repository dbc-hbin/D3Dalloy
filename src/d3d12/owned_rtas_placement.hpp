#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace owned_graphics {

// Only live physical aliases invalidate a placement. Once invalidated, a retained
// allocation never regains proof: the alias may already have overwritten its AS.
class RTASPlacementRanges : public std::enable_shared_from_this<RTASPlacementRanges> {
  struct Entry { uint64_t end; std::atomic<bool> *exclusive; };
  using Ranges = std::multimap<uint64_t, Entry>;
  std::mutex mutex_;
  Ranges ranges_;
  uint64_t max_span_ = 0;
  bool disabled_ = false;

  struct Lease {
    std::shared_ptr<RTASPlacementRanges> owner;
    Ranges::iterator entry;
    std::atomic<bool> exclusive{true};
    ~Lease() {
      if (owner) {
        std::lock_guard lock(owner->mutex_);
        owner->ranges_.erase(entry);
      }
    }
  };

public:
  // Native placement validation has already checked begin + size against the heap.
  std::shared_ptr<const std::atomic<bool>> Register(uint64_t begin, uint64_t size) {
    std::lock_guard lock(mutex_);
    if (disabled_) return {};
    try {
      auto lease = std::make_shared<Lease>();
      const uint64_t end = begin + size;
      auto it = ranges_.lower_bound(begin > max_span_ ? begin - max_span_ : 0);
      for (; it != ranges_.end() && it->first < end; ++it) {
        if (begin < it->second.end) {
          it->second.exclusive->store(false, std::memory_order_release);
          lease->exclusive.store(false, std::memory_order_relaxed);
        }
      }
      lease->entry = ranges_.emplace(begin, Entry{end, &lease->exclusive});
      lease->owner = shared_from_this();
      max_span_ = std::max(max_span_, size);
      return {lease, &lease->exclusive};
    } catch (const std::bad_alloc &) {
      DisableLocked(); // The resource can still exist even though its span cannot be tracked.
      return {};
    }
  }

  void Disable() {
    std::lock_guard lock(mutex_);
    DisableLocked();
  }

private:
  void DisableLocked() {
    disabled_ = true;
    for (const auto &range : ranges_)
      range.second.exclusive->store(false, std::memory_order_release);
  }
};

} // namespace owned_graphics

#pragma once
#include <cassert>
#include <mutex>
#include <vector>

namespace d3dalloy {
// A queue segment keeps each feature's reservation through native commit and
// event publication, not GPU completion. Never block acquiring a second feature:
// the caller must first commit/release this set, then AcquireFirst on a fresh
// segment. This prevents AB/BA deadlocks without serializing unrelated features.
// Recording/inflight frame owners must outlive the mutex references in this set.
class OwnedTemporalSubmissionLocks {
  std::vector<std::unique_lock<std::mutex>> locks_;
public:
  bool TryAcquire(std::mutex &mutex) {
    for (const auto &lock : locks_) if (lock.mutex() == &mutex) return true;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock) return false;
    locks_.push_back(std::move(lock));
    return true;
  }
  void AcquireFirst(std::mutex &mutex) {
    assert(locks_.empty());
    locks_.reserve(1);
    locks_.emplace_back(mutex);
  }
};
} // namespace d3dalloy

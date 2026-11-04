#pragma once
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <span>
#include <utility>

namespace d3dalloy {
struct OwnedHeapRange {
  uintptr_t heap = 0;
  uint64_t begin = 0, end = 0;
};

// Negative proof only: ordinary heap accesses may omit sparse publication when
// no currently mapped physical tile overlaps. Reserved/cast/shadow accesses keep
// their existing conservative path. Native heap owners stay with the resources.
class OwnedSparseHeapIndex {
  using Pages = std::map<uint32_t, uint64_t>;
  mutable std::mutex mutex_;
  std::map<uintptr_t, Pages> heaps_;
  uint64_t generation_ = 1;
  uint64_t pending_ = 0;
  bool known_ = true;

public:
  static constexpr uint64_t kTileBytes = 65536;
  struct Proof {
    uint64_t generation = 0;
    bool disjoint = false;
  };

  // CPU targets may already describe a remap while native users still see the
  // old pages. Keep negative proofs conservative until every queued publication
  // has actually retired. Completion never takes the queue-enqueue mutex.
  class Pending {
    friend class OwnedSparseHeapIndex;
    OwnedSparseHeapIndex *index_ = nullptr;
    explicit Pending(OwnedSparseHeapIndex &index) noexcept : index_(&index) {
      std::lock_guard lock(index.mutex_);
      if (index.pending_ == UINT64_MAX) {
        index.known_ = false;
        index_ = nullptr;
      } else ++index.pending_;
    }
  public:
    Pending() = default;
    Pending(const Pending &) = delete;
    Pending &operator=(const Pending &) = delete;
    Pending(Pending &&other) noexcept : index_(std::exchange(other.index_, nullptr)) {}
    Pending &operator=(Pending &&other) noexcept {
      if (this != &other) {
        Retire(false);
        index_ = std::exchange(other.index_, nullptr);
      }
      return *this;
    }
    ~Pending() { Retire(false); }
    explicit operator bool() const { return index_ != nullptr; }
    void Retire(bool completed) noexcept {
      auto *index = std::exchange(index_, nullptr);
      if (!index) return;
      std::lock_guard lock(index->mutex_);
      if (!completed) {
        index->known_ = false;
        index->heaps_.clear();
      }
      --index->pending_;
    }
  };

  Pending BeginPending() { return Pending(*this); }

  class Update {
    friend class OwnedSparseHeapIndex;
    OwnedSparseHeapIndex *index_;
    std::unique_lock<std::mutex> lock_;
    int exceptions_;
    bool changed_ = false;

    explicit Update(OwnedSparseHeapIndex &index) :
        index_(&index), lock_(index.mutex_), exceptions_(std::uncaught_exceptions()) {}

    void Unknown() noexcept {
      index_->known_ = false;
      // Optional bookkeeping must not leave a partially updated catalog usable
      // after failure. Clearing nodes allocates nothing and releases no native owners.
      index_->heaps_.clear();
      changed_ = true;
    }

  public:
    Update(const Update &) = delete;
    Update &operator=(const Update &) = delete;
    Update(Update &&other) noexcept :
        index_(std::exchange(other.index_, nullptr)), lock_(std::move(other.lock_)),
        exceptions_(other.exceptions_), changed_(other.changed_) {}
    Update &operator=(Update &&) = delete;

    ~Update() noexcept {
      if (!index_) return;
      if (std::uncaught_exceptions() > exceptions_) Unknown();
      if (changed_) {
        if (index_->generation_ == UINT64_MAX) Unknown();
        else ++index_->generation_;
      }
    }

    void Add(uintptr_t heap, uint32_t tile) noexcept {
      if (!index_->known_) return;
      if (!heap) { Unknown(); return; }
      try {
        auto &pages = index_->heaps_.try_emplace(heap).first->second;
        auto &count = pages.try_emplace(tile, 0).first->second;
        if (count == UINT64_MAX) { Unknown(); return; }
        ++count;
        changed_ = true;
      } catch (...) {
        // Index storage is an optimization: mapping may continue, but no later
        // caller may infer absence from an incomplete catalog.
        Unknown();
      }
    }

    void Remove(uintptr_t heap, uint32_t tile) noexcept {
      if (!index_->known_) return;
      const auto owner = index_->heaps_.find(heap);
      if (owner == index_->heaps_.end()) { Unknown(); return; }
      auto &pages = owner->second;
      const auto page = pages.find(tile);
      if (page == pages.end() || !page->second) { Unknown(); return; }
      if (!--page->second) pages.erase(page);
      if (pages.empty()) index_->heaps_.erase(owner);
      changed_ = true;
    }
  };

  Update BeginUpdate() { return Update(*this); }

  Proof Prove(std::span<const OwnedHeapRange> ranges, std::mutex &native_mapping_mutex) const {
    // The gate protects enqueue/CPU publication; pending_ covers the remaining
    // native lifetime. Neither a busy gate nor pending GPU work causes a wait.
    std::unique_lock native_lock(native_mapping_mutex, std::try_to_lock);
    if (!native_lock.owns_lock()) return {};
    std::lock_guard lock(mutex_);
    Proof proof{generation_, false};
    if (!known_ || pending_ || ranges.empty()) return proof;
    for (const auto &range : ranges) {
      if (!range.heap || range.begin >= range.end) return proof;
      const auto heap = heaps_.find(range.heap);
      if (heap == heaps_.end()) continue;
      const uint64_t first = range.begin / kTileBytes;
      const uint64_t last = (range.end - 1) / kTileBytes;
      if (first > UINT32_MAX) continue;
      const auto page = heap->second.lower_bound(uint32_t(first));
      if (page != heap->second.end() && uint64_t(page->first) <= last) return proof;
    }
    proof.disjoint = true;
    return proof;
  }

  bool Current(Proof proof, std::mutex &native_mapping_mutex) const {
    std::unique_lock native_lock(native_mapping_mutex, std::try_to_lock);
    if (!native_lock.owns_lock()) return false;
    std::lock_guard lock(mutex_);
    return known_ && !pending_ && proof.disjoint && proof.generation == generation_;
  }
};
} // namespace d3dalloy

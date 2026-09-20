#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace owned_graphics {

// A bundle may be reset after submission, including while that submission is
// still in flight, but not while an unsubmitted parent refers to it.
struct BundleRecordingState {
  std::atomic<uint64_t> unsubmitted{0};
};

class BundleReference {
  std::shared_ptr<BundleRecordingState> recording_;
  std::shared_ptr<std::atomic<uint64_t>> allocator_generation_;
  uint64_t generation_ = 0;
  bool pending_ = false;

public:
  BundleReference(std::shared_ptr<BundleRecordingState> recording,
                  std::shared_ptr<std::atomic<uint64_t>> allocator_generation,
                  uint64_t generation) noexcept
      : recording_(std::move(recording)), allocator_generation_(std::move(allocator_generation)),
        generation_(generation), pending_(true) {
    recording_->unsubmitted.fetch_add(1, std::memory_order_relaxed);
  }
  BundleReference(const BundleReference &) = delete;
  BundleReference &operator=(const BundleReference &) = delete;
  BundleReference(BundleReference &&other) noexcept
      : recording_(std::move(other.recording_)),
        allocator_generation_(std::move(other.allocator_generation_)), generation_(other.generation_),
        pending_(std::exchange(other.pending_, false)) {}
  BundleReference &operator=(BundleReference &&other) noexcept {
    if (this != &other) {
      submitted();
      recording_ = std::move(other.recording_);
      allocator_generation_ = std::move(other.allocator_generation_);
      generation_ = other.generation_;
      pending_ = std::exchange(other.pending_, false);
    }
    return *this;
  }
  ~BundleReference() { submitted(); }

  bool valid() const noexcept {
    return allocator_generation_ && allocator_generation_->load(std::memory_order_acquire) == generation_;
  }
  void submitted() noexcept {
    if (std::exchange(pending_, false))
      recording_->unsubmitted.fetch_sub(1, std::memory_order_release);
  }
};

} // namespace owned_graphics

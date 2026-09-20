// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned D3D12 queue ordering: CPU-side producer-commit visibility per fence generation.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace owned_graphics {

class FenceSignalSchedule {
public:
  enum class Result {
    Accepted,
    DecreasingUnsupported,
    MixedProducerUnsupported,
  };

  struct State {
    uint64_t last_scheduled = 0;
    uint64_t queue_producer = 0;
    bool cpu_producer = false;
  };

private:
  uint64_t last_scheduled_ = 0;
  uint64_t queue_producer_ = 0;
  bool cpu_producer_ = false;

public:
  Result initialize(uint64_t value) {
    if (value < last_scheduled_)
      return Result::DecreasingUnsupported;
    last_scheduled_ = value;
    return Result::Accepted;
  }

  Result scheduleQueue(uint64_t queue, uint64_t value, bool previous_settled) {
    if (value < last_scheduled_)
      return Result::DecreasingUnsupported;
    if (cpu_producer_ || (queue_producer_ && queue_producer_ != queue)) {
      if (!previous_settled)
        return Result::MixedProducerUnsupported;
      cpu_producer_ = false;
    }
    queue_producer_ = queue;
    last_scheduled_ = value;
    return Result::Accepted;
  }

  Result scheduleCpu(uint64_t value, bool previous_settled) {
    if (value < last_scheduled_)
      return Result::DecreasingUnsupported;
    if (queue_producer_) {
      if (!previous_settled)
        return Result::MixedProducerUnsupported;
      queue_producer_ = 0;
    }
    cpu_producer_ = true;
    last_scheduled_ = value;
    return Result::Accepted;
  }

  void reset() {
    last_scheduled_ = 0;
    queue_producer_ = 0;
    cpu_producer_ = false;
  }

  uint64_t lastScheduled() const {
    return last_scheduled_;
  }

  State state() const {
    return State{last_scheduled_, queue_producer_, cpu_producer_};
  }

  void restore(State state) {
    last_scheduled_ = state.last_scheduled;
    queue_producer_ = state.queue_producer;
    cpu_producer_ = state.cpu_producer;
  }
};

class FenceOrderEpoch {
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  uint64_t committed_value_ = 0;
  const uint64_t generation_;

public:
  explicit FenceOrderEpoch(uint64_t generation) : generation_(generation) {}

  uint64_t generation() const {
    return generation_;
  }

  uint64_t committedValue() const {
    std::lock_guard lock(mutex_);
    return committed_value_;
  }

  bool ready(uint64_t value) const {
    std::lock_guard lock(mutex_);
    return committed_value_ >= value;
  }

  void markCommitted(uint64_t value) {
    {
      std::lock_guard lock(mutex_);
      if (value <= committed_value_)
        return;
      committed_value_ = value;
    }
    cv_.notify_all();
  }

  template <typename Rep, typename Period>
  bool waitFor(uint64_t value, const std::chrono::duration<Rep, Period> &timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return committed_value_ >= value; });
  }
};

} // namespace owned_graphics

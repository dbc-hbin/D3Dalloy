// SPDX-License-Identifier: LGPL-2.1-or-later
// Logical D3D12 fence timeline independent of monotonic native event values.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace owned_graphics {

class FenceOrderEpoch {
public:
  struct Producer {
    uint64_t id = 0;
    uint64_t value = 0;
    bool published = false;
    bool completed = false;
    std::function<bool()> completion_probe;
  };

private:
  struct CompletionWaiter {
    uint64_t value = 0;
    std::function<void()> callback;
  };

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  uint64_t next_producer_id_ = 1;
  uint64_t completed_value_ = 0;
  std::vector<std::shared_ptr<Producer>> producers_;
  std::vector<CompletionWaiter> completion_waiters_;
  const uint64_t generation_;

  void completeValueLocked(uint64_t value) {
    completed_value_ = value;
    for (auto it = completion_waiters_.begin(); it != completion_waiters_.end();) {
      if (value >= it->value) {
        auto callback = std::move(it->callback);
        it = completion_waiters_.erase(it);
        callback();
      } else {
        ++it;
      }
    }
  }

  void reconcileLocked() {
    for (auto it = producers_.begin(); it != producers_.end();) {
      const auto &producer = *it;
      if (producer->published && !producer->completed && producer->completion_probe && producer->completion_probe()) {
        producer->completed = true;
        completeValueLocked(producer->value);
        it = producers_.erase(it);
      } else {
        ++it;
      }
    }
  }

public:
  explicit FenceOrderEpoch(uint64_t generation, uint64_t initial_value = 0)
      : completed_value_(initial_value), generation_(generation) {}

  uint64_t generation() const {
    return generation_;
  }

  std::shared_ptr<Producer> prepare(uint64_t value, std::function<bool()> completion_probe = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto producer = std::make_shared<Producer>();
    producer->id = next_producer_id_++;
    producer->value = value;
    producer->completion_probe = std::move(completion_probe);
    producers_.push_back(producer);
    return producer;
  }

  void publish(const std::shared_ptr<Producer> &producer) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      producer->published = true;
    }
    cv_.notify_all();
  }

  void cancel(const std::shared_ptr<Producer> &producer) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = producers_.begin(); it != producers_.end(); ++it) {
      if (*it == producer) {
        producers_.erase(it);
        return;
      }
    }
  }

  void complete(const std::shared_ptr<Producer> &producer) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (producer->completed)
        return;
      producer->completed = true;
      completeValueLocked(producer->value);
      for (auto it = producers_.begin(); it != producers_.end(); ++it) {
        if (*it == producer) {
          producers_.erase(it);
          break;
        }
      }
    }
    cv_.notify_all();
  }

  void signalCpu(uint64_t value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Probe and publish share one critical section. A GPU token observed here
      // is ordered before this CPU signal; a retirement arriving later is after it.
      reconcileLocked();
      completeValueLocked(value);
    }
    cv_.notify_all();
  }

  void reconcile() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      reconcileLocked();
    }
    cv_.notify_all();
  }

  uint64_t completedValue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_value_;
  }

  bool addCompletionWaiter(uint64_t value, std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Registration and its initial observation are atomic with completion and
    // rewind, so a qualifying crossing cannot disappear between two calls.
    reconcileLocked();
    if (completed_value_ >= value) {
      callback();
      return true;
    }
    try {
      completion_waiters_.push_back(CompletionWaiter{value, std::move(callback)});
    } catch (...) {
      return false;
    }
    return true;
  }
};

class FenceCompletionLatch {
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::function<void()>> callbacks_;
  bool signaled_ = false;

public:
  void signal() {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (signaled_)
        return;
      signaled_ = true;
      callbacks.swap(callbacks_);
    }
    cv_.notify_all();
    for (auto &callback : callbacks)
      callback();
  }

  bool addWaiter(std::function<void()> callback) {
    bool ready_now = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_now = signaled_;
      if (!ready_now) {
        try {
          callbacks_.push_back(std::move(callback));
        } catch (...) {
          return false;
        }
      }
    }
    if (ready_now)
      callback();
    return true;
  }

  bool ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_;
  }

  template <typename Rep, typename Period>
  bool waitFor(const std::chrono::duration<Rep, Period> &timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return signaled_; });
  }
};

} // namespace owned_graphics

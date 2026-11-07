// SPDX-License-Identifier: LGPL-2.1-or-later
// Logical D3D12 fence timeline published only after ordered completion validation.
#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <list>
#include <mutex>
#include <vector>

namespace owned_graphics {

class FenceOrderEpoch {
public:
  struct Producer {
    uint64_t value = 0;
    bool completed = false;
  };

private:
  struct CompletionWaiter {
    uint64_t value = 0;
    uint64_t id = 0;
    std::function<void()> callback;
  };

  mutable std::mutex mutex_;
  // Written only under mutex_; also read lock-free by completedValue(). The
  // release store publishes every write the completing thread made first.
  std::atomic<uint64_t> completed_value_ = 0;
  bool removed_ = false;
  std::weak_ptr<Producer> committed_producer_;
  std::list<CompletionWaiter> completion_waiters_;
  uint64_t next_waiter_id_ = 1;
  const uint64_t generation_;

  void completeValueLocked(uint64_t value, std::list<CompletionWaiter> &ready) {
    completed_value_.store(value, std::memory_order_release);
    for (auto it = completion_waiters_.begin(); it != completion_waiters_.end();) {
      if (value >= it->value) {
        auto completed = it++;
        ready.splice(ready.end(), completion_waiters_, completed);
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

  std::shared_ptr<Producer> prepare(uint64_t value) {
    auto producer = std::make_shared<Producer>();
    producer->value = value;
    return producer;
  }

  void complete(const std::shared_ptr<Producer> &producer) {
    std::list<CompletionWaiter> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (removed_ || producer->completed)
        return;
      producer->completed = true;
      completeValueLocked(producer->value, ready);
    }
    for (auto &waiter : ready)
      waiter.callback();
  }

  // A submitted local producer can finish independently of a consumer preparing its next list.
  // This is a progress proof only. The waiter's own completion latch still chooses the crossing.
  void markSubmitted(const std::shared_ptr<Producer> &producer) {
    std::lock_guard lock(mutex_);
    committed_producer_ = producer;
  }

  bool hasSubmittedProducer(uint64_t value) const {
    std::lock_guard lock(mutex_);
    auto producer = committed_producer_.lock();
    return !removed_ && producer && !producer->completed && producer->value >= value;
  }

  void signalCpu(uint64_t value) {
    std::list<CompletionWaiter> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!removed_)
        completeValueLocked(value, ready);
    }
    for (auto &waiter : ready)
      waiter.callback();
  }

  // Device removal is terminal, unlike a user signal of UINT64_MAX. Release
  // every pending/late waiter, and never let a delayed producer rewind it.
  void remove() {
    std::list<CompletionWaiter> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (removed_)
        return;
      removed_ = true;
      completeValueLocked(UINT64_MAX, ready);
    }
    for (auto &waiter : ready)
      waiter.callback();
  }

  uint64_t completedValue() const {
    return completed_value_.load(std::memory_order_acquire);
  }

  bool addCompletionWaiter(uint64_t value, std::function<void()> callback, uint64_t *registration = nullptr) {
    // Allocate before locking: callback destruction can itself cancel a wait.
    std::list<CompletionWaiter> pending;
    try {
      pending.push_back(CompletionWaiter{value, 0, std::move(callback)});
    } catch (...) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Registration and its initial observation are atomic with completion and
      // rewind, so a qualifying crossing cannot disappear between two calls.
      if (completed_value_.load(std::memory_order_relaxed) >= value) {
        if (registration)
          *registration = 0;
      } else {
        const uint64_t id = registration ? next_waiter_id_++ : 0;
        pending.front().id = id;
        completion_waiters_.splice(completion_waiters_.end(), pending);
        if (registration)
          *registration = id;
      }
    }
    if (!pending.empty())
      pending.front().callback();
    return true;
  }

  void removeCompletionWaiter(uint64_t registration) {
    if (!registration)
      return;
    std::list<CompletionWaiter> cancelled;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = completion_waiters_.begin(); it != completion_waiters_.end(); ++it)
      if (it->id == registration) {
        cancelled.splice(cancelled.end(), completion_waiters_, it);
        return;
      }
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

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return signaled_; });
  }

  bool waitUnless(const std::atomic<bool> &stopping) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return signaled_ || stopping.load(std::memory_order_acquire); });
    return signaled_;
  }

  // Wakes only a queue-local waiter. The latch remains unsignaled for every
  // other queue and any application completion event observing this fence.
  void wake() {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
  }
};

} // namespace owned_graphics

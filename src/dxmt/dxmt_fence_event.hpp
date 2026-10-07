// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "dxmt_queue_order.hpp"
#include <utility>

namespace owned_graphics {

class FenceWaitSubscription {
  std::weak_ptr<FenceOrderEpoch> order_;
  uint64_t id_ = 0;

public:
  FenceWaitSubscription() = default;
  FenceWaitSubscription(const std::shared_ptr<FenceOrderEpoch> &order, uint64_t id) : order_(order), id_(id) {}
  FenceWaitSubscription(const FenceWaitSubscription &) = delete;
  FenceWaitSubscription &operator=(const FenceWaitSubscription &) = delete;
  FenceWaitSubscription(FenceWaitSubscription &&other) noexcept
      : order_(std::move(other.order_)), id_(std::exchange(other.id_, 0)) {}
  FenceWaitSubscription &operator=(FenceWaitSubscription &&other) noexcept {
    cancel();
    order_ = std::move(other.order_);
    id_ = std::exchange(other.id_, 0);
    return *this;
  }
  ~FenceWaitSubscription() { cancel(); }
  void cancel() {
    const auto id = std::exchange(id_, 0);
    if (id)
      if (auto order = order_.lock())
        order->removeCompletionWaiter(id);
  }
};

struct FenceWaitValue {
  std::shared_ptr<FenceCompletionLatch> latch;
  FenceWaitSubscription subscription;

  static FenceWaitValue prepare(const std::shared_ptr<FenceOrderEpoch> &order, uint64_t value) {
    auto latch = std::make_shared<FenceCompletionLatch>();
    uint64_t id = 0;
    if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }, &id))
      throw std::bad_alloc();
    return {std::move(latch), FenceWaitSubscription(order, id)};
  }
};

// Own only cancellation tokens, never the latches retaining our callbacks.
// Detached callbacks may still run after cancellation; the one-shot state wins.
class FenceEventRegistration {
  std::mutex mutex_;
  std::vector<bool> reached_;
  std::vector<FenceWaitSubscription> subscriptions_;
  uint32_t remaining_;
  bool wait_all_;
  bool armed_ = false;
  bool finished_ = false;
  std::function<void()> fire_;

public:
  FenceEventRegistration(uint32_t count, bool wait_all, std::function<void()> fire)
      : reached_(count, false), remaining_(count), wait_all_(wait_all), fire_(std::move(fire)) {
    subscriptions_.reserve(count);
  }

  // Called only during setup, before arm allows a completion to cancel tokens.
  void take(FenceWaitSubscription subscription) { subscriptions_.push_back(std::move(subscription)); }

  void reach(uint32_t index) {
    std::vector<FenceWaitSubscription> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finished_ || reached_[index])
        return;
      reached_[index] = true;
      --remaining_;
      if (!armed_ || (wait_all_ && remaining_))
        return;
      finished_ = true;
      cancelled.swap(subscriptions_);
    }
    // ponytail: each removal scans its epoch; index IDs if cancellation fanout matters.
    cancelled.clear();
    fire_();
  }

  void arm() {
    std::vector<FenceWaitSubscription> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finished_)
        return;
      armed_ = true;
      if (wait_all_ ? remaining_ != 0 : remaining_ == reached_.size())
        return;
      finished_ = true;
      cancelled.swap(subscriptions_);
    }
    cancelled.clear();
    fire_();
  }

  void cancel() {
    std::vector<FenceWaitSubscription> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
      cancelled.swap(subscriptions_);
    }
  }

  static bool set(FenceWaitValue *values, uint32_t count, bool wait_all, std::function<void()> fire) {
    std::shared_ptr<FenceEventRegistration> registration;
    try {
      registration = std::make_shared<FenceEventRegistration>(count, wait_all, std::move(fire));
      for (uint32_t i = 0; i < count; ++i) {
        registration->take(std::move(values[i].subscription));
        if (!values[i].latch->addWaiter([registration, i] { registration->reach(i); }))
          throw std::bad_alloc();
      }
      registration->arm();
      return true;
    } catch (const std::bad_alloc &) {
      if (registration)
        registration->cancel();
      for (uint32_t i = 0; i < count; ++i)
        values[i].subscription.cancel();
      return false;
    }
  }
};

} // namespace owned_graphics

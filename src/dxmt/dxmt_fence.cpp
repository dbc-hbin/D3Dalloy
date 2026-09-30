/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#include "dxmt_fence.hpp"

#include <new>
#include <vector>

namespace dxmt {

void
Fence::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
}

void
Fence::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
}

std::shared_ptr<owned_graphics::FenceOrderEpoch>
Fence::currentOrder() const {
  std::lock_guard<std::mutex> lock(schedule_mutex_);
  return order_;
}

bool
Fence::initialize(uint64_t value) {
  std::lock_guard<std::mutex> lock(schedule_mutex_);
  try {
    order_ = std::make_shared<owned_graphics::FenceOrderEpoch>(++generation_, value);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

uint64_t
Fence::completedValue() {
  auto order = currentOrder();
  if (shared_event_)
    observeShared();
  return order->completedValue();
}

bool
Fence::signal(uint64_t value) {
  auto order = currentOrder();
  if (shared_event_) {
    shared_event_.signalValue(reserveSharedToken(value));
    observeShared();
    return true;
  }
  order->signalCpu(value);
  return true;
}

uint64_t
Fence::reserveSharedToken(uint64_t value) {
  /*
   * Backpressure: a token may be reserved only while fewer than kRing tokens are
   * outstanding beyond the event's current value, so the ring entry of the current
   * token is never overwritten and every observer can always resolve it. Callers are
   * the CPU Signal thread or the queue submission worker; the GPU never waits here.
   * Later tokens need not complete in order: any signal moving the event forward
   * frees ring space.
   */
  uint64_t token = 0;
  for (;;) {
    const auto current = uint64_t(InterlockedCompareExchange64(&timeline_->next_token, 0, 0));
    const uint64_t signaled = shared_event_.signaledValue();
    if (current + 1 - signaled >= SharedFenceTimeline::kRing) {
      shared_event_.waitUntilSignaledValue(current + 2 - SharedFenceTimeline::kRing, 100);
      continue;
    }
    if (uint64_t(InterlockedCompareExchange64(&timeline_->next_token, LONG64(current + 1), LONG64(current))) ==
        current) {
      token = current + 1;
      break;
    }
  }
  auto &entry = timeline_->ring[token % SharedFenceTimeline::kRing];
  InterlockedExchange64(&entry.logical, LONG64(value));
  // Publish the mapping before any signal can make the event reach `token`.
  InterlockedExchange64(&entry.token, LONG64(token));
  return token;
}

uint64_t
Fence::observeShared() {
  auto order = currentOrder();
  std::lock_guard<std::mutex> lock(observe_mutex_);
  const uint64_t token = shared_event_.signaledValue();
  if (token <= observed_token_.load(std::memory_order_relaxed))
    return order->completedValue();
  const auto &entry = timeline_->ring[token % SharedFenceTimeline::kRing];
  const uint64_t logical = uint64_t(InterlockedCompareExchange64(const_cast<LONG64 *>(&entry.logical), 0, 0));
  // reserveSharedToken's backpressure keeps the current token's entry intact; a mismatch
  // can only be a reservation racing ahead of the event, which a later read resolves.
  if (uint64_t(InterlockedCompareExchange64(const_cast<LONG64 *>(&entry.token), 0, 0)) != token ||
      uint64_t(InterlockedCompareExchange64(const_cast<LONG64 *>(&entry.logical), 0, 0)) != logical)
    return order->completedValue();
  observed_token_.store(token, std::memory_order_release);
  // Rewind-aware: a lower logical value (D3D12 Signal(lower)) moves the local timeline back.
  order->signalCpu(logical);
  return logical;
}

bool
Fence::initializeShared(WMT::Reference<WMT::SharedEvent> event, SharedFenceTimeline *timeline,
                        std::shared_ptr<void> owner, bool publish_initial, uint64_t initial_value) {
  if (!event || !timeline || watcher_)
    return false;
  auto order = currentOrder();
  if (!order)
    return false;
  shared_event_ = std::move(event);
  timeline_ = timeline;
  timeline_owner_ = std::move(owner);
  if (publish_initial)
    shared_event_.signalValue(reserveSharedToken(initial_value));
  observeShared();
  watcher_ = CreateThread(nullptr, 0, SharedWatcher, this, 0, nullptr);
  return watcher_ != nullptr;
}

DWORD WINAPI
Fence::SharedWatcher(void *arg) {
  auto *fence = static_cast<Fence *>(arg);
  while (!fence->watcher_stop_.load(std::memory_order_acquire)) {
    const uint64_t token = fence->shared_event_.signaledValue();
    fence->observeShared();
    // Wakes as soon as any device or process publishes a later token; the
    // timeout only bounds how long destruction waits for this thread.
    fence->shared_event_.waitUntilSignaledValue(token + 1, 25);
  }
  return 0;
}

Fence::~Fence() {
  if (watcher_) {
    watcher_stop_.store(true, std::memory_order_release);
    WaitForSingleObject(watcher_, INFINITE);
    CloseHandle(watcher_);
  }
}

Fence::QueueTicket
Fence::prepareQueueSignal(uint64_t value) {
  auto order = currentOrder();
  // Native completion is not enough: publish only after the ordered queue
  // completion thread has validated all preceding GPU work.
  return QueueTicket{order, order->prepare(value), {}, order->generation(), value};
}

Fence::QueueTicket
Fence::prepareQueueWait(uint64_t value) {
  auto order = currentOrder();
  auto latch = std::make_shared<owned_graphics::FenceCompletionLatch>();
  uint64_t waiter_id = 0;
  if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }, &waiter_id))
    throw std::bad_alloc();
  return QueueTicket{order, {}, std::move(latch), order->generation(), value, waiter_id};
}

FenceWaitValue
Fence::prepareWaitValue(uint64_t value) {
  auto order = currentOrder();
  auto latch = std::make_shared<owned_graphics::FenceCompletionLatch>();
  if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }))
    throw std::bad_alloc();
  return FenceWaitValue{std::move(latch)};
}

void
Fence::wait(uint64_t value, uint64_t timeout) {
  auto ticket = prepareQueueWait(value);
  if (timeout == ~0ULL) {
    ticket.latch->wait();
    return;
  }
  ticket.latch->waitFor(std::chrono::nanoseconds(timeout));
  cancelQueueWait(ticket);
}

bool
Fence::waitForQueueProducer(const QueueTicket &ticket, const std::atomic<bool> &stopping) const {
  return ticket.latch->waitUnless(stopping);
}

void
Fence::cancelQueueWait(const QueueTicket &ticket) const {
  ticket.order->removeCompletionWaiter(ticket.waiter_id);
}

namespace {

class EventRegistration {
public:
  EventRegistration(uint32_t count, bool wait_all, HANDLE event)
      : reached_(count, false), wait_all_(wait_all), event_(event) {}

  void reach(uint32_t index) {
    bool fire = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_ || fired_ || reached_[index])
        return;
      reached_[index] = true;
      if (!armed_)
        return;
      fire = !wait_all_;
      if (wait_all_) {
        fire = true;
        for (bool reached : reached_)
          fire &= reached;
      }
      fired_ = fire;
    }
    if (fire)
      SetEvent(event_);
  }

  void arm() {
    bool fire = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_ || fired_)
        return;
      armed_ = true;
      fire = !wait_all_;
      if (wait_all_) {
        fire = true;
        for (bool reached : reached_)
          fire &= reached;
      } else {
        fire = false;
        for (bool reached : reached_)
          fire |= reached;
      }
      fired_ = fire;
    }
    if (fire)
      SetEvent(event_);
  }

  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
  }

private:
  std::mutex mutex_;
  std::vector<bool> reached_;
  bool wait_all_;
  HANDLE event_;
  bool armed_ = false;
  bool fired_ = false;
  bool cancelled_ = false;
};

} // namespace

bool
EventListener::setEventOnValue(Fence *fence, HANDLE event, uint64_t value) {
  try {
    auto wait = fence->prepareWaitValue(value);
    return setEventOnValues(&wait, 1, true, event);
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool
EventListener::setEventOnValues(const FenceWaitValue *values, uint32_t count, bool wait_all, HANDLE event) {
  std::shared_ptr<EventRegistration> registration;
  try {
    registration = std::make_shared<EventRegistration>(count, wait_all, event);
    for (uint32_t i = 0; i < count; ++i) {
      if (!values[i].latch->addWaiter([registration, i] { registration->reach(i); })) {
        registration->cancel();
        return false;
      }
    }
    registration->arm();
  } catch (const std::bad_alloc &) {
    if (registration)
      registration->cancel();
    return false;
  }
  return true;
}

} // namespace dxmt

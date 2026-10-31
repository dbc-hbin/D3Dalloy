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
namespace {

// A per-section kernel mutex has abandoned-owner recovery, unlike a shared
// spinlock. Never held while waiting for GPU work or an observer to run.
class SharedTimelineLock {
public:
  SharedTimelineLock(HANDLE mutex, SharedFenceTimeline *timeline) : mutex_(mutex) {
    WaitForSingleObject(mutex_, INFINITE);
    if (timeline->version == SharedFenceTimeline::kVersion)
      timeline->finishPublication();
  }
  ~SharedTimelineLock() { ReleaseMutex(mutex_); }
private:
  HANDLE mutex_;
};

} // namespace

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
  if (shared_event_)
    return observeShared();
  // order_ is assigned only by initialize() before the fence is published, so
  // this polling path needs neither schedule_mutex_ nor a shared_ptr copy.
  return order_->completedValue();
}

bool
Fence::signal(uint64_t value) {
  auto order = currentOrder();
  if (shared_event_) {
    uint64_t serial;
    {
      SharedTimelineLock lock(timeline_mutex_, timeline_);
      serial = timeline_->publish(value);
    }
    // Serial allocation occurs at completion publication, not producer enqueue.
    // Reordered native wakeups are harmless: observers read shared state.
    shared_event_.signalValue(serial);
    observeShared();
    return true;
  }
  order->signalCpu(value);
  return true;
}

uint64_t
Fence::observeShared() {
  std::lock_guard<std::mutex> lock(observe_mutex_);
  return observeSharedLocked();
}

uint64_t
Fence::observeSharedLocked() {
  auto order = currentOrder();
  SharedFenceTimeline::Observation observation;
  {
    SharedTimelineLock lock(timeline_mutex_, timeline_);
    observation = timeline_->take(observer_index_);
  }
  // Drain the peak into preexisting waits, then rewind to the current value.
  // observe_mutex_ also covers new local registrations; they see only current.
  order->signalCpu(observation.highest);
  order->signalCpu(observation.logical);
  observed_token_.store(observation.serial, std::memory_order_release);
  return order->completedValue();
}

bool
Fence::initializeShared(WMT::Reference<WMT::SharedEvent> event, SharedFenceTimeline *timeline,
                        std::shared_ptr<void> owner, const WCHAR *mutex_name, bool publish_initial,
                        uint64_t initial_value) {
  if (!event || !timeline || watcher_)
    return false;
  auto order = currentOrder();
  if (!order)
    return false;
  shared_event_ = std::move(event);
  timeline_ = timeline;
  timeline_owner_ = std::move(owner);
  timeline_mutex_ = CreateMutexW(nullptr, FALSE, mutex_name);
  if (!timeline_mutex_)
    return false;
  {
    SharedTimelineLock lock(timeline_mutex_, timeline_);
    if (publish_initial) {
      timeline_->version = SharedFenceTimeline::kVersion;
      timeline_->publish(initial_value);
    } else if (timeline_->version != SharedFenceTimeline::kVersion) {
      return false;
    }
    observer_index_ = timeline_->attach(GetCurrentProcessId());
    if (observer_index_ < 0) {
      // A crashed process must not permanently consume observer capacity.
      for (auto &observer : timeline_->observers) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, observer.pid);
        const bool dead = process ? WaitForSingleObject(process, 0) == WAIT_OBJECT_0
                                  : GetLastError() == ERROR_INVALID_PARAMETER;
        if (process)
          CloseHandle(process);
        if (dead)
          observer.pid = 0;
      }
      observer_index_ = timeline_->attach(GetCurrentProcessId());
    }
    if (observer_index_ < 0)
      return false;
  }
  observeShared();
  watcher_ = CreateThread(nullptr, 0, SharedWatcher, this, 0, nullptr);
  return watcher_ != nullptr;
}

DWORD WINAPI
Fence::SharedWatcher(void *arg) {
  auto *fence = static_cast<Fence *>(arg);
  while (!fence->watcher_stop_.load(std::memory_order_acquire)) {
    fence->observeShared();
    // Wakes as soon as any device or process publishes a later token; the
    // timeout only bounds how long destruction waits for this thread.
    fence->shared_event_.waitUntilSignaledValue(fence->observed_token_.load(std::memory_order_acquire) + 1, 25);
  }
  return 0;
}

Fence::~Fence() {
  if (watcher_) {
    watcher_stop_.store(true, std::memory_order_release);
    WaitForSingleObject(watcher_, INFINITE);
    CloseHandle(watcher_);
  }
  if (timeline_mutex_) {
    if (observer_index_ >= 0) {
      SharedTimelineLock lock(timeline_mutex_, timeline_);
      timeline_->observers[observer_index_].pid = 0;
    }
    CloseHandle(timeline_mutex_);
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
  std::lock_guard<std::mutex> lock(observe_mutex_);
  if (shared_event_)
    observeSharedLocked();
  auto latch = std::make_shared<owned_graphics::FenceCompletionLatch>();
  uint64_t waiter_id = 0;
  if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }, &waiter_id))
    throw std::bad_alloc();
  return QueueTicket{order, {}, std::move(latch), order->generation(), value, waiter_id};
}

FenceWaitValue
Fence::prepareWaitValue(uint64_t value) {
  auto order = currentOrder();
  std::lock_guard<std::mutex> lock(observe_mutex_);
  if (shared_event_)
    observeSharedLocked();
  return FenceWaitValue::prepare(order, value);
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
EventListener::setEventOnValues(FenceWaitValue *values, uint32_t count, bool wait_all, HANDLE event) {
  return owned_graphics::FenceEventRegistration::set(values, count, wait_all, [event] { SetEvent(event); });
}

} // namespace dxmt

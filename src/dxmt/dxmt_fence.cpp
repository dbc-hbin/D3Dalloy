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

Fence::Fence(WMT::Device device) : device_(device) {}

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
  order->reconcile();
  return order->completedValue();
}

bool
Fence::signal(uint64_t value) {
  auto order = currentOrder();
  order->signalCpu(value);
  return true;
}

Fence::QueueSignalPreparation
Fence::prepareQueueSignal(uint64_t queue, uint64_t value) {
  (void)queue;
  auto order = currentOrder();
  auto event = device_.newSharedEvent();
  auto producer = order->prepare(value, [event]() mutable { return event.signaledValue() >= 1; });
  return QueueSignalPreparation{QueueTicket{event, order, producer, {}, order->generation(), value}};
}

void
Fence::commitQueueSignal(QueueSignalPreparation &preparation) {
  preparation.ticket.order->publish(preparation.ticket.producer);
}

void
Fence::cancelQueueSignal(QueueSignalPreparation &preparation) {
  preparation.ticket.order->cancel(preparation.ticket.producer);
}

Fence::QueueTicket
Fence::prepareQueueWait(uint64_t value) {
  auto order = currentOrder();
  order->reconcile();
  auto latch = std::make_shared<owned_graphics::FenceCompletionLatch>();
  if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }))
    throw std::bad_alloc();
  return QueueTicket{{}, order, {}, std::move(latch), order->generation(), value};
}

FenceWaitValue
Fence::prepareWaitValue(uint64_t value) {
  auto order = currentOrder();
  order->reconcile();
  auto latch = std::make_shared<owned_graphics::FenceCompletionLatch>();
  if (!order->addCompletionWaiter(value, [latch] { latch->signal(); }))
    throw std::bad_alloc();
  return FenceWaitValue{std::move(latch)};
}

void
Fence::wait(uint64_t value, uint64_t timeout) {
  auto ticket = prepareQueueWait(value);
  using namespace std::chrono_literals;
  if (timeout == ~0ULL) {
    while (!ticket.latch->waitFor(10ms))
      ticket.order->reconcile();
    return;
  }
  ticket.latch->waitFor(std::chrono::nanoseconds(timeout));
}

void
Fence::waitForQueueProducer(const QueueTicket &ticket) const {
  using namespace std::chrono_literals;
  while (!ticket.latch->waitFor(10ms))
    ticket.order->reconcile();
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

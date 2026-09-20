/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxmt_fence.hpp"
#include <cassert>
#include <thread>

namespace dxmt {

void
Fence::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Fence::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Fence::Fence(WMT::Device device) : device_(device) {
  resetLocked();
}

void
Fence::resetLocked() {
  this->event_ = device_.newSharedEvent();
  this->order_ = std::make_shared<owned_graphics::FenceOrderEpoch>(++generation_);
  this->schedule_.reset();
}

bool
Fence::initialize(uint64_t value) {
  WMT::Reference<WMT::SharedEvent> event;
  std::shared_ptr<owned_graphics::FenceOrderEpoch> order;
  {
    std::lock_guard lock(schedule_mutex_);
    if (schedule_.initialize(value) != owned_graphics::FenceSignalSchedule::Result::Accepted)
      return false;
    event = event_;
    order = order_;
  }
  event.signalValue(value);
  order->markCommitted(value);
  return true;
}

bool
Fence::signal(uint64_t value) {
  WMT::Reference<WMT::SharedEvent> event;
  std::shared_ptr<owned_graphics::FenceOrderEpoch> order;
  {
    std::lock_guard lock(schedule_mutex_);
    const bool previous_settled = event_.signaledValue() >= schedule_.lastScheduled();
    if (schedule_.scheduleCpu(value, previous_settled) != owned_graphics::FenceSignalSchedule::Result::Accepted)
      return false;
    event = event_;
    order = order_;
  }
  // An accepted CPU producer signal is immediately visible. Mixed producer
  // handoff is accepted only after the prior scheduled value has completed.
  event.signalValue(value);
  order->markCommitted(value);
  return true;
}

std::optional<Fence::QueueSignalPreparation>
Fence::prepareQueueSignal(uint64_t queue, uint64_t value) {
  std::unique_lock lock(schedule_mutex_);
  const bool previous_settled = event_.signaledValue() >= schedule_.lastScheduled();
  const auto previous_schedule = schedule_.state();
  if (schedule_.scheduleQueue(queue, value, previous_settled) != owned_graphics::FenceSignalSchedule::Result::Accepted)
    return std::nullopt;
  return QueueSignalPreparation{
      QueueTicket{event_, order_, generation_, value}, previous_schedule, std::move(lock)};
}

void
Fence::commitQueueSignal(QueueSignalPreparation &preparation) {
  preparation.schedule_lock.unlock();
}

void
Fence::cancelQueueSignal(QueueSignalPreparation &preparation) {
  // prepareQueueSignal intentionally keeps schedule_mutex_ locked until the
  // owning queue either publishes the task into its CPU FIFO or rolls back.
  // No other queue/CPU producer can observe an intermediate scheduling state.
  assert(preparation.schedule_lock.owns_lock());
  schedule_.restore(preparation.previous_schedule);
  preparation.schedule_lock.unlock();
}

Fence::QueueTicket
Fence::prepareQueueWait(uint64_t value) const {
  std::lock_guard lock(schedule_mutex_);
  return QueueTicket{event_, order_, generation_, value};
}

bool
Fence::queueWaitReady(const QueueTicket &ticket) const {
  if (ticket.order->ready(ticket.value))
    return true;
  auto event = ticket.event;
  return event.signaledValue() >= ticket.value;
}

void
Fence::waitForQueueProducer(const QueueTicket &ticket) const {
  using namespace std::chrono_literals;
  while (!queueWaitReady(ticket))
    ticket.order->waitFor(ticket.value, 10ms);
}

EventListener::EventListener() :
    shared_event_listener_(SharedEventListener_create()),
    event_listener_thread_([this]() { SharedEventListener_start(this->shared_event_listener_); }) {};

EventListener::~EventListener() {
  SharedEventListener_destroy(shared_event_listener_);
  event_listener_thread_.join();
}

bool
EventListener::setEventOnValue(Fence const *fence, HANDLE event, uint64_t value) {
  auto shared_event = fence->sharedEvent();
  const WMTSharedEventValue item = {shared_event.handle, value};
  return setEventOnValues(&item, 1, true, event);
}

bool
EventListener::setEventOnValues(const WMTSharedEventValue *values, uint32_t count, bool wait_all, HANDLE event) {
  WMTConstMemoryPointer pointer{};
  pointer.set(values);
  return MTLSharedEvent_setWin32EventOnValues(pointer, count, wait_all, shared_event_listener_, event) != 0;
}

}; // namespace dxmt

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

#pragma once

#include "Metal.hpp"
#include "dxmt_queue_order.hpp"
#include "thread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace dxmt {

class Fence {
public:
  struct QueueTicket {
    WMT::Reference<WMT::SharedEvent> event;
    std::shared_ptr<owned_graphics::FenceOrderEpoch> order;
    uint64_t generation = 0;
    uint64_t value = 0;
  };

  struct QueueSignalPreparation {
    QueueTicket ticket;
    owned_graphics::FenceSignalSchedule::State previous_schedule;
    std::unique_lock<std::mutex> schedule_lock;

    QueueSignalPreparation(QueueTicket ticket, owned_graphics::FenceSignalSchedule::State previous_schedule,
                           std::unique_lock<std::mutex> schedule_lock)
        : ticket(std::move(ticket)), previous_schedule(previous_schedule), schedule_lock(std::move(schedule_lock)) {}
    QueueSignalPreparation(QueueSignalPreparation &&) = default;
    QueueSignalPreparation &operator=(QueueSignalPreparation &&) = default;
    QueueSignalPreparation(const QueueSignalPreparation &) = delete;
    QueueSignalPreparation &operator=(const QueueSignalPreparation &) = delete;
  };

  void incRef();
  void decRef();

  WMT::Reference<WMT::SharedEvent>
  sharedEvent() const {
    std::lock_guard lock(schedule_mutex_);
    return event_;
  }

  uint64_t
  completedValue() {
    auto event = sharedEvent();
    return event.signaledValue();
  }

  bool
  initialize(uint64_t value);

  bool
  signal(uint64_t value);

  std::optional<QueueSignalPreparation> prepareQueueSignal(uint64_t queue, uint64_t value);
  void commitQueueSignal(QueueSignalPreparation &preparation);
  void cancelQueueSignal(QueueSignalPreparation &preparation);
  QueueTicket prepareQueueWait(uint64_t value) const;

  void
  wait(uint64_t value, uint64_t timeout = ~0ULL) {
    auto event = sharedEvent();
    event.waitUntilSignaledValue(value, timeout);
  }

  void
  encodeQueueSignal(WMT::CommandBuffer cmdbuf, const QueueTicket &ticket) {
    cmdbuf.encodeSignalEvent(ticket.event, ticket.value);
  }

  void
  encodeQueueWait(WMT::CommandBuffer cmdbuf, const QueueTicket &ticket) {
    cmdbuf.encodeWaitForEvent(ticket.event, ticket.value);
  }

  void
  markQueueSignalCommitted(const QueueTicket &ticket) {
    ticket.order->markCommitted(ticket.value);
  }

  bool queueWaitReady(const QueueTicket &ticket) const;
  void waitForQueueProducer(const QueueTicket &ticket) const;

  Fence(WMT::Device device);

private:
  void resetLocked();

  WMT::Device device_;
  WMT::Reference<WMT::SharedEvent> event_;
  std::shared_ptr<owned_graphics::FenceOrderEpoch> order_;
  owned_graphics::FenceSignalSchedule schedule_;
  mutable std::mutex schedule_mutex_;
  uint64_t generation_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

class EventListener {
public:
  EventListener();
  ~EventListener();

  bool setEventOnValue(Fence const *fence, HANDLE event, uint64_t value);
  bool setEventOnValues(const WMTSharedEventValue *values, uint32_t count, bool wait_all, HANDLE event);

private:
  obj_handle_t shared_event_listener_;
  dxmt::thread event_listener_thread_;
};

}; // namespace dxmt

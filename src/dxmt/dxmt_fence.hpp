/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#pragma once

#include "Metal.hpp"
#include "dxmt_queue_order.hpp"
#include "windef.h"
#include "winbase.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dxmt {

struct FenceWaitValue {
  std::shared_ptr<owned_graphics::FenceCompletionLatch> latch;
};

class Fence {
public:
  struct QueueTicket {
    WMT::Reference<WMT::SharedEvent> event;
    std::shared_ptr<owned_graphics::FenceOrderEpoch> order;
    std::shared_ptr<owned_graphics::FenceOrderEpoch::Producer> producer;
    std::shared_ptr<owned_graphics::FenceCompletionLatch> latch;
    uint64_t generation = 0;
    uint64_t value = 0;
  };

  struct QueueSignalPreparation {
    QueueTicket ticket;
  };

  void incRef();
  void decRef();

  uint64_t completedValue();
  bool initialize(uint64_t value);
  bool signal(uint64_t value);

  QueueSignalPreparation prepareQueueSignal(uint64_t queue, uint64_t value);
  void commitQueueSignal(QueueSignalPreparation &preparation);
  void cancelQueueSignal(QueueSignalPreparation &preparation);
  QueueTicket prepareQueueWait(uint64_t value);
  FenceWaitValue prepareWaitValue(uint64_t value);

  void wait(uint64_t value, uint64_t timeout = ~0ULL);

  void encodeQueueSignal(WMT::CommandBuffer cmdbuf, const QueueTicket &ticket) {
    cmdbuf.encodeSignalEvent(ticket.event, 1);
  }

  void markQueueSignalCompleted(const QueueTicket &ticket) {
    ticket.order->complete(ticket.producer);
  }

  bool queueWaitReady(const QueueTicket &ticket) const {
    return ticket.latch->ready();
  }
  void waitForQueueProducer(const QueueTicket &ticket) const;

  Fence(WMT::Device device);

private:
  std::shared_ptr<owned_graphics::FenceOrderEpoch> currentOrder() const;

  WMT::Device device_;
  std::shared_ptr<owned_graphics::FenceOrderEpoch> order_;
  mutable std::mutex schedule_mutex_;
  uint64_t generation_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

class EventListener {
public:
  bool setEventOnValue(Fence *fence, HANDLE event, uint64_t value);
  bool setEventOnValues(const FenceWaitValue *values, uint32_t count, bool wait_all, HANDLE event);
};

} // namespace dxmt

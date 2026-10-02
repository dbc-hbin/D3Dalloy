/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#pragma once

#include "dxmt_queue_order.hpp"
#include "dxmt_shared_fence.hpp"
#include "Metal.hpp"
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
    std::shared_ptr<owned_graphics::FenceOrderEpoch> order;
    std::shared_ptr<owned_graphics::FenceOrderEpoch::Producer> producer;
    std::shared_ptr<owned_graphics::FenceCompletionLatch> latch;
    uint64_t generation = 0;
    uint64_t value = 0;
    uint64_t waiter_id = 0;
  };

  void incRef();
  void decRef();

  uint64_t completedValue();
  bool initialize(uint64_t value);
  bool signal(uint64_t value);
  // A D3D12 device tracks weak epoch references to terminate waits on removal.
  std::shared_ptr<owned_graphics::FenceOrderEpoch> currentOrder() const;

  QueueTicket prepareQueueSignal(uint64_t value);
  QueueTicket prepareQueueWait(uint64_t value);
  FenceWaitValue prepareWaitValue(uint64_t value);

  void wait(uint64_t value, uint64_t timeout = ~0ULL);

  /*
   * Shared fences retain the current logical value and a high-water mark for
   * each open Fence, so a stalled observer cannot lose a qualifying crossing.
   * The event wakes observers; only validated queue retirement publishes GPU
   * signals. `owner` keeps the section alive, including retained queue tasks.
   */
  bool initializeShared(WMT::Reference<WMT::SharedEvent> event, SharedFenceTimeline *timeline,
                        std::shared_ptr<void> owner, const WCHAR *mutex_name, bool publish_initial,
                        uint64_t initial_value);
  /* Drain all qualifying crossings before exposing the current logical value. */
  uint64_t observeShared();
  ~Fence();

  void markQueueSignalCompleted(const QueueTicket &ticket) {
    if (shared_event_) {
      signal(ticket.value);
      return;
    }
    ticket.order->complete(ticket.producer);
  }

  bool queueWaitReady(const QueueTicket &ticket) const {
    return ticket.latch->ready();
  }
  bool waitForQueueProducer(const QueueTicket &ticket, const std::atomic<bool> &stopping) const;
  void cancelQueueWait(const QueueTicket &ticket) const;

private:
  std::shared_ptr<owned_graphics::FenceOrderEpoch> order_;
  mutable std::mutex schedule_mutex_;
  uint64_t generation_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
  WMT::Reference<WMT::SharedEvent> shared_event_;
  SharedFenceTimeline *timeline_ = nullptr;
  std::shared_ptr<void> timeline_owner_;
  HANDLE timeline_mutex_ = nullptr;
  int32_t observer_index_ = -1;
  std::atomic<uint64_t> observed_token_ = 0;
  std::mutex observe_mutex_;
  std::atomic<bool> watcher_stop_ = false;
  HANDLE watcher_ = nullptr;
  static DWORD WINAPI SharedWatcher(void *fence);
  uint64_t observeSharedLocked();
};

class EventListener {
public:
  bool setEventOnValue(Fence *fence, HANDLE event, uint64_t value);
  bool setEventOnValues(const FenceWaitValue *values, uint32_t count, bool wait_all, HANDLE event);
};

} // namespace dxmt

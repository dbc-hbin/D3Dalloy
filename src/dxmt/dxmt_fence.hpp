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

/*
 * Cross-process shared-fence timeline, placed in shared memory. The MTLSharedEvent
 * carries monotonic native tokens; every Signal (CPU or GPU, any device or process)
 * reserves the next token and publishes token -> logical value here before the
 * event can reach it. The logical (API) value of the fence is the one published
 * for the event's current token, so D3D12 Signal(lower) rewinds consistently in
 * every process while the native timeline never goes backwards.
 */
struct SharedFenceTimeline {
  static constexpr uint32_t kRing = 1024;
  struct Entry {
    volatile LONG64 token;
    volatile LONG64 logical;
  };
  volatile LONG64 next_token;
  Entry ring[kRing];
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
    uint64_t native_token = 0; // shared fences: token this signal publishes
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
   * Shared fences: `event` is the monotonic native-token timeline shared by every
   * device and process and `timeline` maps tokens to logical values (see
   * SharedFenceTimeline). `owner` keeps the shared memory mapped for as long as
   * this Fence lives, including queue tasks that outlive the COM object. A
   * watcher thread publishes the logical value of every observed token into the
   * local order epoch through the rewind-aware CPU path.
   */
  bool initializeShared(WMT::Reference<WMT::SharedEvent> event, SharedFenceTimeline *timeline,
                        std::shared_ptr<void> owner, bool publish_initial, uint64_t initial_value);
  WMT::SharedEvent sharedEvent() const {
    return shared_event_;
  }
  /* Reserves the next native token for a signal of logical `value` and publishes the mapping. */
  uint64_t reserveSharedToken(uint64_t value);
  /* Logical value of the event's current token, published into the local epoch. */
  uint64_t observeShared();
  /* Latest native token this process observed (a GPU wait on it is already satisfied). */
  uint64_t observedSharedToken() const {
    return observed_token_.load(std::memory_order_acquire);
  }
  ~Fence();

  void markQueueSignalCompleted(const QueueTicket &ticket) {
    if (shared_event_) {
      // The GPU already signaled ticket.native_token; the CPU write is a no-op unless
      // a later token was published meanwhile (events never move backwards).
      shared_event_.signalValue(ticket.native_token);
      observeShared();
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
  std::atomic<uint64_t> observed_token_ = 0;
  std::mutex observe_mutex_;
  std::atomic<bool> watcher_stop_ = false;
  HANDLE watcher_ = nullptr;
  static DWORD WINAPI SharedWatcher(void *fence);
};

class EventListener {
public:
  bool setEventOnValue(Fence *fence, HANDLE event, uint64_t value);
  bool setEventOnValues(const FenceWaitValue *values, uint32_t count, bool wait_all, HANDLE event);
};

} // namespace dxmt

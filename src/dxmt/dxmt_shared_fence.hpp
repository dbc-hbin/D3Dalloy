// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace dxmt {

// POD in the shared section. Every access is protected by the section's named
// kernel mutex. The Metal event is only a wakeup, not completion authority.
struct SharedFenceTimeline {
  static constexpr uint32_t kVersion = 2;
  static constexpr uint32_t kObservers = 1024;
  struct Observer {
    uint32_t pid;
    uint32_t reserved;
    uint64_t highest;
  };
  struct Observation {
    uint64_t highest;
    uint64_t logical;
    uint64_t serial;
  };

  uint32_t version;
  volatile uint32_t publishing;
  uint64_t pending_value;
  uint64_t logical;
  uint64_t serial;
  Observer observers[kObservers];

  // Replay is idempotent if a process died partway through publication. A
  // recovered publication may advance the wakeup serial twice, never lose a
  // threshold crossing or move a still-live observer's high-water mark back.
  void finishPublication() {
    if (!publishing)
      return;
    for (auto &observer : observers)
      if (observer.pid)
        observer.highest = std::max(observer.highest, pending_value);
    logical = pending_value;
    ++serial;
    std::atomic_thread_fence(std::memory_order_release);
    publishing = 0;
  }

  uint64_t publish(uint64_t value) {
    pending_value = value;
    std::atomic_thread_fence(std::memory_order_release);
    publishing = 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    finishPublication();
    return serial;
  }

  int32_t attach(uint32_t pid) {
    for (uint32_t i = 0; i < kObservers; ++i) {
      auto &observer = observers[i];
      if (!observer.pid) {
        observer.highest = logical;
        observer.pid = pid;
        return int32_t(i);
      }
    }
    return -1;
  }

  Observation take(uint32_t index) {
    auto &observer = observers[index];
    const Observation result{observer.highest, logical, serial};
    observer.highest = logical;
    return result;
  }
};

} // namespace dxmt

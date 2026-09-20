// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned graphics backend: publication and reclamation of in-flight GPU work.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace owned_graphics {

// The queue's commit mutex must serialize reserve(), publish(), and stop().
// Exactly one completion thread calls wait() and retire() in sequence order.
// Payload writes precede publish(); payload destruction precedes retire().
// Separate monotonic cursors avoid a count that can underflow when completion
// wins a race against the publishing thread. No slot is reused before retire.
template <std::size_t Capacity> class SubmissionRing {
  static_assert(Capacity > 0);
  std::atomic<std::uint64_t> published_{1};
  std::atomic<std::uint64_t> retired_{1};
  std::atomic<std::uint64_t> stop_at_{0};

public:
  std::uint64_t reserve() {
    const auto sequence = published_.load(std::memory_order_relaxed);
    auto retired = retired_.load(std::memory_order_acquire);
    while (sequence - retired >= Capacity) {
      retired_.wait(retired, std::memory_order_acquire);
      retired = retired_.load(std::memory_order_acquire);
    }
    return sequence;
  }

  void publish(std::uint64_t sequence) {
    assert(sequence == published_.load(std::memory_order_relaxed));
    published_.store(sequence + 1, std::memory_order_release);
    published_.notify_one();
  }

  bool wait(std::uint64_t sequence) {
    published_.wait(sequence, std::memory_order_acquire);
    return stop_at_.load(std::memory_order_relaxed) != sequence;
  }

  void retire(std::uint64_t sequence) {
    assert(sequence == retired_.load(std::memory_order_relaxed));
    retired_.store(sequence + 1, std::memory_order_release);
    retired_.notify_one();
  }

  std::uint64_t lastPublished() const {
    const auto next = published_.load(std::memory_order_acquire);
    return next > 1 ? next - 1 : 0;
  }

  void waitUntilRetired(std::uint64_t sequence) const {
    auto retired = retired_.load(std::memory_order_acquire);
    while (retired <= sequence) {
      retired_.wait(retired, std::memory_order_acquire);
      retired = retired_.load(std::memory_order_acquire);
    }
  }

  void stop() {
    const auto sequence = published_.load(std::memory_order_relaxed);
    // Publish the sentinel *after* its identity, including when the completion
    // thread observes the changed cursor before notify_one() executes.
    stop_at_.store(sequence, std::memory_order_relaxed);
    publish(sequence);
  }
};

} // namespace owned_graphics

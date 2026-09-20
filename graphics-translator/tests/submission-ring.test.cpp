// SPDX-License-Identifier: LGPL-2.1-or-later
#include "owned_submission_ring.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

static void empty_stop() {
  owned_graphics::SubmissionRing<2> ring;
  ring.stop();
  // The completion thread can start after destruction has requested stop.
  CHECK(!ring.wait(1));
}

static void completion_before_next_reservation() {
  owned_graphics::SubmissionRing<1> ring;
  for (std::uint64_t sequence = 1; sequence <= 100000; ++sequence) {
    CHECK(ring.reserve() == sequence);
    ring.publish(sequence);
    CHECK(ring.wait(sequence));
    // Force immediate completion, the interleaving that made the old count
    // underflow when completion preceded its post-publication increment.
    ring.retire(sequence);
  }
  ring.stop();
  CHECK(!ring.wait(100001));
}

static void capacity_and_drain() {
  owned_graphics::SubmissionRing<2> ring;
  CHECK(ring.reserve() == 1);
  ring.publish(1);
  CHECK(ring.reserve() == 2);
  ring.publish(2);
  std::atomic<bool> attempting{false}, acquired{false};
  std::thread producer([&] {
    attempting.store(true, std::memory_order_release);
    attempting.notify_one();
    CHECK(ring.reserve() == 3);
    acquired.store(true, std::memory_order_release);
    acquired.notify_one();
    ring.publish(3);
  });
  attempting.wait(false, std::memory_order_acquire);
  for (unsigned n = 0; n < 1000; ++n) {
    CHECK(!acquired.load(std::memory_order_acquire));
    std::this_thread::yield();
  }
  CHECK(ring.wait(1));
  ring.retire(1);
  acquired.wait(false, std::memory_order_acquire);
  producer.join();
  // Stop does not occupy/overwrite a payload slot even with a full ring.
  ring.stop();
  CHECK(ring.wait(2));
  ring.retire(2);
  CHECK(ring.wait(3));
  ring.retire(3);
  CHECK(!ring.wait(4));
}

template <std::size_t Capacity> static void concurrent_producers() {
  constexpr unsigned producers = 4;
  constexpr unsigned per_producer = 25000;
  struct Payload { std::uint64_t sequence{}, checksum{}; };
  std::array<Payload, Capacity> slots{};
  owned_graphics::SubmissionRing<Capacity> ring;
  std::mutex commit;
  std::uint64_t consumed = 0;
  const auto checksum = [](std::uint64_t seq) { return seq * 0x9e3779b97f4a7c15ULL; };
  std::thread consumer([&] {
    std::uint64_t sequence = 1;
    while (ring.wait(sequence)) {
      auto &slot = slots[sequence % Capacity];
      CHECK(slot.sequence == sequence);
      CHECK(slot.checksum == checksum(sequence));
      if ((sequence % 113) == 0)
        std::this_thread::yield();
      slot = {}; // Must happen before a producer may reuse this same slot.
      ring.retire(sequence);
      consumed = sequence++;
    }
  });
  std::vector<std::thread> threads;
  for (unsigned p = 0; p < producers; ++p) {
    threads.emplace_back([&] {
      for (unsigned n = 0; n < per_producer; ++n) {
        std::lock_guard lock(commit);
        const auto sequence = ring.reserve();
        auto &slot = slots[sequence % Capacity];
        CHECK(slot.sequence == 0);
        slot = {sequence, checksum(sequence)};
        ring.publish(sequence);
      }
    });
  }
  for (auto &thread : threads)
    thread.join();
  {
    std::lock_guard lock(commit);
    ring.stop();
  }
  consumer.join();
  CHECK(consumed == producers * per_producer);
  for (const auto &slot : slots)
    CHECK(slot.sequence == 0);
  std::printf("SUBMISSION_RING_CONCURRENT_PASS capacity=%zu submissions=%llu\n",
              Capacity, static_cast<unsigned long long>(consumed));
}

int main() {
  empty_stop();
  completion_before_next_reservation();
  capacity_and_drain();
  concurrent_producers<1>();
  concurrent_producers<4>();
  concurrent_producers<32>();
  std::puts("OWNED_SUBMISSION_RING_PASS");
}

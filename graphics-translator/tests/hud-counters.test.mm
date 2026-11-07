// CPU-only: compile with clang++ -std=c++20 -O2 -fblocks -framework Foundation
// -framework Metal -framework QuartzCore. Includes the production counter unit.
#include "../../src/winemetal/unix/winemetal_metal4_hud.c"
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
  constexpr unsigned producers = 4, increments = 10000;
  std::atomic<unsigned> remaining{producers};
  std::atomic<bool> start{false};
  std::array<uint64_t, WMTMetal4HUDCounterCount> totals{};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < producers; ++i) {
    threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned n = 0; n < increments; ++n) {
        m4_hud_count(WMTMetal4HUDCompute);
        m4_hud_count(WMTMetal4HUDFenceWait);
      }
      remaining.fetch_sub(1, std::memory_order_release);
    });
  }
  start.store(true, std::memory_order_release);
  do {
    std::array<uint64_t, WMTMetal4HUDCounterCount> sample{};
    m4_hud_drain(sample.data());
    for (size_t i = 0; i < totals.size(); ++i) totals[i] += sample[i];
  } while (remaining.load(std::memory_order_acquire));
  for (auto &thread : threads) thread.join();
  std::array<uint64_t, WMTMetal4HUDCounterCount> sample{};
  m4_hud_drain(sample.data());
  for (size_t i = 0; i < totals.size(); ++i) {
    totals[i] += sample[i];
    const uint64_t expected = i == WMTMetal4HUDCompute || i == WMTMetal4HUDFenceWait
        ? uint64_t(producers) * increments : 0;
    if (totals[i] != expected) {
      std::fprintf(stderr, "counter %zu: got %llu expected %llu\n", i,
          (unsigned long long)totals[i], (unsigned long long)expected);
      return 1;
    }
  }
  sample.fill(UINT64_MAX);
  m4_hud_drain(sample.data());
  for (auto value : sample) if (value) return 1;
  std::puts("HUD_COUNTERS_PASS producers=4 increments=10000 conserved=80000 empty_drain=0");
}

// SPDX-License-Identifier: LGPL-2.1-or-later
// Manual CPU test: clang++ -std=c++20 -Isrc/dxgi graphics-translator/tests/dxgi-memory.test.cpp -o /tmp/dxgi-memory-test && /tmp/dxgi-memory-test
#include "dxgi_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); \
} } while (false)

int main() {
  constexpr uint64_t gib = uint64_t{1} << 30;
  constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
  struct Case { uint64_t ram; uint64_t expected; };
  const Case cases[] = {
    {0, 0}, {1, 0}, {2, 1},
    {16 * gib - 1, 8 * gib - 1}, {16 * gib, 8 * gib}, {16 * gib + 1, 8 * gib},
    {24 * gib, 12 * gib},
    {32 * gib - 1, 16 * gib - 1}, {32 * gib, 16 * gib}, {32 * gib + 1, 16 * gib + 1},
    {48 * gib, 32 * gib}, {64 * gib, 48 * gib},
    {80 * gib - 1, 64 * gib - 1}, {80 * gib, 64 * gib}, {80 * gib + 1, 64 * gib},
    {96 * gib, 82463372083ULL},
    {maximum, 14757395258967641292ULL},
    {maximum - 1, 14757395258967641291ULL},
  };
  for (const auto &test : cases) {
    const auto actual = dxmt::advertisedDedicatedVideoMemory<uint64_t>(true, maximum, test.ram);
    CHECK(actual == test.expected);
    std::printf("ram=%llu advertised=%llu\n", static_cast<unsigned long long>(test.ram),
                static_cast<unsigned long long>(actual));
  }

  // The Metal recommendation caps advertising; a discrete adapter ignores RAM.
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint64_t>(true, 10 * gib, 64 * gib) == 10 * gib);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint64_t>(true, 0, 64 * gib) == 0);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint64_t>(false, 60 * gib, 16 * gib) == 60 * gib);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint64_t>(false, maximum, 0) == maximum);

  // UMA SIZE_T must saturate, not wrap at 4 GiB or a larger multiple.
  constexpr uint64_t maximum32 = std::numeric_limits<uint32_t>::max();
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(true, maximum, 8 * gib - 2) == maximum32);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(true, maximum, 8 * gib) == maximum32);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(true, maximum, 16 * gib) == maximum32);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(true, maximum32 - 1, 64 * gib) == maximum32 - 1);
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(true, 2 * gib, 64 * gib) == 2 * gib);
  // Preserve the existing discrete-adapter conversion even for 32-bit callers.
  CHECK(dxmt::advertisedDedicatedVideoMemory<uint32_t>(false, 5 * gib, 0) == gib);
  std::puts("DXGI_MEMORY_POLICY_PASS");
}

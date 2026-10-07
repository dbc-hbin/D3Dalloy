#include "util_bit.hpp"
#include <cassert>
#include <cstdio>

int main() {
  using namespace dxmt::bit;
  assert(popcnt(0) == 0 && popcnt(UINT32_MAX) == 32);
  assert(tzcnt(uint32_t(0)) == 32 && lzcnt(0) == 32);
  assert(tzcnt(uint64_t(0)) == 64);
  assert(popcnt(0x55555555u) == 16 && popcnt(0xaaaaaaaau) == 16);
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t value = uint32_t(1) << i;
    assert(popcnt(value) == 1 && popcnt(~value) == 31);
    assert(tzcnt(value) == i && lzcnt(value) == 31 - i);
    assert(popcnt(value - 1) == i);
  }
  for (uint32_t i = 0; i < 64; ++i) {
    const uint64_t value = uint64_t(1) << i;
    assert(tzcnt(value) == i);
    assert(tzcnt(UINT64_MAX << i) == i);
  }
  std::puts("UTIL_BIT_CPU_PASS zero=1 width32=1 width64=1 patterns=1");
}

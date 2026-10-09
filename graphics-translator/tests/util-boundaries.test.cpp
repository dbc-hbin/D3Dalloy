// clang++ -std=c++20 -Iinclude/native/windows -Isrc/util \
//   graphics-translator/tests/util-boundaries.test.cpp src/util/util_string.cpp \
//   src/util/wsi_platform_darwin.cpp -o build/util-boundaries.test
#include "util_string.hpp"
#include "wsi_platform.hpp"
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <array>
#include <cassert>

int main() {
  constexpr std::array<uint8_t, 4> expected = {0xf0, 0x9f, 0x98, 0x80};
  std::array<uint8_t, 6> buffer;
  assert(dxmt::str::encodeTypedChar(static_cast<uint8_t *>(nullptr), nullptr, 0x1f600) == 4);
  for (size_t capacity = 0; capacity <= 5; ++capacity) {
    buffer.fill(0xa5);
    const size_t count = dxmt::str::encodeTypedChar(buffer.data(), buffer.data() + capacity, 0x1f600);
    assert(count == (capacity >= 4 ? 4 : 0));
    if (count) assert(std::equal(expected.begin(), expected.end(), buffer.begin()));
    assert(std::all_of(buffer.begin() + count, buffer.end(), [](uint8_t value) { return value == 0xa5; }));
  }
  constexpr char16_t source[] = {0xd83d, 0xde00, 'x', 0};
  char output[6] = {};
  assert(dxmt::str::transcodeString<char>(nullptr, 0, source, 4) == 6);
  assert(dxmt::str::transcodeString(output, sizeof(output), source, 4) == 6);
  assert(std::memcmp(output, "\xf0\x9f\x98\x80x", sizeof(output)) == 0);

  for (size_t alignment : {size_t(16), size_t(4096)}) {
    for (size_t size : {size_t(1), size_t(255), size_t(4096), size_t(8193)}) {
      auto *memory = static_cast<uint8_t *>(dxmt::wsi::aligned_malloc(size, alignment));
      assert(memory && reinterpret_cast<uintptr_t>(memory) % alignment == 0);
      std::memset(memory, 0x5a, size);
      assert(memory[0] == 0x5a && memory[size - 1] == 0x5a);
      dxmt::wsi::aligned_free(memory);
    }
  }
  dxmt::Sha1Digest first{}, second{};
  assert(std::equal_to<dxmt::Sha1Digest>{}(first, second));
  second.data[19] = 1;
  assert(!std::equal_to<dxmt::Sha1Digest>{}(first, second));
}

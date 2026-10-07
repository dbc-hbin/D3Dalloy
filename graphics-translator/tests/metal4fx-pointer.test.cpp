// SPDX-License-Identifier: LGPL-2.1-or-later
// clang++ -std=c++20 graphics-translator/tests/metal4fx-pointer.test.cpp -o build/metal4fx-pointer
#define WINEMETAL_API extern "C"
#include "../../src/winemetal/Metal4FX.hpp"
#include <bit>
#include <cstdio>

static uint64_t guest_base;
static struct WMTMetal4Call captured;
static unsigned releases;

extern "C" uint64_t WMTGetGuestWindowBase() { return guest_base; }
extern "C" void NSObject_release(obj_handle_t object) {
  assert(object == 0xabcdef0123456789ull || object == 0x123456789abcdef0ull);
  ++releases;
}
extern "C" int32_t WMTMetal4Call(struct WMTMetal4Call *call) {
  captured = *call;
  call->ret[0] = call->operation == WMTMetal4FXCreateTemporal ? 0xabcdef0123456789ull : 0x123456789abcdef0ull;
  call->ret[1] = 0;
  call->ret[2] = 0xfedcba9876543210ull;
  return 0;
}

int main() {
  WMT::Device device;
  device.handle = 0x9988776655443322ull;
  WMT4::Compiler compiler;
  compiler.handle = 0x8877665544332211ull;
  WMT::MetalDrawable drawable;
  drawable.handle = 0x7766554433221100ull;
  for (uint64_t base : {0ull, 0x1000000000ull}) {
    guest_base = base;
    for (uintptr_t address : {uintptr_t(0), uintptr_t(1), uintptr_t(0x7fffffff), uintptr_t(0xffffffff)}) {
      WMT4::PresentMetal4Drawable(drawable, 0.25, reinterpret_cast<const WMTCompositionFrame *>(address), 2, true);
      assert(captured.operation == WMTMetal4FXPresentDrawable);
      assert(captured.args[0] == drawable.handle && captured.args[1] == std::bit_cast<uint64_t>(0.25));
      assert(captured.args[2] == (address ? base + uint64_t(address) : 0));
      assert(captured.args[3] == 2 && captured.args[4] == 1);
    }
    WMTMetal4FXTemporalInfo info{16, 16, 32, 32, 0, 0, 0};
    int32_t result = -1;
    auto feature = WMT4::TemporalFeature::Create(device, compiler, info, result);
    assert(result == 0 && feature.handle == 0xabcdef0123456789ull);
    assert(captured.operation == WMTMetal4FXCreateTemporal);
    assert(captured.args[0] == device.handle && captured.args[1] == compiler.handle);
    assert(captured.args[2] == base + uint64_t(reinterpret_cast<uintptr_t>(&info)));
    WMTMetal4FXTemporalFrame input{};
    input.color = 0xfedcba9876543210ull;
    uint64_t generation = 0;
    auto frame = feature.prepare(input, result, generation);
    assert(result == 0 && frame.handle == 0x123456789abcdef0ull && generation == 0xfedcba9876543210ull);
    assert(captured.operation == WMTMetal4FXPrepareTemporal && captured.args[0] == feature.handle);
    assert(captured.args[1] == base + uint64_t(reinterpret_cast<uintptr_t>(&input)));
    assert(input.color == 0xfedcba9876543210ull);
  }
  assert(releases == 4);
  std::puts("METAL4FX_POINTER_PASS bases=2 composition=8 create=2 prepare=2 handles=unchanged");
}

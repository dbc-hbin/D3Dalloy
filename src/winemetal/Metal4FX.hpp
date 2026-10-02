// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "Metal4.hpp"
#include "Metal4Pipeline.hpp"
#include "winemetal_metal4_fx.h"

namespace WMT4 {
inline void PresentMetal4Drawable(WMT::MetalDrawable drawable, double duration = 0) {
  uint64_t bits;
  std::memcpy(&bits, &duration, sizeof(bits));
  WMT::Metal4Invoke(WMTMetal4FXPresentDrawable, drawable.handle, bits);
}
class TemporalFrame : public WMT::Object {
public:
  int32_t encode(WMT::MTL4CommandBuffer buffer, WMT::Fence fence, bool reset) const {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4FXEncodeTemporal;
    call.args[0] = buffer.handle; call.args[1] = handle; call.args[2] = fence.handle; call.args[3] = reset;
    const auto status = WMTMetal4Call(&call);
    return status ? int32_t(0x887a0020) : int32_t(call.ret[1]);
  }
};
class TemporalFeature : public WMT::Object {
public:
  static bool Supports(WMT::Device device) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4FXSupportsTemporal; call.args[0] = device.handle;
    return WMTMetal4Call(&call) == 0 && call.ret[0];
  }
  static WMT::Reference<TemporalFeature> Create(WMT::Device device, Compiler compiler,
      const WMTMetal4FXTemporalInfo &info, int32_t &result) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4FXCreateTemporal;
    call.args[0] = device.handle; call.args[1] = compiler.handle; call.args[2] = uintptr_t(&info);
    const auto status = WMTMetal4Call(&call);
    result = status ? int32_t(0x887a0020) : int32_t(call.ret[1]);
    return WMT::Reference<TemporalFeature>(call.ret[0]);
  }
  WMT::Reference<TemporalFrame> prepare(const WMTMetal4FXTemporalFrame &info, int32_t &result,
                                      uint64_t &generation) const {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4FXPrepareTemporal;
    call.args[0] = handle; call.args[1] = uintptr_t(&info);
    const auto status = WMTMetal4Call(&call);
    result = status ? int32_t(0x887a0020) : int32_t(call.ret[1]);
    generation = call.ret[2];
    return WMT::Reference<TemporalFrame>(call.ret[0]);
  }
  void markDormant() const { WMT::Metal4Invoke(WMTMetal4FXDormantTemporal, handle); }
};
} // namespace WMT4

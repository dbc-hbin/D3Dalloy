// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "Metal4.hpp"
#include "winemetal_metal4_query.h"
#include <stdexcept>

namespace WMT {

class MTL4CounterHeap : public Object {
public:
  using Object::Object;
};

inline Reference<MTL4CounterHeap>
newTimestampCounterHeap(Device device, uint64_t count) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QueryCreateTimestampHeap;
  call.args[0] = device.handle;
  call.args[1] = count;
  if (WMTMetal4Call(&call) != 0)
    return {};
  return Reference<MTL4CounterHeap>(call.ret[0]);
}

inline uint64_t queryTimestampFrequency(Device device) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QueryTimestampFrequency;
  call.args[0] = device.handle;
  return WMTMetal4Call(&call) == 0 ? call.ret[0] : 0;
}

inline uint64_t timestampCounterEntrySize(Device device) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QueryTimestampEntrySize;
  call.args[0] = device.handle;
  return WMTMetal4Call(&call) == 0 ? call.ret[0] : 0;
}

inline void writeTimestamp(MTL4CommandBuffer command, MTL4CounterHeap heap, uint64_t index) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QueryWriteTimestamp;
  call.args[0] = command.handle;
  call.args[1] = heap.handle;
  call.args[2] = index;
  if (WMTMetal4Call(&call) != 0)
    throw std::runtime_error("Metal4 timestamp write failed");
}

inline void resolveTimestampCounters(MTL4CommandBuffer command, MTL4CounterHeap heap,
                                    uint64_t first, uint64_t count, Buffer destination,
                                    uint64_t offset, Fence waitFence = {}, Fence updateFence = {}) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QueryResolveTimestamps;
  call.args[0] = command.handle;
  call.args[1] = heap.handle;
  call.args[2] = first;
  call.args[3] = count;
  call.args[4] = destination.handle;
  call.args[5] = offset;
  call.args[6] = waitFence.handle;
  call.args[7] = updateFence.handle;
  if (WMTMetal4Call(&call) != 0)
    throw std::runtime_error("Metal4 timestamp resolve failed");
}

inline bool sampleTimestampCalibration(Device device, uint64_t &cpuHostNanoseconds, uint64_t &gpuCounterTicks) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QuerySampleCalibration;
  call.args[0] = device.handle;
  if (WMTMetal4Call(&call) != 0)
    return false;
  cpuHostNanoseconds = call.ret[0];
  gpuCounterTicks = call.ret[1];
  return true;
}

inline void setVisibilityResultMode(MTL4RenderCommandEncoder encoder, uint32_t mode, uint64_t offset) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4QuerySetVisibilityMode;
  call.args[0] = encoder.handle;
  call.args[1] = mode;
  call.args[2] = offset;
  if (WMTMetal4Call(&call) != 0)
    throw std::runtime_error("Metal4 visibility mode failed");
}

} // namespace WMT

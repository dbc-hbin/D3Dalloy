// SPDX-License-Identifier: LGPL-2.1-or-later
#import <Metal/Metal.h>
#include "../winemetal_metal4_query.h"
#include <stdint.h>

#define QUERY_SUCCESS 0
#define QUERY_INVALID ((int32_t)0xc000000d)
#define QUERY_UNSUPPORTED ((int32_t)0xc0000002)
#define QUERY_FAILED ((int32_t)0xc0000001)

_Static_assert(sizeof(MTL4TimestampHeapEntry) == sizeof(struct WMTMetal4TimestampHeapEntry),
               "D3D12 timestamp resolve layout must match Metal4");

int32_t winemetal_metal4_query_call(struct WMTMetal4Call *call) {
  if (!call || call->reserved)
    return QUERY_INVALID;
  switch (call->operation) {
  case WMTMetal4QueryCreateTimestampHeap: {
    id<MTLDevice> device = (id<MTLDevice>)call->args[0];
    if (!device || !call->args[1])
      return QUERY_INVALID;
    if ([device sizeOfCounterHeapEntry:MTL4CounterHeapTypeTimestamp] != sizeof(MTL4TimestampHeapEntry))
      return QUERY_UNSUPPORTED;
    MTL4CounterHeapDescriptor *descriptor = [MTL4CounterHeapDescriptor new];
    descriptor.type = MTL4CounterHeapTypeTimestamp;
    descriptor.count = call->args[1];
    NSError *error = nil;
    id<MTL4CounterHeap> heap = [device newCounterHeapWithDescriptor:descriptor error:&error];
    [descriptor release];
    call->ret[0] = (uint64_t)heap;
    return heap ? QUERY_SUCCESS : QUERY_FAILED;
  }
  case WMTMetal4QueryTimestampFrequency:
    if (!call->args[0])
      return QUERY_INVALID;
    call->ret[0] = [(id<MTLDevice>)call->args[0] queryTimestampFrequency];
    return call->ret[0] ? QUERY_SUCCESS : QUERY_UNSUPPORTED;
  case WMTMetal4QueryTimestampEntrySize:
    if (!call->args[0])
      return QUERY_INVALID;
    call->ret[0] = [(id<MTLDevice>)call->args[0] sizeOfCounterHeapEntry:MTL4CounterHeapTypeTimestamp];
    return call->ret[0] ? QUERY_SUCCESS : QUERY_UNSUPPORTED;
  case WMTMetal4QueryWriteTimestamp: {
    if (!call->args[0])
      return QUERY_INVALID;
    id<MTL4CommandBuffer> command = winemetal_metal4_command_buffer(call->args[0]);
    id<MTL4CounterHeap> heap = (id<MTL4CounterHeap>)call->args[1];
    if (!command || !heap || heap.type != MTL4CounterHeapTypeTimestamp || call->args[2] >= heap.count)
      return QUERY_INVALID;
    winemetal_metal4_retain_resource(call->args[0], heap);
    [command writeTimestampIntoHeap:heap atIndex:call->args[2]];
    return QUERY_SUCCESS;
  }
  case WMTMetal4QueryResolveTimestamps: {
    if (!call->args[0])
      return QUERY_INVALID;
    id<MTL4CommandBuffer> command = winemetal_metal4_command_buffer(call->args[0]);
    id<MTL4CounterHeap> heap = (id<MTL4CounterHeap>)call->args[1];
    id<MTLBuffer> buffer = (id<MTLBuffer>)call->args[4];
    const uint64_t first = call->args[2], count = call->args[3], offset = call->args[5];
    if (!command || !heap || !buffer || heap.type != MTL4CounterHeapTypeTimestamp ||
        first > heap.count || count > heap.count - first || offset > buffer.length ||
        offset % sizeof(MTL4TimestampHeapEntry) || count > (buffer.length - offset) / sizeof(MTL4TimestampHeapEntry))
      return QUERY_INVALID;
    winemetal_metal4_retain_resource(call->args[0], heap);
    winemetal_metal4_use_allocation(call->args[0], buffer);
    if (call->args[6])
      winemetal_metal4_retain_resource(call->args[0], (id)call->args[6]);
    if (call->args[7])
      winemetal_metal4_retain_resource(call->args[0], (id)call->args[7]);
    [command resolveCounterHeap:heap withRange:NSMakeRange(first, count)
                    intoBuffer:MTL4BufferRangeMake(buffer.gpuAddress + offset, count * sizeof(MTL4TimestampHeapEntry))
                     waitFence:(id<MTLFence>)call->args[6] updateFence:(id<MTLFence>)call->args[7]];
    return QUERY_SUCCESS;
  }
  case WMTMetal4QuerySampleCalibration: {
    id<MTLDevice> device = (id<MTLDevice>)call->args[0];
    if (!device)
      return QUERY_INVALID;
    const uint64_t frequency = [device queryTimestampFrequency];
    MTLTimestamp cpu = 0, gpuNanoseconds = 0;
    [device sampleTimestamps:&cpu gpuTimestamp:&gpuNanoseconds];
    if (!frequency || !cpu || !gpuNanoseconds)
      return QUERY_FAILED;
    // sampleTimestamps uses the legacy nanosecond GPU clock, whereas Metal4
    // timestamp heaps use queryTimestampFrequency ticks. The common epoch and
    // this scale are checked against real counter-heap samples in the probe.
    // Keep the multiplication wide: uptime * frequency can exceed uint64_t.
    const __uint128_t ticks = (__uint128_t)gpuNanoseconds * frequency / 1000000000ull;
    if (ticks > UINT64_MAX)
      return QUERY_FAILED;
    call->ret[0] = cpu;
    call->ret[1] = (uint64_t)ticks;
    return QUERY_SUCCESS;
  }
  case WMTMetal4QuerySetVisibilityMode: {
    if (!call->args[0])
      return QUERY_INVALID;
    id<MTL4RenderCommandEncoder> encoder = winemetal_metal4_render_encoder(call->args[0]);
    if (!encoder || call->args[1] > MTLVisibilityResultModeCounting || call->args[2] % sizeof(uint64_t))
      return QUERY_INVALID;
    [encoder setVisibilityResultMode:(MTLVisibilityResultMode)call->args[1] offset:call->args[2]];
    return QUERY_SUCCESS;
  }
  default:
    return QUERY_UNSUPPORTED;
  }
}

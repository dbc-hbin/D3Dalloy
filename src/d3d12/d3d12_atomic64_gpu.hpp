#pragma once
#ifdef __METAL__
#include <metal_stdlib>
using Atomic64Uint = metal::uint;
using Atomic64Ulong = metal::ulong;
#else
#include <cstdint>
using Atomic64Uint = uint32_t;
using Atomic64Ulong = uint64_t;
#endif

namespace dxmt {
struct Atomic64ImportCell {
  Atomic64Ulong bytes[8];
};
struct Atomic64TransferParams {
  Atomic64Ulong sources;
  Atomic64Ulong roots;
  Atomic64Ulong nodes;
  Atomic64Ulong baseline;
  Atomic64Ulong cellMap;
  Atomic64Ulong byteBase;
  Atomic64Uint cellCount;
  Atomic64Uint width;
  Atomic64Uint height;
  Atomic64Uint depth;
  Atomic64Uint rowPitch;
  Atomic64Uint unitBytes;
  Atomic64Uint channelBits;
  Atomic64Uint channels;
  Atomic64Ulong nativePositions;
  Atomic64Uint texelStride;
  Atomic64Uint reserved;
};
#ifndef __METAL__
static_assert(sizeof(Atomic64ImportCell) == 64);
static_assert(sizeof(Atomic64TransferParams) == 96);
#endif
} // namespace dxmt

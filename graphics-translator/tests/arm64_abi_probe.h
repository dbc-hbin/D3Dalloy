#pragma once
#include <windows.h>
#include <unknwn.h>
#include <cstddef>
#include <cstdint>
#include "../../src/util/com/com_pointer.hpp"

// A fixed-width Unix-boundary packet; pointers travel as zero-extended addresses.
struct alignas(8) AbiPacket {
  uint32_t tag;
  int32_t delta;
  uint64_t token;
  uint64_t address;
  double value;
};
static_assert(sizeof(AbiPacket) == 32 && alignof(AbiPacket) == 8);
static_assert(offsetof(AbiPacket, token) == 8 && offsetof(AbiPacket, address) == 16);
static_assert(offsetof(AbiPacket, value) == 24);
static_assert(sizeof(HRESULT) == 4 && sizeof(ULONG) == 4 && sizeof(WCHAR) == 2);
#if defined(_M_IX86) || defined(__i386__)
static_assert(sizeof(void *) == 4);
#else
static_assert(sizeof(void *) == 8);
#endif

struct AbiCallback : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Check(AbiPacket packet, uint64_t *reply) = 0;
};
struct AbiObject : IUnknown {
  virtual d3dalloy::Com<AbiObject> STDMETHODCALLTYPE Retain(UINT tag, void *address) = 0;
  virtual HRESULT STDMETHODCALLTYPE Exchange(AbiPacket packet, AbiCallback *callback, AbiPacket *reply) = 0;
};
static_assert(sizeof(d3dalloy::Com<AbiObject>) == sizeof(void *));
static_assert(alignof(d3dalloy::Com<AbiObject>) == alignof(void *));
using AbiCreate = HRESULT(WINAPI *)(AbiObject **);
using AbiArithmetic = double(WINAPI *)(uint64_t, int32_t, double, void *);

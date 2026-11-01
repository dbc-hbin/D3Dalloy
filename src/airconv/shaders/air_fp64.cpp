#include "fp64_integer.h"
using ulong = d3dalloy_softfloat::uint64_t;
using signed64 = d3dalloy_softfloat::int64_t;
using uint = unsigned int;
using ushort = unsigned short;
using uchar = unsigned char;
using namespace d3dalloy_softfloat;

#define FP64_BINARY(name, operation) \
  ulong fp64_##name(ulong a, ulong b) asm("d3dalloy.fp64." #name); \
  ulong fp64_##name(ulong a, ulong b) { return operation({a}, {b}).v; }
FP64_BINARY(add, f64_add)
FP64_BINARY(sub, f64_sub)
FP64_BINARY(mul, f64_mul)
FP64_BINARY(div, f64_div)
#undef FP64_BINARY

ulong fp64_rem(ulong a, ulong b) asm("d3dalloy.fp64.rem");
ulong fp64_rem(ulong a, ulong b) {
  // LLVM frem truncates the quotient, unlike IEEE remainder's nearest quotient.
  ulong r = f64_rem({a}, {b}).v;
  if ((r & 0x7fffffffffffffffUL) && ((r ^ a) >> 63) &&
      (r & 0x7fffffffffffffffUL) <= 0x7ff0000000000000UL)
    r = f64_add({r}, {(b & 0x7fffffffffffffffUL) | (a & 0x8000000000000000UL)}).v;
  return r;
}
ulong fp64_fma(ulong a, ulong b, ulong c) asm("d3dalloy.fp64.fma");
ulong fp64_fma(ulong a, ulong b, ulong c) { return f64_mulAdd({a}, {b}, {c}).v; }
bool fp64_eq(ulong a, ulong b) asm("d3dalloy.fp64.eq");
bool fp64_eq(ulong a, ulong b) { return f64_eq({a}, {b}); }
ulong fp64_min(ulong a, ulong b) asm("d3dalloy.fp64.min");
ulong fp64_min(ulong a, ulong b) {
  if ((a & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL) return b;
  if ((b & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL) return a;
  if (((a | b) & 0x7fffffffffffffffUL) == 0) return a | b;
  return f64_lt_quiet({a}, {b}) ? a : b;
}
ulong fp64_max(ulong a, ulong b) asm("d3dalloy.fp64.max");
ulong fp64_max(ulong a, ulong b) {
  if ((a & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL) return b;
  if ((b & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL) return a;
  if (((a | b) & 0x7fffffffffffffffUL) == 0) return a & b;
  return f64_lt_quiet({b}, {a}) ? a : b;
}
ulong fp64_sqrt(ulong a) asm("d3dalloy.fp64.sqrt");
ulong fp64_sqrt(ulong a) { return f64_sqrt({a}).v; }
ulong fp64_round(ulong a, uint mode) asm("d3dalloy.fp64.round");
ulong fp64_round(ulong a, uint mode) { return f64_roundToInt({a}, uchar(mode), false).v; }

ulong fp64_from_f32(uint a) asm("d3dalloy.fp64.from.f32");
ulong fp64_from_f32(uint a) { return f32_to_f64({a}).v; }
ulong fp64_from_f16(ushort a) asm("d3dalloy.fp64.from.f16");
ulong fp64_from_f16(ushort a) { return f16_to_f64({a}).v; }
uint fp64_to_f32(ulong a) asm("d3dalloy.fp64.to.f32");
uint fp64_to_f32(ulong a) { return f64_to_f32({a}).v; }
ushort fp64_to_f16(ulong a) asm("d3dalloy.fp64.to.f16");
ushort fp64_to_f16(ulong a) { return f64_to_f16({a}).v; }
ulong fp64_from_i64(signed64 a) asm("d3dalloy.fp64.from.i64");
ulong fp64_from_i64(signed64 a) { return i64_to_f64(a).v; }
ulong fp64_from_u64(ulong a) asm("d3dalloy.fp64.from.u64");
ulong fp64_from_u64(ulong a) { return ui64_to_f64(a).v; }

// D3D's double-to-integer instructions clamp out-of-range values and map NaN to zero.
signed64 fp64_to_i64(ulong a) asm("d3dalloy.fp64.to.i64");
signed64 fp64_to_i64(ulong a) {
  ulong magnitude = a & 0x7fffffffffffffffUL;
  if (magnitude > 0x7ff0000000000000UL) return 0;
  if (magnitude >= 0x43e0000000000000UL) return a >> 63 ? (-0x7fffffffffffffffL - 1) : 0x7fffffffffffffffL;
  return f64_to_i64_r_minMag({a}, false);
}
ulong fp64_to_u64(ulong a) asm("d3dalloy.fp64.to.u64");
ulong fp64_to_u64(ulong a) {
  ulong magnitude = a & 0x7fffffffffffffffUL;
  if ((a >> 63) || magnitude > 0x7ff0000000000000UL) return 0;
  if (magnitude >= 0x43f0000000000000UL) return 0xffffffffffffffffUL;
  return f64_to_ui64_r_minMag({a}, false);
}
int fp64_to_i32(ulong a) asm("d3dalloy.fp64.to.i32");
int fp64_to_i32(ulong a) {
  ulong magnitude = a & 0x7fffffffffffffffUL;
  if (magnitude > 0x7ff0000000000000UL) return 0;
  if (magnitude >= 0x41e0000000000000UL) return a >> 63 ? (-0x7fffffff - 1) : 0x7fffffff;
  return f64_to_i32_r_minMag({a}, false);
}
uint fp64_to_u32(ulong a) asm("d3dalloy.fp64.to.u32");
uint fp64_to_u32(ulong a) {
  ulong magnitude = a & 0x7fffffffffffffffUL;
  if ((a >> 63) || magnitude > 0x7ff0000000000000UL) return 0;
  if (magnitude >= 0x41f0000000000000UL) return 0xffffffffU;
  return f64_to_ui32_r_minMag({a}, false);
}

#include <metal_stdlib>

using namespace metal;

/*
 * Exact IEEE binary32 add/sub/mul/div for DXIL shaders that declare
 * "fp32-denorm-mode"="preserve" (SM 6.2). Apple GPUs flush fp32 denormals on input
 * and output of every arithmetic instruction, and neither `-fdenorms`
 * (air.compile.denorms_enable) nor any other Metal option changes that on the GPU.
 * Each operation is rebuilt from steps that stay in the normal range and are exact:
 * power of two scaling, fma residuals and an explicit round-to-nearest-even into the
 * denormal grid. dxil_operations.cpp rewrites fp32 fadd/fsub/fmul/fdiv of such
 * shaders into calls to these symbols.
 *
 * Built with -fno-fast-math (src/airconv/meson.build): every step must be IEEE exact.
 */

namespace {

inline uint bits(float x) { return as_type<uint>(x); }
inline float value(uint x) { return as_type<float>(x); }
inline bool is_denormal(uint b) { return (b & 0x7f800000u) == 0 && (b & 0x007fffffu) != 0; }
inline bool is_zero(uint b) { return (b & 0x7fffffffu) == 0; }
inline bool is_special(uint b) { return (b & 0x7f800000u) == 0x7f800000u; }

/* x * 2^90, exact for |x| < 2^-60 (the result is normal or zero). */
inline float scale_up(uint b) {
  const uint magnitude = b & 0x7fffffffu;
  float scaled;
  if (magnitude < 0x00800000u)
    /* 1.m - 1 is m * 2^-23 exactly; * 2^-36 gives m * 2^-59 = (m * 2^-149) * 2^90. */
    scaled = (value(0x3f800000u | magnitude) - 1.0f) * value(0x2d800000u);
  else
    scaled = value(magnitude) * value(0x6c800000u);
  return value(bits(scaled) | (b & 0x80000000u));
}

/* |x| of a finite non-zero x normalised into [1, 2); `exponent` gets its unbiased
 * binary exponent (below -126 for a denormal). */
inline float normalise(uint b, thread int &exponent) {
  const uint magnitude = b & 0x7fffffffu;
  if (magnitude < 0x00800000u) {
    const uint f = bits(value(0x3f800000u | magnitude) - 1.0f);
    exponent = int(f >> 23) - 127 - 126;
    return value((f & 0x007fffffu) | 0x3f800000u);
  }
  exponent = int(magnitude >> 23) - 127;
  return value((magnitude & 0x007fffffu) | 0x3f800000u);
}

/*
 * (p + residual) * 2^e rounded to binary32 with `sign`. p is the correctly rounded
 * product or quotient of two [1, 2) mantissas (p in [0.5, 4)); `residual` has the sign
 * of p's exact rounding error and breaks the ties that p hits exactly when it is
 * rounded again into the denormal grid (p cannot round past a denormal grid point:
 * every such point is representable at p's precision).
 */
inline float assemble(float p, float residual, int e, uint sign) {
  const uint pb = bits(p);
  const int exponent = int(pb >> 23) - 127 + e;
  if (exponent > 127)
    return value(sign | 0x7f800000u);
  if (exponent >= -126)
    return value(sign | (uint(exponent + 127) << 23) | (pb & 0x007fffffu));
  const int shift = e + 149; /* p * 2^shift counts units of 2^-149 */
  if (shift < -2)
    return value(sign);
  const float units = p * value(uint(shift + 127) << 23);
  float rounded = rint(units);
  const float remainder = units - rounded;
  if (remainder == 0.5f && residual > 0.0f)
    rounded += 1.0f;
  if (remainder == -0.5f && residual < 0.0f)
    rounded -= 1.0f;
  return value(sign | uint(rounded));
}

/* A denormal operand of an operation with a zero, infinite or NaN operand only
 * contributes its sign and "finite, non-zero": substitute ±1. */
inline float stand_in(uint b) { return is_denormal(b) ? value((b & 0x80000000u) | 0x3f800000u) : value(b); }

} // namespace

float denorm_fadd(float a, float b) asm("dxmt.denorm.fadd.f32");
float denorm_fsub(float a, float b) asm("dxmt.denorm.fsub.f32");
float denorm_fmul(float a, float b) asm("dxmt.denorm.fmul.f32");
float denorm_fdiv(float a, float b) asm("dxmt.denorm.fdiv.f32");

float denorm_fadd(float a, float b) {
  const uint ba = bits(a), bb = bits(b);
  /* An operand of at least 2^-60 makes the native sum exact: a denormal is below half
   * its ulp and the result cannot be denormal. */
  if ((ba & 0x7fffffffu) >= 0x21800000u || (bb & 0x7fffffffu) >= 0x21800000u)
    return a + b;
  /* Both below 2^-60: every binary32 is a multiple of 2^-149, so the copies scaled by
   * 2^90 add exactly when the result is denormal and round at the same bit as the
   * unscaled sum when it is normal. */
  const float sum = scale_up(ba) + scale_up(bb);
  const uint magnitude = bits(sum) & 0x7fffffffu, sign = bits(sum) & 0x80000000u;
  if (magnitude >= 0x2d800000u)
    return sum * value(0x12800000u); /* * 2^-90, normal result */
  return value(sign | uint(value(magnitude) * value(0x5d000000u))); /* units of 2^-149 */
}

float denorm_fsub(float a, float b) {
  return denorm_fadd(a, value(bits(b) ^ 0x80000000u));
}

float denorm_fmul(float a, float b) {
  const uint ba = bits(a), bb = bits(b);
  const float native = a * b;
  const bool denormal_input = is_denormal(ba) || is_denormal(bb);
  if (is_zero(ba) || is_zero(bb) || is_special(ba) || is_special(bb))
    return denormal_input ? stand_in(ba) * stand_in(bb) : native;
  if (!denormal_input && (bits(native) & 0x7fffffffu) >= 0x00800000u)
    return native;
  int ea, eb;
  const float ma = normalise(ba, ea), mb = normalise(bb, eb);
  const float p = ma * mb;
  return assemble(p, fma(ma, mb, -p), ea + eb, (ba ^ bb) & 0x80000000u);
}

float denorm_fdiv(float a, float b) {
  const uint ba = bits(a), bb = bits(b);
  if (is_zero(ba) || is_zero(bb) || is_special(ba) || is_special(bb))
    return stand_in(ba) / stand_in(bb);
  const float native = a / b;
  const uint magnitude = bits(native) & 0x7fffffffu;
  if (!is_denormal(ba) && !is_denormal(bb) && magnitude >= 0x00800000u && magnitude < 0x7f800000u)
    return native;
  int ea, eb;
  const float ma = normalise(ba, ea), mb = normalise(bb, eb);
  const float q = ma / mb;
  return assemble(q, fma(-q, mb, ma), ea - eb, (ba ^ bb) & 0x80000000u);
}

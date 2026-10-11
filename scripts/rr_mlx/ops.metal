// RR PTX numerical helpers. Include as an mx.fast.metal_kernel header.
// ABI: half registers occupy uint bits[15:0]; half2 registers pack low lane in
// bits[15:0], high lane in bits[31:16]. Scalar results zero-extend to uint.
// rr_half{,2}_{add,sub,mul}(a,b), rr_half{,2}_fma(a,b,c): RNE at this
// instruction, gradual half underflow, IEEE signed zero; NaNs canonical 0x7e00.
// Compile with math_mode=safe, precise math functions, RTNE float conversion.
// This header disables contraction: only explicit metal::fma may fuse.
// Half FMA uses an exact FP32 product plus TwoSum residual to remove FP32->half
// double rounding, not an unverified native half FMA accuracy assumption.
// rr_fp8x2(half2,relu): cvt.rn.satfinite{.relu}.e4m3x2.f16x2, uint low16
// result. rr_fp8_decode2(fp8x2,relu=false): cvt.rn{.relu}.f16x2.e4m3x2.
// E4M3 has bias7, subnormal quantum 2^-9, max448, NaN codes +/-0x7f; no Inf.
// ReLU clamps strictly negative values, preserves -0, canonicalizes NaNs.
// rr_half_bits_rz(float)->uint: cvt.rz.f16.f32, truncating finite magnitudes
// toward zero (overflow to signed65504, Inf unchanged, canonical half NaN).
// rr_add_rz(float,float)->float: exact FP32 addition toward zero; caller
// wraps rr_ftz on inputs/results for add.rz.ftz.f32 (observed HKPN).
// rr_movmatrix(uint packed_half2,uint lane)->uint: the original32-lane
// movmatrix.sync.trans.aligned.m8n8.b16, with the same convergence contract
// as rr_mma. Lane holds row=lane>>2, columns=2*(lane&3)+{0,1}.
// rr_mma(A:uint4,B:uint2,C:uint2,lane:uint)->uint2 implements
// mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16. Every physical SIMD
// group must have exactly32 lanes, identical original warp/lane mapping, and
// execute this call convergently. A/B bytes and C halfs are low-to-high in
// PTX register order. Accumulate ascending k=0..31 in FP32, starting at C,
// explicit FP32 FMA, then publish FP16 RNE. The public fragment layout is exact;
// NVIDIA tensor-core accumulation order/rounding is unspecified and unknown.
// This deterministic accumulation is NOT a hardware-bit-exact NVIDIA claim.
// rr_shuffle(value,lane,b,c,membermask,mode)->uint2(value,flags):
// mode0=up,1=down,2=bfly,3=idx; flags bit0=PTX range predicate,
// bit1=defined membership (caller and selected source both named). Clamp c[4:0]
// and segment mask c[12:8], offset/index b[4:0], follow PTX literally.
// Membership failure returns canonical poison 0xdeadbeef with bit1 clear;
// the emitter MUST diagnose it, not commit it as a numerical result.
// The emitter/scheduler owns shfl.sync wait/active-source validation; this
// pure helper cannot reconstruct predication or exited lanes. All physical
// lanes execute it; only original participating lanes commit destinations.
// rr_sfu(x,flags)->float: op in bits[2:0] = ex2,lg2,rcp,rsqrt,sqrt,tanh,sin,cos;
// bit8=ftz (flush input AND output subnormals to sign-preserving zero).
// PTX approximation is evaluated using Metal precise functions with explicit
// PTX special cases. NVIDIA SFU approximation bits are NOT reproduced; callers
// must report this limit and verify public PTX error bounds independently.
// PTX acceptance bounds: ex2 <=2 ULP, rcp <=1 ULP; sqrt relative <=2^-23,
// rsqrt relative <=2^-22.9; lg2 absolute <=2^-22 for x in(0.5,2), relative
// <=2^-22 otherwise (positive finite x). These are targets, not GPU results.
// Observed RR ops: ex2/lg2/rcp/rsqrt/sqrt.approx.ftz.f32; no half arithmetic
// .ftz/.sat/.relu in extracted module PTX. FP8 cvt.relu is present.
// Sources: https://docs.nvidia.com/cuda/parallel-thread-execution/index.html
// #half-precision-floating-point-instructions-fma
// #data-movement-and-conversion-instructions-cvt
// #data-movement-and-conversion-instructions-shfl-sync
// #warp-level-matrix-fragment-mma-16832 (E4M3 A/B and f16 C/D formulae)
// #warp-level-matrix-instructions-movmatrix
// #floating-point-instructions-{ex2,lg2,rcp,rsqrt,sqrt,tanh,sin,cos}
// Independent references: reconstruction/cuda/transformer_reference.py
// (module-14.ptx:14773-14845), reconstruction/verify_fp8_layout.py.
// Root QA entry points: call helpers directly in one32-lane metal_kernel;
// half2 inputs (h,h^0x8000), all65536 h; compare FP8 to nearest-code Fraction
// oracle with even-code ties, and finite FMA to embedding_reference.rounded.
// FMA double-rounding witness: rr_half_fma(0x3c01,0x3e00,0x0001)==0x3e02;
// opposite residual rr_half_fma(0x3c01,0x3e00,0x8001)==0x3e01.
// RZ QA: rr_half_bits_rz(1.0009f)==0x3c00; finite1e10f->0x7bff.
// rr_add_rz(1.0f,-0x1p-25f) bits=0x3f7fffff; +0x1p-25f->0x3f800000.
// Max-finite + max-finite ->max-finite; signed zero and exact cancellation.
// Movmatrix QA: input half2 codes (8*row+col,8*row+col+1), output codes
// (8*col+row,8*(col+1)+row), for row=lane>>2,col=2*(lane&3).
// FP8 vectors: 0x80003c00->0x8038, 0x5f005c00->0x7e78,
// 0x7c00fc00->0x7efe, relu->0x7e00, 0x7e007e00->0x7f7f;
// half midpoint 0x2080 (9*2^-10)->E4M3 0x04, 0x2180->0x06.
// MMA layout QA: encode a single1 in each of512 A positions and each of256 B
// positions using the public formulae, C=0; exactly matching k contributes1
// at the corresponding D row/col, otherwise D=0. Also A=B=1,C=1 ->all33.
// Shuffle QA input=lane: b=1,c=31,mask=0xffffffff,down yields lane+1 (p=1)
// except31->31(p=0); up uses c=0, lane0->0(p=0). Segment test c=0x181f,
// down7->7(p=0), idx b=3 gives (lane&24)|3. mask excludes selected source:
// flags bit1 must clear. b=33 equals b=1. Check all modes and clamp values.
// SFU QA +/-0, +/-Inf, NaN, +/-smallest-subnormal, x=1,4; FTZ preserves
// sign for rcp/rsqrt zero and sqrt(-0). GPU checks are root-owned, unrun here.

#ifndef RR_MLX_OPS_METAL
#define RR_MLX_OPS_METAL
#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)

// Metal has no double type. The original exposure-copy PTX requires binary64
// conversion, max and correctly rounded division; preserve those bits with
// integer arithmetic rather than silently replacing them with float division.
inline ulong rr_round_shift(ulong value, uint shift, bool sticky) {
    if (shift == 0) return value;
    if (shift >= 64) return 0;
    ulong result = value >> shift, tail = value & ((1ul << shift) - 1ul);
    ulong midpoint = 1ul << (shift - 1);
    return result + (tail > midpoint || (tail == midpoint && (sticky || (result & 1ul))));
}

inline ulong rr_f64_significand(ulong bits, thread int& exponent) {
    uint encoded = uint((bits >> 52) & 2047ul);
    ulong significand = bits & 0xffffffffffffful;
    exponent = int(encoded) - 1023;
    if (encoded) return significand | (1ul << 52);
    uint shift = clz(significand) - 11u;
    exponent = -1022 - int(shift);
    return significand << shift;
}

inline ulong rr_f32_to_f64(float value) {
    uint bits = as_type<uint>(value), encoded = (bits >> 23) & 255u;
    ulong sign = ulong(bits & 0x80000000u) << 32;
    uint significand = bits & 0x7fffffu;
    if (encoded == 255) return sign | 0x7ff0000000000000ul | (significand ? 0x8000000000000ul : 0ul);
    if ((bits & 0x7fffffffu) == 0) return sign;
    int exponent = int(encoded) - 127;
    if (encoded) significand |= 1u << 23;
    else { uint shift = clz(significand) - 8u; significand <<= shift; exponent = -126 - int(shift); }
    return sign | (ulong(exponent + 1023) << 52) | (ulong(significand & 0x7fffffu) << 29);
}

inline float rr_f64_to_f32(ulong bits) {
    uint sign = uint(bits >> 32) & 0x80000000u;
    ulong magnitude = bits & 0x7ffffffffffffffful;
    if (magnitude >= 0x7ff0000000000000ul)
        return as_type<float>(sign | 0x7f800000u | (magnitude > 0x7ff0000000000000ul ? 0x400000u : 0u));
    if (!magnitude) return as_type<float>(sign);
    int exponent; ulong significand = rr_f64_significand(bits, exponent);
    if (exponent > 127) return as_type<float>(sign | 0x7f800000u);
    ulong rounded = rr_round_shift(significand, uint(29 + max(-126 - exponent, 0)), false);
    if (exponent < -126) return as_type<float>(sign | uint(rounded));
    if (rounded == (1ul << 24)) { rounded >>= 1; ++exponent; }
    if (exponent > 127) return as_type<float>(sign | 0x7f800000u);
    return as_type<float>(sign | (uint(exponent + 127) << 23) | (uint(rounded) & 0x7fffffu));
}

inline ulong rr_f64_max(ulong a, ulong b) {
    ulong am = a & 0x7ffffffffffffffful, bm = b & 0x7ffffffffffffffful;
    if (am > 0x7ff0000000000000ul) return bm > 0x7ff0000000000000ul ? 0x7ff8000000000000ul : b;
    if (bm > 0x7ff0000000000000ul) return a;
    if (!am && !bm) return a & b;
    if ((a ^ b) >> 63) return (a >> 63) ? b : a;
    return (a >> 63) ? (am < bm ? a : b) : (am > bm ? a : b);
}

inline ulong rr_f64_div(ulong a, ulong b) {
    ulong sign = (a ^ b) & 0x8000000000000000ul;
    ulong am = a & 0x7ffffffffffffffful, bm = b & 0x7ffffffffffffffful;
    const ulong infinity = 0x7ff0000000000000ul;
    if (am > infinity || bm > infinity || (am == infinity && bm == infinity) || (!am && !bm))
        return 0x7ff8000000000000ul;
    if (am == infinity || !bm) return sign | infinity;
    if (!am || bm == infinity) return sign;
    int ae, be; ulong numerator = rr_f64_significand(a, ae), denominator = rr_f64_significand(b, be);
    int exponent = ae - be;
    if (numerator < denominator) { numerator <<= 1; --exponent; }
    ulong quotient = 1ul << 52, remainder = numerator - denominator;
    for (int bit = 51; bit >= 0; --bit) {
        remainder <<= 1;
        if (remainder >= denominator) { remainder -= denominator; quotient |= 1ul << bit; }
    }
    if (exponent < -1022) return sign | rr_round_shift(quotient, uint(-1022 - exponent), remainder != 0);
    quotient += 2 * remainder > denominator || (2 * remainder == denominator && (quotient & 1ul));
    if (quotient == (1ul << 53)) { quotient >>= 1; ++exponent; }
    if (exponent > 1023) return sign | infinity;
    return sign | (ulong(exponent + 1023) << 52) | (quotient & 0xffffffffffffful);
}

inline float rr_half_float(uint h) {
    return float(as_type<half>(ushort(h)));
}

inline uint rr_half_bits(float x) {
    return isnan(x) ? 0x7e00u : uint(as_type<ushort>(half(x)));
}

inline uint rr_half_add(uint a, uint b) {
    return rr_half_bits(rr_half_float(a) + rr_half_float(b));
}

inline uint rr_half_bits_rz(float x) {
    uint bits = as_type<uint>(x), sign = (bits >> 16) & 0x8000u;
    uint magnitude = bits & 0x7fffffffu, exponent = magnitude >> 23;
    uint fraction = magnitude & 0x7fffffu;
    if (exponent == 255u) return fraction != 0u ? 0x7e00u : sign | 0x7c00u;
    if (exponent > 142u) return sign | 0x7bffu;
    if (exponent >= 113u)
        return sign | ((exponent - 112u) << 10) | (fraction >> 13);
    if (exponent < 103u) return sign;
    return sign | ((fraction | 0x800000u) >> (126u - exponent));
}

inline float rr_add_rz(float x, float y) {
    uint a = as_type<uint>(x), b = as_type<uint>(y);
    uint am = a & 0x7fffffffu, bm = b & 0x7fffffffu;
    if (am > 0x7f800000u || bm > 0x7f800000u)
        return as_type<float>(0x7fc00000u);
    if (am == 0x7f800000u || bm == 0x7f800000u) {
        if (am == bm && ((a ^ b) >> 31) != 0u)
            return as_type<float>(0x7fc00000u);
        return am == 0x7f800000u ? x : y;
    }
    // Larger magnitude first makes subtraction nonnegative. Three low bits
    // retain guard/round/sticky through alignment and cancellation.
    if (am < bm) { uint t = a; a = b; b = t; t = am; am = bm; bm = t; }
    uint ae = am >> 23, be = bm >> 23;
    ulong av = ulong((am & 0x7fffffu) | (ae != 0u ? 0x800000u : 0u)) << 3;
    ulong bv = ulong((bm & 0x7fffffu) | (be != 0u ? 0x800000u : 0u)) << 3;
    ae = max(ae, 1u); be = max(be, 1u);
    uint distance = ae - be;
    if (distance >= 32u) bv = ulong(bv != 0ul);
    else if (distance != 0u)
        bv = (bv >> distance) | ulong((bv & ((1ul << distance) - 1ul)) != 0ul);
    bool subtract = ((a ^ b) >> 31) != 0u;
    ulong sum = subtract ? av - bv : av + bv;
    uint sign = a & 0x80000000u;
    if (sum == 0ul) return as_type<float>(subtract ? 0u : sign);
    if (sum >= (1ul << 27)) { sum = (sum >> 1) | (sum & 1ul); ++ae; }
    while (sum < (1ul << 26) && ae > 1u) { sum <<= 1; --ae; }
    if (ae >= 255u) return as_type<float>(sign | 0x7f7fffffu);
    uint significand = uint(sum >> 3);
    uint exponent = (ae == 1u && significand < 0x800000u) ? 0u : ae;
    return as_type<float>(sign | (exponent << 23) | (significand & 0x7fffffu));
}

inline uint rr_half_sub(uint a, uint b) {
    return rr_half_bits(rr_half_float(a) - rr_half_float(b));
}

inline uint rr_half_mul(uint a, uint b) {
    return rr_half_bits(rr_half_float(a) * rr_half_float(b));
}

inline uint rr_half_fma(uint a, uint b, uint c) {
    float x = rr_half_float(a), y = rr_half_float(b), z = rr_half_float(c);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return rr_half_bits(metal::fma(x, y, z));
    // Half significands have at most11 bits, so the22-bit product is exact
    // FP32 (including half subnormals); product range2^-48..65504^2 is normal.
    float p = x * y;
    float s = p + z;
    uint bits = as_type<uint>(s);
    uint exponent = (bits >> 23) & 255u;
    // Only a half rounding midpoint can double-round. For subnormal half
    // output the quantum is2^-24; for normal output discard13 FP32 bits.
    if (exponent >= 102u && exponent <= 142u) {
        uint shift = exponent < 113u ? 126u - exponent : 13u;
        uint significand = (bits & 0x7fffffu) | 0x800000u;
        if ((significand & ((1u << shift) - 1u)) == (1u << (shift - 1u))) {
            float v = s - p;
            float residual = (p - (s - v)) + (z - v);
            if (residual != 0.0f)
                s = nextafter(s, residual > 0.0f ? INFINITY : -INFINITY);
        }
    }
    return rr_half_bits(s);
}

inline uint rr_half2_add(uint a, uint b) {
    return rr_half_add(a, b) | (rr_half_add(a >> 16, b >> 16) << 16);
}

inline uint rr_half2_sub(uint a, uint b) {
    return rr_half_sub(a, b) | (rr_half_sub(a >> 16, b >> 16) << 16);
}

inline uint rr_half2_mul(uint a, uint b) {
    return rr_half_mul(a, b) | (rr_half_mul(a >> 16, b >> 16) << 16);
}

inline uint rr_half2_fma(uint a, uint b, uint c) {
    return rr_half_fma(a, b, c) | (rr_half_fma(a >> 16, b >> 16, c >> 16) << 16);
}

inline uint rr_fp8_encode(uint h, bool relu) {
    uint magnitude = h & 0x7fffu;
    uint sign = (h >> 8) & 0x80u;
    if (magnitude > 0x7c00u) return 0x7fu;
    if (relu && sign != 0u && magnitude != 0u) return 0u;
    if (magnitude >= 0x5f00u) return sign | 0x7eu; //448 and above, including Inf
    uint code;
    if (magnitude < 0x2400u) {
        // Exact power-of-two scale; rint is nearest integer, ties to even.
        code = uint(rint(rr_half_float(magnitude) * 512.0f));
    } else {
        uint upper = magnitude >> 7;
        uint discarded = magnitude & 127u;
        upper += uint(discarded > 64u || (discarded == 64u && (upper & 1u)));
        code = min(upper - 64u, 126u);
    }
    return sign | code;
}

inline uint rr_fp8x2(uint a, bool relu) {
    return rr_fp8_encode(a & 0xffffu, relu) |
           (rr_fp8_encode(a >> 16, relu) << 8);
}

inline uint rr_fp8_decode(uint a, bool relu = false) {
    uint magnitude = a & 127u;
    uint sign = (a & 128u) << 8;
    if (magnitude == 127u) return 0x7e00u;
    if (relu && sign != 0u && magnitude != 0u) return 0u;
    if (magnitude < 8u)
        return sign | rr_half_bits(float(magnitude) * 0.001953125f);
    return sign | (((magnitude >> 3) + 8u) << 10) | ((magnitude & 7u) << 7);
}

inline uint rr_fp8_decode2(uint a, bool relu = false) {
    return rr_fp8_decode(a, relu) | (rr_fp8_decode(a >> 8, relu) << 16);
}

// Keep the software tensor instruction out of thousands of caller blocks.
// Unrolling each copy made the Metal backend spend minutes expanding loops.
__attribute__((noinline)) uint2 rr_mma(uint4 a, uint2 b, uint2 c, uint lane) {
    float4 sum = float4(rr_half_float(c.x), rr_half_float(c.x >> 16),
                        rr_half_float(c.y), rr_half_float(c.y >> 16));
    uint group = lane >> 2, column = (lane & 3u) * 2u;
    #pragma clang loop unroll(disable)
    for (uint k = 0u; k < 32u; ++k) {
        uint kthread = (k & 15u) >> 2;
        uint4 ar = simd_shuffle(a, group * 4u + kthread);
        uint2 br0 = simd_shuffle(b, column * 4u + kthread);
        uint2 br1 = simd_shuffle(b, (column + 1u) * 4u + kthread);
        uint byte_shift = (k & 3u) * 8u;
        uint ablock = (k >> 4) * 2u, bblock = k >> 4;
        float al = rr_half_float(rr_fp8_decode(ar[ablock] >> byte_shift));
        float ah = rr_half_float(rr_fp8_decode(ar[ablock + 1u] >> byte_shift));
        float bl = rr_half_float(rr_fp8_decode(br0[bblock] >> byte_shift));
        float bh = rr_half_float(rr_fp8_decode(br1[bblock] >> byte_shift));
        sum = metal::fma(float4(al, al, ah, ah), float4(bl, bh, bl, bh), sum);
    }
    return uint2(rr_half_bits(sum.x) | (rr_half_bits(sum.y) << 16),
                 rr_half_bits(sum.z) | (rr_half_bits(sum.w) << 16));
}

inline uint2 rr_shuffle(uint value, uint lane, uint b, uint c,
                        uint membermask, uint mode) {
    uint offset = b & 31u, clamp = c & 31u, segment = (c >> 8) & 31u;
    uint min_lane = lane & segment;
    uint max_lane = min_lane | (clamp & (~segment & 31u));
    int source;
    bool predicate;
    switch (mode) {
        case 0u: source = int(lane) - int(offset);
                 predicate = source >= int(max_lane); break;
        case 1u: source = int(lane + offset);
                 predicate = source <= int(max_lane); break;
        case 2u: source = int(lane ^ offset);
                 predicate = source <= int(max_lane); break;
        default: source = int(min_lane | (offset & (~segment & 31u)));
                 predicate = source <= int(max_lane); break;
    }
    if (!predicate) source = int(lane);
    bool membership = ((membermask >> lane) & 1u) != 0u &&
                      ((membermask >> uint(source)) & 1u) != 0u;
    uint shuffled = simd_shuffle(value, uint(source));
    return uint2(membership ? shuffled : 0xdeadbeefu,
                 uint(predicate) | (uint(membership) << 1));
}

inline uint rr_movmatrix(uint value, uint lane) {
    uint row = lane >> 2, column = (lane & 3u) * 2u;
    uint source = column * 4u + (row >> 1);
    uint low = simd_shuffle(value, source);
    uint high = simd_shuffle(value, source + 4u);
    uint shift = (row & 1u) * 16u;
    return ((low >> shift) & 0xffffu) | (((high >> shift) & 0xffffu) << 16);
}

inline float rr_ftz(float x) {
    uint bits = as_type<uint>(x);
    return (bits & 0x7f800000u) == 0u ? as_type<float>(bits & 0x80000000u) : x;
}

inline float rr_sfu(float x, uint flags) {
    bool ftz = (flags & 256u) != 0u;
    if (ftz) x = rr_ftz(x);
    uint op = flags & 7u;
    float y;
    if (isnan(x)) return as_type<float>(0x7fc00000u);
    switch (op) {
        case 0u: y = metal::exp2(x); break;
        case 1u: y = x < 0.0f ? as_type<float>(0x7fc00000u) :
                     (x == 0.0f ? -INFINITY : metal::log2(x)); break;
        case 2u: y = x == 0.0f ? copysign(INFINITY, x) :
                     (isinf(x) ? copysign(0.0f, x) : 1.0f / x); break;
        case 3u: y = x < 0.0f ? as_type<float>(0x7fc00000u) :
                     (x == 0.0f ? copysign(INFINITY, x) :
                      (isinf(x) ? 0.0f : metal::rsqrt(x))); break;
        case 4u: y = x < 0.0f ? as_type<float>(0x7fc00000u) :
                     (x == 0.0f ? x : metal::sqrt(x)); break;
        case 5u: y = x == 0.0f ? x : (isinf(x) ? copysign(1.0f, x) : metal::tanh(x)); break;
        case 6u: y = isinf(x) ? as_type<float>(0x7fc00000u) :
                     (x == 0.0f ? x : metal::sin(x)); break;
        default: y = isinf(x) ? as_type<float>(0x7fc00000u) : metal::cos(x); break;
    }
    return ftz ? rr_ftz(y) : y;
}
#endif

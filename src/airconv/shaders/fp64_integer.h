/*============================================================================

This C header file is part of the SoftFloat IEEE Floating-Point Arithmetic
Package, Release 3e, by John R. Hauser.

Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017 The Regents of the
University of California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions, and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions, and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

 3. Neither the name of the University nor the names of its contributors may
    be used to endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

=============================================================================*/
#pragma once
/* Berkeley SoftFloat 3e integer binary64 subset. Fixed RNE arithmetic; explicit integer rounding intrinsics.
 * Floating exception status is not observable in D3D. CPU and Metal use the same integer core.
 * Upstream https://www.jhauser.us/arithmetic/SoftFloat.html; only binary16/32 conversions and binary64 operations retained. */
#ifdef __METAL_VERSION__
#define SF_THREAD thread
#define SF_CONST constant
#elif defined(__AIR64__)
#define SF_THREAD
#define SF_CONST const __attribute__((address_space(2)))
#else
#define SF_THREAD
#define SF_CONST const
#endif
namespace dxmt_softfloat {
typedef unsigned char uint8_t; typedef signed char int8_t;
typedef unsigned short uint16_t; typedef short int16_t;
typedef unsigned int uint32_t; typedef int int32_t;
typedef __UINT64_TYPE__ uint64_t; typedef __INT64_TYPE__ int64_t;
static_assert(sizeof(uint64_t) == 8 && sizeof(int64_t) == 8, "binary64 requires exactly 64 integer bits");
struct float16_t { uint16_t v; }; struct float32_t { uint32_t v; }; struct float64_t { uint64_t v; };
union ui16_f16 { uint16_t ui; float16_t f; }; union ui32_f32 { uint32_t ui; float32_t f; }; union ui64_f64 { uint64_t ui; float64_t f; };
struct uint128 { uint64_t v0, v64; }; struct exp8_sig16 { int8_t exp; uint32_t sig; };
struct exp16_sig32 { int32_t exp; uint32_t sig; }; struct exp16_sig64 { int32_t exp; uint64_t sig; };
struct commonNaN { bool sign; uint64_t v0, v64; };
enum { softfloat_round_near_even=0, softfloat_round_minMag=1, softfloat_round_min=2, softfloat_round_max=3, softfloat_round_near_maxMag=4, softfloat_round_odd=6,
softfloat_tininess_beforeRounding=0,softfloat_tininess_afterRounding=1,softfloat_flag_inexact=1,softfloat_flag_underflow=2,softfloat_flag_overflow=4,softfloat_flag_infinite=8,softfloat_flag_invalid=16,softfloat_mulAdd_subC=1,softfloat_mulAdd_subProd=2 };
inline void softfloat_raiseFlags(uint8_t) {}
extern SF_CONST uint8_t softfloat_countLeadingZeros8[256];
inline void softfloat_f64UIToCommonNaN(uint64_t, SF_THREAD commonNaN *);
inline void softfloat_f32UIToCommonNaN(uint32_t, SF_THREAD commonNaN *);
inline void softfloat_f16UIToCommonNaN(uint32_t, SF_THREAD commonNaN *);
inline float64_t i32_to_f64( int32_t a );
inline int32_t f64_to_i32_r_minMag( float64_t a, bool exact );
inline float32_t f64_to_f32( float64_t a );
inline uint32_t softfloat_commonNaNToF32UI( SF_THREAD const struct commonNaN *aPtr );
inline float16_t f64_to_f16( float64_t a );
inline uint32_t softfloat_commonNaNToF16UI( SF_THREAD const struct commonNaN *aPtr );
inline float64_t f32_to_f64( float32_t a );
inline uint64_t softfloat_commonNaNToF64UI( SF_THREAD const struct commonNaN *aPtr );
inline float64_t f16_to_f64( float16_t a );
inline bool f64_le_quiet( float64_t a, float64_t b );
inline bool f64_lt_quiet( float64_t a, float64_t b );
inline bool f64_eq( float64_t a, float64_t b );
inline float64_t f64_roundToInt( float64_t a, uint8_t roundingMode, bool exact );
inline uint64_t
 softfloat_propagateNaNF64UI( uint64_t uiA, uint64_t uiB );
inline float64_t f64_mulAdd( float64_t a, float64_t b, float64_t c );
inline float64_t f64_sqrt( float64_t a );
inline float64_t f64_rem( float64_t a, float64_t b );
inline float64_t f64_div( float64_t a, float64_t b );
inline float64_t f64_mul( float64_t a, float64_t b );
inline float64_t f64_sub( float64_t a, float64_t b );
inline float64_t f64_add( float64_t a, float64_t b );
inline float64_t ui64_to_f64( uint64_t a );
inline float64_t
 softfloat_normRoundPackToF64( bool sign, int32_t exp, uint64_t sig );
inline float64_t
 softfloat_roundPackToF64( bool sign, int32_t exp, uint64_t sig );
inline uint64_t softfloat_shiftRightJam64( uint64_t a, uint32_t dist );
inline uint8_t softfloat_countLeadingZeros64( uint64_t a );
inline uint64_t softfloat_shortShiftRightJam64( uint64_t a, uint8_t dist );
inline float64_t i64_to_f64( int64_t a );
inline float64_t ui32_to_f64( uint32_t a );
inline uint8_t softfloat_countLeadingZeros32( uint32_t a );
inline uint64_t f64_to_ui64_r_minMag( float64_t a, bool exact );
inline int64_t f64_to_i64_r_minMag( float64_t a, bool exact );
inline float64_t
 softfloat_subMagsF64( uint64_t uiA, uint64_t uiB, bool signZ );
inline float64_t
 softfloat_addMagsF64( uint64_t uiA, uint64_t uiB, bool signZ );
inline struct uint128 softfloat_mul64To128( uint64_t a, uint64_t b );
inline struct exp16_sig64 softfloat_normSubnormalF64Sig( uint64_t sig );
inline uint32_t softfloat_approxRecip32_1( uint32_t a );
inline uint32_t softfloat_approxRecipSqrt32_1( unsigned int oddExpA, uint32_t a );
inline float64_t
 softfloat_mulAddF64(
     uint64_t uiA, uint64_t uiB, uint64_t uiC, uint8_t op );
inline struct uint128
 softfloat_shortShiftLeft128( uint64_t a64, uint64_t a0, uint8_t dist );
inline struct uint128
 softfloat_sub128( uint64_t a64, uint64_t a0, uint64_t b64, uint64_t b0 );
inline struct uint128
 softfloat_add128( uint64_t a64, uint64_t a0, uint64_t b64, uint64_t b0 );
inline struct uint128
 softfloat_shiftRightJam128( uint64_t a64, uint64_t a0, uint32_t dist );
inline struct uint128
 softfloat_shortShiftRightJam128(
     uint64_t a64, uint64_t a0, uint8_t dist );
inline struct exp8_sig16 softfloat_normSubnormalF16Sig( uint32_t sig );
inline uint8_t softfloat_countLeadingZeros16( uint16_t a );
inline struct exp16_sig32 softfloat_normSubnormalF32Sig( uint32_t sig );
inline float16_t
 softfloat_roundPackToF16( bool sign, int32_t exp, uint32_t sig );
inline uint32_t softfloat_shiftRightJam32( uint32_t a, uint32_t dist );
inline float32_t
 softfloat_roundPackToF32( bool sign, int32_t exp, uint32_t sig );
inline uint32_t f64_to_ui32_r_minMag( float64_t a, bool exact );
inline float64_t i32_to_f64( int32_t a )
{
    uint64_t uiZ;
    bool sign;
    uint32_t absA;
    int8_t shiftDist;
    union ui64_f64 uZ;
    if ( ! a ) {
        uiZ = 0;
    } else {
        sign = (a < 0);
        absA = sign ? -(uint32_t) a : (uint32_t) a;
        shiftDist = softfloat_countLeadingZeros32( absA ) + 21;
        uiZ =
            ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0x432 - shiftDist)<<52) + ((uint64_t) absA<<shiftDist)));
    }
    uZ.ui = uiZ;
    return uZ.f;
}
inline int32_t f64_to_i32_r_minMag( float64_t a, bool exact )
{
    union ui64_f64 uA;
    uint64_t uiA;
    int32_t exp;
    uint64_t sig;
    int32_t shiftDist;
    bool sign;
    int32_t absZ;
    uA.f = a;
    uiA = uA.ui;
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    sig = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    shiftDist = 0x433 - exp;
    if ( 53 <= shiftDist ) {
        if ( exact && (exp | sig) ) {
        }
        return 0;
    }
    sign = ((bool) ((uint64_t) (uiA)>>63));
    if ( shiftDist < 22 ) {
        if (
            sign && (exp == 0x41E) && (sig < 0x0000000000200000UL)
        ) {
            if ( exact && sig ) {
            }
            return -0x7FFFFFFF - 1;
        }
        softfloat_raiseFlags( softfloat_flag_invalid );
        return
            (exp == 0x7FF) && sig ? (-0x7FFFFFFF - 1)
                : sign ? (-0x7FFFFFFF - 1) : (-0x7FFFFFFF - 1);
    }
    sig |= 0x0010000000000000UL;
    absZ = sig>>shiftDist;
    if ( exact && ((uint64_t) (uint32_t) absZ<<shiftDist != sig) ) {
    }
    return sign ? -absZ : absZ;
}
inline float32_t f64_to_f32( float64_t a )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool sign;
    int32_t exp;
    uint64_t frac;
    struct commonNaN commonNaN;
    uint32_t uiZ, frac32;
    union ui32_f32 uZ;
    uA.f = a;
    uiA = uA.ui;
    sign = ((bool) ((uint64_t) (uiA)>>63));
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    frac = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    if ( exp == 0x7FF ) {
        if ( frac ) {
            softfloat_f64UIToCommonNaN( uiA, &commonNaN );
            uiZ = softfloat_commonNaNToF32UI( &commonNaN );
        } else {
            uiZ = (((uint32_t) (sign)<<31) + ((uint32_t) (0xFF)<<23) + (0));
        }
        goto uiZ;
    }
    frac32 = softfloat_shortShiftRightJam64( frac, 22 );
    if ( ! (exp | frac32) ) {
        uiZ = (((uint32_t) (sign)<<31) + ((uint32_t) (0)<<23) + (0));
        goto uiZ;
    }
    return softfloat_roundPackToF32( sign, exp - 0x381, frac32 | 0x40000000 );
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint32_t softfloat_commonNaNToF32UI( SF_THREAD const struct commonNaN *aPtr )
{
    return (uint32_t) aPtr->sign<<31 | 0x7FC00000 | aPtr->v64>>41;
}
inline void softfloat_f64UIToCommonNaN( uint64_t uiA, SF_THREAD struct commonNaN *zPtr )
{
    if ( ((((uiA) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiA) & 0x0007FFFFFFFFFFFFUL)) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    zPtr->sign = uiA>>63;
    zPtr->v64 = uiA<<12;
    zPtr->v0 = 0;
}
inline float16_t f64_to_f16( float64_t a )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool sign;
    int32_t exp;
    uint64_t frac;
    struct commonNaN commonNaN;
    uint32_t uiZ, frac16;
    union ui16_f16 uZ;
    uA.f = a;
    uiA = uA.ui;
    sign = ((bool) ((uint64_t) (uiA)>>63));
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    frac = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    if ( exp == 0x7FF ) {
        if ( frac ) {
            softfloat_f64UIToCommonNaN( uiA, &commonNaN );
            uiZ = softfloat_commonNaNToF16UI( &commonNaN );
        } else {
            uiZ = (((uint16_t) (sign)<<15) + ((uint16_t) (0x1F)<<10) + (0));
        }
        goto uiZ;
    }
    frac16 = softfloat_shortShiftRightJam64( frac, 38 );
    if ( ! (exp | frac16) ) {
        uiZ = (((uint16_t) (sign)<<15) + ((uint16_t) (0)<<10) + (0));
        goto uiZ;
    }
    return softfloat_roundPackToF16( sign, exp - 0x3F1, frac16 | 0x4000 );
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint32_t softfloat_commonNaNToF16UI( SF_THREAD const struct commonNaN *aPtr )
{
    return (uint32_t) aPtr->sign<<15 | 0x7E00 | aPtr->v64>>54;
}
inline float64_t f32_to_f64( float32_t a )
{
    union ui32_f32 uA;
    uint32_t uiA;
    bool sign;
    int32_t exp;
    uint32_t frac;
    struct commonNaN commonNaN;
    uint64_t uiZ;
    struct exp16_sig32 normExpSig;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    sign = ((bool) ((uint32_t) (uiA)>>31));
    exp = ((int32_t) ((uiA)>>23) & 0xFF);
    frac = ((uiA) & 0x007FFFFF);
    if ( exp == 0xFF ) {
        if ( frac ) {
            softfloat_f32UIToCommonNaN( uiA, &commonNaN );
            uiZ = softfloat_commonNaNToF64UI( &commonNaN );
        } else {
            uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
        }
        goto uiZ;
    }
    if ( ! exp ) {
        if ( ! frac ) {
            uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0)<<52) + (0)));
            goto uiZ;
        }
        normExpSig = softfloat_normSubnormalF32Sig( frac );
        exp = normExpSig.exp - 1;
        frac = normExpSig.sig;
    }
    uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (exp + 0x380)<<52) + ((uint64_t) frac<<29)));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint64_t softfloat_commonNaNToF64UI( SF_THREAD const struct commonNaN *aPtr )
{
    return
        (uint64_t) aPtr->sign<<63 | 0x7FF8000000000000UL
            | aPtr->v64>>12;
}
inline void softfloat_f32UIToCommonNaN( uint32_t uiA, SF_THREAD struct commonNaN *zPtr )
{
    if ( ((((uiA) & 0x7FC00000) == 0x7F800000) && ((uiA) & 0x003FFFFF)) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    zPtr->sign = uiA>>31;
    zPtr->v64 = (uint64_t) uiA<<41;
    zPtr->v0 = 0;
}
inline float64_t f16_to_f64( float16_t a )
{
    union ui16_f16 uA;
    uint32_t uiA;
    bool sign;
    int8_t exp;
    uint32_t frac;
    struct commonNaN commonNaN;
    uint64_t uiZ;
    struct exp8_sig16 normExpSig;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    sign = ((bool) ((uint16_t) (uiA)>>15));
    exp = ((int8_t) ((uiA)>>10) & 0x1F);
    frac = ((uiA) & 0x03FF);
    if ( exp == 0x1F ) {
        if ( frac ) {
            softfloat_f16UIToCommonNaN( uiA, &commonNaN );
            uiZ = softfloat_commonNaNToF64UI( &commonNaN );
        } else {
            uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
        }
        goto uiZ;
    }
    if ( ! exp ) {
        if ( ! frac ) {
            uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0)<<52) + (0)));
            goto uiZ;
        }
        normExpSig = softfloat_normSubnormalF16Sig( frac );
        exp = normExpSig.exp - 1;
        frac = normExpSig.sig;
    }
    uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (exp + 0x3F0)<<52) + ((uint64_t) frac<<42)));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline void softfloat_f16UIToCommonNaN( uint32_t uiA, SF_THREAD struct commonNaN *zPtr )
{
    if ( ((((uiA) & 0x7E00) == 0x7C00) && ((uiA) & 0x01FF)) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    zPtr->sign = uiA>>15;
    zPtr->v64 = (uint64_t) uiA<<54;
    zPtr->v0 = 0;
}
inline bool f64_le_quiet( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signA, signB;
    uA.f = a;
    uiA = uA.ui;
    uB.f = b;
    uiB = uB.ui;
    if ( (((~(uiA) & 0x7FF0000000000000UL) == 0) && ((uiA) & 0x000FFFFFFFFFFFFFUL)) || (((~(uiB) & 0x7FF0000000000000UL) == 0) && ((uiB) & 0x000FFFFFFFFFFFFFUL)) ) {
        if (
            ((((uiA) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiA) & 0x0007FFFFFFFFFFFFUL)) || ((((uiB) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiB) & 0x0007FFFFFFFFFFFFUL))
        ) {
            softfloat_raiseFlags( softfloat_flag_invalid );
        }
        return false;
    }
    signA = ((bool) ((uint64_t) (uiA)>>63));
    signB = ((bool) ((uint64_t) (uiB)>>63));
    return
        (signA != signB)
            ? signA || ! ((uiA | uiB) & 0x7FFFFFFFFFFFFFFFUL)
            : (uiA == uiB) || (signA ^ (uiA < uiB));
}
inline bool f64_lt_quiet( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signA, signB;
    uA.f = a;
    uiA = uA.ui;
    uB.f = b;
    uiB = uB.ui;
    if ( (((~(uiA) & 0x7FF0000000000000UL) == 0) && ((uiA) & 0x000FFFFFFFFFFFFFUL)) || (((~(uiB) & 0x7FF0000000000000UL) == 0) && ((uiB) & 0x000FFFFFFFFFFFFFUL)) ) {
        if (
            ((((uiA) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiA) & 0x0007FFFFFFFFFFFFUL)) || ((((uiB) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiB) & 0x0007FFFFFFFFFFFFUL))
        ) {
            softfloat_raiseFlags( softfloat_flag_invalid );
        }
        return false;
    }
    signA = ((bool) ((uint64_t) (uiA)>>63));
    signB = ((bool) ((uint64_t) (uiB)>>63));
    return
        (signA != signB)
            ? signA && ((uiA | uiB) & 0x7FFFFFFFFFFFFFFFUL)
            : (uiA != uiB) && (signA ^ (uiA < uiB));
}
inline bool f64_eq( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    union ui64_f64 uB;
    uint64_t uiB;
    uA.f = a;
    uiA = uA.ui;
    uB.f = b;
    uiB = uB.ui;
    if ( (((~(uiA) & 0x7FF0000000000000UL) == 0) && ((uiA) & 0x000FFFFFFFFFFFFFUL)) || (((~(uiB) & 0x7FF0000000000000UL) == 0) && ((uiB) & 0x000FFFFFFFFFFFFFUL)) ) {
        if (
            ((((uiA) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiA) & 0x0007FFFFFFFFFFFFUL)) || ((((uiB) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiB) & 0x0007FFFFFFFFFFFFUL))
        ) {
            softfloat_raiseFlags( softfloat_flag_invalid );
        }
        return false;
    }
    return (uiA == uiB) || ! ((uiA | uiB) & 0x7FFFFFFFFFFFFFFFUL);
}
inline float64_t f64_roundToInt( float64_t a, uint8_t roundingMode, bool exact )
{
    union ui64_f64 uA;
    uint64_t uiA;
    int32_t exp;
    uint64_t uiZ, lastBitMask, roundBitsMask;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    if ( exp <= 0x3FE ) {
        if ( !(uiA & 0x7FFFFFFFFFFFFFFFUL) ) return a;
        uiZ = uiA & ((uint64_t) (((uint64_t) (1)<<63) + ((uint64_t) (0)<<52) + (0)));
        switch ( roundingMode ) {
         case softfloat_round_near_even:
            if ( !((uiA) & 0x000FFFFFFFFFFFFFUL) ) break;
         case softfloat_round_near_maxMag:
            if ( exp == 0x3FE ) uiZ |= ((uint64_t) (((uint64_t) (0)<<63) + ((uint64_t) (0x3FF)<<52) + (0)));
            break;
         case softfloat_round_min:
            if ( uiZ ) uiZ = ((uint64_t) (((uint64_t) (1)<<63) + ((uint64_t) (0x3FF)<<52) + (0)));
            break;
         case softfloat_round_max:
            if ( !uiZ ) uiZ = ((uint64_t) (((uint64_t) (0)<<63) + ((uint64_t) (0x3FF)<<52) + (0)));
            break;
        }
        goto uiZ;
    }
    if ( 0x433 <= exp ) {
        if ( (exp == 0x7FF) && ((uiA) & 0x000FFFFFFFFFFFFFUL) ) {
            uiZ = softfloat_propagateNaNF64UI( uiA, 0 );
            goto uiZ;
        }
        return a;
    }
    uiZ = uiA;
    lastBitMask = (uint64_t) 1<<(0x433 - exp);
    roundBitsMask = lastBitMask - 1;
    if ( roundingMode == softfloat_round_near_maxMag ) {
        uiZ += lastBitMask>>1;
    } else if ( roundingMode == softfloat_round_near_even ) {
        uiZ += lastBitMask>>1;
        if ( !(uiZ & roundBitsMask) ) uiZ &= ~lastBitMask;
    } else if (
        roundingMode
            == (((bool) ((uint64_t) (uiZ)>>63)) ? softfloat_round_min : softfloat_round_max)
    ) {
        uiZ += roundBitsMask;
    }
    uiZ &= ~roundBitsMask;
    if ( uiZ != uiA ) {
    }
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint64_t
 softfloat_propagateNaNF64UI( uint64_t uiA, uint64_t uiB )
{
    bool isSigNaNA;
    isSigNaNA = ((((uiA) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiA) & 0x0007FFFFFFFFFFFFUL));
    if ( isSigNaNA || ((((uiB) & 0x7FF8000000000000UL) == 0x7FF0000000000000UL) && ((uiB) & 0x0007FFFFFFFFFFFFUL)) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
        if ( isSigNaNA ) return uiA | 0x0008000000000000UL;
    }
    return ((((~(uiA) & 0x7FF0000000000000UL) == 0) && ((uiA) & 0x000FFFFFFFFFFFFFUL)) ? uiA : uiB) | 0x0008000000000000UL;
}
inline float64_t f64_mulAdd( float64_t a, float64_t b, float64_t c )
{
    union ui64_f64 uA;
    uint64_t uiA;
    union ui64_f64 uB;
    uint64_t uiB;
    union ui64_f64 uC;
    uint64_t uiC;
    uA.f = a;
    uiA = uA.ui;
    uB.f = b;
    uiB = uB.ui;
    uC.f = c;
    uiC = uC.ui;
    return softfloat_mulAddF64( uiA, uiB, uiC, 0 );
}
inline float64_t f64_sqrt( float64_t a )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    int32_t expA;
    uint64_t sigA, uiZ;
    struct exp16_sig64 normExpSig;
    int32_t expZ;
    uint32_t sig32A, recipSqrt32, sig32Z;
    uint64_t rem;
    uint32_t q;
    uint64_t sigZ, shiftedSigZ;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    if ( expA == 0x7FF ) {
        if ( sigA ) {
            uiZ = softfloat_propagateNaNF64UI( uiA, 0 );
            goto uiZ;
        }
        if ( ! signA ) return a;
        goto invalid;
    }
    if ( signA ) {
        if ( ! (expA | sigA) ) return a;
        goto invalid;
    }
    if ( ! expA ) {
        if ( ! sigA ) return a;
        normExpSig = softfloat_normSubnormalF64Sig( sigA );
        expA = normExpSig.exp;
        sigA = normExpSig.sig;
    }
    expZ = ((expA - 0x3FF)>>1) + 0x3FE;
    expA &= 1;
    sigA |= 0x0010000000000000UL;
    sig32A = sigA>>21;
    recipSqrt32 = softfloat_approxRecipSqrt32_1( expA, sig32A );
    sig32Z = ((uint64_t) sig32A * recipSqrt32)>>32;
    if ( expA ) {
        sigA <<= 8;
        sig32Z >>= 1;
    } else {
        sigA <<= 9;
    }
    rem = sigA - (uint64_t) sig32Z * sig32Z;
    q = ((uint32_t) (rem>>2) * (uint64_t) recipSqrt32)>>32;
    sigZ = ((uint64_t) sig32Z<<32 | 1<<5) + ((uint64_t) q<<3);
    if ( (sigZ & 0x1FF) < 0x22 ) {
        sigZ &= ~(uint64_t) 0x3F;
        shiftedSigZ = sigZ>>6;
        rem = (sigA<<52) - shiftedSigZ * shiftedSigZ;
        if ( rem & 0x8000000000000000UL ) {
            --sigZ;
        } else {
            if ( rem ) sigZ |= 1;
        }
    }
    return softfloat_roundPackToF64( 0, expZ, sigZ );
 invalid:
    softfloat_raiseFlags( softfloat_flag_invalid );
    uiZ = 0xFFF8000000000000UL;
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline float64_t f64_rem( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    int32_t expA;
    uint64_t sigA;
    union ui64_f64 uB;
    uint64_t uiB;
    int32_t expB;
    uint64_t sigB;
    struct exp16_sig64 normExpSig;
    uint64_t rem;
    int32_t expDiff;
    uint32_t q, recip32;
    uint64_t q64;
    uint64_t altRem, meanRem;
    bool signRem;
    uint64_t uiZ;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    uB.f = b;
    uiB = uB.ui;
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    if ( expA == 0x7FF ) {
        if ( sigA || ((expB == 0x7FF) && sigB) ) goto propagateNaN;
        goto invalid;
    }
    if ( expB == 0x7FF ) {
        if ( sigB ) goto propagateNaN;
        return a;
    }
    if ( expA < expB - 1 ) return a;
    if ( ! expB ) {
        if ( ! sigB ) goto invalid;
        normExpSig = softfloat_normSubnormalF64Sig( sigB );
        expB = normExpSig.exp;
        sigB = normExpSig.sig;
    }
    if ( ! expA ) {
        if ( ! sigA ) return a;
        normExpSig = softfloat_normSubnormalF64Sig( sigA );
        expA = normExpSig.exp;
        sigA = normExpSig.sig;
    }
    rem = sigA | 0x0010000000000000UL;
    sigB |= 0x0010000000000000UL;
    expDiff = expA - expB;
    if ( expDiff < 1 ) {
        if ( expDiff < -1 ) return a;
        sigB <<= 9;
        if ( expDiff ) {
            rem <<= 8;
            q = 0;
        } else {
            rem <<= 9;
            q = (sigB <= rem);
            if ( q ) rem -= sigB;
        }
    } else {
        recip32 = softfloat_approxRecip32_1( sigB>>21 );
        rem <<= 9;
        expDiff -= 30;
        sigB <<= 9;
        for (;;) {
            q64 = (uint32_t) (rem>>32) * (uint64_t) recip32;
            if ( expDiff < 0 ) break;
            q = (q64 + 0x80000000)>>32;
            rem <<= 29;
            rem -= q * (uint64_t) sigB;
            if ( rem & 0x8000000000000000UL ) rem += sigB;
            expDiff -= 29;
        }
        q = (uint32_t) (q64>>32)>>(~expDiff & 31);
        rem = (rem<<(expDiff + 30)) - q * (uint64_t) sigB;
        if ( rem & 0x8000000000000000UL ) {
            altRem = rem + sigB;
            goto selectRem;
        }
    }
    do {
        altRem = rem;
        ++q;
        rem -= sigB;
    } while ( ! (rem & 0x8000000000000000UL) );
 selectRem:
    meanRem = rem + altRem;
    if (
        (meanRem & 0x8000000000000000UL) || (! meanRem && (q & 1))
    ) {
        rem = altRem;
    }
    signRem = signA;
    if ( rem & 0x8000000000000000UL ) {
        signRem = ! signRem;
        rem = -rem;
    }
    return softfloat_normRoundPackToF64( signRem, expB, rem );
 propagateNaN:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
    goto uiZ;
 invalid:
    softfloat_raiseFlags( softfloat_flag_invalid );
    uiZ = 0xFFF8000000000000UL;
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline float64_t f64_div( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    int32_t expA;
    uint64_t sigA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signB;
    int32_t expB;
    uint64_t sigB;
    bool signZ;
    struct exp16_sig64 normExpSig;
    int32_t expZ;
    uint32_t recip32, sig32Z, doubleTerm;
    uint64_t rem;
    uint32_t q;
    uint64_t sigZ;
    uint64_t uiZ;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    uB.f = b;
    uiB = uB.ui;
    signB = ((bool) ((uint64_t) (uiB)>>63));
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    signZ = signA ^ signB;
    if ( expA == 0x7FF ) {
        if ( sigA ) goto propagateNaN;
        if ( expB == 0x7FF ) {
            if ( sigB ) goto propagateNaN;
            goto invalid;
        }
        goto infinity;
    }
    if ( expB == 0x7FF ) {
        if ( sigB ) goto propagateNaN;
        goto zero;
    }
    if ( ! expB ) {
        if ( ! sigB ) {
            if ( ! (expA | sigA) ) goto invalid;
            softfloat_raiseFlags( softfloat_flag_infinite );
            goto infinity;
        }
        normExpSig = softfloat_normSubnormalF64Sig( sigB );
        expB = normExpSig.exp;
        sigB = normExpSig.sig;
    }
    if ( ! expA ) {
        if ( ! sigA ) goto zero;
        normExpSig = softfloat_normSubnormalF64Sig( sigA );
        expA = normExpSig.exp;
        sigA = normExpSig.sig;
    }
    expZ = expA - expB + 0x3FE;
    sigA |= 0x0010000000000000UL;
    sigB |= 0x0010000000000000UL;
    if ( sigA < sigB ) {
        --expZ;
        sigA <<= 11;
    } else {
        sigA <<= 10;
    }
    sigB <<= 11;
    recip32 = softfloat_approxRecip32_1( sigB>>32 ) - 2;
    sig32Z = ((uint32_t) (sigA>>32) * (uint64_t) recip32)>>32;
    doubleTerm = sig32Z<<1;
    rem =
        ((sigA - (uint64_t) doubleTerm * (uint32_t) (sigB>>32))<<28)
            - (uint64_t) doubleTerm * ((uint32_t) sigB>>4);
    q = (((uint32_t) (rem>>32) * (uint64_t) recip32)>>32) + 4;
    sigZ = ((uint64_t) sig32Z<<32) + ((uint64_t) q<<4);
    if ( (sigZ & 0x1FF) < 4<<4 ) {
        q &= ~7;
        sigZ &= ~(uint64_t) 0x7F;
        doubleTerm = q<<1;
        rem =
            ((rem - (uint64_t) doubleTerm * (uint32_t) (sigB>>32))<<28)
                - (uint64_t) doubleTerm * ((uint32_t) sigB>>4);
        if ( rem & 0x8000000000000000UL ) {
            sigZ -= 1<<7;
        } else {
            if ( rem ) sigZ |= 1;
        }
    }
    return softfloat_roundPackToF64( signZ, expZ, sigZ );
 propagateNaN:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
    goto uiZ;
 invalid:
    softfloat_raiseFlags( softfloat_flag_invalid );
    uiZ = 0xFFF8000000000000UL;
    goto uiZ;
 infinity:
    uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
    goto uiZ;
 zero:
    uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0)<<52) + (0)));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline float64_t f64_mul( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    int32_t expA;
    uint64_t sigA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signB;
    int32_t expB;
    uint64_t sigB;
    bool signZ;
    uint64_t magBits;
    struct exp16_sig64 normExpSig;
    int32_t expZ;
    struct uint128 sig128Z;
    uint64_t sigZ, uiZ;
    union ui64_f64 uZ;
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    uB.f = b;
    uiB = uB.ui;
    signB = ((bool) ((uint64_t) (uiB)>>63));
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    signZ = signA ^ signB;
    if ( expA == 0x7FF ) {
        if ( sigA || ((expB == 0x7FF) && sigB) ) goto propagateNaN;
        magBits = expB | sigB;
        goto infArg;
    }
    if ( expB == 0x7FF ) {
        if ( sigB ) goto propagateNaN;
        magBits = expA | sigA;
        goto infArg;
    }
    if ( ! expA ) {
        if ( ! sigA ) goto zero;
        normExpSig = softfloat_normSubnormalF64Sig( sigA );
        expA = normExpSig.exp;
        sigA = normExpSig.sig;
    }
    if ( ! expB ) {
        if ( ! sigB ) goto zero;
        normExpSig = softfloat_normSubnormalF64Sig( sigB );
        expB = normExpSig.exp;
        sigB = normExpSig.sig;
    }
    expZ = expA + expB - 0x3FF;
    sigA = (sigA | 0x0010000000000000UL)<<10;
    sigB = (sigB | 0x0010000000000000UL)<<11;
    sig128Z = softfloat_mul64To128( sigA, sigB );
    sigZ = sig128Z.v64 | (sig128Z.v0 != 0);
    if ( sigZ < 0x4000000000000000UL ) {
        --expZ;
        sigZ <<= 1;
    }
    return softfloat_roundPackToF64( signZ, expZ, sigZ );
 propagateNaN:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
    goto uiZ;
 infArg:
    if ( ! magBits ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
        uiZ = 0xFFF8000000000000UL;
    } else {
        uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
    }
    goto uiZ;
 zero:
    uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0)<<52) + (0)));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline float64_t f64_sub( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signB;
    float64_t (*magsFuncPtr)( uint64_t, uint64_t, bool );
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    uB.f = b;
    uiB = uB.ui;
    signB = ((bool) ((uint64_t) (uiB)>>63));
    if ( signA == signB ) {
        return softfloat_subMagsF64( uiA, uiB, signA );
    } else {
        return softfloat_addMagsF64( uiA, uiB, signA );
    }
}
inline float64_t f64_add( float64_t a, float64_t b )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool signA;
    union ui64_f64 uB;
    uint64_t uiB;
    bool signB;
    float64_t (*magsFuncPtr)( uint64_t, uint64_t, bool );
    uA.f = a;
    uiA = uA.ui;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    uB.f = b;
    uiB = uB.ui;
    signB = ((bool) ((uint64_t) (uiB)>>63));
    if ( signA == signB ) {
        return softfloat_addMagsF64( uiA, uiB, signA );
    } else {
        return softfloat_subMagsF64( uiA, uiB, signA );
    }
}
inline float64_t ui64_to_f64( uint64_t a )
{
    union ui64_f64 uZ;
    if ( ! a ) {
        uZ.ui = 0;
        return uZ.f;
    }
    if ( a & 0x8000000000000000UL ) {
        return
            softfloat_roundPackToF64(
                0, 0x43D, softfloat_shortShiftRightJam64( a, 1 ) );
    } else {
        return softfloat_normRoundPackToF64( 0, 0x43C, a );
    }
}
inline float64_t
 softfloat_normRoundPackToF64( bool sign, int32_t exp, uint64_t sig )
{
    int8_t shiftDist;
    union ui64_f64 uZ;
    shiftDist = softfloat_countLeadingZeros64( sig ) - 1;
    exp -= shiftDist;
    if ( (10 <= shiftDist) && ((unsigned int) exp < 0x7FD) ) {
        uZ.ui = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (sig ? exp : 0)<<52) + (sig<<(shiftDist - 10))));
        return uZ.f;
    } else {
        return softfloat_roundPackToF64( sign, exp, sig<<shiftDist );
    }
}
inline float64_t
 softfloat_roundPackToF64( bool sign, int32_t exp, uint64_t sig )
{
    uint8_t roundingMode;
    bool roundNearEven;
    uint32_t roundIncrement, roundBits;
    bool isTiny;
    uint64_t uiZ;
    union ui64_f64 uZ;
    roundingMode = softfloat_round_near_even;
    roundNearEven = (roundingMode == softfloat_round_near_even);
    roundIncrement = 0x200;
    if ( ! roundNearEven && (roundingMode != softfloat_round_near_maxMag) ) {
        roundIncrement =
            (roundingMode
                 == (sign ? softfloat_round_min : softfloat_round_max))
                ? 0x3FF
                : 0;
    }
    roundBits = sig & 0x3FF;
    if ( 0x7FD <= (uint16_t) exp ) {
        if ( exp < 0 ) {
            isTiny =
                (softfloat_tininess_afterRounding == softfloat_tininess_beforeRounding)
                    || (exp < -1)
                    || (sig + roundIncrement < 0x8000000000000000UL);
            sig = softfloat_shiftRightJam64( sig, -exp );
            exp = 0;
            roundBits = sig & 0x3FF;
            if ( isTiny && roundBits ) {
                softfloat_raiseFlags( softfloat_flag_underflow );
            }
        } else if (
            (0x7FD < exp)
                || (0x8000000000000000UL <= sig + roundIncrement)
        ) {
            softfloat_raiseFlags(
                softfloat_flag_overflow | softfloat_flag_inexact );
            uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (0x7FF)<<52) + (0))) - ! roundIncrement;
            goto uiZ;
        }
    }
    sig = (sig + roundIncrement)>>10;
    if ( roundBits ) {
    }
    sig &= ~(uint64_t) (! (roundBits ^ 0x200) & roundNearEven);
    if ( ! sig ) exp = 0;
 packReturn:
    uiZ = ((uint64_t) (((uint64_t) (sign)<<63) + ((uint64_t) (exp)<<52) + (sig)));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint64_t softfloat_shiftRightJam64( uint64_t a, uint32_t dist )
{
    return
        (dist < 63) ? a>>dist | ((uint64_t) (a<<(-dist & 63)) != 0) : (a != 0);
}
inline uint8_t softfloat_countLeadingZeros64( uint64_t a )
{
    uint8_t count;
    uint32_t a32;
    count = 0;
    a32 = a>>32;
    if ( ! a32 ) {
        count = 32;
        a32 = a;
    }
    if ( a32 < 0x10000 ) {
        count += 16;
        a32 <<= 16;
    }
    if ( a32 < 0x1000000 ) {
        count += 8;
        a32 <<= 8;
    }
    count += softfloat_countLeadingZeros8[a32>>24];
    return count;
}
inline uint64_t softfloat_shortShiftRightJam64( uint64_t a, uint8_t dist )
{
    return a>>dist | ((a & (((uint64_t) 1<<dist) - 1)) != 0);
}
inline float64_t i64_to_f64( int64_t a )
{
    bool sign;
    union ui64_f64 uZ;
    uint64_t absA;
    sign = (a < 0);
    if ( ! (a & 0x7FFFFFFFFFFFFFFFUL) ) {
        uZ.ui = sign ? ((uint64_t) (((uint64_t) (1)<<63) + ((uint64_t) (0x43E)<<52) + (0))) : 0;
        return uZ.f;
    }
    absA = sign ? -(uint64_t) a : (uint64_t) a;
    return softfloat_normRoundPackToF64( sign, 0x43C, absA );
}
inline float64_t ui32_to_f64( uint32_t a )
{
    uint64_t uiZ;
    int8_t shiftDist;
    union ui64_f64 uZ;
    if ( ! a ) {
        uiZ = 0;
    } else {
        shiftDist = softfloat_countLeadingZeros32( a ) + 21;
        uiZ =
            ((uint64_t) (((uint64_t) (0)<<63) + ((uint64_t) (0x432 - shiftDist)<<52) + ((uint64_t) a<<shiftDist)));
    }
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint8_t softfloat_countLeadingZeros32( uint32_t a )
{
    uint8_t count;
    count = 0;
    if ( a < 0x10000 ) {
        count = 16;
        a <<= 16;
    }
    if ( a < 0x1000000 ) {
        count += 8;
        a <<= 8;
    }
    count += softfloat_countLeadingZeros8[a>>24];
    return count;
}
inline uint64_t f64_to_ui64_r_minMag( float64_t a, bool exact )
{
    union ui64_f64 uA;
    uint64_t uiA;
    int32_t exp;
    uint64_t sig;
    int32_t shiftDist;
    bool sign;
    uint64_t z;
    uA.f = a;
    uiA = uA.ui;
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    sig = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    shiftDist = 0x433 - exp;
    if ( 53 <= shiftDist ) {
        if ( exact && (exp | sig) ) {
        }
        return 0;
    }
    sign = ((bool) ((uint64_t) (uiA)>>63));
    if ( sign ) goto invalid;
    if ( shiftDist <= 0 ) {
        if ( shiftDist < -11 ) goto invalid;
        z = (sig | 0x0010000000000000UL)<<-shiftDist;
    } else {
        sig |= 0x0010000000000000UL;
        z = sig>>shiftDist;
        if ( exact && (uint64_t) (sig<<(-shiftDist & 63)) ) {
        }
    }
    return z;
 invalid:
    softfloat_raiseFlags( softfloat_flag_invalid );
    return
        (exp == 0x7FF) && sig ? 0xFFFFFFFFFFFFFFFFUL
            : sign ? 0xFFFFFFFFFFFFFFFFUL : 0xFFFFFFFFFFFFFFFFUL;
}
inline int64_t f64_to_i64_r_minMag( float64_t a, bool exact )
{
    union ui64_f64 uA;
    uint64_t uiA;
    bool sign;
    int32_t exp;
    uint64_t sig;
    int32_t shiftDist;
    int64_t absZ;
    uA.f = a;
    uiA = uA.ui;
    sign = ((bool) ((uint64_t) (uiA)>>63));
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    sig = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    shiftDist = 0x433 - exp;
    if ( shiftDist <= 0 ) {
        if ( shiftDist < -10 ) {
            if ( uiA == ((uint64_t) (((uint64_t) (1)<<63) + ((uint64_t) (0x43E)<<52) + (0))) ) {
                return -0x7FFFFFFFFFFFFFFFL - 1;
            }
            softfloat_raiseFlags( softfloat_flag_invalid );
            return
                (exp == 0x7FF) && sig ? (-0x7FFFFFFFFFFFFFFFL - 1)
                    : sign ? (-0x7FFFFFFFFFFFFFFFL - 1) : (-0x7FFFFFFFFFFFFFFFL - 1);
        }
        sig |= 0x0010000000000000UL;
        absZ = sig<<-shiftDist;
    } else {
        if ( 53 <= shiftDist ) {
            if ( exact && (exp | sig) ) {
            }
            return 0;
        }
        sig |= 0x0010000000000000UL;
        absZ = sig>>shiftDist;
        if ( exact && (absZ<<shiftDist != sig) ) {
        }
    }
    return sign ? -absZ : absZ;
}
inline float64_t
 softfloat_subMagsF64( uint64_t uiA, uint64_t uiB, bool signZ )
{
    int32_t expA;
    uint64_t sigA;
    int32_t expB;
    uint64_t sigB;
    int32_t expDiff;
    uint64_t uiZ;
    int64_t sigDiff;
    int8_t shiftDist;
    int32_t expZ;
    uint64_t sigZ;
    union ui64_f64 uZ;
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    expDiff = expA - expB;
    if ( ! expDiff ) {
        if ( expA == 0x7FF ) {
            if ( sigA | sigB ) goto propagateNaN;
            softfloat_raiseFlags( softfloat_flag_invalid );
            uiZ = 0xFFF8000000000000UL;
            goto uiZ;
        }
        sigDiff = sigA - sigB;
        if ( ! sigDiff ) {
            uiZ =
                ((uint64_t) (((uint64_t) ((softfloat_round_near_even == softfloat_round_min))<<63) + ((uint64_t) (0)<<52) + (0)));
            goto uiZ;
        }
        if ( expA ) --expA;
        if ( sigDiff < 0 ) {
            signZ = ! signZ;
            sigDiff = -sigDiff;
        }
        shiftDist = softfloat_countLeadingZeros64( sigDiff ) - 11;
        expZ = expA - shiftDist;
        if ( expZ < 0 ) {
            shiftDist = expA;
            expZ = 0;
        }
        uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (expZ)<<52) + (sigDiff<<shiftDist)));
        goto uiZ;
    } else {
        sigA <<= 10;
        sigB <<= 10;
        if ( expDiff < 0 ) {
            signZ = ! signZ;
            if ( expB == 0x7FF ) {
                if ( sigB ) goto propagateNaN;
                uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
                goto uiZ;
            }
            sigA += expA ? 0x4000000000000000UL : sigA;
            sigA = softfloat_shiftRightJam64( sigA, -expDiff );
            sigB |= 0x4000000000000000UL;
            expZ = expB;
            sigZ = sigB - sigA;
        } else {
            if ( expA == 0x7FF ) {
                if ( sigA ) goto propagateNaN;
                uiZ = uiA;
                goto uiZ;
            }
            sigB += expB ? 0x4000000000000000UL : sigB;
            sigB = softfloat_shiftRightJam64( sigB, expDiff );
            sigA |= 0x4000000000000000UL;
            expZ = expA;
            sigZ = sigA - sigB;
        }
        return softfloat_normRoundPackToF64( signZ, expZ - 1, sigZ );
    }
 propagateNaN:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline float64_t
 softfloat_addMagsF64( uint64_t uiA, uint64_t uiB, bool signZ )
{
    int32_t expA;
    uint64_t sigA;
    int32_t expB;
    uint64_t sigB;
    int32_t expDiff;
    uint64_t uiZ;
    int32_t expZ;
    uint64_t sigZ;
    union ui64_f64 uZ;
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    expDiff = expA - expB;
    if ( ! expDiff ) {
        if ( ! expA ) {
            uiZ = uiA + sigB;
            goto uiZ;
        }
        if ( expA == 0x7FF ) {
            if ( sigA | sigB ) goto propagateNaN;
            uiZ = uiA;
            goto uiZ;
        }
        expZ = expA;
        sigZ = 0x0020000000000000UL + sigA + sigB;
        sigZ <<= 9;
    } else {
        sigA <<= 9;
        sigB <<= 9;
        if ( expDiff < 0 ) {
            if ( expB == 0x7FF ) {
                if ( sigB ) goto propagateNaN;
                uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
                goto uiZ;
            }
            expZ = expB;
            if ( expA ) {
                sigA += 0x2000000000000000UL;
            } else {
                sigA <<= 1;
            }
            sigA = softfloat_shiftRightJam64( sigA, -expDiff );
        } else {
            if ( expA == 0x7FF ) {
                if ( sigA ) goto propagateNaN;
                uiZ = uiA;
                goto uiZ;
            }
            expZ = expA;
            if ( expB ) {
                sigB += 0x2000000000000000UL;
            } else {
                sigB <<= 1;
            }
            sigB = softfloat_shiftRightJam64( sigB, expDiff );
        }
        sigZ = 0x2000000000000000UL + sigA + sigB;
        if ( sigZ < 0x4000000000000000UL ) {
            --expZ;
            sigZ <<= 1;
        }
    }
    return softfloat_roundPackToF64( signZ, expZ, sigZ );
 propagateNaN:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline struct uint128 softfloat_mul64To128( uint64_t a, uint64_t b )
{
    uint32_t a32, a0, b32, b0;
    struct uint128 z;
    uint64_t mid1, mid;
    a32 = a>>32;
    a0 = a;
    b32 = b>>32;
    b0 = b;
    z.v0 = (uint64_t) a0 * b0;
    mid1 = (uint64_t) a32 * b0;
    mid = mid1 + (uint64_t) a0 * b32;
    z.v64 = (uint64_t) a32 * b32;
    z.v64 += (uint64_t) (mid < mid1)<<32 | mid>>32;
    mid <<= 32;
    z.v0 += mid;
    z.v64 += (z.v0 < mid);
    return z;
}
inline struct exp16_sig64 softfloat_normSubnormalF64Sig( uint64_t sig )
{
    int8_t shiftDist;
    struct exp16_sig64 z;
    shiftDist = softfloat_countLeadingZeros64( sig ) - 11;
    z.exp = 1 - shiftDist;
    z.sig = sig<<shiftDist;
    return z;
}
extern SF_CONST uint16_t softfloat_approxRecip_1k0s[16];
extern SF_CONST uint16_t softfloat_approxRecip_1k1s[16];
inline uint32_t softfloat_approxRecip32_1( uint32_t a )
{
    int index;
    uint16_t eps, r0;
    uint32_t sigma0;
    uint32_t r;
    uint32_t sqrSigma0;
    index = a>>27 & 0xF;
    eps = (uint16_t) (a>>11);
    r0 = softfloat_approxRecip_1k0s[index]
             - ((softfloat_approxRecip_1k1s[index] * (uint32_t) eps)>>20);
    sigma0 = ~(uint32_t) ((r0 * (uint64_t) a)>>7);
    r = ((uint32_t) r0<<16) + ((r0 * (uint64_t) sigma0)>>24);
    sqrSigma0 = ((uint64_t) sigma0 * sigma0)>>32;
    r += ((uint32_t) r * (uint64_t) sqrSigma0)>>48;
    return r;
}
extern SF_CONST uint16_t softfloat_approxRecipSqrt_1k0s[];
extern SF_CONST uint16_t softfloat_approxRecipSqrt_1k1s[];
inline uint32_t softfloat_approxRecipSqrt32_1( unsigned int oddExpA, uint32_t a )
{
    int index;
    uint16_t eps, r0;
    uint32_t ESqrR0;
    uint32_t sigma0;
    uint32_t r;
    uint32_t sqrSigma0;
    index = (a>>27 & 0xE) + oddExpA;
    eps = (uint16_t) (a>>12);
    r0 = softfloat_approxRecipSqrt_1k0s[index]
             - ((softfloat_approxRecipSqrt_1k1s[index] * (uint32_t) eps)
                    >>20);
    ESqrR0 = (uint32_t) r0 * r0;
    if ( ! oddExpA ) ESqrR0 <<= 1;
    sigma0 = ~(uint32_t) (((uint32_t) ESqrR0 * (uint64_t) a)>>23);
    r = ((uint32_t) r0<<16) + ((r0 * (uint64_t) sigma0)>>25);
    sqrSigma0 = ((uint64_t) sigma0 * sigma0)>>32;
    r += ((uint32_t) ((r>>1) + (r>>3) - ((uint32_t) r0<<14))
              * (uint64_t) sqrSigma0)
             >>48;
    if ( ! (r & 0x80000000) ) r = 0x80000000;
    return r;
}
inline float64_t
 softfloat_mulAddF64(
     uint64_t uiA, uint64_t uiB, uint64_t uiC, uint8_t op )
{
    bool signA;
    int32_t expA;
    uint64_t sigA;
    bool signB;
    int32_t expB;
    uint64_t sigB;
    bool signC;
    int32_t expC;
    uint64_t sigC;
    bool signZ;
    uint64_t magBits, uiZ;
    struct exp16_sig64 normExpSig;
    int32_t expZ;
    struct uint128 sig128Z;
    uint64_t sigZ;
    int32_t expDiff;
    struct uint128 sig128C;
    int8_t shiftDist;
    union ui64_f64 uZ;
    signA = ((bool) ((uint64_t) (uiA)>>63));
    expA = ((int32_t) ((uiA)>>52) & 0x7FF);
    sigA = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    signB = ((bool) ((uint64_t) (uiB)>>63));
    expB = ((int32_t) ((uiB)>>52) & 0x7FF);
    sigB = ((uiB) & 0x000FFFFFFFFFFFFFUL);
    signC = ((bool) ((uint64_t) (uiC)>>63)) ^ (op == softfloat_mulAdd_subC);
    expC = ((int32_t) ((uiC)>>52) & 0x7FF);
    sigC = ((uiC) & 0x000FFFFFFFFFFFFFUL);
    signZ = signA ^ signB ^ (op == softfloat_mulAdd_subProd);
    if ( expA == 0x7FF ) {
        if ( sigA || ((expB == 0x7FF) && sigB) ) goto propagateNaN_ABC;
        magBits = expB | sigB;
        goto infProdArg;
    }
    if ( expB == 0x7FF ) {
        if ( sigB ) goto propagateNaN_ABC;
        magBits = expA | sigA;
        goto infProdArg;
    }
    if ( expC == 0x7FF ) {
        if ( sigC ) {
            uiZ = 0;
            goto propagateNaN_ZC;
        }
        uiZ = uiC;
        goto uiZ;
    }
    if ( ! expA ) {
        if ( ! sigA ) goto zeroProd;
        normExpSig = softfloat_normSubnormalF64Sig( sigA );
        expA = normExpSig.exp;
        sigA = normExpSig.sig;
    }
    if ( ! expB ) {
        if ( ! sigB ) goto zeroProd;
        normExpSig = softfloat_normSubnormalF64Sig( sigB );
        expB = normExpSig.exp;
        sigB = normExpSig.sig;
    }
    expZ = expA + expB - 0x3FE;
    sigA = (sigA | 0x0010000000000000UL)<<10;
    sigB = (sigB | 0x0010000000000000UL)<<10;
    sig128Z = softfloat_mul64To128( sigA, sigB );
    if ( sig128Z.v64 < 0x2000000000000000UL ) {
        --expZ;
        sig128Z =
            softfloat_add128(
                sig128Z.v64, sig128Z.v0, sig128Z.v64, sig128Z.v0 );
    }
    if ( ! expC ) {
        if ( ! sigC ) {
            --expZ;
            sigZ = sig128Z.v64<<1 | (sig128Z.v0 != 0);
            goto roundPack;
        }
        normExpSig = softfloat_normSubnormalF64Sig( sigC );
        expC = normExpSig.exp;
        sigC = normExpSig.sig;
    }
    sigC = (sigC | 0x0010000000000000UL)<<9;
    expDiff = expZ - expC;
    if ( expDiff < 0 ) {
        expZ = expC;
        if ( (signZ == signC) || (expDiff < -1) ) {
            sig128Z.v64 = softfloat_shiftRightJam64( sig128Z.v64, -expDiff );
        } else {
            sig128Z =
                softfloat_shortShiftRightJam128( sig128Z.v64, sig128Z.v0, 1 );
        }
    } else if ( expDiff ) {
        sig128C = softfloat_shiftRightJam128( sigC, 0, expDiff );
    }
    if ( signZ == signC ) {
        if ( expDiff <= 0 ) {
            sigZ = (sigC + sig128Z.v64) | (sig128Z.v0 != 0);
        } else {
            sig128Z =
                softfloat_add128(
                    sig128Z.v64, sig128Z.v0, sig128C.v64, sig128C.v0 );
            sigZ = sig128Z.v64 | (sig128Z.v0 != 0);
        }
        if ( sigZ < 0x4000000000000000UL ) {
            --expZ;
            sigZ <<= 1;
        }
    } else {
        if ( expDiff < 0 ) {
            signZ = signC;
            sig128Z = softfloat_sub128( sigC, 0, sig128Z.v64, sig128Z.v0 );
        } else if ( ! expDiff ) {
            sig128Z.v64 = sig128Z.v64 - sigC;
            if ( ! (sig128Z.v64 | sig128Z.v0) ) goto completeCancellation;
            if ( sig128Z.v64 & 0x8000000000000000UL ) {
                signZ = ! signZ;
                sig128Z = softfloat_sub128( 0, 0, sig128Z.v64, sig128Z.v0 );
            }
        } else {
            sig128Z =
                softfloat_sub128(
                    sig128Z.v64, sig128Z.v0, sig128C.v64, sig128C.v0 );
        }
        if ( ! sig128Z.v64 ) {
            expZ -= 64;
            sig128Z.v64 = sig128Z.v0;
            sig128Z.v0 = 0;
        }
        shiftDist = softfloat_countLeadingZeros64( sig128Z.v64 ) - 1;
        expZ -= shiftDist;
        if ( shiftDist < 0 ) {
            sigZ = softfloat_shortShiftRightJam64( sig128Z.v64, -shiftDist );
        } else {
            sig128Z =
                softfloat_shortShiftLeft128(
                    sig128Z.v64, sig128Z.v0, shiftDist );
            sigZ = sig128Z.v64;
        }
        sigZ |= (sig128Z.v0 != 0);
    }
 roundPack:
    return softfloat_roundPackToF64( signZ, expZ, sigZ );
 propagateNaN_ABC:
    uiZ = softfloat_propagateNaNF64UI( uiA, uiB );
    goto propagateNaN_ZC;
 infProdArg:
    if ( magBits ) {
        uiZ = ((uint64_t) (((uint64_t) (signZ)<<63) + ((uint64_t) (0x7FF)<<52) + (0)));
        if ( expC != 0x7FF ) goto uiZ;
        if ( sigC ) goto propagateNaN_ZC;
        if ( signZ == signC ) goto uiZ;
    }
    softfloat_raiseFlags( softfloat_flag_invalid );
    uiZ = 0xFFF8000000000000UL;
 propagateNaN_ZC:
    uiZ = softfloat_propagateNaNF64UI( uiZ, uiC );
    goto uiZ;
 zeroProd:
    uiZ = uiC;
    if ( ! (expC | sigC) && (signZ != signC) ) {
 completeCancellation:
        uiZ =
            ((uint64_t) (((uint64_t) ((softfloat_round_near_even == softfloat_round_min))<<63) + ((uint64_t) (0)<<52) + (0)));
    }
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline struct uint128
 softfloat_shortShiftLeft128( uint64_t a64, uint64_t a0, uint8_t dist )
{
    struct uint128 z;
    z.v64 = a64<<dist | a0>>(-dist & 63);
    z.v0 = a0<<dist;
    return z;
}
inline struct uint128
 softfloat_sub128( uint64_t a64, uint64_t a0, uint64_t b64, uint64_t b0 )
{
    struct uint128 z;
    z.v0 = a0 - b0;
    z.v64 = a64 - b64 - (a0 < b0);
    return z;
}
inline struct uint128
 softfloat_add128( uint64_t a64, uint64_t a0, uint64_t b64, uint64_t b0 )
{
    struct uint128 z;
    z.v0 = a0 + b0;
    z.v64 = a64 + b64 + (z.v0 < a0);
    return z;
}
inline struct uint128
 softfloat_shiftRightJam128( uint64_t a64, uint64_t a0, uint32_t dist )
{
    uint8_t u8NegDist;
    struct uint128 z;
    if ( dist < 64 ) {
        u8NegDist = -dist;
        z.v64 = a64>>dist;
        z.v0 =
            a64<<(u8NegDist & 63) | a0>>dist
                | ((uint64_t) (a0<<(u8NegDist & 63)) != 0);
    } else {
        z.v64 = 0;
        z.v0 =
            (dist < 127)
                ? a64>>(dist & 63)
                      | (((a64 & (((uint64_t) 1<<(dist & 63)) - 1)) | a0)
                             != 0)
                : ((a64 | a0) != 0);
    }
    return z;
}
inline struct uint128
 softfloat_shortShiftRightJam128(
     uint64_t a64, uint64_t a0, uint8_t dist )
{
    uint8_t uNegDist;
    struct uint128 z;
    uNegDist = -dist;
    z.v64 = a64>>dist;
    z.v0 =
        a64<<(uNegDist & 63) | a0>>dist
            | ((uint64_t) (a0<<(uNegDist & 63)) != 0);
    return z;
}
inline struct exp8_sig16 softfloat_normSubnormalF16Sig( uint32_t sig )
{
    int8_t shiftDist;
    struct exp8_sig16 z;
    shiftDist = softfloat_countLeadingZeros16( sig ) - 5;
    z.exp = 1 - shiftDist;
    z.sig = sig<<shiftDist;
    return z;
}
inline uint8_t softfloat_countLeadingZeros16( uint16_t a )
{
    uint8_t count;
    count = 8;
    if ( 0x100 <= a ) {
        count = 0;
        a >>= 8;
    }
    count += softfloat_countLeadingZeros8[a];
    return count;
}
inline struct exp16_sig32 softfloat_normSubnormalF32Sig( uint32_t sig )
{
    int8_t shiftDist;
    struct exp16_sig32 z;
    shiftDist = softfloat_countLeadingZeros32( sig ) - 8;
    z.exp = 1 - shiftDist;
    z.sig = sig<<shiftDist;
    return z;
}
inline float16_t
 softfloat_roundPackToF16( bool sign, int32_t exp, uint32_t sig )
{
    uint8_t roundingMode;
    bool roundNearEven;
    uint8_t roundIncrement, roundBits;
    bool isTiny;
    uint32_t uiZ;
    union ui16_f16 uZ;
    roundingMode = softfloat_round_near_even;
    roundNearEven = (roundingMode == softfloat_round_near_even);
    roundIncrement = 0x8;
    if ( ! roundNearEven && (roundingMode != softfloat_round_near_maxMag) ) {
        roundIncrement =
            (roundingMode
                 == (sign ? softfloat_round_min : softfloat_round_max))
                ? 0xF
                : 0;
    }
    roundBits = sig & 0xF;
    if ( 0x1D <= (unsigned int) exp ) {
        if ( exp < 0 ) {
            isTiny =
                (softfloat_tininess_afterRounding == softfloat_tininess_beforeRounding)
                    || (exp < -1) || (sig + roundIncrement < 0x8000);
            sig = softfloat_shiftRightJam32( sig, -exp );
            exp = 0;
            roundBits = sig & 0xF;
            if ( isTiny && roundBits ) {
                softfloat_raiseFlags( softfloat_flag_underflow );
            }
        } else if ( (0x1D < exp) || (0x8000 <= sig + roundIncrement) ) {
            softfloat_raiseFlags(
                softfloat_flag_overflow | softfloat_flag_inexact );
            uiZ = (((uint16_t) (sign)<<15) + ((uint16_t) (0x1F)<<10) + (0)) - ! roundIncrement;
            goto uiZ;
        }
    }
    sig = (sig + roundIncrement)>>4;
    if ( roundBits ) {
    }
    sig &= ~(uint32_t) (! (roundBits ^ 8) & roundNearEven);
    if ( ! sig ) exp = 0;
 packReturn:
    uiZ = (((uint16_t) (sign)<<15) + ((uint16_t) (exp)<<10) + (sig));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
inline uint32_t softfloat_shiftRightJam32( uint32_t a, uint32_t dist )
{
    return
        (dist < 31) ? a>>dist | ((uint32_t) (a<<(-dist & 31)) != 0) : (a != 0);
}
inline float32_t
 softfloat_roundPackToF32( bool sign, int32_t exp, uint32_t sig )
{
    uint8_t roundingMode;
    bool roundNearEven;
    uint8_t roundIncrement, roundBits;
    bool isTiny;
    uint32_t uiZ;
    union ui32_f32 uZ;
    roundingMode = softfloat_round_near_even;
    roundNearEven = (roundingMode == softfloat_round_near_even);
    roundIncrement = 0x40;
    if ( ! roundNearEven && (roundingMode != softfloat_round_near_maxMag) ) {
        roundIncrement =
            (roundingMode
                 == (sign ? softfloat_round_min : softfloat_round_max))
                ? 0x7F
                : 0;
    }
    roundBits = sig & 0x7F;
    if ( 0xFD <= (unsigned int) exp ) {
        if ( exp < 0 ) {
            isTiny =
                (softfloat_tininess_afterRounding == softfloat_tininess_beforeRounding)
                    || (exp < -1) || (sig + roundIncrement < 0x80000000);
            sig = softfloat_shiftRightJam32( sig, -exp );
            exp = 0;
            roundBits = sig & 0x7F;
            if ( isTiny && roundBits ) {
                softfloat_raiseFlags( softfloat_flag_underflow );
            }
        } else if ( (0xFD < exp) || (0x80000000 <= sig + roundIncrement) ) {
            softfloat_raiseFlags(
                softfloat_flag_overflow | softfloat_flag_inexact );
            uiZ = (((uint32_t) (sign)<<31) + ((uint32_t) (0xFF)<<23) + (0)) - ! roundIncrement;
            goto uiZ;
        }
    }
    sig = (sig + roundIncrement)>>7;
    if ( roundBits ) {
    }
    sig &= ~(uint32_t) (! (roundBits ^ 0x40) & roundNearEven);
    if ( ! sig ) exp = 0;
 packReturn:
    uiZ = (((uint32_t) (sign)<<31) + ((uint32_t) (exp)<<23) + (sig));
 uiZ:
    uZ.ui = uiZ;
    return uZ.f;
}
SF_CONST uint8_t softfloat_countLeadingZeros8[256] = {
    8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
SF_CONST uint16_t softfloat_approxRecip_1k0s[16] = {
    0xFFC4, 0xF0BE, 0xE363, 0xD76F, 0xCCAD, 0xC2F0, 0xBA16, 0xB201,
    0xAA97, 0xA3C6, 0x9D7A, 0x97A6, 0x923C, 0x8D32, 0x887E, 0x8417
};
SF_CONST uint16_t softfloat_approxRecip_1k1s[16] = {
    0xF0F1, 0xD62C, 0xBFA1, 0xAC77, 0x9C0A, 0x8DDB, 0x8185, 0x76BA,
    0x6D3B, 0x64D4, 0x5D5C, 0x56B1, 0x50B6, 0x4B55, 0x4679, 0x4211
};
SF_CONST uint16_t softfloat_approxRecipSqrt_1k0s[16] = {
    0xB4C9, 0xFFAB, 0xAA7D, 0xF11C, 0xA1C5, 0xE4C7, 0x9A43, 0xDA29,
    0x93B5, 0xD0E5, 0x8DED, 0xC8B7, 0x88C6, 0xC16D, 0x8424, 0xBAE1
};
SF_CONST uint16_t softfloat_approxRecipSqrt_1k1s[16] = {
    0xA5A5, 0xEA42, 0x8C21, 0xC62D, 0x788F, 0xAA7F, 0x6928, 0x94B6,
    0x5CC7, 0x8335, 0x52A6, 0x74E2, 0x4A3E, 0x68FE, 0x432B, 0x5EFD
};
inline uint32_t f64_to_ui32_r_minMag( float64_t a, bool exact )
{
    union ui64_f64 uA;
    uint64_t uiA;
    int32_t exp;
    uint64_t sig;
    int32_t shiftDist;
    bool sign;
    uint32_t z;
    uA.f = a;
    uiA = uA.ui;
    exp = ((int32_t) ((uiA)>>52) & 0x7FF);
    sig = ((uiA) & 0x000FFFFFFFFFFFFFUL);
    shiftDist = 0x433 - exp;
    if ( 53 <= shiftDist ) {
        if ( exact && (exp | sig) ) {
        }
        return 0;
    }
    sign = ((bool) ((uint64_t) (uiA)>>63));
    if ( sign || (shiftDist < 21) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
        return
            (exp == 0x7FF) && sig ? 0xFFFFFFFF
                : sign ? 0xFFFFFFFF : 0xFFFFFFFF;
    }
    sig |= 0x0010000000000000UL;
    z = sig>>shiftDist;
    if ( exact && ((uint64_t) z<<shiftDist != sig) ) {
    }
    return z;
}
}
#undef SF_THREAD
#undef SF_CONST


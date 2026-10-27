/*
    Copyright (C) 1995-2014, The AROS Development Team. All rights reserved.
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    mathffp.library for 68k callers. The bit layout, sign operations and
    floor/ceil approach are adapted from workbench/libs/mathffp (AROS
    Public License 1.1). Arithmetic and conversion are rewritten using
    unsigned, explicitly sized integers: the historical C implementations
    depend on signed char, overflowing signed arithmetic and invalid shifts.
    No AmigaOS implementation code is used.

    A normal FFP word contains a 24-bit explicit mantissa in bits 31..8,
    sign in bit 7 and an excess-64 exponent in bits 6..0. Its value is
    (-1)^sign * mantissa * 2^(exponent-88). Zero is the all-zero word.
    These routines never reinterpret the word as a native float.
*/

#include <stdint.h>

#include "kaffeine_thunks_mathffp.h"

#define FFP_SIGN UINT32_C(0x80)
#define FFP_MAX  UINT32_C(0xffffff7f)
#define FFP_ONE  UINT32_C(0x80000041)
#define CCR_V   UINT16_C(0x02)
#define CCR_Z   UINT16_C(0x04)
#define CCR_N   UINT16_C(0x08)
#define CCR_NZV (CCR_N | CCR_Z | CCR_V)

static void set_ccr(void *cpu, UWORD flags)
{
    Kaffeine_SetSR(cpu, (Kaffeine_GetSR(cpu) & ~(UWORD)CCR_NZV) | flags);
}

static IPTR return_ffp(void *cpu, uint32_t bits, int overflow)
{
    UWORD flags = overflow ? CCR_V : 0;

    /* Sign operations can expose a positive exponent-zero word. The native
       FFP routines classify it by its low byte, retaining its mantissa. */
    if ((bits & 255u) == 0)
        flags |= CCR_Z;
    else if (bits & FFP_SIGN)
        flags |= CCR_N;
    set_ccr(cpu, flags);
    return (IPTR)bits;
}

/* Return an already rounded mantissa, normalizing a rounding carry.
   Underflow is flushed to the unique zero; overflow saturates by sign.
   The reference can return exponent zero for a negative result; a positive
   exponent-zero result underflows. Input aliases remain unspecified. */
static IPTR pack_mantissa(void *cpu, uint32_t mantissa, int exponent,
                          uint32_t sign)
{
    if (mantissa == UINT32_C(0x1000000))
    {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent < (sign ? 0 : 1))
        return return_ffp(cpu, 0, 0);
    if (exponent > 127)
        return return_ffp(cpu, FFP_MAX | sign, 1);
    return return_ffp(cpu, (mantissa << 8) | (uint32_t)exponent | sign, 0);
}

/* Quantize magnitude * 2^scale to 24 bits, rounding nearest. SPFlt rounds
   halfway magnitudes upward; SPAdd/SPSub/SPMul round halfway downward.
   These distinct rules are observed in the supervisor's mathffp 46.1
   probes on AmigaOS 3.2.3 (26 October 2026).
   No host floating-point environment is used. */
static IPTR pack_integer(void *cpu, uint64_t magnitude, int scale,
                         uint32_t sign, int ties_up)
{
    uint64_t scan = magnitude;
    uint32_t mantissa;
    unsigned top = 0;

    if (magnitude == 0)
        return return_ffp(cpu, 0, 0);
    while (scan >>= 1)
        ++top;
    if (top > 23)
    {
        unsigned shift = top - 23; /* 1..40, never a full-width shift */

        mantissa = (uint32_t)(magnitude >> shift);
        {
            uint64_t discarded = magnitude & ((UINT64_C(1) << shift) - 1u);
            uint64_t halfway = UINT64_C(1) << (shift - 1);

            if (discarded > halfway || (ties_up && discarded == halfway))
                ++mantissa;
        }
    }
    else
        mantissa = (uint32_t)magnitude << (23 - top);
    return pack_mantissa(cpu, mantissa, (int)top + scale + 65, sign);
}

static IPTR add_ffp(void *cpu, uint32_t a, uint32_t b)
{
    int ea, eb, difference, scale;
    uint64_t ma, mb, magnitude;
    uint32_t sign;

    if (a == 0)
        return return_ffp(cpu, b, 0);
    if (b == 0)
        return return_ffp(cpu, a, 0);
    ea = (int)(a & 127u);
    eb = (int)(b & 127u);
    ma = a >> 8;
    mb = b >> 8;
    difference = ea - eb;
    /* mathffp 46.1 ignores the smaller operand at exponent gaps >=24,
       including 0.75 ULP and opposite-sign half ULP (reference probe 2).
       With a smaller gap the exact aligned sum fits in 48 bits. */
    if (difference >= 24)
        return return_ffp(cpu, a, 0);
    if (difference <= -24)
        return return_ffp(cpu, b, 0);
    if ((a ^ b) & FFP_SIGN)
    {
        /* Opposite signs round the smaller aligned magnitude before
           cancellation. In particular a gap-one half-bit rounds UP, and
           cannot be recovered by normalizing the difference afterward. */
        if (difference > 0)
        {
            mb = (mb + (UINT64_C(1) << (difference - 1))) >> difference;
            scale = ea - 88;
        }
        else if (difference < 0)
        {
            ma = (ma + (UINT64_C(1) << (-difference - 1))) >> -difference;
            scale = eb - 88;
        }
        else
            scale = ea - 88;
    }
    else if (difference >= 0)
    {
        ma <<= (unsigned)difference;
        scale = eb - 88;
    }
    else
    {
        mb <<= (unsigned)-difference;
        scale = ea - 88;
    }
    sign = a & FFP_SIGN;
    if (((a ^ b) & FFP_SIGN) == 0)
        magnitude = ma + mb;
    else if (ma >= mb)
        magnitude = ma - mb;
    else
    {
        magnitude = mb - ma;
        sign = b & FFP_SIGN;
    }
    return pack_integer(cpu, magnitude, scale, sign, 0);
}

/* -30: D0=FFP -> D0=signed integer, truncating toward zero. */
IPTR kaffeine_mathffp_SPFix(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t bits = (uint32_t)THUNK_D(0);
    uint32_t sign = bits & FFP_SIGN;
    int exponent = (int)(bits & 127u);
    uint64_t magnitude;
    uint32_t result;

    (void)ctx;
    if (bits == 0 || exponent <= 64)
    {
        set_ccr(cpu, CCR_Z);
        return 0;
    }
    if (exponent > 96)
        magnitude = UINT64_MAX;
    else if (exponent >= 88)
        magnitude = (uint64_t)(bits >> 8) << (unsigned)(exponent - 88);
    else
        magnitude = (bits >> 8) >> (unsigned)(88 - exponent);
    if (magnitude > (sign ? UINT64_C(0x80000000) : UINT64_C(0x7fffffff)))
    {
        /* The reference returns Z|V, not N|V, on negative overflow,
           including overflow within exponent 0x60. Exact INT32_MIN takes
           the normal path and returns N without V. */
        set_ccr(cpu, CCR_V | (sign ? CCR_Z : 0));
        return sign ? UINT32_C(0x80000000) : UINT32_C(0x7fffffff);
    }
    result = sign ? 0u - (uint32_t)magnitude : (uint32_t)magnitude;
    set_ccr(cpu, result == 0 ? CCR_Z : (sign ? CCR_N : 0));
    return (IPTR)result;
}

/* -36: D0=two's-complement integer -> D0=FFP. SPFlt clears V even when
   rounding an inexact integer (unlike the historical AROS C routine). */
IPTR kaffeine_mathffp_SPFlt(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t integer = (uint32_t)THUNK_D(0);
    uint32_t sign = (integer & UINT32_C(0x80000000)) ? FFP_SIGN : 0;
    uint32_t magnitude = sign ? 0u - integer : integer;

    (void)ctx;
    return pack_integer(cpu, magnitude, 0, sign, 1);
}

/* -42: return sign(D1-D0); relational CCR describes D0 compared with D1,
   as required by the FFP assembly interface's GT/GE/LT/LE branches. */
IPTR kaffeine_mathffp_SPCmp(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t left = (uint32_t)THUNK_D(1), right = (uint32_t)THUNK_D(0);
    uint32_t lm = ((left & 127u) << 24) | (left >> 8);
    uint32_t rm = ((right & 127u) << 24) | (right >> 8);
    int comparison;

    (void)ctx;
    if (left == right)
        comparison = 0;
    else if ((left ^ right) & FFP_SIGN)
        comparison = (left & FFP_SIGN) ? -1 : 1;
    else
    {
        comparison = lm > rm ? 1 : -1;
        if (left & FFP_SIGN)
            comparison = -comparison;
    }
    set_ccr(cpu, comparison == 0 ? CCR_Z : (comparison > 0 ? CCR_N : 0));
    return comparison < 0 ? (IPTR)UINT32_MAX : (IPTR)(unsigned)comparison;
}

/* -48: D1=FFP -> D0=-1/0/+1. D0 is not an input. */
IPTR kaffeine_mathffp_SPTst(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t bits = (uint32_t)THUNK_D(1);

    (void)ctx;
    set_ccr(cpu, (bits & 255u) == 0 ? CCR_Z : ((bits & FFP_SIGN) ? CCR_N : 0));
    return (bits & 255u) == 0 ? 0 : ((bits & FFP_SIGN) ? (IPTR)UINT32_MAX : 1);
}

/* -54, -60: sign operations preserve the unique zero. */
IPTR kaffeine_mathffp_SPAbs(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return return_ffp(cpu, (uint32_t)THUNK_D(0) & ~FFP_SIGN, 0);
}

IPTR kaffeine_mathffp_SPNeg(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t bits = (uint32_t)THUNK_D(0);

    (void)ctx;
    return return_ffp(cpu, bits == 0 ? 0 : bits ^ FFP_SIGN, 0);
}

/* -66, -72: binary FFP; subtraction is D0 minus D1. */
IPTR kaffeine_mathffp_SPAdd(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return add_ffp(cpu, (uint32_t)THUNK_D(0), (uint32_t)THUNK_D(1));
}

IPTR kaffeine_mathffp_SPSub(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t subtrahend = (uint32_t)THUNK_D(1);

    (void)ctx;
    return add_ffp(cpu, (uint32_t)THUNK_D(0),
                   subtrahend == 0 ? 0 : subtrahend ^ FFP_SIGN);
}

/* -78: keep the high 32 bits of the fixed-point fractional product before
   normalization and rounding, as observed in the guard-byte probes. This
   retains 31 significant bits when the product needs a left normalization
   shift, and 32 otherwise; the discarded tail is not a sticky bit. */
IPTR kaffeine_mathffp_SPMul(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t a = (uint32_t)THUNK_D(0), b = (uint32_t)THUNK_D(1);
    uint64_t product = (uint64_t)(a >> 8) * (b >> 8);

    (void)ctx;
    return pack_integer(cpu, product >> 16,
                         (int)(a & 127u) + (int)(b & 127u) - 160,
                         (a ^ b) & FFP_SIGN, 0);
}

/* -84: D0=dividend, D1=divisor. The integer quotient supplies the eight
   guard bits; there is no native floating-point division. */
IPTR kaffeine_mathffp_SPDiv(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t a = (uint32_t)THUNK_D(0), b = (uint32_t)THUNK_D(1);
    uint32_t ma = a >> 8, mb = b >> 8;
    uint32_t sign = (a ^ b) & FFP_SIGN;
    unsigned shift;
    uint64_t numerator;
    uint32_t quotient, mantissa;

    (void)ctx;
    if (mb == 0)
        return return_ffp(cpu, FFP_MAX | sign, 1);
    if ((a & 127u) == 0)
        return return_ffp(cpu, 0, 0);
    shift = ma >= mb ? 23 : 24;
    /* This bridge's deterministic approximation retains eight guard bits,
       discards the quotient tail, then rounds a guard-byte midpoint down.
       It matches rnd_84_023, unlike infinitely precise nearest rounding.
       Some native 46.1 guard-boundary cases differ by one mantissa ULP;
       the fixture records the bound and optional reference replay stays
       strict. This is not a claim of bit-exact native division. */
    numerator = (uint64_t)ma << (shift + 8);
    quotient = (uint32_t)(numerator / mb);
    mantissa = (quotient >> 8) + ((quotient & 255u) > 128u ? 1u : 0u);
    return pack_mantissa(cpu, mantissa,
                          (int)(a & 127u) - (int)(b & 127u) + 88 - (int)shift,
                          sign);
}

static IPTR round_integral(void *cpu, uint32_t bits, int ceiling)
{
    int exponent = (int)(bits & 127u);
    uint32_t sign = bits & FFP_SIGN;
    uint32_t mantissa = bits >> 8;
    unsigned fractional;
    uint32_t mask, truncated;

    /* AmigaOS 3.2.3 keeps the mantissa of an exponent-zero input but clears
       its sign, with Z set. This also applies to the negative lower bound. */
    if (exponent == 0)
        return return_ffp(cpu, bits & ~FFP_SIGN, 0);
    if (bits == 0 || exponent >= 88)
        return return_ffp(cpu, bits, 0);
    if (exponent <= 64)
        return return_ffp(cpu, (sign != 0) != (ceiling != 0) ? FFP_ONE | sign : 0, 0);
    fractional = (unsigned)(88 - exponent); /* 1..23 */
    mask = (UINT32_C(1) << fractional) - 1u;
    truncated = mantissa & ~mask;
    if ((mantissa & mask) != 0 && ((sign != 0) != (ceiling != 0)))
        truncated += UINT32_C(1) << fractional;
    return pack_mantissa(cpu, truncated, exponent, sign);
}

/* -90, -96: integral values remain in FFP, including magnitudes above
   the signed 32-bit integer range; no SPFix/SPFlt round trip. */
IPTR kaffeine_mathffp_SPFloor(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return round_integral(cpu, (uint32_t)THUNK_D(0), 0);
}

IPTR kaffeine_mathffp_SPCeil(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return round_integral(cpu, (uint32_t)THUNK_D(0), 1);
}

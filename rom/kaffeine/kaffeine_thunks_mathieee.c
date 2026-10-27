/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    mathieeedoubbas.library, mathieeedoubtrans.library and
    mathieeesingbas.library for 68k callers.
    On the m68k, an IEEE 754 double occupies a register pair with the high
    32 bits in D0 (or D2) and the low 32 bits in D1 (or D3), and returns in
    D0/D1. Native 64-bit AROS passes and returns a single C double, so these
    thunks unpack the register pairs, compute in host IEEE 754 double
    precision, update the 68k condition codes (N, Z and V) and write both
    result registers.
*/

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_thunks_mathieee.h"

/* 68k CCR flags modified by the IEEE math libraries: N=0x08, Z=0x04, V=0x02.
   X (0x10), C (0x01) and the upper system byte of SR are left unchanged,
   matching AROS's SetSR(flags, Zero_Bit | Negative_Bit | Overflow_Bit). */
#define M68K_CCR_V        0x0002U
#define M68K_CCR_Z        0x0004U
#define M68K_CCR_N        0x0008U
#define M68K_CCR_NZV_MASK (M68K_CCR_N | M68K_CCR_Z | M68K_CCR_V)

static double bits_to_double(ULONG hi, ULONG lo)
{
    uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t double_to_bits(double value)
{
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static IPTR return_double(void *cpu, double value)
{
    uint64_t bits = double_to_bits(value);

    THUNK_SET_D(1, (ULONG)(uint32_t)bits);
    return (IPTR)(ULONG)(uint32_t)(bits >> 32);
}

static void set_math_ccr(void *cpu, int zero, int negative, int overflow)
{
    UWORD sr = Kaffeine_GetSR(cpu) & ~(UWORD)M68K_CCR_NZV_MASK;

    if (zero)
        sr |= M68K_CCR_Z;
    if (negative)
        sr |= M68K_CCR_N;
    if (overflow)
        sr |= M68K_CCR_V;
    Kaffeine_SetSR(cpu, sr);
}

/* mathieeedoubbas.library */

/* -30: IEEEDPFix(D0/D1=parm) -> D0=integer */
IPTR kaffeine_mathieeedoubbas_IEEEDPFix(struct KaffeineContext *ctx, void *cpu)
{
    ULONG hi = THUNK_D(0), lo = THUNK_D(1);
    double parm = bits_to_double(hi, lo);
    int32_t res;
    int overflow = !(parm >= -2147483648.0 && parm < 2147483648.0);

    (void)ctx;
    /* Out-of-range magnitudes and NaN saturate to the 32-bit signed limits
       matching workbench/libs/mathieeedoubbas/ieeedpfix.c without invoking
       undefined float-to-int overflow in C. */
    if (overflow)
        res = (hi & 0x80000000UL) ? INT32_MIN : INT32_MAX;
    else
        res = (int32_t)parm;

    set_math_ccr(cpu, res == 0, res < 0, overflow);
    return (IPTR)(ULONG)res;
}

/* -36: IEEEDPFlt(D0=integer) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPFlt(struct KaffeineContext *ctx, void *cpu)
{
    int32_t integer = (int32_t)THUNK_D(0);
    double res = (double)integer;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* -42: IEEEDPCmp(D0/D1=leftParm, D2/D3=rightParm) -> D0=-1/0/+1, sets CCR */
IPTR kaffeine_mathieeedoubbas_IEEEDPCmp(struct KaffeineContext *ctx, void *cpu)
{
    double left = bits_to_double(THUNK_D(0), THUNK_D(1));
    double right = bits_to_double(THUNK_D(2), THUNK_D(3));
    LONG cmp;

    (void)ctx;
    if (left > right)
        cmp = 1;
    else if (left < right)
        cmp = -1;
    else
        cmp = 0;

    set_math_ccr(cpu, cmp == 0, cmp < 0, 0);
    return (IPTR)(ULONG)cmp;
}

/* -48: IEEEDPTst(D0/D1=parm) -> D0=-1/0/+1, sets CCR */
IPTR kaffeine_mathieeedoubbas_IEEEDPTst(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    LONG cmp;

    (void)ctx;
    if (parm > 0.0)
        cmp = 1;
    else if (parm < 0.0)
        cmp = -1;
    else
        cmp = 0;

    set_math_ccr(cpu, cmp == 0, cmp < 0, 0);
    return (IPTR)(ULONG)cmp;
}

/* -54: IEEEDPAbs(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPAbs(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = fabs(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, 0, 0);
    return return_double(cpu, res);
}

/* -60: IEEEDPNeg(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPNeg(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = -parm;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* -66: IEEEDPAdd(D0/D1=leftParm, D2/D3=rightParm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPAdd(struct KaffeineContext *ctx, void *cpu)
{
    double left = bits_to_double(THUNK_D(0), THUNK_D(1));
    double right = bits_to_double(THUNK_D(2), THUNK_D(3));
    double res = left + right;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isinf(res));
    return return_double(cpu, res);
}

/* ieeedpsub.c, ieeedpmul.c and ieeedpdiv.c have no Flags section in their
   AROS headers; set Z and N from the result, and V when finite arguments
   produce an infinite or NaN result. */

/* -72: IEEEDPSub(D0/D1=leftParm, D2/D3=rightParm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPSub(struct KaffeineContext *ctx, void *cpu)
{
    double left = bits_to_double(THUNK_D(0), THUNK_D(1));
    double right = bits_to_double(THUNK_D(2), THUNK_D(3));
    double res = left - right;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0,
                 isfinite(left) && isfinite(right) && !isfinite(res));
    return return_double(cpu, res);
}

/* -78: IEEEDPMul(D0/D1=factor1, D2/D3=factor2) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPMul(struct KaffeineContext *ctx, void *cpu)
{
    double factor1 = bits_to_double(THUNK_D(0), THUNK_D(1));
    double factor2 = bits_to_double(THUNK_D(2), THUNK_D(3));
    double res = factor1 * factor2;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0,
                 isfinite(factor1) && isfinite(factor2) && !isfinite(res));
    return return_double(cpu, res);
}

/* -84: IEEEDPDiv(D0/D1=dividend, D2/D3=divisor) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPDiv(struct KaffeineContext *ctx, void *cpu)
{
    double dividend = bits_to_double(THUNK_D(0), THUNK_D(1));
    double divisor = bits_to_double(THUNK_D(2), THUNK_D(3));
    double res = dividend / divisor;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0,
                 isfinite(dividend) && isfinite(divisor) && !isfinite(res));
    return return_double(cpu, res);
}

/* -90: IEEEDPFloor(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPFloor(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = floor(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* -96: IEEEDPCeil(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubbas_IEEEDPCeil(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = ceil(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* mathieeesingbas.library

   An IEEE 754 single travels in one data register, D0 (D1 for a second
   operand), and returns in D0. AROS implements the library with integer
   code that truncates instead of rounding; as the double precision thunks
   do, these compute in host IEEE 754 single precision, which is what 68k
   programs expect, and set the flags of the Flags sections in
   workbench/libs/mathieeesingbas. */

static float bits_to_float(ULONG bits)
{
    uint32_t raw = (uint32_t)bits;
    float value;

    memcpy(&value, &raw, sizeof(value));
    return value;
}

static IPTR return_float(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return (IPTR)(ULONG)bits;
}

/* -30: IEEESPFix(D0=parm) -> D0=integer */
IPTR kaffeine_mathieeesingbas_IEEESPFix(struct KaffeineContext *ctx, void *cpu)
{
    ULONG bits = THUNK_D(0);
    float parm = bits_to_float(bits);
    int32_t res;
    int overflow = !(parm >= -2147483648.0f && parm < 2147483648.0f);

    (void)ctx;
    /* As IEEEDPFix(): out-of-range magnitudes and NaN saturate by sign. */
    if (overflow)
        res = (bits & 0x80000000UL) ? INT32_MIN : INT32_MAX;
    else
        res = (int32_t)parm;

    set_math_ccr(cpu, res == 0, res < 0, overflow);
    return (IPTR)(ULONG)res;
}

/* -36: IEEESPFlt(D0=integer) -> D0=single; V when the single is not
   exactly the integer, which happens above 2^24 */
IPTR kaffeine_mathieeesingbas_IEEESPFlt(struct KaffeineContext *ctx, void *cpu)
{
    int32_t integer = (int32_t)THUNK_D(0);
    float res = (float)integer;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, (double)res != (double)integer);
    return return_float(res);
}

/* -42: IEEESPCmp(D0=leftParm, D1=rightParm) -> D0=-1/0/+1, sets CCR */
IPTR kaffeine_mathieeesingbas_IEEESPCmp(struct KaffeineContext *ctx, void *cpu)
{
    float left = bits_to_float(THUNK_D(0));
    float right = bits_to_float(THUNK_D(1));
    LONG cmp;

    (void)ctx;
    if (left > right)
        cmp = 1;
    else if (left < right)
        cmp = -1;
    else
        cmp = 0;

    set_math_ccr(cpu, cmp == 0, cmp < 0, 0);
    return (IPTR)(ULONG)cmp;
}

/* -48: IEEESPTst(D0=parm) -> D0=-1/0/+1, sets CCR */
IPTR kaffeine_mathieeesingbas_IEEESPTst(struct KaffeineContext *ctx, void *cpu)
{
    float parm = bits_to_float(THUNK_D(0));
    LONG cmp;

    (void)ctx;
    if (parm > 0.0f)
        cmp = 1;
    else if (parm < 0.0f)
        cmp = -1;
    else
        cmp = 0;

    set_math_ccr(cpu, cmp == 0, cmp < 0, 0);
    return (IPTR)(ULONG)cmp;
}

/* -54: IEEESPAbs(D0=parm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPAbs(struct KaffeineContext *ctx, void *cpu)
{
    float res = fabsf(bits_to_float(THUNK_D(0)));

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, 0, 0);
    return return_float(res);
}

/* -60: IEEESPNeg(D0=parm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPNeg(struct KaffeineContext *ctx, void *cpu)
{
    float res = -bits_to_float(THUNK_D(0));

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, 0);
    return return_float(res);
}

/* -66: IEEESPAdd(D0=leftParm, D1=rightParm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPAdd(struct KaffeineContext *ctx, void *cpu)
{
    float res = bits_to_float(THUNK_D(0)) + bits_to_float(THUNK_D(1));

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, isinf(res));
    return return_float(res);
}

/* As for IEEEDPSub(), IEEEDPMul() and IEEEDPDiv(): V when finite operands
   give an infinite or NaN result. The Flags of IEEESPMul() and IEEESPDiv()
   call a result out of range, and the AROS code sets V for an exponent that
   is too small as well, so these two also set V on IEEE 754 underflow: a
   zero or denormal result that is not exact. The product of two singles is
   exact in double precision, so it tells whether a result was rounded. */

/* -72: IEEESPSub(D0=leftParm, D1=rightParm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPSub(struct KaffeineContext *ctx, void *cpu)
{
    float left = bits_to_float(THUNK_D(0));
    float right = bits_to_float(THUNK_D(1));
    float res = left - right;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f,
                 isfinite(left) && isfinite(right) && !isfinite(res));
    return return_float(res);
}

/* -78: IEEESPMul(D0=factor1, D1=factor2) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPMul(struct KaffeineContext *ctx, void *cpu)
{
    float factor1 = bits_to_float(THUNK_D(0));
    float factor2 = bits_to_float(THUNK_D(1));
    float res = factor1 * factor2;
    int underflow = fabsf(res) < FLT_MIN && (double)res != (double)factor1 * factor2;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, underflow ||
                 (isfinite(factor1) && isfinite(factor2) && !isfinite(res)));
    return return_float(res);
}

/* -84: IEEESPDiv(D0=dividend, D1=divisor) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPDiv(struct KaffeineContext *ctx, void *cpu)
{
    float dividend = bits_to_float(THUNK_D(0));
    float divisor = bits_to_float(THUNK_D(1));
    float res = dividend / divisor;
    /* An exact quotient times the divisor gives back the dividend. A finite
       number divided by an infinity is an exact zero. */
    int underflow = isfinite(divisor) && fabsf(res) < FLT_MIN &&
                    (double)res * divisor != (double)dividend;

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, underflow ||
                 (isfinite(dividend) && isfinite(divisor) && !isfinite(res)));
    return return_float(res);
}

/* -90: IEEESPFloor(D0=parm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPFloor(struct KaffeineContext *ctx, void *cpu)
{
    float res = floorf(bits_to_float(THUNK_D(0)));

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, 0);
    return return_float(res);
}

/* -96: IEEESPCeil(D0=parm) -> D0=single */
IPTR kaffeine_mathieeesingbas_IEEESPCeil(struct KaffeineContext *ctx, void *cpu)
{
    float res = ceilf(bits_to_float(THUNK_D(0)));

    (void)ctx;
    set_math_ccr(cpu, res == 0.0f, res < 0.0f, 0);
    return return_float(res);
}

/* mathieeedoubtrans.library */

/* ieeedpatan.c, ieeedptan.c and ieeedpsincos.c have no Flags section in
   their AROS headers; set Z and N from the D0/D1 result (the sine for
   IEEEDPSincos()), and V when a finite argument produces an infinite or
   NaN result.

   ieeedpsin.c and ieeedpcos.c document "overflow : 0", but their code sets
   V for an infinite argument, and the AROS IEEEDPTan() and IEEEDPSincos()
   end with the flags of IEEEDPSin(). The code is the more precise contract
   here, so these four set V for an infinite argument too. */

/* -30: IEEEDPAtan(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPAtan(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = atan(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isfinite(parm) && !isfinite(res));
    return return_double(cpu, res);
}

/* -36: IEEEDPSin(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPSin(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = sin(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isinf(parm));
    return return_double(cpu, res);
}

/* -42: IEEEDPCos(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPCos(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = cos(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isinf(parm));
    return return_double(cpu, res);
}

/* -48: IEEEDPTan(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPTan(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = tan(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0,
                 isinf(parm) || (isfinite(parm) && !isfinite(res)));
    return return_double(cpu, res);
}

/* -54: IEEEDPSincos(A0=pf2, D0/D1=parm) -> D0/D1=sin(parm), *pf2=cos(parm) */
IPTR kaffeine_mathieeedoubtrans_IEEEDPSincos(struct KaffeineContext *ctx, void *cpu)
{
    ULONG pf2 = THUNK_A(0);
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double s = sin(parm);
    double c = cos(parm);

    if (pf2 != 0 && pf2 < ctx->mem_size && ctx->mem_size - pf2 >= 8)
    {
        uint64_t cos_bits = double_to_bits(c);

        m68k_write32(ctx, pf2, (ULONG)(uint32_t)(cos_bits >> 32));
        m68k_write32(ctx, pf2 + 4, (ULONG)(uint32_t)cos_bits);
    }
    set_math_ccr(cpu, s == 0.0, s < 0.0,
                 isinf(parm) || (isfinite(parm) && !isfinite(s)));
    return return_double(cpu, s);
}

/* -60: IEEEDPSinh(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPSinh(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = sinh(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isinf(res));
    return return_double(cpu, res);
}

/* -66: IEEEDPCosh(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPCosh(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = cosh(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, 0, isinf(res));
    return return_double(cpu, res);
}

/* -72: IEEEDPTanh(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPTanh(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = tanh(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* -78: IEEEDPExp(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPExp(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = exp(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, 0, isinf(res));
    return return_double(cpu, res);
}

/* -84: IEEEDPLog(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPLog(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = log(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, parm < 0.0);
    return return_double(cpu, res);
}

/* -90: IEEEDPPow(D2/D3=exp, D0/D1=arg) -> D0/D1=arg^exp */
IPTR kaffeine_mathieeedoubtrans_IEEEDPPow(struct KaffeineContext *ctx, void *cpu)
{
    /* In mathieeedoubtrans_lib.fd, exp is in D2/D3 and arg (the base) is in
       D0/D1; IEEEDPPow() computes arg raised to the exp power. */
    double arg = bits_to_double(THUNK_D(0), THUNK_D(1));
    double exp_val = bits_to_double(THUNK_D(2), THUNK_D(3));
    double res = pow(arg, exp_val);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, isinf(res));
    return return_double(cpu, res);
}

/* -96: IEEEDPSqrt(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPSqrt(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = sqrt(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, 0, isnan(res));
    return return_double(cpu, res);
}

/* -102: IEEEDPTieee(D0/D1=parm) -> D0=IEEE 754 single-precision bits */
IPTR kaffeine_mathieeedoubtrans_IEEEDPTieee(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    float single = (float)parm;
    uint32_t bits;

    (void)ctx;
    set_math_ccr(cpu, single == 0.0f, single < 0.0f, isinf(single));
    memcpy(&bits, &single, sizeof(bits));
    return (IPTR)(ULONG)bits;
}

/* -108: IEEEDPFieee(D0=single) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPFieee(struct KaffeineContext *ctx, void *cpu)
{
    uint32_t bits = THUNK_D(0);
    float single;
    double res;

    (void)ctx;
    memcpy(&single, &bits, sizeof(single));
    res = (double)single;
    set_math_ccr(cpu, res == 0.0, res < 0.0, 0);
    return return_double(cpu, res);
}

/* -114: IEEEDPAsin(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPAsin(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = asin(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, !(parm >= -1.0 && parm <= 1.0));
    return return_double(cpu, res);
}

/* -120: IEEEDPAcos(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPAcos(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = acos(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, !(parm >= -1.0 && parm <= 1.0));
    return return_double(cpu, res);
}

/* -126: IEEEDPLog10(D0/D1=parm) -> D0/D1=double */
IPTR kaffeine_mathieeedoubtrans_IEEEDPLog10(struct KaffeineContext *ctx, void *cpu)
{
    double parm = bits_to_double(THUNK_D(0), THUNK_D(1));
    double res = log10(parm);

    (void)ctx;
    set_math_ccr(cpu, res == 0.0, res < 0.0, parm < 0.0);
    return return_double(cpu, res);
}

const struct M68KThunkEntry kaffeine_thunks_mathieeedoubbas[] = {
    { 30, kaffeine_mathieeedoubbas_IEEEDPFix },
    { 36, kaffeine_mathieeedoubbas_IEEEDPFlt },
    { 42, kaffeine_mathieeedoubbas_IEEEDPCmp },
    { 48, kaffeine_mathieeedoubbas_IEEEDPTst },
    { 54, kaffeine_mathieeedoubbas_IEEEDPAbs },
    { 60, kaffeine_mathieeedoubbas_IEEEDPNeg },
    { 66, kaffeine_mathieeedoubbas_IEEEDPAdd },
    { 72, kaffeine_mathieeedoubbas_IEEEDPSub },
    { 78, kaffeine_mathieeedoubbas_IEEEDPMul },
    { 84, kaffeine_mathieeedoubbas_IEEEDPDiv },
    { 90, kaffeine_mathieeedoubbas_IEEEDPFloor },
    { 96, kaffeine_mathieeedoubbas_IEEEDPCeil },
    { 0, NULL }
};
const ULONG kaffeine_thunks_mathieeedoubbas_count =
    sizeof(kaffeine_thunks_mathieeedoubbas) / sizeof(kaffeine_thunks_mathieeedoubbas[0]) - 1;

const struct M68KThunkEntry kaffeine_thunks_mathieeesingbas[] = {
    { 30, kaffeine_mathieeesingbas_IEEESPFix },
    { 36, kaffeine_mathieeesingbas_IEEESPFlt },
    { 42, kaffeine_mathieeesingbas_IEEESPCmp },
    { 48, kaffeine_mathieeesingbas_IEEESPTst },
    { 54, kaffeine_mathieeesingbas_IEEESPAbs },
    { 60, kaffeine_mathieeesingbas_IEEESPNeg },
    { 66, kaffeine_mathieeesingbas_IEEESPAdd },
    { 72, kaffeine_mathieeesingbas_IEEESPSub },
    { 78, kaffeine_mathieeesingbas_IEEESPMul },
    { 84, kaffeine_mathieeesingbas_IEEESPDiv },
    { 90, kaffeine_mathieeesingbas_IEEESPFloor },
    { 96, kaffeine_mathieeesingbas_IEEESPCeil },
    { 0, NULL }
};
const ULONG kaffeine_thunks_mathieeesingbas_count =
    sizeof(kaffeine_thunks_mathieeesingbas) / sizeof(kaffeine_thunks_mathieeesingbas[0]) - 1;

const struct M68KThunkEntry kaffeine_thunks_mathieeedoubtrans[] = {
    {  30, kaffeine_mathieeedoubtrans_IEEEDPAtan },
    {  36, kaffeine_mathieeedoubtrans_IEEEDPSin },
    {  42, kaffeine_mathieeedoubtrans_IEEEDPCos },
    {  48, kaffeine_mathieeedoubtrans_IEEEDPTan },
    {  54, kaffeine_mathieeedoubtrans_IEEEDPSincos },
    {  60, kaffeine_mathieeedoubtrans_IEEEDPSinh },
    {  66, kaffeine_mathieeedoubtrans_IEEEDPCosh },
    {  72, kaffeine_mathieeedoubtrans_IEEEDPTanh },
    {  78, kaffeine_mathieeedoubtrans_IEEEDPExp },
    {  84, kaffeine_mathieeedoubtrans_IEEEDPLog },
    {  90, kaffeine_mathieeedoubtrans_IEEEDPPow },
    {  96, kaffeine_mathieeedoubtrans_IEEEDPSqrt },
    { 102, kaffeine_mathieeedoubtrans_IEEEDPTieee },
    { 108, kaffeine_mathieeedoubtrans_IEEEDPFieee },
    { 114, kaffeine_mathieeedoubtrans_IEEEDPAsin },
    { 120, kaffeine_mathieeedoubtrans_IEEEDPAcos },
    { 126, kaffeine_mathieeedoubtrans_IEEEDPLog10 },
    {   0, NULL }
};
const ULONG kaffeine_thunks_mathieeedoubtrans_count =
    sizeof(kaffeine_thunks_mathieeedoubtrans) / sizeof(kaffeine_thunks_mathieeedoubtrans[0]) - 1;

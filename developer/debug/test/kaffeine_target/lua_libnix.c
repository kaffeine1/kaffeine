/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The snprintf(), strtod() and IEEE-754 double comparison helpers of Lua
    5.4.7 for its m68k build with libnix. In libnix, __vfprintf_total_size()
    writes zero padding before the sign on floating-point conversions and
    scales large exponents by multiplying by 0.0000000001 in a loop, which
    leaves rounding drift in digits 16 to 21 of %.99f on 1e308;
    lua_libnix_snprintf() hands every single %e, %f or %g conversion to the
    exact formatter of libnix_fmt.c. In libnix, strtod()
    calls sscanf("%lf%ln"), whose vfscanf() stores 0.0 and advances endptr
    past the first non-digit character when no number is present, and rounds
    after 16 decimal digits on 2^63; lua_libnix_strtod() is the exact
    libnix_fmt_strtod(), which leaves *endptr at nptr when no number
    matches. In libm.a of libnix,
    __eqdf2, __nedf2, __gtdf2, __gedf2, __ltdf2, __ledf2 and __cmpdf2 are all
    aliases for IEEEDPCmp(), which has no unordered return value: on AmigaOS
    3.2.3 IEEEDPCmp(NaN, NaN) is 0 and IEEEDPCmp(NaN, DBL_MAX) is +1. The
    soft-float helpers below check the IEEE-754 exponent and mantissa words
    for NaN before calling IEEEDPCmp(), and return a quiet NaN on 0.0 / 0.0
    or inf / inf in __divdf3().
*/

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <proto/mathieeedoubbas.h>

#include "libnix_fmt.h"

typedef union {
    double d;
    struct {
        unsigned long hi;
        unsigned long lo;
    } w;
} lua_ieee_double_t;

static int lua_ieee_isnan(double x)
{
    lua_ieee_double_t u;

    u.d = x;
    return ((u.w.hi & 0x7ff00000UL) == 0x7ff00000UL) &&
           (((u.w.hi & 0x000fffffUL) != 0UL) || (u.w.lo != 0UL));
}

static int lua_ieee_iszero(double x)
{
    lua_ieee_double_t u;

    u.d = x;
    return ((u.w.hi & 0x7fffffffUL) == 0UL) && (u.w.lo == 0UL);
}

static int lua_ieee_isinf(double x)
{
    lua_ieee_double_t u;

    u.d = x;
    return ((u.w.hi & 0x7fffffffUL) == 0x7ff00000UL) && (u.w.lo == 0UL);
}

double __divdf3(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b) ||
        (lua_ieee_iszero(a) && lua_ieee_iszero(b)) ||
        (lua_ieee_isinf(a) && lua_ieee_isinf(b)))
    {
        lua_ieee_double_t nan;

        nan.w.hi = 0x7ff80000UL;
        nan.w.lo = 0x00000000UL;
        return nan.d;
    }
    return IEEEDPDiv(a, b);
}

long __unorddf2(double a, double b)
{
    return lua_ieee_isnan(a) || lua_ieee_isnan(b);
}

long __eqdf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return 1;
    return IEEEDPCmp(a, b);
}

long __nedf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return 1;
    return IEEEDPCmp(a, b);
}

long __gtdf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return -1;
    return IEEEDPCmp(a, b);
}

long __gedf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return -1;
    return IEEEDPCmp(a, b);
}

long __ltdf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return 1;
    return IEEEDPCmp(a, b);
}

long __ledf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return 1;
    return IEEEDPCmp(a, b);
}

long __cmpdf2(double a, double b)
{
    if (lua_ieee_isnan(a) || lua_ieee_isnan(b))
        return 1;
    return IEEEDPCmp(a, b);
}

int lua_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    int r;
    size_t flen = strlen(fmt);

    /* string.format() and tostring() pass one conversion at a time */
    if (flen >= 2 && fmt[0] == '%' && strchr(fmt + 1, '%') == NULL &&
        strchr(fmt + 1, '*') == NULL && strchr("eEfFgG", fmt[flen - 1]) != NULL)
    {
        double v;

        va_start(ap, fmt);
        v = va_arg(ap, double);
        va_end(ap);
        r = libnix_fmt_double(buf, sz, fmt, v);
        if (r != -2)
            return r;
    }

    va_start(ap, fmt);
    r = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return r;
}

double lua_libnix_strtod(const char *nptr, char **endptr)
{
    return libnix_fmt_strtod(nptr, endptr);
}

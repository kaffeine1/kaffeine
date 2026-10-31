/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The snprintf(), strtod() and IEEE-754 double comparison helpers of Lua
    5.4.7 for its m68k build with libnix. In libnix, __vfprintf_total_size()
    writes zero padding before the sign on floating-point conversions and
    scales large exponents by multiplying by 0.0000000001 in a loop, which
    leaves rounding drift in digits 16 to 21 of %.99f on 1e308;
    lua_libnix_snprintf() moves the sign in front of leading zero padding and
    clears integer digits past DBL_DIG on %f and %F. In libnix, strtod()
    calls sscanf("%lf%ln"), whose vfscanf() stores 0.0 and advances endptr
    past the first non-digit character when no number is present, and rounds
    after 16 decimal digits on 2^63; lua_libnix_strtod() parses decimal
    floats with a 64-bit integer mantissa of up to 19 significant digits and
    leaves *endptr at nptr when no digits match. In libm.a of libnix,
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
    size_t len;

    va_start(ap, fmt);
    r = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);

    if (sz == 0)
        return r;
    len = strlen(buf);
    if (len > 1 && buf[0] == '0')
    {
        size_t k = 1;

        while (k < len && buf[k] == '0')
            k++;
        if (k < len && (buf[k] == '+' || buf[k] == '-' || buf[k] == ' ') &&
            isdigit((unsigned char)buf[k + 1]))
        {
            buf[0] = buf[k];
            buf[k] = '0';
        }
    }
    if (len > 0)
    {
        char spec = fmt[strlen(fmt) - 1];

        if (spec == 'f' || spec == 'F')
        {
            size_t i = (buf[0] == '+' || buf[0] == '-' || buf[0] == ' ') ? 1 : 0;
            size_t digits = 0;

            for (; i < len && isdigit((unsigned char)buf[i]); i++)
            {
                digits++;
                if (digits > DBL_DIG)
                    buf[i] = '0';
            }
        }
    }
    return r;
}

double lua_libnix_strtod(const char *nptr, char **endptr)
{
    const char *p = nptr;
    unsigned long long mant = 0;
    int neg = 0, sig = 0, any = 0, frac_exp = 0, exp_neg = 0;
    long exp10 = 0, e = 0;
    double val;

    if (endptr)
        *endptr = (char *)nptr;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-' || *p == '+')
        neg = (*p++ == '-');

    while (isdigit((unsigned char)*p))
    {
        any = 1;
        if (mant != 0 || *p != '0')
        {
            if (sig < 19)
            {
                mant = mant * 10 + (unsigned)(*p - '0');
                sig++;
            }
            else
            {
                frac_exp++;
            }
        }
        p++;
    }
    if (*p == '.')
    {
        p++;
        while (isdigit((unsigned char)*p))
        {
            any = 1;
            if (mant != 0 || *p != '0')
            {
                if (sig < 19)
                {
                    mant = mant * 10 + (unsigned)(*p - '0');
                    sig++;
                    frac_exp--;
                }
            }
            else
            {
                frac_exp--;
            }
            p++;
        }
    }
    if (!any)
        return 0.0;

    if (*p == 'e' || *p == 'E')
    {
        const char *ep = p + 1;

        if (*ep == '-' || *ep == '+')
            exp_neg = (*ep++ == '-');
        if (isdigit((unsigned char)*ep))
        {
            while (isdigit((unsigned char)*ep))
            {
                if (e < 100000)
                    e = e * 10 + (*ep - '0');
                ep++;
            }
            p = ep;
            exp10 = exp_neg ? -e : e;
        }
    }

    if (endptr)
        *endptr = (char *)p;
    if (mant == 0)
        return neg ? -0.0 : 0.0;

    exp10 += frac_exp;
    val = (double)mant;
    if (exp10 > 0)
    {
        if (exp10 > 308)
        {
            val *= pow(10.0, 308);
            exp10 -= 308;
            if (exp10 > 308)
            {
                errno = ERANGE;
                return neg ? -HUGE_VAL : HUGE_VAL;
            }
        }
        val *= pow(10.0, (double)exp10);
        if (val == HUGE_VAL)
            errno = ERANGE;
    }
    else if (exp10 < 0)
    {
        long neg_exp = -exp10;

        if (neg_exp > 308)
        {
            val /= pow(10.0, 308);
            neg_exp -= 308;
            if (neg_exp > 308)
            {
                errno = ERANGE;
                return neg ? -0.0 : 0.0;
            }
        }
        val /= pow(10.0, (double)neg_exp);
        if (val == 0.0)
            errno = ERANGE;
    }
    return neg ? -val : val;
}

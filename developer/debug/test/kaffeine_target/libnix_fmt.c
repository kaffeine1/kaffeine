/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    One %e, %E, %f, %F, %g or %G conversion of a double, exact, for the
    ports that the battery builds with libnix. The printf() of libnix scales
    large exponents by multiplying by 0.0000000001 in a loop, so the digits
    of %.99f of 1e308 drift after the 16th, and the comparisons of the ports
    with their host builds need every digit. This file uses no floating
    point at all: it takes the 53-bit mantissa and the exponent from the
    bits, computes the exact decimal expansion of m * 2^e in base 10^9
    limbs (m shifted left, or m * 5^k with the point k digits from the
    right), rounds it to nearest with ties to even, as the C libraries of
    the hosts do in the default rounding mode, and lays it out as C99 says
    for the "C" locale (the point is '.'). The length is computed apart, so
    the output stops at sz - 1 bytes without counting further. Infinities
    are inf or INF with their sign, as C99 says. C leaves the sign of a NaN
    to the implementation: here a NaN is nan or NAN without a sign, and +
    or a space add none, as the libc of macOS prints it (glibc prints the
    sign). A 0 flag pads both with spaces.
*/

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "libnix_fmt.h"

#define LIMB_BASE 1000000000u
#define MAX_LIMBS 96            /* 2^53 * 5^1074 has 767 digits */
#define MAX_DIGITS (MAX_LIMBS * 9)

/* The exact value is 0.dig[0] dig[1] ... dig[n - 1] times 10^point, with
   dig[0] not '0' and no trailing '0'; zero has n 0 and point 1. */
struct decimal {
    char dig[MAX_DIGITS];
    int n;
    int point;
};

/* Stores at most cap bytes; the length is computed apart, so the loops
   stop as soon as the buffer is full. */
struct sink {
    char *buf;
    size_t cap;
    size_t len;
};

static void put(struct sink *s, char c)
{
    if (s->len < s->cap)
        s->buf[s->len++] = c;
}

static void put_n(struct sink *s, char c, unsigned long long n)
{
    while (n-- > 0 && s->len < s->cap)
        s->buf[s->len++] = c;
}

static void mul_small(unsigned int *limb, int *count, unsigned int mul)
{
    unsigned long long carry = 0;
    int i;

    for (i = 0; i < *count; i++)
    {
        unsigned long long t = (unsigned long long)limb[i] * mul + carry;

        limb[i] = (unsigned int)(t % LIMB_BASE);
        carry = t / LIMB_BASE;
    }
    while (carry != 0)
    {
        limb[(*count)++] = (unsigned int)(carry % LIMB_BASE);
        carry /= LIMB_BASE;
    }
}

static void to_decimal(unsigned long long bits, struct decimal *d)
{
    static const unsigned int pow5[14] = {
        1u, 5u, 25u, 125u, 625u, 3125u, 15625u, 78125u, 390625u, 1953125u,
        9765625u, 48828125u, 244140625u, 1220703125u
    };
    unsigned int limb[MAX_LIMBS];
    unsigned long long m = bits & 0xfffffffffffffULL;
    int e = (int)((bits >> 52) & 0x7ff), count = 0, k = 0, i, j;

    if (e != 0)
    {
        m |= 1ULL << 52;
        e -= 1075;
    }
    else
        e = -1074;
    if (m == 0)
    {
        d->n = 0;
        d->point = 1;
        return;
    }
    while (m != 0)
    {
        limb[count++] = (unsigned int)(m % LIMB_BASE);
        m /= LIMB_BASE;
    }
    if (e > 0)
    {
        for (; e > 0; e -= 29)
            mul_small(limb, &count, 1u << (e > 29 ? 29 : e));
    }
    else
    {
        k = -e;
        for (i = k; i > 0; i -= 13)
            mul_small(limb, &count, pow5[i > 13 ? 13 : i]);
    }

    d->n = 0;
    for (i = count - 1; i >= 0; i--)
    {
        char nine[9];
        unsigned int x = limb[i];

        for (j = 8; j >= 0; j--)
        {
            nine[j] = (char)('0' + x % 10);
            x /= 10;
        }
        for (j = 0; j < 9; j++)
            if (d->n > 0 || nine[j] != '0')
                d->dig[d->n++] = nine[j];
    }
    d->point = d->n - k;
    while (d->dig[d->n - 1] == '0')
        d->n--;
}

/* Keeps the first keep digits, rounding half to even on the exact rest. */
static void round_to(struct decimal *d, long long keep)
{
    int i, up;

    if (keep >= d->n)
        return;
    if (keep < 0)
    {
        d->n = 0;
        d->point = 1;
        return;
    }
    if (d->dig[keep] != '5')
        up = d->dig[keep] > '5';
    else
    {
        up = 0;
        for (i = (int)keep + 1; i < d->n; i++)
            if (d->dig[i] != '0')
                up = 1;
        if (!up && keep > 0)
            up = (d->dig[keep - 1] - '0') & 1;
    }
    d->n = (int)keep;
    if (up)
    {
        for (i = d->n - 1; i >= 0 && d->dig[i] == '9'; i--)
            d->dig[i] = '0';
        if (i >= 0)
            d->dig[i]++;
        else
        {
            d->dig[0] = '1';
            if (d->n == 0)
                d->n = 1;
            d->point++;
        }
    }
    while (d->n > 0 && d->dig[d->n - 1] == '0')
        d->n--;
    if (d->n == 0)
        d->point = 1;
}

static char dig_at(const struct decimal *d, long long i)
{
    return i >= 0 && i < d->n ? d->dig[i] : '0';
}

/* The number without padding: the sign, then style f with frac digits
   after the point, or style e with exponent x. */
static void put_number(struct sink *s, char sign, const struct decimal *d, int style_e,
                       unsigned long long frac, int alt, int x, int upper)
{
    unsigned long long j;
    int i;

    if (sign)
        put(s, sign);
    if (!style_e)
    {
        if (d->point > 0)
            for (i = 0; i < d->point && s->len < s->cap; i++)
                put(s, dig_at(d, i));
        else
            put(s, '0');
        if (frac > 0 || alt)
            put(s, '.');
        for (j = 0; j < frac && s->len < s->cap; j++)
            put(s, dig_at(d, (long long)d->point + (long long)j));
    }
    else
    {
        char exp[8];
        int n = 0, ax = x < 0 ? -x : x;

        put(s, dig_at(d, 0));
        if (frac > 0 || alt)
            put(s, '.');
        for (j = 1; j <= frac && s->len < s->cap; j++)
            put(s, dig_at(d, (long long)j));
        put(s, upper ? 'E' : 'e');
        put(s, x < 0 ? '-' : '+');
        do
        {
            exp[n++] = (char)('0' + ax % 10);
            ax /= 10;
        } while (ax != 0);
        if (n < 2)
            exp[n++] = '0';
        while (n > 0)
            put(s, exp[--n]);
    }
}

/* The length that put_number() gives, without writing it. */
static unsigned long long number_length(char sign, const struct decimal *d, int style_e,
                                        unsigned long long frac, int alt, int x)
{
    unsigned long long len = (sign ? 1 : 0) + (frac > 0 || alt ? 1 : 0) + frac;

    if (!style_e)
        return len + (d->point > 0 ? (unsigned long long)d->point : 1);
    len += 1 + 2;
    if (x < 0)
        x = -x;
    return len + (x >= 100 ? 3 : 2);
}

/* Reads a decimal field of at most INT_MAX; returns -1 past it. */
static long long read_field(const char **p)
{
    long long v = 0;

    while (**p >= '0' && **p <= '9')
    {
        v = v * 10 + (**p - '0');
        if (v > INT_MAX)
            return -1;
        (*p)++;
    }
    return v;
}

int libnix_fmt_double(char *buf, size_t sz, const char *spec, double v)
{
    const char *p = spec;
    int minus = 0, plus = 0, space = 0, alt = 0, zero = 0, upper, style_e = 0, x = 0;
    long long width = 0, prec = -1;
    unsigned long long bits, frac = 0, len, pad;
    char conv, sign = 0;
    /* static: the ports run with small stacks, and nothing here recurses */
    static struct decimal d, t;
    struct sink out;

    if (*p++ != '%')
        return -2;
    for (;; p++)
    {
        if (*p == '-')
            minus = 1;
        else if (*p == '+')
            plus = 1;
        else if (*p == ' ')
            space = 1;
        else if (*p == '#')
            alt = 1;
        else if (*p == '0')
            zero = 1;
        else
            break;
    }
    if ((width = read_field(&p)) < 0)
        return -1;
    if (*p == '.')
    {
        p++;
        if ((prec = read_field(&p)) < 0)
            return -1;
    }
    conv = *p;
    if (conv == '\0' || strchr("eEfFgG", conv) == NULL || p[1] != '\0')
        return -2;
    upper = conv == 'E' || conv == 'F' || conv == 'G';

    memcpy(&bits, &v, sizeof(bits));
    if (bits >> 63)
        sign = '-';
    else if (plus)
        sign = '+';
    else if (space)
        sign = ' ';

    if (((bits >> 52) & 0x7ff) == 0x7ff)
    {
        const char *word = (bits & 0xfffffffffffffULL) ? (upper ? "NAN" : "nan")
                                                       : (upper ? "INF" : "inf");

        if (bits & 0xfffffffffffffULL)
            sign = 0;
        len = (sign ? 1 : 0) + 3;
        pad = (unsigned long long)width > len ? (unsigned long long)width - len : 0;
        if (len + pad > INT_MAX)
            return -1;
        out.buf = buf;
        out.cap = sz ? sz - 1 : 0;
        out.len = 0;
        if (!minus)
            put_n(&out, ' ', pad);
        if (sign)
            put(&out, sign);
        while (*word)
            put(&out, *word++);
        if (minus)
            put_n(&out, ' ', pad);
        if (sz)
            buf[out.len] = '\0';
        return (int)(len + pad);
    }

    to_decimal(bits & ~(1ULL << 63), &d);
    if (conv == 'f' || conv == 'F')
    {
        if (prec < 0)
            prec = 6;
        round_to(&d, (long long)d.point + prec);
        frac = (unsigned long long)prec;
    }
    else if (conv == 'e' || conv == 'E')
    {
        if (prec < 0)
            prec = 6;
        style_e = 1;
        round_to(&d, prec + 1);
        x = d.n ? d.point - 1 : 0;
        frac = (unsigned long long)prec;
    }
    else
    {
        long long g = prec < 0 ? 6 : prec == 0 ? 1 : prec;
        long long maxfrac;

        if (d.n)
        {
            t = d;
            round_to(&t, g);
            x = t.point - 1;
        }
        if (g > x && x >= -4)
        {
            round_to(&d, (long long)d.point + g - 1 - x);
            frac = (unsigned long long)(g - 1 - x);
            maxfrac = (long long)d.n - d.point;
        }
        else
        {
            style_e = 1;
            round_to(&d, g);
            x = d.n ? d.point - 1 : 0;
            frac = (unsigned long long)(g - 1);
            maxfrac = (long long)d.n - 1;
        }
        if (!alt)
        {
            /* no trailing zeros: past the last digit of d there are none */
            if (maxfrac < 0)
                maxfrac = 0;
            if (frac > (unsigned long long)maxfrac)
                frac = (unsigned long long)maxfrac;
            while (frac > 0 &&
                   dig_at(&d, (style_e ? 0 : (long long)d.point) + (long long)frac -
                                  (style_e ? 0 : 1)) == '0')
                frac--;
        }
    }

    len = number_length(sign, &d, style_e, frac, alt, x);
    pad = (unsigned long long)width > len ? (unsigned long long)width - len : 0;
    if (len + pad > INT_MAX)
        return -1;

    out.buf = buf;
    out.cap = sz ? sz - 1 : 0;
    out.len = 0;
    if (minus)
    {
        put_number(&out, sign, &d, style_e, frac, alt, x, upper);
        put_n(&out, ' ', pad);
    }
    else if (zero)
    {
        if (sign)
            put(&out, sign);
        put_n(&out, '0', pad);
        put_number(&out, 0, &d, style_e, frac, alt, x, upper);
    }
    else
    {
        put_n(&out, ' ', pad);
        put_number(&out, sign, &d, style_e, frac, alt, x, upper);
    }
    if (sz)
        buf[out.len] = '\0';
    return (int)(len + pad);
}

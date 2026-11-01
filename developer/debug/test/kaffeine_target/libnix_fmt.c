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
    and NaNs are inf, INF, nan or NAN, with the sign bit as a minus and the
    -, + and space flags as for numbers, while # and 0 have no effect (C11
    7.21.6.1, footnote 277), as glibc prints them. The libc of macOS prints
    a NaN without a sign and ignores + and space there.

    The strtod() of the other direction is exact too, for the same reason:
    libnix rounds after 16 digits, and an adapter that scales a 64-bit
    mantissa by powers of ten rounds twice, while the IEEEDPPow() and the
    multiplications of AmigaOS 3.2.3 are not correctly rounded. It keeps
    the significant digits as a big integer D in base 2^32 words, takes the
    quotient of D * 10^E by a power of two to two or three bits beyond the
    precision of the result, and rounds that once, to nearest with ties to
    even, with a sticky bit for the remainder.
*/

#include <errno.h>
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

/* libnix_fmt_strtod(): the value D * 10^E of the significant digits D, in
   base 2^32 words, least significant first. 10^1124 shifted left by 55
   bits, the largest number the conversion builds, has 3789 bits. */
#define BIN_WORDS 128
/* A midpoint of two doubles, or of 0 and the smallest subnormal, or of
   DBL_MAX and 2^1024, is m * 2^e with m odd and below 2^54 and e at least
   -1075. Its significant digits are those of m * 5^-e when e < 0 (odd, so
   no trailing zero), at most the 768 of (2^54 - 1) * 5^1075, and at most
   309 when e >= 0. The midpoint of the largest subnormal and DBL_MIN,
   (2^53 - 1) * 2^-1075, has 768 too. */
#define SCAN_DIGITS 800

struct bin {
    unsigned int w[BIN_WORDS];
    int n;                      /* words in use, the top one not 0 */
};

static void bin_mul_add(struct bin *a, unsigned int mul, unsigned int add)
{
    unsigned long long carry = add;
    int i;

    for (i = 0; i < a->n; i++)
    {
        unsigned long long t = (unsigned long long)a->w[i] * mul + carry;

        a->w[i] = (unsigned int)t;
        carry = t >> 32;
    }
    if (carry != 0)
        a->w[a->n++] = (unsigned int)carry;
}

static int bin_bits(const struct bin *a)
{
    unsigned int top;
    int bits;

    if (a->n == 0)
        return 0;
    top = a->w[a->n - 1];
    for (bits = (a->n - 1) * 32; top != 0; top >>= 1)
        bits++;
    return bits;
}

static void bin_shift_left(struct bin *a, int k)
{
    int words = k / 32, bits = k % 32, i;

    if (a->n == 0)
        return;
    a->w[a->n] = 0;
    for (i = a->n; i >= 0; i--)
    {
        unsigned int v = a->w[i] << bits;

        if (bits != 0 && i > 0)
            v |= a->w[i - 1] >> (32 - bits);
        a->w[i + words] = v;
    }
    for (i = 0; i < words; i++)
        a->w[i] = 0;
    a->n += words + 1;
    while (a->n > 0 && a->w[a->n - 1] == 0)
        a->n--;
}

static void bin_shift_right1(struct bin *a)
{
    int i;

    for (i = 0; i < a->n; i++)
        a->w[i] = (a->w[i] >> 1) | (i + 1 < a->n ? a->w[i + 1] << 31 : 0);
    while (a->n > 0 && a->w[a->n - 1] == 0)
        a->n--;
}

static int bin_compare(const struct bin *a, const struct bin *b)
{
    int i;

    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* a -= b, for a >= b */
static void bin_subtract(struct bin *a, const struct bin *b)
{
    unsigned int borrow = 0;
    int i;

    for (i = 0; i < a->n; i++)
    {
        unsigned long long s = (unsigned long long)a->w[i]
                               - (i < b->n ? b->w[i] : 0) - borrow;

        a->w[i] = (unsigned int)s;
        borrow = (unsigned int)(s >> 63);
    }
    while (a->n > 0 && a->w[a->n - 1] == 0)
        a->n--;
}

/* floor(a / t) for a quotient below 2^56, leaving the remainder in a and
   t shifted. */
static unsigned long long bin_divide(struct bin *a, struct bin *t)
{
    unsigned long long q = 0;
    int i;

    bin_shift_left(t, 55);
    for (i = 55; i >= 0; i--)
    {
        q <<= 1;
        if (bin_compare(a, t) >= 0)
        {
            bin_subtract(a, t);
            q |= 1;
        }
        bin_shift_right1(t);
    }
    return q;
}

static double from_bits(unsigned long long bits)
{
    union { unsigned long long u; double d; } x;

    x.u = bits;
    return x.d;
}

static int c_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int c_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int c_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c_lower(c) >= 'a' && c_lower(c) <= 'z');
}

static int match_lower(const char *s, const char *word)
{
    while (*word)
        if (c_lower(*s++) != *word++)
            return 0;
    return 1;
}

double libnix_fmt_strtod(const char *nptr, char **endptr)
{
    static const unsigned int pow10[10] = {
        1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u,
        100000000u, 1000000000u
    };
    /* static: the ports run with small stacks, and nothing here recurses */
    static struct bin a, t;
    const char *p = nptr;
    unsigned long long sign = 0, q, rest, half;
    unsigned int chunk = 0;
    int any = 0, point = 0, sticky = 0, chunk_digits = 0;
    int b, lsb, drop, inexact, k;
    long nd = 0;
    long long adjust = 0, e = 0, exp10, cap;

    if (endptr)
        *endptr = (char *)nptr;
    while (c_space(*p))
        p++;
    if (*p == '-' || *p == '+')
        sign = *p++ == '-' ? 1ULL << 63 : 0;

    if (match_lower(p, "inf"))
    {
        p += match_lower(p, "infinity") ? 8 : 3;
        if (endptr)
            *endptr = (char *)p;
        return from_bits(sign | 0x7ff0000000000000ULL);
    }
    if (match_lower(p, "nan"))
    {
        p += 3;
        if (*p == '(')
        {
            const char *s = p + 1;

            while (c_alnum(*s) || *s == '_')
                s++;
            if (*s == ')')
                p = s + 1;
        }
        if (endptr)
            *endptr = (char *)p;
        return from_bits(sign | 0x7ff8000000000000ULL);
    }

    /* The significant digits go to a, nine at a time, and adjust counts the
       digits of the fraction among them, or of the integer part past them.
       After SCAN_DIGITS digits, a non-zero digit only sets sticky, which
       becomes one digit 1 after them: a midpoint between the exact value
       and D with that digit would lie strictly between two numbers of
       SCAN_DIGITS digits at the same scale, so it would need more digits
       than any midpoint has. */
    a.n = 0;
    for (;; p++)
    {
        if (*p == '.' && !point)
        {
            point = 1;
            continue;
        }
        if (*p < '0' || *p > '9')
            break;
        any = 1;
        if (nd == 0 && *p == '0')
        {
            if (point)
                adjust--;
        }
        else if (nd < SCAN_DIGITS)
        {
            chunk = chunk * 10 + (unsigned int)(*p - '0');
            if (++chunk_digits == 9)
            {
                bin_mul_add(&a, pow10[9], chunk);
                chunk = 0;
                chunk_digits = 0;
            }
            nd++;
            if (point)
                adjust--;
        }
        else
        {
            if (*p != '0')
                sticky = 1;
            if (!point)
                adjust++;
        }
    }
    if (!any)
        return 0.0;
    if (sticky)
    {
        chunk = chunk * 10 + 1;
        chunk_digits++;
        nd++;
        adjust--;
    }
    bin_mul_add(&a, pow10[chunk_digits], chunk);

    /* The exponent saturates at cap, past which adjust cannot bring it back
       into range: |E| >= |adjust| + 2000 gives |E + adjust| >= 2000, which
       the checks below take for an overflow or an underflow, as they would
       the exact exponent. adjust counts digits of the string, so neither it
       nor 10 * cap overflows a long long for any string that fits in a
       57-bit address space. */
    if (*p == 'e' || *p == 'E')
    {
        const char *s = p + 1;
        int neg = 0;

        cap = (adjust < 0 ? -adjust : adjust) + 2000;
        if (*s == '-' || *s == '+')
            neg = *s++ == '-';
        if (*s >= '0' && *s <= '9')
        {
            for (; *s >= '0' && *s <= '9'; s++)
                if (e < cap)
                    e = e * 10 + (*s - '0');
            if (neg)
                e = -e;
            p = s;
        }
        else
            e = 0;
    }
    if (endptr)
        *endptr = (char *)p;
    if (nd == 0)
        return from_bits(sign);

    /* D < 10^nd, so D * 10^E < 10^(nd + E): above 10^310 is past DBL_MAX,
       below 10^-323 is under half the smallest subnormal. */
    exp10 = e + adjust;
    if (nd + exp10 > 310)
    {
        errno = ERANGE;
        return from_bits(sign | 0x7ff0000000000000ULL);
    }
    if (nd + exp10 < -323)
    {
        errno = ERANGE;
        return from_bits(sign);
    }

    /* q = floor(D * 10^E / 2^b), b chosen for a q of 55 or 56 bits, or
       -1076 for the subnormals: two or three bits under the last bit of
       the result, with sticky for the remainder. */
    t.n = 1;
    t.w[0] = 1;
    for (k = (int)(exp10 < 0 ? -exp10 : exp10); k > 0; k -= 9)
        bin_mul_add(exp10 < 0 ? &t : &a, pow10[k > 9 ? 9 : k], 0);
    b = bin_bits(&a) - bin_bits(&t) - 55;
    if (b < -1076)
        b = -1076;
    if (b < 0)
        bin_shift_left(&a, -b);
    else
        bin_shift_left(&t, b);
    q = bin_divide(&a, &t);
    for (k = 0; (q >> k) != 0; k++)
        ;
    lsb = b + k - 53;
    if (lsb < -1074)
        lsb = -1074;
    drop = lsb - b;
    half = 1ULL << (drop - 1);
    rest = q & ((half << 1) - 1);
    inexact = rest != 0 || a.n != 0;
    q >>= drop;
    if (rest > half || (rest == half && (a.n != 0 || (q & 1))))
        q++;
    if (q == 1ULL << 53)
    {
        q >>= 1;
        lsb++;
    }
    if (lsb > 971)
    {
        errno = ERANGE;
        return from_bits(sign | 0x7ff0000000000000ULL);
    }
    if (inexact && q < 1ULL << 52)
        errno = ERANGE;
    return from_bits(sign | (((unsigned long long)(lsb + 1074) << 52) + q));
}

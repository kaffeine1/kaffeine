/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Host test of libnix_fmt.c: every conversion must give the bytes and the
    return value of the host snprintf(), for fixed edge cases and for a
    seeded sweep of random bit patterns and formats, with buffers of size
    0, 1, too small, exact and large. "make check-libnix-fmt" runs it; an
    argument sets the number of random cases (200000 by default).

    A NaN is compared with the rule of C11 7.21.6.1 and its footnote 277:
    nan or NAN with the sign bit as a minus and the -, + and space flags as
    for numbers, padded with spaces. The libc of macOS prints a NaN without
    a sign and ignores + and space, glibc follows the rule. Everything
    finite and the infinities are compared with the host as they are.

    %g and %G are built from the host %e and %f, which both libcs get right,
    by the rule of C11 7.21.6.1p8: the exponent X of %.(P-1)e chooses style
    f with precision P-1-X when P > X >= -4, else style e with precision
    P-1, and without # the trailing zeros of the fraction go. The host %g
    itself is wrong at both ends: macOS keeps a trailing zero when %g of an
    integer rounds a tie down (%G of 1133505 gives 1.13350E+06), and glibc
    drops the zeros of %#g when rounding carries into a new digit (%#g of
    999999.5 gives 1.e+06 for 1.00000e+06).

    libnix_fmt_strtod() must give the bits, the end and the ERANGE of the
    host strtod(): on fixed edge cases (the neighbours of 2^53 and 2^64,
    subnormals, the normal boundary, overflow, where a number ends), on
    strings of 767 to 1000 digits, on the exact midpoints of neighbouring
    doubles and on decimals just above and below them, on spellings of one
    value with the point moved, and on random doubles and digit strings.
    Two cases are fixed instead: the libc of macOS takes any bytes up to
    ')' in nan(...), where C99 allows letters, digits and '_', and glibc on
    arm64 sets ERANGE when a value under DBL_MIN rounds up to DBL_MIN,
    which macOS and glibc on x86_64 do not.
*/

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libnix_fmt.h"

static unsigned long long checks, failures;

/* The %g or %G of spec under C11 7.21.6.1p8, from the host %e and %f:
   the core without padding, then padded as the flags say. */
static int expected_g(char *out, size_t size, const char *spec, double v)
{
    char sub[48], core[4096], *mant_end, *z;
    const char *p = spec + 1;
    char flags[4], upper = spec[strlen(spec) - 1] == 'G';
    int minus = 0, zero = 0, alt = 0, width = 0, prec = -1, nf = 0, len, pad, x = 0;

    for (; *p && strchr("-+ #0", *p) != NULL; p++)
    {
        if (*p == '-')
            minus = 1;
        else if (*p == '0')
            zero = 1;
        else if (*p == '#')
            alt = 1;
        else if (nf < 2)
            flags[nf++] = *p;
    }
    flags[nf] = '\0';
    while (*p >= '0' && *p <= '9')
        width = width * 10 + (*p++ - '0');
    if (*p == '.')
    {
        prec = 0;
        for (p++; *p >= '0' && *p <= '9'; p++)
            prec = prec * 10 + (*p - '0');
    }
    if (!isfinite(v))
    {
        snprintf(sub, sizeof(sub), "%%%s%c", flags, upper ? 'G' : 'g');
        snprintf(core, sizeof(core), sub, v);
        zero = 0;
    }
    else
    {
        int g = prec < 0 ? 6 : prec == 0 ? 1 : prec;

        if (v != 0.0)
        {
            snprintf(sub, sizeof(sub), "%%.%de", g - 1);
            snprintf(core, sizeof(core), sub, v);
            x = atoi(strchr(core, 'e') + 1);
        }
        if (g > x && x >= -4)
            snprintf(sub, sizeof(sub), "%%%s%s.%df", flags, alt ? "#" : "", g - 1 - x);
        else
            snprintf(sub, sizeof(sub), "%%%s%s.%d%c", flags, alt ? "#" : "", g - 1, upper ? 'E' : 'e');
        snprintf(core, sizeof(core), sub, v);
        if (!alt && strchr(core, '.') != NULL)
        {
            mant_end = core + strcspn(core, "eE");
            z = mant_end;
            while (z[-1] == '0')
                z--;
            if (z[-1] == '.')
                z--;
            memmove(z, mant_end, strlen(mant_end) + 1);
        }
    }
    len = (int)strlen(core);
    pad = width > len ? width - len : 0;
    if (minus)
        return snprintf(out, size, "%s%*s", core, pad, "");
    if (zero)
    {
        static char zeros[4096];
        int s = core[0] == '-' || core[0] == '+' || core[0] == ' ';

        memset(zeros, '0', (size_t)pad);
        zeros[pad] = '\0';
        return snprintf(out, size, "%.*s%s%s", s, core, zeros, core + s);
    }
    return snprintf(out, size, "%*s%s", pad, "", core);
}

/* The NaN v of spec under C11 7.21.6.1 and its footnote 277. */
static int expected_nan(char *out, size_t size, const char *spec, double v)
{
    const char *p = spec + 1;
    char word[5];
    int minus = 0, plus = 0, space = 0, width = 0, n = 0;

    for (; *p && strchr("-+ #0", *p) != NULL; p++)
    {
        minus |= *p == '-';
        plus |= *p == '+';
        space |= *p == ' ';
    }
    while (*p >= '0' && *p <= '9')
        width = width * 10 + (*p++ - '0');
    if (signbit(v))
        word[n++] = '-';
    else if (plus)
        word[n++] = '+';
    else if (space)
        word[n++] = ' ';
    strcpy(word + n, strchr("EFG", spec[strlen(spec) - 1]) != NULL ? "NAN" : "nan");
    return minus ? snprintf(out, size, "%-*s", width, word) : snprintf(out, size, "%*s", width, word);
}

static void compare(const char *spec, double v)
{
    static char want[4096], got[4096];
    char conv = spec[strlen(spec) - 1];
    int nw = isnan(v) ? expected_nan(want, sizeof(want), spec, v)
             : conv == 'g' || conv == 'G' ? expected_g(want, sizeof(want), spec, v)
                                          : snprintf(want, sizeof(want), spec, v);
    size_t sizes[6];
    int k;

    sizes[0] = 0;
    sizes[1] = 1;
    sizes[2] = nw > 2 ? (size_t)nw / 2 : 1;
    sizes[3] = (size_t)nw;
    sizes[4] = (size_t)nw + 1;
    sizes[5] = sizeof(got);
    for (k = 0; k < 6; k++)
    {
        int ng;

        memset(got, 'X', sizeof(got));
        ng = libnix_fmt_double(k == 0 ? NULL : got, sizes[k], spec, v);
        checks++;
        if (ng != nw ||
            (sizes[k] > 0 && (strncmp(got, want, sizes[k] - 1) != 0 ||
                              got[(size_t)nw < sizes[k] - 1 ? (size_t)nw : sizes[k] - 1] != '\0')) ||
            (sizes[k] > 0 && sizes[k] < sizeof(got) && got[sizes[k]] != 'X'))
        {
            if (failures++ < 20)
                printf("FAIL %s of %a (size %zu): got %d \"%.*s\", expected %d \"%s\"\n",
                       spec, v, sizes[k], ng, sizes[k] ? (int)strnlen(got, sizes[k]) : 0,
                       got, nw, want);
        }
    }
}

static double from_bits(unsigned long long bits)
{
    double v;

    memcpy(&v, &bits, sizeof(v));
    return v;
}

static unsigned long long state = 0x2545F4914F6CDD1DULL;

static unsigned long long next(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

static void long_cases(void)
{
    static char got[1100];
    static const char want_start[] = "1.25000";
    size_t i;
    int n;

    /* the case of the review: the whole %.1000f of 1.25 */
    n = libnix_fmt_double(got, sizeof(got), "%.1000f", 1.25);
    checks++;
    if (n != 1002 || strlen(got) != 1002 || strncmp(got, want_start, 7) != 0)
    {
        failures++;
        printf("FAIL %%.1000f of 1.25: %d bytes\n", n);
    }
    n = libnix_fmt_double(NULL, 0, "%.1000f", 1.25);
    checks++;
    if (n != 1002)
    {
        failures++;
        printf("FAIL %%.1000f of 1.25 with size 0: %d\n", n);
    }
    /* a length of INT_MAX is fine, one more is refused, and neither is
       written out past the buffer */
    n = libnix_fmt_double(got, sizeof(got), "%2147483647.1f", 1.0);
    checks++;
    if (n != INT_MAX || strlen(got) != sizeof(got) - 1)
    {
        failures++;
        printf("FAIL width INT_MAX: %d\n", n);
    }
    n = libnix_fmt_double(got, sizeof(got), "%+.2147483646f", 1.0);
    checks++;
    if (n != -1)
    {
        failures++;
        printf("FAIL length INT_MAX + 1: %d\n", n);
    }
    n = libnix_fmt_double(got, sizeof(got), "%.2147483648f", 1.0);
    checks++;
    if (n != -1)
    {
        failures++;
        printf("FAIL precision past INT_MAX: %d\n", n);
    }
    /* malformed and cut specs, each in a buffer of its own exact size so
       that a sanitizer sees any read past the end */
    {
        static const char *const bad[] = {
            "", "%", "%.", "%+", "%-", "% ", "%#", "%0", "%5", "%.3", "%5.", "%+08.3",
            "f", "%%", "%d", "%s", "%.3fx", "%lf", "%Le", "%*f", "%.*f", "x%f"
        };

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            size_t len = strlen(bad[i]);
            char *spec = malloc(len + 1);

            memcpy(spec, bad[i], len + 1);
            n = libnix_fmt_double(got, sizeof(got), spec, 1.0);
            checks++;
            if (n != -2)
            {
                failures++;
                printf("FAIL malformed spec \"%s\": %d\n", bad[i], n);
            }
            free(spec);
        }
    }
    /* not a single double conversion */
    n = libnix_fmt_double(got, sizeof(got), "%d", 1.0);
    checks++;
    if (n != -2 || libnix_fmt_double(got, sizeof(got), "%.2fx", 1.0) != -2 ||
        libnix_fmt_double(got, sizeof(got), "%Lf", 1.0) != -2)
    {
        failures++;
        printf("FAIL non-double specs\n");
    }
}

/* strtod: the bits, the end and ERANGE of libnix_fmt_strtod() against the
   host strtod(), on a copy of s in a buffer of its exact size. */
static void strtod_compare(const char *s)
{
    size_t len = strlen(s);
    char *copy = malloc(len + 1), *end_want, *end_got;
    double want, got;
    int erange_want, erange_got;
    unsigned long long bw, bg;

    memcpy(copy, s, len + 1);
    errno = 0;
    want = strtod(copy, &end_want);
    erange_want = errno == ERANGE;
    errno = 0;
    got = libnix_fmt_strtod(copy, &end_got);
    erange_got = errno == ERANGE;
    memcpy(&bw, &want, sizeof(bw));
    memcpy(&bg, &got, sizeof(bg));
    /* glibc on arm64 tests for tininess before rounding and sets ERANGE
       when a value under DBL_MIN rounds up to it; macOS and glibc on
       x86_64 test after rounding, as libnix_fmt_strtod() does */
    if (fabs(want) == DBL_MIN)
        erange_want = 0;
    checks++;
    if (!(bw == bg || (isnan(want) && isnan(got))) || end_got != end_want ||
        erange_got != erange_want)
    {
        if (failures++ < 20)
            printf("FAIL strtod(\"%.80s\"%s): got %a, end %ld, ERANGE %d; expected %a, "
                   "end %ld, ERANGE %d\n", copy, len > 80 ? "..." : "", got,
                   (long)(end_got - copy), erange_got, want, (long)(end_want - copy),
                   erange_want);
    }
    free(copy);
}

/* strtod of a case where the hosts differ from C99 or from each other */
static void strtod_expect(const char *s, unsigned long long bits, long end, int erange)
{
    char *end_got;
    double got;
    unsigned long long bg;

    errno = 0;
    got = libnix_fmt_strtod(s, &end_got);
    memcpy(&bg, &got, sizeof(bg));
    checks++;
    if (bg != bits || end_got - s != end || (errno == ERANGE) != erange)
    {
        if (failures++ < 20)
            printf("FAIL strtod(\"%s\"): got %016llx, end %ld, ERANGE %d; expected %016llx, "
                   "end %ld, ERANGE %d\n", s, bg, (long)(end_got - s), errno == ERANGE,
                   bits, end, erange);
    }
}

/* Exact decimals of the doubles in a fixed layout of FIXED_FRAC fraction
   digits, more than the 1074 that the smallest subnormal needs, so that
   sums and halves of them stay exact. */
#define FIXED_FRAC 1080
#define FIXED_SIZE 1500

static void fixed_of(char *out, double v)
{
    snprintf(out, FIXED_SIZE, "%.*f", FIXED_FRAC, v);
}

/* out = (a + b) / halve: the digits right-aligned, the point kept */
static void fixed_add(char *out, const char *a, const char *b, int halve)
{
    char da[FIXED_SIZE], db[FIXED_SIZE], sum[FIXED_SIZE];
    size_t la = 0, lb = 0, n, i;
    int carry = 0, rem = 0;
    const char *p;

    for (p = a; *p; p++)
        if (*p != '.')
            da[la++] = *p;
    for (p = b; *p; p++)
        if (*p != '.')
            db[lb++] = *p;
    n = (la > lb ? la : lb) + 1;
    for (i = 0; i < n; i++)
    {
        int d = carry + (i < la ? da[la - 1 - i] - '0' : 0) + (i < lb ? db[lb - 1 - i] - '0' : 0);

        sum[n - 1 - i] = (char)('0' + d % 10);
        carry = d / 10;
    }
    if (halve)
    {
        for (i = 0; i < n; i++)
        {
            int d = rem * 10 + (sum[i] - '0');

            sum[i] = (char)('0' + d / 2);
            rem = d % 2;
        }
    }
    for (i = 0; i + 1 < n - FIXED_FRAC && sum[i] == '0'; i++)
        ;
    memcpy(out, sum + i, n - FIXED_FRAC - i);
    out[n - FIXED_FRAC - i] = '.';
    memcpy(out + n - FIXED_FRAC - i + 1, sum + n - FIXED_FRAC, FIXED_FRAC);
    out[n - i + 1] = '\0';
}

/* The midpoint of v and the next double up, and the decimals one unit of
   the last fixed digit above and below it: the midpoint rounds to the even
   one, above it to the upper one and below it to v. */
static void strtod_midpoint(double v, double up, const char *up_fixed)
{
    static char a[FIXED_SIZE], b[FIXED_SIZE], mid[FIXED_SIZE];
    size_t n;
    int i;

    fixed_of(a, v);
    if (!up_fixed)
    {
        fixed_of(b, up);
        up_fixed = b;
    }
    fixed_add(mid, a, up_fixed, 1);
    strtod_compare(mid);
    n = strlen(mid);
    mid[n - 1] = '1';
    strtod_compare(mid);
    mid[n - 1] = '0';
    for (i = (int)n - 1; mid[i] == '0' || mid[i] == '.'; i--)
        if (mid[i] == '0')
            mid[i] = '9';
    mid[i]--;
    strtod_compare(mid);
}

/* Spellings of one value with the point moved and zeros around it */
static void strtod_spellings(double v)
{
    char e[64], digits[32], s[160];
    int x, nd = 0;
    const char *p;

    snprintf(e, sizeof(e), "%.16e", v);
    strtod_compare(e);
    for (p = e; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9')
            digits[nd++] = *p;
    digits[nd] = '\0';
    x = atoi(p + 1);
    snprintf(s, sizeof(s), "%s%se%d", e[0] == '-' ? "-" : "", digits, x - 16);
    strtod_compare(s);
    snprintf(s, sizeof(s), "%s0.%se%d", e[0] == '-' ? "-" : "", digits, x + 1);
    strtod_compare(s);
    snprintf(s, sizeof(s), "%s0000.00000%se%d", e[0] == '-' ? "-" : "", digits, x + 6);
    strtod_compare(s);
    snprintf(s, sizeof(s), "%s%c.%s000000000000000000000000e%d", e[0] == '-' ? "-" : "",
             digits[0], digits + 1, x);
    strtod_compare(s);
    snprintf(s, sizeof(s), "%s%s00000000000000000000000.e%d", e[0] == '-' ? "-" : "", digits,
             x - 39);
    strtod_compare(s);
}

static void strtod_cases(unsigned long long cases)
{
    static const char *const fixed[] = {
        /* the case of the review, its spellings and the neighbours of 2^53 */
        "9007199254740991.0", "9007199254740991.00", "90071992547409910e-1",
        "900719925474099.1e1", "0.9007199254740991e16", "9007199254740992",
        "9007199254740993", "9007199254740994", "9007199254740995", "9007199254740993.0000001",
        "18014398509481983", "18014398509481985", "18014398509481987",
        /* 19 and 20 digits, around 2^63 and 2^64 */
        "9223372036854775807", "9223372036854775808", "9223372036854776832",
        "9223372036854776833", "18446744073709551615", "18446744073709551616",
        "1234567890123456789", "12345678901234567890", "0.1234567890123456789012345",
        /* subnormals and the normal boundary */
        "4.9406564584124654e-324", "4.9406564584124654417656879286822137236505980e-324",
        "2.4703282292062327e-324", "2.4703282292062328e-324", "2.4703282292062327209e-324",
        "1e-323", "1e-320", "1e-310", "2.2250738585072009e-308", "2.2250738585072011e-308",
        "2.2250738585072012e-308", "2.2250738585072013e-308", "2.2250738585072014e-308",
        "2.225073858507201136057409796709131975934819546351645648e-308", "1e-324", "1e-400",
        "-1e-400", "0.0000000000000000000000000000000000000001e-290",
        /* overflow */
        "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
        "1.797693134862315807937289714053e308", "1e308", "1e309", "1e400", "-1e400",
        "1e99999999", "1e999999999999999999999", "1e-999999999999999999999",
        "0.0000e999999999999", "0.0000000001e318", "10000000000e298",
        /* zeros, plain values and white space */
        "0", "-0", "+0", "0.0e10", "0e-99999999", "000", "0.", ".0", "-.0e5", "3.14159",
        "1e23", "8.98846567431158e307", "123.456", "123456e-3", "0.000123456e6", "1e0",
        "100", "1E5", "1e+5", "1e-5", ".5", "5.", " \t\n\v\f\r1",
        /* where the number ends */
        "", ".", "+", "-", "+.", "-.", "e5", ".e5", "1e", "1e+", "1e-", "1e-x", "1ex",
        "1x", "1.2.3", "1..2", "--1", "+-1", "- 1", "1 2", "1e5x", "1e+05", "0x", "0xg",
        /* infinities and NaNs */
        "inf", "INF", "Inf", "infinity", "INFINITY", "infin", "infinit", "infinityx",
        "-inf", "+inf", "nan", "NAN", "nan(", "nan()", "nan(abc)", "nan(ABC_123)",
        "nan(abc", "nanx", "-nan", "+nan", "in", "na", "i", "n"
    };
    static const char two_1024[] =      /* 2^1024, the double after DBL_MAX */
        "1797693134862315907729305190789024733617976978942306572734300811577326758055"
        "0096313270847732240753602112011387987139335765878976881441662249284743063947"
        "4124377767893424865485276302219601246094119453082952085005768838150682342462"
        "8814739131105408272371633505106845862982399472459384797163048353563296242241"
        "37216";
    static char big[FIXED_SIZE], tmp[FIXED_SIZE];
    size_t i;
    int k;

    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        strtod_compare(fixed[i]);

    /* C99 where the hosts are not: the libc of macOS takes any bytes up
       to ')' in nan(...), and strtod() here reads no hex */
    strtod_expect("nan(a b)", 0x7ff8000000000000ULL, 3, 0);
    strtod_expect("nan(a-b)", 0x7ff8000000000000ULL, 3, 0);
    strtod_expect("-nan(x)", 0xfff8000000000000ULL, 7, 0);
    strtod_expect("0x1p3", 0, 1, 0);
    strtod_expect("-0x10", 0x8000000000000000ULL, 2, 0);
    strtod_expect("2.2250738585072013e-308", 0x0010000000000000ULL, 23, 0);

    /* long strings: 767 to 1000 digits, a midpoint with a non-zero digit
       only at position 900, the same with only zeros, and zero runs */
    for (k = 0; k < 4; k++)
    {
        static const int lengths[4] = { 767, 800, 801, 1000 };
        int n;

        for (n = 0; n < lengths[k]; n++)
            big[n] = (char)('1' + (n * 7 + k) % 9);
        snprintf(big + n, sizeof(big) - (size_t)n, "e-%d", lengths[k] - 1 + 300);
        strtod_compare(big);
        snprintf(big + n, sizeof(big) - (size_t)n, "e%d", 300 - lengths[k]);
        strtod_compare(big);
    }
    strcpy(big, "9007199254740993");
    memset(big + 16, '0', 883);
    strcpy(big + 899, "1e-884");
    strtod_compare(big);
    big[899] = '0';
    strtod_compare(big);
    strcpy(big, "1.");
    memset(big + 2, '0', 1200);
    strcpy(big + 1202, "1e-5");
    strtod_compare(big);
    strcpy(big, "0.");
    memset(big + 2, '0', 1300);
    strcpy(big + 1302, "4941e1000");
    strtod_compare(big);

    /* midpoints: fixed ones, then random ones of every exponent */
    strtod_midpoint(0.0, 4.9406564584124654e-324, NULL);
    strtod_midpoint(4.9406564584124654e-324, 2 * 4.9406564584124654e-324, NULL);
    strtod_midpoint(nextafter(DBL_MIN, 0.0), DBL_MIN, NULL);
    strtod_midpoint(DBL_MIN, nextafter(DBL_MIN, 1.0), NULL);
    strtod_midpoint(9007199254740992.0, 9007199254740994.0, NULL);
    strtod_midpoint(nextafter(DBL_MAX, 0.0), DBL_MAX, NULL);
    snprintf(tmp, sizeof(tmp), "%s.%0*d", two_1024, FIXED_FRAC, 0);
    strtod_midpoint(DBL_MAX, 0.0, tmp);
    for (i = 0; i < cases / 200 + 50; i++)
    {
        unsigned long long bits = next() & 0x7fffffffffffffffULL;
        double v = from_bits(bits);

        if (bits >= 0x7fefffffffffffffULL)
            continue;
        strtod_midpoint(v, nextafter(v, INFINITY), NULL);
        bits = next() & 0x000fffffffffffffULL;      /* subnormal */
        strtod_midpoint(from_bits(bits), from_bits(bits + 1), NULL);
    }

    /* random doubles in several layouts and spellings, random digits */
    for (i = 0; i < cases; i++)
    {
        static const char *const layouts[] = { "%.17g", "%.16g", "%.15g", "%.25e", "%.3e", "%.20g" };
        unsigned long long bits = next();
        double v = from_bits(bits);
        char s[96], *p = s;
        int n = 1 + (int)(next() % 40), j;

        if (isfinite(v))
        {
            snprintf(s, sizeof(s), layouts[i % 6], v);
            strtod_compare(s);
            if (i % 8 == 0)
                strtod_spellings(v);
        }
        for (j = 0; j < n; j++)
            *p++ = (char)('0' + next() % 10);
        if (next() & 1)
        {
            int at = (int)(next() % (unsigned)(n + 1));

            memmove(s + at + 1, s + at, (size_t)(n - at));
            s[at] = '.';
            p++;
        }
        snprintf(p, sizeof(s) - (size_t)(p - s), "e%d", (int)(next() % 681) - 350);
        strtod_compare(s);
    }
}

int main(int argc, char **argv)
{
    static const char *const fixed_specs[] = {
        "%f", "%.0f", "%.1f", "%.2f", "%.17f", "%.99f", "%.300f", "%#.0f", "%+f", "% f",
        "%020.2f", "%-20.2f", "%e", "%.0e", "%.17e", "%#.0e", "%E", "%+.3e", "%g", "%.0g",
        "%#.0g", "%.1g", "%.15g", "%.17g", "%.30g", "%#g", "%G", "%25.0f", "%-+#012.4g", "%08.3e",
        /* carries of %g into a new digit, where glibc %#g drops the zeros */
        "%-+ #0.2g", "%#.1g", "%#.2g", "%#.4g", "%#.6G", "%.6g", "%#12.3g", "%#-12.5G"
    };
    static const double fixed_values[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, 3.5, 0.125, 0.375, 0.1, 0.2, 0.3, 1.25,
        -1.25, 9.9995, 99.5, 999999.5, 1e-5, 0.0001, 123456.0, 1234567.0, 1e15, 1e16,
        1234567890123456.0, 9007199254740992.0, 9007199254740993.0, -9007199254740991.0,
        1e21, 1e22, 1e23, 1e100, 1e300, 1e308, DBL_MAX, -DBL_MAX, DBL_MIN, -DBL_MIN,
        4.9406564584124654e-324, 2.2250738585072009e-308, 5e-324 * 3, 1.0 / 3.0,
        2.0 / 3.0, 3.14159265358979, 2.718281828459045, 6.02214076e23, 1.602e-19,
        /* their neighbours: carries up, negative, and at small exponents */
        99.9140625, -999999.5, 999999.4, 9.5, 0.95, 0.095, 99999.95, 99.95, -99.95,
        9.9999995e-5, 0.00009999995, 9.99999e-5, 0.0099999, 9.999999e22, 1e-4
    };
    unsigned long long cases = argc > 1 ? strtoull(argv[1], NULL, 10) : 200000;
    static const char flags[] = "-+ #0";
    static const char convs[] = "eEfFgG";
    size_t i, j;

    for (i = 0; i < sizeof(fixed_specs) / sizeof(fixed_specs[0]); i++)
    {
        for (j = 0; j < sizeof(fixed_values) / sizeof(fixed_values[0]); j++)
            compare(fixed_specs[i], fixed_values[j]);
        compare(fixed_specs[i], INFINITY);
        compare(fixed_specs[i], -INFINITY);
        compare(fixed_specs[i], NAN);
        compare(fixed_specs[i], -NAN);
    }
    long_cases();
    strtod_cases(cases);

    for (; cases > 0; cases--)
    {
        unsigned long long r = next(), bits;
        char spec[32];
        size_t n = 0;
        int f;
        double v;

        switch (r & 3)
        {
        case 0:     /* any bit pattern */
            bits = next();
            break;
        case 1:     /* an integer of up to 2^64 */
            v = (double)(next() >> (next() & 63));
            memcpy(&bits, &v, sizeof(bits));
            break;
        case 2:     /* a decimal with few digits, where ties appear */
            v = (double)(long long)(next() % 2000001 - 1000000) / (double)(1ULL << (next() % 12));
            memcpy(&bits, &v, sizeof(bits));
            break;
        default:    /* small and large exponents */
            bits = (next() & 0x800fffffffffffffULL) | ((next() % 2047) << 52);
            break;
        }
        v = from_bits(bits);
        spec[n++] = '%';
        for (f = 0; f < 5; f++)
            if ((r >> (8 + f)) & 1)
                spec[n++] = flags[f];
        if ((r >> 16) & 1)
            n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%u", (unsigned)((r >> 20) % 40));
        if ((r >> 17) & 1)
        {
            static const unsigned precs[] = { 0, 1, 2, 3, 5, 6, 9, 14, 15, 16, 17, 20, 30, 50, 99, 340 };

            n += (size_t)snprintf(spec + n, sizeof(spec) - n, ".%u", precs[(r >> 28) % 16]);
        }
        spec[n++] = convs[(r >> 32) % 6];
        spec[n] = '\0';
        compare(spec, v);
    }

    printf("%s: %llu checks, %llu failed\n", "test_libnix_fmt", checks, failures);
    return failures != 0;
}

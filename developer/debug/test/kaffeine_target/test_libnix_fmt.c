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
*/

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

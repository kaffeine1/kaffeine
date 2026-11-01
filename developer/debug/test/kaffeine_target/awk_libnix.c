/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The runtime, locale, snprintf() and strtod() helpers of the one true awk
    for its m68k build with libnix.
    - libnix (-noixemul) does not define environ, which tran.c reads in
      envinit(), and declares popen() and pclose() in <stdio.h> while providing
      only an EIO stub for popen() in libnix20.a and no pclose().
    - libnix's _ctype_ table marks ISO-8859-1 bytes 161..255 as punctuation or
      letters, which b.c's POSIX character-class expansion (1..UCHAR_MAX) feeds
      as raw high bytes into cclenter()'s UTF-8 decoder u8_rune();
      awk_libnix_init_locale() restricts _ctype_ to 7-bit ASCII for the C
      locale.
    - In libnix, __vfprintf_total_size() keeps a 20-digit buffer for floating
      conversions, miscomputes the field width on %e/%E (padding %12.3e one
      space short and ignoring %-12.3e), switches %g/%G to exponential style
      only when exp10 < -5 instead of exp10 < -4, writes zero padding before
      the sign, and drifts after the 16th digit of large values;
      awk_libnix_snprintf() hands every single %e, %f or %g conversion, the
      "%.30g" of tostring() among them, to the exact formatter of
      libnix_fmt.c, which also returns the C99 length of long results.
    - In libnix, strtod() calls sscanf("%lf%ln"), whose vfscanf() stores 0.0
      and advances endptr past "." when no digits are present;
      awk_libnix_strtod() parses decimal floats, infinities and NaNs and leaves
      *endptr at nptr when no number matches.
*/

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libnix_fmt.h"

#undef snprintf
#undef strtod

static char *empty_env[] = { NULL };
char **environ = empty_env;

int pclose(FILE *stream)
{
    (void)stream;
    errno = EIO;
    return -1;
}

void awk_libnix_init_locale(void)
{
#ifdef __mc68000__
    static char ascii_ctype[257];
    static int initialized;

    if (!initialized)
    {
        memcpy(ascii_ctype, _ctype_, 129);
        memset(ascii_ctype + 129, 0, 128);
        _ctype_ = ascii_ctype;
        initialized = 1;
    }
#endif
}

int awk_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    int r;
    size_t flen = strlen(fmt);

    /* format() and tostring() pass one conversion at a time */
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

static int match_ci(const char *s, const char *word, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (tolower((unsigned char)s[i]) != word[i])
            return 0;
    }
    return 1;
}

double awk_libnix_strtod(const char *nptr, char **endptr)
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

    if (match_ci(p, "inf", 3))
    {
        p += 3;
        if (match_ci(p, "inity", 5))
            p += 5;
        if (endptr)
            *endptr = (char *)p;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if (match_ci(p, "nan", 3))
    {
        p += 3;
        if (*p == '(')
        {
            const char *q = p + 1;

            while (*q && *q != ')' && !isspace((unsigned char)*q))
                q++;
            if (*q == ')')
                p = q + 1;
        }
        if (endptr)
            *endptr = (char *)p;
        return NAN;
    }

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

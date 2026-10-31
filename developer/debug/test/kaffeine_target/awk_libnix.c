/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The runtime, snprintf() and strtod() helpers of the one true awk for its
    m68k build with libnix. libnix (-noixemul) does not define environ, which
    tran.c reads in envinit(), and declares popen() and pclose() in <stdio.h>
    while providing only an EIO stub for popen() in libnix20.a and no pclose().
    In libnix, __vfprintf_total_size() keeps a 20-digit buffer for floating
    conversions and leaves the excess precision count of %.30g (which tran.c
    uses for integral doubles) in its trailing-zero counter, appending
    ".0000000000" to every integer, and writes zero padding before the sign on
    negative floats. In libnix, strtod() calls sscanf("%lf%ln"), whose
    vfscanf() stores 0.0 and advances endptr past "." when no digits are
    present; awk_libnix_strtod() parses decimal floats, infinities and NaNs and
    leaves *endptr at nptr when no number matches.
*/

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

int awk_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    int r;
    size_t len;

    if (strcmp(fmt, "%.30g") == 0)
    {
        double v, ip;

        va_start(ap, fmt);
        v = va_arg(ap, double);
        va_end(ap);

        if (isfinite(v) && modf(v, &ip) == 0.0 &&
            v >= -9007199254740992.0 && v <= 9007199254740992.0)
        {
            if (v == 0.0)
                return snprintf(buf, sz, signbit(v) ? "-0" : "0");
            return snprintf(buf, sz, "%lld", (long long)v);
        }
        return snprintf(buf, sz, "%.17g", v);
    }

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

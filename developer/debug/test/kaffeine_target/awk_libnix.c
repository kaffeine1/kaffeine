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
      only when exp10 < -5 instead of exp10 < -4, and writes zero padding
      before the sign; awk_libnix_snprintf() formats floating-point conversions
      with exact C99 %e/%f/%g rules and applies field width and padding itself.
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

static void clamp_f_digits(char *s)
{
    size_t i = (s[0] == '+' || s[0] == '-' || s[0] == ' ') ? 1 : 0;
    size_t digits = 0;

    for (; s[i] != '\0' && isdigit((unsigned char)s[i]); i++)
    {
        digits++;
        if (digits > DBL_DIG)
            s[i] = '0';
    }
}

static int format_float_conv(char *buf, size_t sz, const char *fmt, double v)
{
    const char *p = fmt + 1;
    int flag_minus = 0, flag_plus = 0, flag_space = 0, flag_alt = 0, flag_zero = 0;
    int width = 0, prec = -1;
    char spec;
    char cfmt[16];
    char core[512];
    size_t ci = 0, core_len, pad = 0, total, pos = 0;

    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
    {
        if (*p == '-')
            flag_minus = 1;
        else if (*p == '+')
            flag_plus = 1;
        else if (*p == ' ')
            flag_space = 1;
        else if (*p == '#')
            flag_alt = 1;
        else if (*p == '0')
            flag_zero = 1;
        p++;
    }
    while (isdigit((unsigned char)*p))
    {
        if (width < 10000)
            width = width * 10 + (*p - '0');
        p++;
    }
    if (*p == '.')
    {
        p++;
        prec = 0;
        while (isdigit((unsigned char)*p))
        {
            if (prec < 10000)
                prec = prec * 10 + (*p - '0');
            p++;
        }
    }
    spec = *p;
    if (p[1] != '\0')
        return -1;

    cfmt[ci++] = '%';
    if (flag_plus)
        cfmt[ci++] = '+';
    else if (flag_space)
        cfmt[ci++] = ' ';

    if (!isfinite(v))
    {
        cfmt[ci++] = (spec == 'E' || spec == 'F' || spec == 'G') ? 'E' : 'e';
        cfmt[ci] = '\0';
        snprintf(core, sizeof(core), cfmt, v);
        flag_zero = 0;
    }
    else if (spec == 'e' || spec == 'E')
    {
        int ep = (prec < 0) ? 6 : (prec > 17 ? 17 : prec);

        if (flag_alt)
            cfmt[ci++] = '#';
        strcpy(cfmt + ci, ".*");
        ci += 2;
        cfmt[ci++] = spec;
        cfmt[ci] = '\0';
        snprintf(core, sizeof(core), cfmt, ep, v);
    }
    else if (spec == 'f' || spec == 'F')
    {
        int fp = (prec < 0) ? 6 : prec;

        if (flag_alt)
            cfmt[ci++] = '#';
        strcpy(cfmt + ci, ".*f");
        snprintf(core, sizeof(core), cfmt, fp, v);
        clamp_f_digits(core);
    }
    else if (spec == 'g' || spec == 'G')
    {
        int gp = (prec < 0) ? 6 : (prec == 0 ? 1 : prec);
        int ep = (gp > 17 ? 17 : gp) - 1;
        char efmt[16];
        char *eptr;
        int exp10 = 0;

        memcpy(efmt, cfmt, ci);
        if (flag_alt)
            efmt[ci++] = '#';
        strcpy(efmt + ci, ".*");
        efmt[ci + 2] = (spec == 'G') ? 'E' : 'e';
        efmt[ci + 3] = '\0';
        snprintf(core, sizeof(core), efmt, ep, v);

        eptr = strchr(core, (spec == 'G') ? 'E' : 'e');
        if (eptr != NULL)
            exp10 = atoi(eptr + 1);

        if (eptr != NULL && (exp10 < -4 || exp10 >= gp))
        {
            if (!flag_alt)
            {
                char *q = eptr;

                while (q > core && q[-1] == '0')
                    q--;
                if (q > core && q[-1] == '.')
                    q--;
                memmove(q, eptr, strlen(eptr) + 1);
            }
        }
        else
        {
            int fp = gp - 1 - exp10;
            char ffmt[16];
            size_t fi = 0;

            if (fp < 0)
                fp = 0;
            ffmt[fi++] = '%';
            if (flag_plus)
                ffmt[fi++] = '+';
            else if (flag_space)
                ffmt[fi++] = ' ';
            if (flag_alt)
                ffmt[fi++] = '#';
            strcpy(ffmt + fi, ".*f");
            snprintf(core, sizeof(core), ffmt, fp, v);
            clamp_f_digits(core);
            if (!flag_alt && strchr(core, '.') != NULL)
            {
                size_t clen = strlen(core);

                while (clen > 0 && core[clen - 1] == '0')
                    clen--;
                if (clen > 0 && core[clen - 1] == '.')
                    clen--;
                core[clen] = '\0';
            }
        }
    }
    else
    {
        return -1;
    }

    core_len = strlen(core);
    if (width > (int)core_len)
        pad = (size_t)(width - (int)core_len);
    total = core_len + pad;

    if (sz > 0)
    {
        size_t cap = sz - 1;

        if (!flag_minus && !flag_zero)
        {
            while (pad > 0 && pos < cap)
            {
                buf[pos++] = ' ';
                pad--;
            }
        }
        if (!flag_minus && flag_zero)
        {
            const char *cp = core;

            if (*cp == '-' || *cp == '+' || *cp == ' ')
            {
                if (pos < cap)
                    buf[pos++] = *cp;
                cp++;
            }
            while (pad > 0 && pos < cap)
            {
                buf[pos++] = '0';
                pad--;
            }
            while (*cp != '\0' && pos < cap)
                buf[pos++] = *cp++;
        }
        else
        {
            const char *cp = core;

            while (*cp != '\0' && pos < cap)
                buf[pos++] = *cp++;
            if (flag_minus)
            {
                while (pad > 0 && pos < cap)
                {
                    buf[pos++] = ' ';
                    pad--;
                }
            }
        }
        buf[pos] = '\0';
    }
    return (int)total;
}

int awk_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap;
    int r;
    size_t flen;

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
        return format_float_conv(buf, sz, "%.17g", v);
    }

    flen = strlen(fmt);
    if (flen >= 2 && fmt[0] == '%' && strchr(fmt + 1, '%') == NULL &&
        strchr(fmt + 1, '*') == NULL &&
        strchr("eEfFgG", fmt[flen - 1]) != NULL)
    {
        double v;

        va_start(ap, fmt);
        v = va_arg(ap, double);
        va_end(ap);

        r = format_float_conv(buf, sz, fmt, v);
        if (r >= 0)
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

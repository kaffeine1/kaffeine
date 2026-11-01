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
      and advances endptr past "." when no digits are present, and rounds
      after 16 decimal digits; awk_libnix_strtod(), which awk_config.h also
      gives atof() for split(), is the exact libnix_fmt_strtod(), with
      ERANGE set as the hosts set it, which is_valid_number() reads.
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
#undef atof

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

double awk_libnix_strtod(const char *nptr, char **endptr)
{
    return libnix_fmt_strtod(nptr, endptr);
}

double awk_libnix_atof(const char *nptr)
{
    return libnix_fmt_strtod(nptr, NULL);
}

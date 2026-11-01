/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    One exact %e, %f or %g conversion of a double, and an exact strtod(),
    for the ports that the battery builds with libnix (libnix_fmt.c).
*/

#ifndef LIBNIX_FMT_H
#define LIBNIX_FMT_H

#include <stddef.h>

/* Formats v with spec, a single conversion "%[flags][width][.prec]C" where
   C is one of e, E, f, F, g and G, into buf as snprintf() does: at most
   sz - 1 bytes and a NUL when sz is not 0. Returns the length of the whole
   conversion, -1 when it would exceed INT_MAX, or -2 when spec is not such
   a conversion. */
int libnix_fmt_double(char *buf, size_t sz, const char *spec, double v);

/* strtod() of C99 for the "C" locale, decimal only: white space (space and
   \t to \r), an optional sign, then digits with at most one point and an
   optional exponent, or inf, infinity, nan or nan(n-char-sequence) in any
   case. The result is the nearest double, ties to even; errno is ERANGE
   when it overflows to an infinity, or when it is zero or subnormal and
   not exact. With no conversion, *endptr is nptr and the result +0. */
double libnix_fmt_strtod(const char *nptr, char **endptr);

#endif

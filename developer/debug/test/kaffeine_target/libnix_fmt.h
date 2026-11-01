/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    One exact %e, %f or %g conversion of a double, for the ports that the
    battery builds with libnix (libnix_fmt.c).
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

#endif

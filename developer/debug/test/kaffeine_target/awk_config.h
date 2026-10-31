/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The configuration header of the one true awk (2ndEdition, 20230911),
    included in front of each file for both the m68k and the host build. It
    renames awk's concat() so that it does not collide with the variadic
    concat() declared in libnix's <string.h>, supplies FOPEN_MAX when <stdio.h>
    omits it, and restricts setlocale() to the C locale so that MB_CUR_MAX and
    the decimal point stay identical on every host and on AmigaOS. On AmigaOS
    it undefines SA_SIGINFO (which newlib's <sys/signal.h> defines without
    sa_sigaction or FPE_INTDIV), maps the wait status macros of run.c's
    system() builtin to the unshifted return code of libnix's SystemTagList()
    call, maps srandom()/random() to srand()/rand(), and routes snprintf() and
    strtod() on m68k to awk_libnix.c.
*/

#ifndef AWK_CONFIG_H
#define AWK_CONFIG_H

#include <locale.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define concat awk_concat

#ifndef FOPEN_MAX
#define FOPEN_MAX 40
#endif

static inline char *awk_c_setlocale(int cat, const char *locale)
{
    (void)cat;
    if (locale == NULL || strcmp(locale, "C") == 0 || strcmp(locale, "") == 0)
        return (char *)"C";
    return NULL;
}

#define setlocale awk_c_setlocale

#ifdef __AMIGA__
#undef SA_SIGINFO
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef WCOREDUMP
#define WIFEXITED(w)   ((w) >= 0)
#define WEXITSTATUS(w) (w)
#define WIFSIGNALED(w) 0
#define WTERMSIG(w)    0
#define srandom(s)     srand((unsigned int)(s))
#define random()       ((long)rand())
#endif

#ifdef __mc68000__
extern int awk_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...);
extern double awk_libnix_strtod(const char *nptr, char **endptr);
#undef snprintf
#define snprintf awk_libnix_snprintf
#undef strtod
#define strtod awk_libnix_strtod
#endif

#endif

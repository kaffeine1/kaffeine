/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The configuration header of the one true awk (20260426), included in
    front of each file for both the m68k and the host build. It renames awk's
    concat() so that it does not collide with the variadic concat() declared
    in libnix's <string.h>, supplies FOPEN_MAX when <stdio.h> omits it, and
    restricts setlocale() and MB_CUR_MAX to the single-byte C locale so that
    awk_mb_cur_max is 1 and tolower()/toupper() do not enter libnix's stub
    mbtowc() (where ___locale_mb_cur_max() is 255, __mb_cur_max is 4, and
    mbtowc() returns 4 on every call, including at the terminating NUL). On
    AmigaOS it undefines SA_SIGINFO (which newlib's <sys/signal.h> defines
    without sa_sigaction or FPE_INTDIV), maps the wait status macros of
    run.c's system() builtin to the unshifted return code of libnix's
    SystemTagList() call, maps srandom()/random() to srand()/rand(), and
    routes locale initialization, snprintf() and strtod() on m68k to
    awk_libnix.c.
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

#undef MB_CUR_MAX
#define MB_CUR_MAX 1

#ifdef __mc68000__
extern void awk_libnix_init_locale(void);
#endif

static inline char *awk_c_setlocale(int cat, const char *locale)
{
    (void)cat;
#ifdef __mc68000__
    awk_libnix_init_locale();
#endif
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

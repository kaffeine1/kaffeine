/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The timezone glue for the m68k libnix build of Info-ZIP UnZip 6.0.
    libnix20.a(__gmtoffset.o) registers an __INIT_LIST__ constructor that
    opens locale.library and sets __gmtoffset from the guest's default
    locale. Defining __gmtoffset, __dstflag, __timezone, __daylight and
    tzset() here satisfies localtime(), mktime() and dos_to_unix_time()
    with a fixed UTC0 offset of 0 without pulling in that constructor, so
    unzip converts MS-DOS archive timestamps identically on the guest and
    under TZ=UTC0 on the host.
*/

long __gmtoffset = 0;
int __dstflag = 0;
long *__timezone = &__gmtoffset;
int *__daylight = &__dstflag;

void tzset(void)
{
}

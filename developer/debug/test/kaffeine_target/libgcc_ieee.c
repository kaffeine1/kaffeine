/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The two soft-float conversions that the floating point printf() of
    libnix (libm.a) calls and that the m68k-amigaos toolchain used for these
    tests lacks: its libgcc.a is empty. libnix builds its other double
    helpers in libm.a, such as __adddf3(), on mathieeedoubbas.library, so
    these two do the same with IEEEDPFix() and IEEEDPFlt(). A C cast from
    double to int truncates toward zero, as IEEEDPFix() does.
*/

#include <exec/types.h>
#include <proto/mathieeedoubbas.h>

long __fixdfsi(double value)
{
    return IEEEDPFix(value);
}

double __floatsidf(long value)
{
    return IEEEDPFlt(value);
}

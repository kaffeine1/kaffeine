/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The soft-float conversions that the ported programs, bzip2 1.0.8 and the
    IJG JPEG library 9f, call beyond those of libgcc_ieee.c: the empty
    libgcc of the m68k-amigaos toolchain lacks them. They use the IEEE
    libraries, as libnix does for its other float and double helpers in
    libm.a. bzip2 converts integers to float and unsigned integers to
    double for its statistics; the JPEG library converts between integers
    and float for its floating point DCT and its quantization tables.
*/

#include <exec/types.h>
#include <proto/mathieeedoubbas.h>
#include <proto/mathieeesingbas.h>

float __floatsisf(long value)
{
    return IEEESPFlt(value);
}

/* A C cast from float to int truncates toward zero, as IEEESPFix() does. */
long __fixsfsi(float value)
{
    return IEEESPFix(value);
}

/* IEEEDPFlt() takes a signed LONG: convert half the value, double it and
   add the low bit back. */
double __floatunsidf(unsigned long value)
{
    double half = IEEEDPFlt((long)(value >> 1));
    double twice = IEEEDPAdd(half, half);

    return (value & 1) ? IEEEDPAdd(twice, IEEEDPFlt(1)) : twice;
}

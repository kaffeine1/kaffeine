/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    What bzip2 1.0.8 needs beyond libnix to run as a target program.
    bzip2 restores the permissions and the owner of an output file with
    fchmod() and fchown(), which libnix lacks; AmigaOS has neither, so they
    succeed without doing anything. Its statistics convert integers to
    float and unsigned integers to double, and the empty libgcc of the
    toolchain lacks both helpers. They use the IEEE libraries, as libnix
    does for its other float and double helpers in libm.a.
*/

#include <sys/types.h>
#include <exec/types.h>
#include <proto/mathieeedoubbas.h>
#include <proto/mathieeesingbas.h>

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

float __floatsisf(long value)
{
    return IEEESPFlt(value);
}

/* IEEEDPFlt() takes a signed LONG: convert half the value, double it and
   add the low bit back. */
double __floatunsidf(unsigned long value)
{
    double half = IEEEDPFlt((long)(value >> 1));
    double twice = IEEEDPAdd(half, half);

    return (value & 1) ? IEEEDPAdd(twice, IEEEDPFlt(1)) : twice;
}

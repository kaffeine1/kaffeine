/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    What bzip2 1.0.8 needs beyond libnix to run as a target program.
    bzip2 restores the permissions and the owner of an output file with
    fchmod() and fchown(), which libnix lacks; AmigaOS has neither, so they
    succeed without doing anything. The float conversions of its statistics
    are in libgcc_ieee_ports.c, which the JPEG library shares.
*/

#include <sys/types.h>

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

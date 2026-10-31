/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Configuration header for Info-ZIP UnZip 6.0, included in front of every
    translation unit with -include when building the Unix port (unix/unix.c)
    for the m68k with libnix and for the host. m68k-amigaos-gcc predefines
    AMIGA, which would select amiga/amiga.h for SAS/C and Aztec C; undefining
    AMIGA and defining UNIX selects unix/unxcfg.h instead.

    NO_OFF_T keeps zoff_t as long on both 32-bit m68k and 64-bit hosts so
    fzofft() formats offsets with the same %ld/%lu width specifiers.
    NO_FCHOWN, NO_FCHMOD, NO_LCHOWN and NO_LCHMOD make close_outfile() close
    each extracted file first and then set its modification time with utime()
    and its permissions with chmod(), which libnix implements on top of
    dos.library/SetFileDate and dos.library/SetProtection.
*/

#ifndef KAFFEINE_UNZIP_CONFIG_H
#define KAFFEINE_UNZIP_CONFIG_H

#undef AMIGA
#define UNIX 1
#define MODERN 1
#define DIRENT
#define GOT_UTIMBUF
#define HAVE_TERMIOS_H 1
#define NO_OFF_T 1
#define NO_FCHOWN 1
#define NO_FCHMOD 1
#define NO_LCHOWN 1
#define NO_LCHMOD 1
#define NO_SETLOCALE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>

#ifdef __mc68000__
extern long __gmtoffset;
#define timezone __gmtoffset
void tzset(void);
int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
#endif

#endif /* KAFFEINE_UNZIP_CONFIG_H */

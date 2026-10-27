/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    jconfig.h of the IJG JPEG library 9f for m68k-amigaos-gcc with libnix,
    after the jconfig.sas of the distribution for SAS/C on the Amiga. The
    Makefile copies it over the jconfig.h of the source tree, which the AROS
    build writes for its own host. The programs read and write through the
    C library only: jmemnobs.c keeps everything in memory, so there is no
    temporary file, and the command line is that of Unix, with standard
    input and output when no file is named.
*/

#define HAVE_PROTOTYPES
#define HAVE_UNSIGNED_CHAR
#define HAVE_UNSIGNED_SHORT
#undef CHAR_IS_UNSIGNED
#define HAVE_STDDEF_H
#define HAVE_STDLIB_H
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN

#ifdef JPEG_INTERNALS
#undef RIGHT_SHIFT_IS_UNSIGNED
#endif

#ifdef JPEG_CJPEG_DJPEG
#define BMP_SUPPORTED
#define GIF_SUPPORTED
#define PPM_SUPPORTED
#undef RLE_SUPPORTED
#define TARGA_SUPPORTED
#undef TWO_FILE_COMMANDLINE
#undef NEED_SIGNAL_CATCHER
#undef DONT_USE_B_MODE
#undef PROGRESS_REPORT
#endif

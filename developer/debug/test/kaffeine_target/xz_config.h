/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The config.h of XZ Utils 5.8.3 for xz, built for the m68k with libnix
    and for the host from the same sources; the Makefile copies it to
    $(XZ_BUILD)/config.h. It holds what the configure of xz finds on both,
    with every encoder, decoder, match finder and integrity check, and
    leaves out what only one of them has: threads, the CRC instructions of
    some processors, the width of wide characters and the ways to find the
    memory and the processors of the machine. Both builds then behave the
    same, and xz takes ASSUME_RAM MiB as the memory of the machine. The
    m68k build gives WORDS_BIGENDIAN on its command line.
*/

#define PACKAGE "xz"
#define PACKAGE_NAME "XZ Utils"
#define PACKAGE_BUGREPORT "xz@tukaani.org"
#define PACKAGE_URL "https://tukaani.org/xz/"
#define ASSUME_RAM 128
#define NDEBUG 1

#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE__BOOL 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_GETOPT_H 1
#define HAVE_GETOPT_LONG 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_UTIME 1
#define HAVE___BUILTIN_BSWAPXX 1
#define HAVE___BUILTIN_ASSUME_ALIGNED 1
#define HAVE_VISIBILITY 0

#define HAVE_ENCODERS 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_ENCODER_DELTA 1
#define HAVE_ENCODER_X86 1
#define HAVE_ENCODER_POWERPC 1
#define HAVE_ENCODER_IA64 1
#define HAVE_ENCODER_ARM 1
#define HAVE_ENCODER_ARMTHUMB 1
#define HAVE_ENCODER_ARM64 1
#define HAVE_ENCODER_SPARC 1
#define HAVE_ENCODER_RISCV 1

#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_DECODER_X86 1
#define HAVE_DECODER_POWERPC 1
#define HAVE_DECODER_IA64 1
#define HAVE_DECODER_ARM 1
#define HAVE_DECODER_ARMTHUMB 1
#define HAVE_DECODER_ARM64 1
#define HAVE_DECODER_SPARC 1
#define HAVE_DECODER_RISCV 1
#define HAVE_LZIP_DECODER 1

#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1

#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

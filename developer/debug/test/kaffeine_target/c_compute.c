/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Compiled C code that needs few library calls: libnix's argv, qsort(),
    strtol() and string functions, a CRC32, and 32 and 64-bit integer
    arithmetic. Run it as

        c_compute one "two words"

    The libnix stdio startup and the libnix malloc() use exec list
    functions (see c_stdio.c), so the program uses neither; the results are
    printed through dos.library.

    Built twice. The 68000 build (c_compute) also checks what programs
    with floating point find: SysBase->AttnFlags, and the IEEE math
    libraries, which libnix's math functions call. Moira decodes FPU
    instructions but does not execute them, so AttnFlags must not report an
    FPU. The build for the 68020 with 68881 instructions (c_compute_fpu)
    adds double precision arithmetic; its libnix startup code (___fpucheck)
    requires AFF_68020 and AFF_68881 in AttnFlags, and without an FPU it
    stops before main() with an Alert and return code 20. The 68000 build
    leaves floating point and 64-bit division out, because the libgcc of
    the m68k-amigaos toolchain used for these tests is empty and the
    software helpers for them are missing.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>

#include "m68ktest.h"

#define VALUES 200

static ULONG crc_table[256];
static UBYTE data[4096];
static LONG values[VALUES];

static int compare_long(const void *a, const void *b)
{
    LONG x = *(const LONG *)a, y = *(const LONG *)b;

    return x < y ? -1 : x > y;
}

static ULONG crc32(const UBYTE *p, ULONG size)
{
    ULONG crc = 0xffffffff, i, j, c;

    for (i = 0; i < 256; i++)
    {
        for (c = i, j = 0; j < 8; j++)
            c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    for (i = 0; i < size; i++)
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffff;
}

/* volatile keeps the compiler from working the results out itself */
static volatile LONG a32 = -123456789, b32 = 1000, c32 = -12345, d32 = 6789;
static volatile unsigned long long a64 = 0x0123456789abcdefULL;
#ifdef __HAVE_68881__
static volatile double one = 1.0, two = 2.0;

/* Within 1e-9 of the expected value, whatever the rounding mode */
static BOOL check_double(const char *what, double got, double expected)
{
    BOOL ok = got - expected < 1e-9 && expected - got < 1e-9;

    Printf("     %s: %ld millionths\n", (LONG)what, (LONG)(got * 1e6));
    return check(ok, what);
}
#else
static BOOL check_library(const char *name)
{
    struct Library *base = OpenLibrary((CONST_STRPTR)name, 0);

    Printf("     OpenLibrary(\"%s\"): 0x%08lx\n", (LONG)name, (LONG)base);
    if (base)
        CloseLibrary(base);
    return check(base != NULL, name);
}
#endif

int main(int argc, char **argv)
{
    unsigned long long p64;
#ifdef __HAVE_68881__
    double sum;
#endif
    char buf[64], *end;
    LONG i, sum32;
    BOOL ok;

    check_long("argc", argc, 3);
    if (argc == 3)
    {
        check_str("argv[1]", argv[1], "one");
        check_str("argv[2]", argv[2], "two words");
    }
    check(argv[0] && argv[0][0], "argv[0] is not empty");

    /* On the 68000, libnix divides and multiplies 32-bit numbers with
       utility.library: SDivMod32() returns the remainder in D1 */
    check_long("-123456789 / 1000", a32 / b32, -123456);
    check_long("-123456789 % 1000", a32 % b32, -789);
    check_long("-12345 * 6789", c32 * d32, -83810205);
    check_long("0x80000000 >> 5, unsigned", (LONG)(0x80000000UL >> (d32 & 7)), 0x04000000);
    check_long("-12345 >> 4, signed", c32 >> 4, -772);

    p64 = a64 * 7;
    check_long("64-bit product, high LONG", (LONG)(p64 >> 32), 0x07f6e5d4);
    check_long("64-bit product, low LONG", (LONG)p64, (LONG)0xc3b2a189);
    p64 = (p64 << 4) + (a64 >> 12);
    check_long("64-bit shifts and sum, high LONG", (LONG)(p64 >> 32), 0x7f6e6f80);
    check_long("64-bit shifts and sum, low LONG", (LONG)p64, (LONG)0x91a2b34c);

    for (i = 0; i < (LONG)sizeof(data); i++)
        data[i] = (UBYTE)((i * 31 + 7) ^ (i >> 3));
    check_long("CRC32 of 4096 bytes", (LONG)crc32(data, sizeof(data)), (LONG)0xab060335);

    /* Without a division, which the 68000 build leaves to utility.library */
    for (i = 0, sum32 = 0; i < VALUES; i++)
        sum32 += values[i] = ((i * 7919) & 255) - 128;
    qsort(values, VALUES, sizeof(values[0]), compare_long);
    for (i = 1, ok = TRUE; i < VALUES; i++)
        if (values[i - 1] > values[i])
            ok = FALSE;
    for (i = 0; i < VALUES; i++)
        sum32 -= values[i];
    check(ok && sum32 == 0, "qsort() of 200 LONGs");

    check_long("strtol(\"-0x7f\", 16)", strtol("-0x7f", &end, 16), -127);
    check_long("strtol(\"12345xyz\", 10)", strtol("12345xyz", &end, 10), 12345);
    check_str("strtol() stops at", end, "xyz");

    strcpy(buf, "0123456789");
    memmove(buf + 2, buf, 8);
    check_str("memmove() of overlapping bytes", buf, "0101234567");
    check(strstr("the quick brown fox", "brown") != NULL, "strstr()");
    check(strncmp("abcdef", "abcxyz", 3) == 0 && strcmp("abc", "abd") < 0, "strncmp() and strcmp()");

#ifdef __HAVE_68881__
    /* Double precision arithmetic in 68881 instructions */
    for (i = 1, sum = 0.0; i <= 1000; i++)
        sum += one / (double)i;
    check_double("sum of 1/i for i = 1..1000", sum, 7.485470860550345);
    check_double("sqrt(2) with fsqrt", __builtin_sqrt(two), 1.4142135623730951);
    check_double("(2 - 1/3) * 3", (two - one / 3.0) * 3.0, 5.0);
    check_long("(LONG)(1000.25 * 4)", (LONG)(1000.25 * 4.0 * one), 4001);

    return test_done("c_compute_fpu");
#else
    /* What programs with floating point find: the CPU, and no FPU while
       Moira cannot execute FPU instructions. An FPU flag would make FPU
       builds run code that the emulation cannot. */
    check((SysBase->AttnFlags & AFF_68020) != 0, "AttnFlags has AFF_68020");
    check((SysBase->AttnFlags & (AFF_68881 | AFF_68882 | AFF_FPU40)) == 0,
          "AttnFlags reports no FPU, which Moira does not execute");
    Printf("     AttnFlags: 0x%04lx\n", (LONG)SysBase->AttnFlags);
    check_library("mathieeedoubbas.library");
    check_library("mathieeedoubtrans.library");

    return test_done("c_compute");
#endif
}

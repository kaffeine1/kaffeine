/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    A libnix C program with stdio: argc and argv, printf() and sprintf()
    formats, malloc(), calloc(), realloc() and free(), strdup(), qsort() and
    strtol(), and stderr. Run it as

        c_stdio one "two words" *>errors

    libnix takes stderr from pr_CES of the process, which the Shell sets for
    *>, and opens the console only without one: the program checks that
    the error stream reaches pr_CES and writes one line to stderr.

    libnix keeps the malloc() heap in a list of MemHeaders that it manages
    with the exec functions AllocMem(), Allocate(), Deallocate(), AddHead()
    and Remove(), and its stdio startup code calls malloc() before main().
    Built with -DMALLOC_ON_ALLOCVEC, the program brings its own malloc()
    family on top of AllocVec() and FreeVec(), which kaffeine serves from its
    own heap, so the rest of the C library can be tested on its own.

    Both builds link the floating point printf() of libnix from libm.a,
    which computes through mathieeedoubbas.library, and libgcc_ieee.c for
    the two conversions it needs that the toolchain's libgcc lacks. Without
    libm.a, the printf() of libnix.a has no floating point formats and
    copies them to the output as they are.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos/dosextens.h>
#include <proto/exec.h>

#define SLOTS 16

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

static void check_str(const char *what, const char *got, const char *expected)
{
    checks++;
    if (got && strcmp(got, expected) == 0)
    {
        printf("ok   %s: \"%s\"\n", what, got);
        fflush(stdout);
        return;
    }
    failures++;
    printf("FAIL %s: got \"%s\", expected \"%s\"\n", what, got ? got : "(null)", expected);
    fflush(stdout);
}

#ifdef MALLOC_ON_ALLOCVEC
#include <exec/memory.h>

/* Each block starts with its size, for realloc(), and a spare LONG that
   keeps the result on the alignment of the AllocVec() block. */
void *malloc(size_t size)
{
    ULONG *p = AllocVec(size + 8, MEMF_ANY);

    if (!p)
        return NULL;
    p[0] = size;
    return p + 2;
}

void free(void *ptr)
{
    if (ptr)
        FreeVec((ULONG *)ptr - 2);
}

void *calloc(size_t count, size_t size)
{
    void *p = malloc(count * size);

    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *old, size_t size)
{
    void *p;
    size_t oldsize;

    if (!old)
        return malloc(size);
    p = malloc(size);
    if (p)
    {
        oldsize = ((ULONG *)old)[-2];
        memcpy(p, old, oldsize < size ? oldsize : size);
        free(old);
    }
    return p;
}
#endif

static int compare_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return x < y ? -1 : x > y;
}

static int all_bytes(const unsigned char *p, size_t size, unsigned char value)
{
    size_t i;

    for (i = 0; i < size; i++)
        if (p[i] != value)
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    static char buf[256];
    unsigned char *slot[SLOTS] = { NULL };
    size_t size[SLOTS] = { 0 };
    int values[100];
    unsigned char *p;
    char *s, *end;
    int i, j, ok;

    printf("c_stdio: argc %d\n", argc);
    for (i = 0; i < argc; i++)
        printf("     argv[%d] \"%s\"\n", i, argv[i]);
    fflush(stdout);
    check(argc == 3, "argc is 3");
    if (argc == 3)
    {
        check_str("argv[1]", argv[1], "one");
        check_str("argv[2]", argv[2], "two words");
    }
    check(argv[0] && argv[0][0], "argv[0] is not empty");

    sprintf(buf, "%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o|%u",
            42, 42, 42, 42, 42, 255, 255, 255, 8, 4000000000u);
    check_str("integer formats", buf, "42|   42|42   |00042|+42|ff|FF|0xff|10|4000000000");
    sprintf(buf, "%ld|%lu|%c|%s|%8s|%-8s|%.2s|%%",
            -123456789L, 3000000000UL, 'x', "str", "right", "left", "cut");
    check_str("long, character and string formats", buf, "-123456789|3000000000|x|str|   right|left    |cu|%");
    sprintf(buf, "%.3f|%8.2f|%.2e", 3.14159, -2.5, 12345.678);
    check_str("floating point formats", buf, "3.142|   -2.50|1.23e+04");

    /* Blocks of changing sizes stay intact while others come and go */
    ok = 1;
    for (i = 0; i < 300 && ok; i++)
    {
        j = i % SLOTS;
        if (slot[j])
        {
            ok = all_bytes(slot[j], size[j], (unsigned char)j);
            free(slot[j]);
        }
        size[j] = 1 + (size_t)(i * 97) % 4000;
        slot[j] = malloc(size[j]);
        if (!slot[j])
            ok = 0;
        else
            memset(slot[j], j, size[j]);
    }
    check(ok, "300 malloc()/free() rounds keep the live blocks intact");
    for (j = 0; j < SLOTS; j++)
        free(slot[j]);

    p = calloc(100, 10);
    check(p && all_bytes(p, 1000, 0), "calloc(100, 10) is zero");
    if (p)
    {
        memset(p, 0x3c, 1000);
        p = realloc(p, 5000);
        check(p && all_bytes(p, 1000, 0x3c), "realloc() to 5000 bytes keeps the first 1000");
        free(p);
    }

    s = strdup("duplicated string");
    check_str("strdup()", s, "duplicated string");
    free(s);

    for (i = 0; i < 100; i++)
        values[i] = (i * 7919) % 100;
    qsort(values, 100, sizeof(values[0]), compare_int);
    for (i = 0, ok = 1; i < 100; i++)
        if (values[i] != i)
            ok = 0;
    check(ok, "qsort() of 100 ints");

    check(strtol("-0x7f", &end, 16) == -127 && *end == 0, "strtol(\"-0x7f\", 16) is -127");
    check(strtol("12345xyz", &end, 10) == 12345 && strcmp(end, "xyz") == 0, "strtol(\"12345xyz\", 10) stops at xyz");

    check(((struct Process *)FindTask(NULL))->pr_CES != 0,
          "the error stream of the Shell (*>) reaches pr_CES of the 68k process");
    fputs("c_stdio: fputs() to stderr\n", stderr);

    fputs("c_stdio: fputs() to stdout\n", stdout);
    fflush(stdout);

    printf("c_stdio: %d checks, %d failed\n", checks, failures);
    return failures ? 10 : 0;
}

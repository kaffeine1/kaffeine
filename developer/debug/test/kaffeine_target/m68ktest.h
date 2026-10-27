/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Check helpers for the kaffeine target test programs. Every check prints
    one line that starts with "ok" or "FAIL", and the program ends with a
    summary line. Output goes through dos.library (PutStr() and VPrintf()),
    so the helpers need neither stdio nor malloc. Printf() passes every
    argument as a LONG, so the formats use %ld, %lx and %s only. Each line is
    flushed, so that the lines printed before a crash or a hang reach the
    file the output is redirected to.
*/

#ifndef M68KTEST_H
#define M68KTEST_H

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#define TEST_UNUSED __attribute__((unused))

static LONG test_checks, test_failures;

static TEST_UNUSED void test_flush(void)
{
    Flush(Output());
}

static TEST_UNUSED BOOL check(BOOL ok, const char *what)
{
    test_checks++;
    if (!ok)
        test_failures++;
    PutStr((CONST_STRPTR)(ok ? "ok   " : "FAIL "));
    PutStr((CONST_STRPTR)what);
    PutStr((CONST_STRPTR)"\n");
    test_flush();
    return ok;
}

static TEST_UNUSED BOOL check_long(const char *what, LONG got, LONG expected)
{
    test_checks++;
    if (got == expected)
    {
        Printf("ok   %s: %ld\n", (LONG)what, got);
        test_flush();
        return TRUE;
    }
    test_failures++;
    Printf("FAIL %s: got %ld (0x%08lx), expected %ld (0x%08lx)\n",
           (LONG)what, got, got, expected, expected);
    test_flush();
    return FALSE;
}

static TEST_UNUSED BOOL check_str(const char *what, const char *got, const char *expected)
{
    const char *a = got, *b = expected;

    test_checks++;
    if (a)
        while (*a && *a == *b)
            a++, b++;
    if (a && *a == *b)
    {
        Printf("ok   %s: \"%s\"\n", (LONG)what, (LONG)got);
        test_flush();
        return TRUE;
    }
    test_failures++;
    if (got)
        Printf("FAIL %s: got \"%s\", expected \"%s\"\n", (LONG)what, (LONG)got, (LONG)expected);
    else
        Printf("FAIL %s: got NULL, expected \"%s\"\n", (LONG)what, (LONG)expected);
    test_flush();
    return FALSE;
}

/* Prints the summary line and returns the program's return code. */
static TEST_UNUSED LONG test_done(const char *name)
{
    Printf("%s: %ld checks, %ld failed\n", (LONG)name, test_checks, test_failures);
    test_flush();
    return test_failures ? RETURN_ERROR : RETURN_OK;
}

#endif /* M68KTEST_H */

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    utility.library Amiga2Date(), Date2Amiga() and CheckDate() on ClockData
    structures in 68k memory, called from a libnix C program that uses
    neither stdio nor malloc.
*/

#include <exec/types.h>
#include <utility/date.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "m68ktest.h"

struct Library *UtilityBase;

struct KnownDate
{
    const char *label;
    ULONG seconds;
    struct ClockData cd;
};

static const struct KnownDate known_dates[] =
{
    { "1978-01-01 00:00:00 (epoch)",        0UL,          {  0,  0,  0,  1,  1, 1978, 0 } },
    { "1978-01-01 00:00:01",                1UL,          {  1,  0,  0,  1,  1, 1978, 0 } },
    { "1978-12-31 23:59:59",                31535999UL,   { 59, 59, 23, 31, 12, 1978, 0 } },
    { "1980-12-31 23:59:59 (leap year)",    94694399UL,   { 59, 59, 23, 31, 12, 1980, 3 } },
    { "2000-02-29 00:00:00 (leap day)",     699321600UL,  {  0,  0,  0, 29,  2, 2000, 2 } },
    { "2000-02-29 12:34:56 (leap day)",     699366896UL,  { 56, 34, 12, 29,  2, 2000, 2 } },
    { "2000-12-31 23:59:59",                725846399UL,  { 59, 59, 23, 31, 12, 2000, 0 } },
    { "2100-02-28 23:59:59 (non-leap 100)", 3855081599UL, { 59, 59, 23, 28,  2, 2100, 0 } },
    { "2100-03-01 00:00:00 (non-leap 100)", 3855081600UL, {  0,  0,  0,  1,  3, 2100, 1 } },
    { "2114-02-07 06:28:15 (0xFFFFFFFF)",   0xFFFFFFFFUL, { 15, 28,  6,  7,  2, 2114, 3 } }
};

static const ULONG roundtrip_fixed[] =
{
    0UL, 1UL, 59UL, 60UL, 3599UL, 3600UL, 86399UL, 86400UL,
    31535999UL, 31536000UL, 63071999UL, 63072000UL,
    68169599UL, 68169600UL, 68255999UL, 68256000UL,
    94694399UL, 94694400UL,
    699235199UL, 699235200UL, 699321599UL, 699321600UL, 699407999UL, 699408000UL,
    725846399UL, 725846400UL,
    3849983999UL, 3849984000UL,
    3855081599UL, 3855081600UL,
    0x7FFFFFFFUL, 0x80000000UL, 0xFFFFFFFEUL, 0xFFFFFFFFUL
};

static BOOL same_clockdata(const struct ClockData *a, const struct ClockData *b)
{
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
           a->mday == b->mday && a->month == b->month && a->year == b->year &&
           a->wday == b->wday;
}

static void check_known(const struct KnownDate *kd)
{
    struct ClockData got;

    got.sec = 0xAAAA;
    got.min = 0xAAAA;
    got.hour = 0xAAAA;
    got.mday = 0xAAAA;
    got.month = 0xAAAA;
    got.year = 0xAAAA;
    got.wday = 0xAAAA;

    Amiga2Date(kd->seconds, &got);
    if (!check(same_clockdata(&got, &kd->cd), kd->label))
    {
        Printf("     got %ld-%02ld-%02ld %02ld:%02ld:%02ld wday=%ld\n",
               (LONG)got.year, (LONG)got.month, (LONG)got.mday,
               (LONG)got.hour, (LONG)got.min, (LONG)got.sec, (LONG)got.wday);
        test_flush();
    }
    check_long(kd->label, (LONG)Date2Amiga(&got), (LONG)kd->seconds);
    check_long(kd->label, (LONG)CheckDate(&got), (LONG)kd->seconds);
}

static void check_invalid_date(const char *what, UWORD sec, UWORD min, UWORD hour,
                               UWORD mday, UWORD month, UWORD year, UWORD wday)
{
    struct ClockData cd;

    cd.sec = sec;
    cd.min = min;
    cd.hour = hour;
    cd.mday = mday;
    cd.month = month;
    cd.year = year;
    cd.wday = wday;
    check_long(what, (LONG)CheckDate(&cd), 0);
}

int main(void)
{
    struct ClockData cd;
    ULONG i, seed, s;
    BOOL rt_ok;

    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37);
    if (!check(UtilityBase != NULL, "OpenLibrary() of utility.library"))
        return test_done("utility_dates");

    for (i = 0; i < sizeof(known_dates) / sizeof(known_dates[0]); i++)
        check_known(&known_dates[i]);

    rt_ok = TRUE;
    for (i = 0; i < sizeof(roundtrip_fixed) / sizeof(roundtrip_fixed[0]); i++)
    {
        s = roundtrip_fixed[i];
        Amiga2Date(s, &cd);
        if (Date2Amiga(&cd) != s || CheckDate(&cd) != s)
            rt_ok = FALSE;
    }
    check(rt_ok, "Date2Amiga(Amiga2Date(s)) == s and CheckDate(Amiga2Date(s)) == s on boundary values");

    seed = 0x9E3779B9UL;
    rt_ok = TRUE;
    for (i = 0; i < 200; i++)
    {
        seed = seed * 1664525UL + 1013904223UL;
        s = seed;
        Amiga2Date(s, &cd);
        if (Date2Amiga(&cd) != s || CheckDate(&cd) != s)
            rt_ok = FALSE;
    }
    check(rt_ok, "Date2Amiga(Amiga2Date(s)) == s and CheckDate(Amiga2Date(s)) == s on 200 random values");

    check_invalid_date("CheckDate() rejects month 0",  0,  0, 12, 15,  0, 2000, 0);
    check_invalid_date("CheckDate() rejects month 13", 0,  0, 12, 15, 13, 2000, 0);
    check_invalid_date("CheckDate() rejects day 0",    0,  0, 12,  0,  5, 2000, 0);
    check_invalid_date("CheckDate() rejects April 31", 0,  0, 12, 31,  4, 2000, 0);
    check_invalid_date("CheckDate() rejects June 31",  0,  0, 12, 31,  6, 2000, 0);
    check_invalid_date("CheckDate() rejects Sept 31",  0,  0, 12, 31,  9, 2000, 0);
    check_invalid_date("CheckDate() rejects Nov 31",   0,  0, 12, 31, 11, 2000, 0);
    check_invalid_date("CheckDate() rejects Jan 32",   0,  0, 12, 32,  1, 2000, 0);
    check_invalid_date("CheckDate() rejects Feb 30 (leap year)",     0, 0, 12, 30, 2, 2000, 0);
    check_invalid_date("CheckDate() rejects Feb 30 (non-leap year)", 0, 0, 12, 30, 2, 1979, 0);
    check_invalid_date("CheckDate() rejects hour 24",  0,  0, 24, 15,  5, 2000, 0);
    check_invalid_date("CheckDate() rejects min 60",   0, 60, 12, 15,  5, 2000, 0);
    check_invalid_date("CheckDate() rejects sec 61",  61,  0, 12, 15,  5, 2000, 0);
    check_invalid_date("CheckDate() rejects year 1977", 0, 0, 12, 15,  5, 1977, 0);
    check_invalid_date("CheckDate() rejects wday 7",   0,  0, 12, 15,  5, 2000, 7);

    /* rom/utility/checkdate.c does not check whether Feb 29 is in a leap year */
    cd.sec = 0;
    cd.min = 0;
    cd.hour = 0;
    cd.mday = 29;
    cd.month = 2;
    cd.year = 1979;
    cd.wday = 4;
    check_long("CheckDate() on 1979-02-29 returns Date2Amiga() as AROS checkdate.c does",
               (LONG)CheckDate(&cd), 36633600L);

    cd.month = 0;
    check_long("Date2Amiga() refuses month 0", (LONG)Date2Amiga(&cd), 0);
    cd.month = 13;
    check_long("Date2Amiga() refuses month 13", (LONG)Date2Amiga(&cd), 0);

    Amiga2Date(699366896UL, NULL);
    check_long("Date2Amiga(NULL) returns 0", (LONG)Date2Amiga(NULL), 0);
    check_long("CheckDate(NULL) returns 0", (LONG)CheckDate(NULL), 0);

    CloseLibrary(UtilityBase);
    return test_done("utility_dates");
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Intuition CurrentTime() writes 68k ULONG outputs, bracketed by the
    independently converted timer.device GetSysTime(). Four short samples
    distinguish native clock bytes from swapped outputs without a clock
    setting or a timing benchmark. Raw LVO calls separately check the
    bridge's successful void-call D0 policy, NULL outputs and alias order.

    BADSECOND and BADMICROS deliberately pass an out-of-arena destination.
    They are fixed-module containment probes only; never run them on the
    old generated CurrentTime thunk. The ordinary mode has no bad pointer.
*/
#include <exec/types.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;

struct GuardedTime {
    ULONG before, seconds, between, micros, after;
};
typedef char check_ulong_size[sizeof(ULONG) == 4 ? 1 : -1];
typedef char check_timeval_size[sizeof(struct timeval) == 8 ? 1 : -1];
typedef char check_guard_size[sizeof(struct GuardedTime) == 20 ? 1 : -1];

#define OUTPUT_SENTINEL 0xa1b2c3d4UL
#define D0_SENTINEL     0x7135a0c9UL
#define SAMPLES 4

static void reset_time(struct GuardedTime *g)
{
    g->before = 0x79a531cfUL;
    g->seconds = OUTPUT_SENTINEL;
    g->between = 0x2468ace0UL;
    g->micros = OUTPUT_SENTINEL;
    g->after = 0x53719bdfUL;
}

static BOOL guards_ok(const struct GuardedTime *g)
{
    return g->before == 0x79a531cfUL && g->between == 0x2468ace0UL &&
           g->after == 0x53719bdfUL;
}

static ULONG swapped(ULONG v)
{
    return ((v & 0xffUL) << 24) | ((v & 0xff00UL) << 8) |
           ((v >> 8) & 0xff00UL) | ((v >> 24) & 0xffUL);
}

static LONG compare_time(const struct timeval *a, const struct timeval *b)
{
    if (a->tv_secs != b->tv_secs)
        return a->tv_secs < b->tv_secs ? -1 : 1;
    if (a->tv_micro != b->tv_micro)
        return a->tv_micro < b->tv_micro ? -1 : 1;
    return 0;
}

static BOOL clock_bracket(const struct timeval *a, const struct timeval *b)
{
    return a->tv_micro < 1000000UL && b->tv_micro < 1000000UL &&
           compare_time(a, b) <= 0;
}

static BOOL pair_between(ULONG sec, ULONG micro,
                         const struct timeval *a, const struct timeval *b)
{
    struct timeval value;
    value.tv_secs = sec;
    value.tv_micro = micro;
    return clock_bracket(a, b) && micro < 1000000UL &&
           compare_time(a, &value) <= 0 && compare_time(&value, b) <= 0;
}

/* Projection of a valid bracket onto its seconds or cyclic micros field.
   A bracket spanning a full micros cycle admits every canonical micros
   value; the full-pair samples remain the endian-specific proof. */
static BOOL seconds_between(ULONG sec, const struct timeval *a, const struct timeval *b)
{
    return clock_bracket(a, b) && sec >= a->tv_secs && sec <= b->tv_secs;
}

static BOOL micros_between(ULONG micro, const struct timeval *a, const struct timeval *b)
{
    if (!clock_bracket(a, b) || micro >= 1000000UL)
        return FALSE;
    if (a->tv_secs == b->tv_secs)
        return micro >= a->tv_micro && micro <= b->tv_micro;
    if (b->tv_secs - a->tv_secs == 1)
        return micro >= a->tv_micro || micro <= b->tv_micro;
    return TRUE;
}

/* CurrentTime is void. Keep D0 as an explicit read/write asm operand so
   the compiler cannot discard its input or invent its returned value.
   A0/A1 and D1 are call-clobbered; A6 is the actual Intuition base. */
static ULONG current_time_raw(ULONG *seconds, ULONG *micros, ULONG marker)
{
    register struct IntuitionBase *base __asm("a6") = IntuitionBase;
    register ULONG *arg_seconds __asm("a0") = seconds;
    register ULONG *arg_micros __asm("a1") = micros;
    register ULONG value __asm("d0") = marker;
    __asm__ volatile ("jsr -84(%%a6)"
                      : "+r"(value), "+r"(arg_seconds), "+r"(arg_micros)
                      : "r"(base) : "d1", "cc", "memory");
    return value;
}

static void normal_samples(void)
{
    struct GuardedTime g;
    struct timeval a, b;
    BOOL brackets = TRUE, canonical = TRUE, within = TRUE, guards = TRUE;
    BOOL nonpalindromic = FALSE, big_endian_only = FALSE, swapped_only = FALSE;
    LONG i;

    for (i = 0; i < SAMPLES; ++i) {
        BOOL raw_matches, reversed_matches;
        reset_time(&g);
        GetSysTime(&a);
        CurrentTime(&g.seconds, &g.micros);
        GetSysTime(&b);
        brackets = brackets && clock_bracket(&a, &b);
        canonical = canonical && g.micros < 1000000UL;
        guards = guards && guards_ok(&g);
        raw_matches = pair_between(g.seconds, g.micros, &a, &b);
        reversed_matches = pair_between(swapped(g.seconds), swapped(g.micros), &a, &b);
        within = within && raw_matches;
        nonpalindromic = nonpalindromic || swapped(a.tv_secs) != a.tv_secs ||
                         swapped(a.tv_micro) != a.tv_micro ||
                         swapped(b.tv_secs) != b.tv_secs || swapped(b.tv_micro) != b.tv_micro;
        big_endian_only = big_endian_only || (raw_matches && !reversed_matches);
        swapped_only = swapped_only || (!raw_matches && reversed_matches);
        Delay(1);
    }
    check(brackets, "GetSysTime sample brackets are canonical and ordered");
    check(nonpalindromic, "clock samples include a non-palindromic value");
    check(canonical, "CurrentTime microseconds are below one million");
    check(within, "CurrentTime pairs lie in their GetSysTime brackets");
    check(guards, "CurrentTime preserves all neighboring sentinels");
    check(big_endian_only, "CurrentTime has a big-endian-only bracket match");
    check(!swapped_only, "CurrentTime has no byte-swapped-only bracket match");
}

static void raw_outputs(void)
{
    struct GuardedTime g;
    struct timeval a, b;
    ULONG d0;

    reset_time(&g);
    GetSysTime(&a);
    d0 = current_time_raw(&g.seconds, &g.micros, D0_SENTINEL);
    GetSysTime(&b);
    check(pair_between(g.seconds, g.micros, &a, &b), "raw CurrentTime pair is bracketed");
    check(g.micros < 1000000UL, "raw CurrentTime microseconds are canonical");
    check(guards_ok(&g), "raw CurrentTime preserves neighboring sentinels");
    check(d0 == D0_SENTINEL, "successful CurrentTime preserves D0");

    reset_time(&g);
    GetSysTime(&a);
    d0 = current_time_raw(NULL, &g.micros, D0_SENTINEL);
    GetSysTime(&b);
    check(micros_between(g.micros, &a, &b), "NULL seconds still returns bracketed microseconds");
    check(g.seconds == OUTPUT_SENTINEL, "NULL seconds leaves unused output untouched");
    check(guards_ok(&g), "NULL seconds preserves neighboring sentinels");
    check(d0 == D0_SENTINEL, "NULL seconds preserves D0");

    reset_time(&g);
    GetSysTime(&a);
    d0 = current_time_raw(&g.seconds, NULL, D0_SENTINEL);
    GetSysTime(&b);
    check(seconds_between(g.seconds, &a, &b), "NULL microseconds still returns bracketed seconds");
    check(g.micros == OUTPUT_SENTINEL, "NULL microseconds leaves unused output untouched");
    check(guards_ok(&g), "NULL microseconds preserves neighboring sentinels");
    check(d0 == D0_SENTINEL, "NULL microseconds preserves D0");

    reset_time(&g);
    d0 = current_time_raw(NULL, NULL, D0_SENTINEL);
    check(guards_ok(&g) && g.seconds == OUTPUT_SENTINEL && g.micros == OUTPUT_SENTINEL,
          "both NULL outputs write nothing");
    check(d0 == D0_SENTINEL, "both NULL outputs preserve D0");

    reset_time(&g);
    GetSysTime(&a);
    d0 = current_time_raw(&g.seconds, &g.seconds, D0_SENTINEL);
    GetSysTime(&b);
    check(micros_between(g.seconds, &a, &b), "same-pointer alias keeps the final microseconds write");
    check(g.micros == OUTPUT_SENTINEL, "same-pointer alias leaves unused output untouched");
    check(guards_ok(&g), "same-pointer alias preserves neighboring sentinels");
    check(d0 == D0_SENTINEL, "same-pointer alias preserves D0");
}

static int bad_output(BOOL seconds)
{
    struct GuardedTime g;
    reset_time(&g);
    PutStr((CONST_STRPTR)(seconds ?
        "STEP intuition_currenttime BADSECOND: invalid seconds; expect controlled stop 20\n" :
        "STEP intuition_currenttime BADMICROS: invalid microseconds; expect controlled stop 20\n"));
    test_flush();
    CurrentTime(seconds ? (ULONG *)0xfffffffcUL : &g.seconds,
                seconds ? &g.micros : (ULONG *)0xfffffffcUL);
    PutStr((CONST_STRPTR)"FAIL intuition_currenttime: bad output returned\n");
    test_flush();
    return RETURN_ERROR;
}

int main(int argc, char **argv)
{
    struct MsgPort *port = NULL;
    struct timerequest *tr = NULL;
    BOOL opened = FALSE;
    int negative = 0;

    if (argc == 2 && !strcmp(argv[1], "BADSECOND"))
        negative = 1;
    else if (argc == 2 && !strcmp(argv[1], "BADMICROS"))
        negative = 2;
    else if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "NORMAL"))) {
        PutStr((CONST_STRPTR)"Usage: intuition_currenttime [NORMAL|BADSECOND|BADMICROS]\n");
        return RETURN_ERROR;
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 0);
    if (!check(IntuitionBase != NULL, "OpenLibrary(intuition.library)"))
        return test_done("intuition_currenttime");
    if (negative) {
        int result = bad_output(negative == 1);
        CloseLibrary((struct Library *)IntuitionBase);
        return result;
    }
    port = CreateMsgPort();
    if (!check(port != NULL, "CreateMsgPort()"))
        goto done;
    tr = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!check(tr != NULL, "CreateIORequest(timerequest)"))
        goto done;
    if (!check_long("OpenDevice(timer.device, UNIT_MICROHZ)",
                    OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &tr->tr_node, 0), 0))
        goto done;
    opened = TRUE;
    TimerBase = tr->tr_node.io_Device;
    if (!check(TimerBase != NULL && tr->tr_node.io_Error == 0, "timer.device supplies TimerBase"))
        goto done;
    normal_samples();
    raw_outputs();
done:
    if (opened) {
        CloseDevice(&tr->tr_node);
        check(tr->tr_node.io_Device == (struct Device *)-1, "CloseDevice() clears the timer identity");
    }
    if (tr)
        DeleteIORequest(&tr->tr_node);
    if (port)
        DeleteMsgPort(port);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("intuition_currenttime");
}

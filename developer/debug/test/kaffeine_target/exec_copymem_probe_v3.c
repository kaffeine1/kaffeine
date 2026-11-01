/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Bounded 68k microprobe (v3) comparing 65,536 one-byte
    exec.library/CopyMem() calls against a 65,536-iteration guest byte-copy
    loop on the same deterministic 64-byte source pattern, with 16-byte front
    and back guard sentinels, full 65,536-byte destination verification, and
    bounded timer.device ReadEClock() and GetSysTime() validity checks.
    Relative tick and microsecond timings are printed for observation without
    hardcoding a performance winner check.
*/

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stddef.h>

#include "m68ktest.h"

struct Device *TimerBase;

#define PROBE_N       65536UL
#define GUARD_BYTES   16UL
#define TOTAL_BYTES   (GUARD_BYTES + PROBE_N + GUARD_BYTES)
#define PATTERN_BYTES 64UL

static const char timer_name[] = "timer.device";

static UBYTE expected_front_guard(ULONG idx)
{
    return (UBYTE)(0xA5U ^ (idx * 13U));
}

static UBYTE expected_back_guard(ULONG idx)
{
    return (UBYTE)(0x5AU ^ (idx * 19U));
}

static UBYTE expected_pattern_byte(ULONG idx)
{
    return (UBYTE)(((idx & (PATTERN_BYTES - 1UL)) * 37U + 0x5AU) & 0xFFU);
}

static void prepare_buffer(UBYTE *buf, UBYTE poison)
{
    ULONG i;
    UBYTE *dst = buf + GUARD_BYTES;
    UBYTE *back = dst + PROBE_N;

    for (i = 0; i < GUARD_BYTES; i++)
    {
        buf[i] = expected_front_guard(i);
        back[i] = expected_back_guard(i);
    }
    for (i = 0; i < PROBE_N; i++)
        dst[i] = poison;
}

static void verify_buffer(const UBYTE *buf, const char *label)
{
    ULONG i;
    LONG front_bad = 0, dst_bad = 0, back_bad = 0;
    const UBYTE *dst = buf + GUARD_BYTES;
    const UBYTE *back = dst + PROBE_N;

    for (i = 0; i < GUARD_BYTES; i++)
    {
        if (buf[i] != expected_front_guard(i))
            front_bad++;
        if (back[i] != expected_back_guard(i))
            back_bad++;
    }
    for (i = 0; i < PROBE_N; i++)
    {
        if (dst[i] != expected_pattern_byte(i))
            dst_bad++;
    }

    Printf("     %s validation: front_bad=%ld dst_bad=%ld back_bad=%ld\n",
           (LONG)label, front_bad, dst_bad, back_bad);
    check_long("front sentinel intact", front_bad, 0);
    check_long("all 65536 destination bytes match", dst_bad, 0);
    check_long("back sentinel intact", back_bad, 0);
}

static __attribute__((noinline)) void run_copymem_1b(const UBYTE *src, UBYTE *dst, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        CopyMem((APTR)&src[i & (PATTERN_BYTES - 1UL)], (APTR)&dst[i], 1);
        __asm__ volatile("" : : : "memory");
    }
}

static __attribute__((noinline)) void run_guest_byteloop(const UBYTE *src, UBYTE *dst, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        dst[i] = src[i & (PATTERN_BYTES - 1UL)];
        __asm__ volatile("" : : : "memory");
    }
}

static unsigned long long eclock_delta_ticks(const struct EClockVal *t0, const struct EClockVal *t1)
{
    unsigned long long v0 = ((unsigned long long)t0->ev_hi << 32) | (unsigned long long)t0->ev_lo;
    unsigned long long v1 = ((unsigned long long)t1->ev_hi << 32) | (unsigned long long)t1->ev_lo;

    return v1 - v0;
}

static ULONG systime_delta_us(const struct timeval *t0, const struct timeval *t1)
{
    struct timeval diff = *t1;
    struct timeval start = *t0;

    /* CmpTime(a, b) is 1 when a is earlier than b (backwards clock). */
    if (CmpTime(&diff, &start) > 0)
        return 0xFFFFFFFFUL;
    SubTime(&diff, &start);
    if (diff.tv_secs > 4000 || diff.tv_micro >= 1000000)
        return 0xFFFFFFFFUL;
    return diff.tv_secs * 1000000UL + diff.tv_micro;
}

int main(void)
{
    ULONG avail_before;
    struct MsgPort *port;
    struct timerequest *tr;
    UBYTE *buf;
    UBYTE *dst;
    UBYTE src_pattern[PATTERN_BYTES];
    struct EClockVal ec0, ec1, ec2, ec3;
    struct timeval tv0, tv1, tv2, tv3;
    unsigned long long copymem_ticks, byteloop_ticks;
    ULONG rate1, rate2, rate3, rate4, copymem_us, byteloop_us;
    ULONG i;

    for (i = 0; i < PATTERN_BYTES; i++)
        src_pattern[i] = expected_pattern_byte(i);

    test_flush();
    avail_before = AvailMem(MEMF_ANY);

    port = CreateMsgPort();
    if (!check(port != NULL, "CreateMsgPort()"))
        return test_done("exec_copymem_probe_v3");

    tr = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!check(tr != NULL, "CreateIORequest(timerequest)"))
    {
        DeleteMsgPort(port);
        return test_done("exec_copymem_probe_v3");
    }

    if (!check_long("OpenDevice(timer.device, UNIT_MICROHZ)",
                    OpenDevice((CONST_STRPTR)timer_name, UNIT_MICROHZ, &tr->tr_node, 0), 0))
    {
        DeleteIORequest(&tr->tr_node);
        DeleteMsgPort(port);
        return test_done("exec_copymem_probe_v3");
    }
    TimerBase = tr->tr_node.io_Device;

    buf = (UBYTE *)AllocMem(TOTAL_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    if (!check(buf != NULL, "AllocMem(65568)"))
    {
        CloseDevice(&tr->tr_node);
        DeleteIORequest(&tr->tr_node);
        DeleteMsgPort(port);
        return test_done("exec_copymem_probe_v3");
    }
    dst = buf + GUARD_BYTES;

    prepare_buffer(buf, 0xCCU);
    GetSysTime(&tv0);
    rate1 = ReadEClock(&ec0);
    run_copymem_1b(src_pattern, dst, PROBE_N);
    rate2 = ReadEClock(&ec1);
    GetSysTime(&tv1);
    verify_buffer(buf, "copymem_1b");

    prepare_buffer(buf, 0x33U);
    GetSysTime(&tv2);
    rate3 = ReadEClock(&ec2);
    run_guest_byteloop(src_pattern, dst, PROBE_N);
    rate4 = ReadEClock(&ec3);
    GetSysTime(&tv3);
    verify_buffer(buf, "guest_byteloop");

    copymem_ticks = eclock_delta_ticks(&ec0, &ec1);
    byteloop_ticks = eclock_delta_ticks(&ec2, &ec3);
    copymem_us = systime_delta_us(&tv0, &tv1);
    byteloop_us = systime_delta_us(&tv2, &tv3);

    Printf("     observation: eclock_hz=%ld copymem_ticks_hi=%ld copymem_ticks_lo=%ld copymem_us=%ld "
           "byteloop_ticks_hi=%ld byteloop_ticks_lo=%ld byteloop_us=%ld\n",
           (LONG)rate1,
           (LONG)(copymem_ticks >> 32), (LONG)(copymem_ticks & 0xFFFFFFFFULL), (LONG)copymem_us,
           (LONG)(byteloop_ticks >> 32), (LONG)(byteloop_ticks & 0xFFFFFFFFULL), (LONG)byteloop_us);
    test_flush();

    check(rate1 > 0 && rate1 == rate2 && rate2 == rate3 && rate3 == rate4,
          "ReadEClock() returns a positive stable E-clock rate");
    check((copymem_ticks >> 63) == 0 && (byteloop_ticks >> 63) == 0,
          "ReadEClock() intervals are non-negative (no backwards wrap)");
    check(copymem_us != 0xFFFFFFFFUL && byteloop_us != 0xFFFFFFFFUL,
          "GetSysTime() intervals are within bounded range (not overflow/backwards sentinel)");

    FreeMem(buf, TOTAL_BYTES);
    CloseDevice(&tr->tr_node);
    DeleteIORequest(&tr->tr_node);
    DeleteMsgPort(port);

    check_long("all memory returned", AvailMem(MEMF_ANY), avail_before);
    return test_done("exec_copymem_probe_v3");
}

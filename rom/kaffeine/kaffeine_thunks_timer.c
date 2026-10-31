/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    timer.device behind the guest base that OpenDevice() gives out: the
    time arithmetic on 68k timevals as AROS does it, and GetSysTime() and
    ReadEClock() of the native timer. BeginIO() of the device stops the
    guest: asynchronous I/O goes through Exec's SendIO() alone
    (kaffeine_exec_io.c), whose AbortIO() the device's is.
*/

#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_offsets.h"

#include <stdint.h>
#include <aros/debug.h>
#include <devices/timer.h>
#include <proto/timer.h>

/* 68k timeval and EClockVal: two big-endian ULONGs each. */
enum { TV_SECS = 0, TV_MICRO = 4, TIMEVAL_SIZE = 8, EV_HI = 0, EV_LO = 4 };

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

static BOOL timevals(struct KaffeineContext *ctx, ULONG dest, ULONG src)
{
    return guest_range(ctx, dest, TIMEVAL_SIZE) && guest_range(ctx, src, TIMEVAL_SIZE);
}

/* -30: BeginIO(A1=ioRequest). Called on the device directly, it is
   asynchronous I/O outside SendIO(), which is not supported. */
IPTR kaffeine_timer_BeginIO(struct KaffeineContext *ctx, void *cpu)
{
    bug("[kaffeine] timer.device BeginIO(%08lx)\n", (unsigned long)THUNK_A(1));
    return Kaffeine_IORefuse(ctx, "BeginIO(): asynchronous device I/O goes through SendIO() only");
}

/* -36: AbortIO(A1=ioRequest) -> D0, as Exec's AbortIO(): the native one
   for a request in flight, else -1, as timer.device answers for a request
   it no longer holds. */
IPTR kaffeine_timer_AbortIO(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_AbortIO(ctx, cpu);
}

/* -42: AddTime(A0=dest, A1=src). AROS's ADDTIME: add, then normalize. */
IPTR kaffeine_timer_AddTime(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dest = THUNK_A(0), src = THUNK_A(1), secs, micro;

    if (!timevals(ctx, dest, src))
        return Kaffeine_IORefuse(ctx, "AddTime(): a timeval is not in guest memory");
    micro = m68k_read32(ctx, dest + TV_MICRO) + m68k_read32(ctx, src + TV_MICRO);
    secs = m68k_read32(ctx, dest + TV_SECS) + m68k_read32(ctx, src + TV_SECS);
    while (micro > 999999)
    {
        secs++;
        micro -= 1000000;
    }
    m68k_write32(ctx, dest + TV_SECS, secs);
    m68k_write32(ctx, dest + TV_MICRO, micro);
    return 0;
}

/* -48: SubTime(A0=dest, A1=src). AROS's SUBTIME normalizes src in place,
   then dest, borrows a second when needed and subtracts. src is written
   before dest is read, as the macro does when both are the same timeval. */
IPTR kaffeine_timer_SubTime(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dest = THUNK_A(0), src = THUNK_A(1), dsecs, dmicro, ssecs, smicro;

    if (!timevals(ctx, dest, src))
        return Kaffeine_IORefuse(ctx, "SubTime(): a timeval is not in guest memory");
    ssecs = m68k_read32(ctx, src + TV_SECS);
    smicro = m68k_read32(ctx, src + TV_MICRO);
    while (smicro > 999999)
    {
        ssecs++;
        smicro -= 1000000;
    }
    m68k_write32(ctx, src + TV_SECS, ssecs);
    m68k_write32(ctx, src + TV_MICRO, smicro);
    dsecs = m68k_read32(ctx, dest + TV_SECS);
    dmicro = m68k_read32(ctx, dest + TV_MICRO);
    while (dmicro > 999999)
    {
        dsecs++;
        dmicro -= 1000000;
    }
    if (dmicro < smicro)
    {
        dmicro += 1000000;
        dsecs--;
    }
    m68k_write32(ctx, dest + TV_SECS, dsecs - ssecs);
    m68k_write32(ctx, dest + TV_MICRO, dmicro - smicro);
    return 0;
}

/* -54: CmpTime(A0=dest, A1=src) -> D0: -1 when dest is the later time, 1
   when src is, 0 when they are equal, as AROS's CMPTIME. */
IPTR kaffeine_timer_CmpTime(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dest = THUNK_A(0), src = THUNK_A(1), a, b;

    if (!timevals(ctx, dest, src))
        return Kaffeine_IORefuse(ctx, "CmpTime(): a timeval is not in guest memory");
    a = m68k_read32(ctx, dest + TV_SECS);
    b = m68k_read32(ctx, src + TV_SECS);
    if (a == b)
    {
        a = m68k_read32(ctx, dest + TV_MICRO);
        b = m68k_read32(ctx, src + TV_MICRO);
    }
    return a > b ? (IPTR)(LONG)-1 : a < b ? 1 : 0;
}

/* -60: ReadEClock(A0=dest) -> D0=E-clock frequency. The native E-clock in
   ticks of the guest one, M68K_ECLOCK_RATE (kaffeine_offsets.h): whole
   native seconds and the rest are scaled apart, so that no product needs
   more than 64 bits for any uptime of a running system. */
IPTR kaffeine_timer_ReadEClock(struct KaffeineContext *ctx, void *cpu)
{
    struct Device *TimerBase = ctx->native_timer;
    ULONG dest = THUNK_A(0), rate;
    struct EClockVal ev;
    uint64_t ticks;

    if (!guest_range(ctx, dest, 8))
        return Kaffeine_IORefuse(ctx, "ReadEClock(): the EClockVal is not in guest memory");
    rate = ReadEClock(&ev);
    ticks = (uint64_t)ev.ev_hi << 32 | ev.ev_lo;
    if (rate && rate != M68K_ECLOCK_RATE)
        ticks = ticks / rate * M68K_ECLOCK_RATE + ticks % rate * M68K_ECLOCK_RATE / rate;
    m68k_write32(ctx, dest + EV_HI, (ULONG)(ticks >> 32));
    m68k_write32(ctx, dest + EV_LO, (ULONG)ticks);
    return M68K_ECLOCK_RATE;
}

/* -66: GetSysTime(A0=dest), of the native timer. */
IPTR kaffeine_timer_GetSysTime(struct KaffeineContext *ctx, void *cpu)
{
    struct Device *TimerBase = ctx->native_timer;
    ULONG dest = THUNK_A(0);
    struct timeval tv;

    if (!guest_range(ctx, dest, TIMEVAL_SIZE))
        return Kaffeine_IORefuse(ctx, "GetSysTime(): the timeval is not in guest memory");
    GetSysTime(&tv);
    m68k_write32(ctx, dest + TV_SECS, tv.tv_secs);
    m68k_write32(ctx, dest + TV_MICRO, tv.tv_micro);
    return 0;
}

const struct M68KThunkEntry kaffeine_thunks_timer[] = {
    { 30, kaffeine_timer_BeginIO },
    { 36, kaffeine_timer_AbortIO },
    { 42, kaffeine_timer_AddTime },
    { 48, kaffeine_timer_SubTime },
    { 54, kaffeine_timer_CmpTime },
    { 60, kaffeine_timer_ReadEClock },
    { 66, kaffeine_timer_GetSysTime },
    { 0, NULL }
};
const ULONG kaffeine_thunks_timer_count =
    sizeof(kaffeine_thunks_timer) / sizeof(kaffeine_thunks_timer[0]) - 1;

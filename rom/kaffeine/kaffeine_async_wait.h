/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_ASYNC_WAIT_H
#define KAFFEINE_ASYNC_WAIT_H

#include <exec/ports.h>
#include <proto/exec.h>
#include "kaffeine_async.h"

/* Native root Wait: consume only guest bits; a private wake is pumped and
   never returned, and a simultaneous guest signal survives that pump. */
static inline IPTR Kaffeine_AsyncWaitSignals(struct KaffeineContext *ctx, ULONG mask)
{
    ULONG got;
    mask &= ~Kaffeine_AsyncSignalMask(ctx);
    for (;;)
    {
        if (!Kaffeine_AsyncPoll(ctx)) return 20;
        got = SetSignal(0, mask) & mask;
        if (got) return got;
        got = Wait(mask | Kaffeine_AsyncSignalMask(ctx)) & mask;
        if (got) SetSignal(got, got);
    }
}

/* Peek without removing a native message, as WaitPort does. With no private
   wake provider retain its existing native route. Otherwise wake for either
   the port or bridge events, poll, then recheck its queue. */
static inline struct Message *Kaffeine_AsyncWaitPort(struct KaffeineContext *ctx,
                                                   struct MsgPort *port)
{
    for (;;)
    {
        ULONG hidden;
        if (!Kaffeine_AsyncPoll(ctx)) return NULL;
        hidden = Kaffeine_AsyncSignalMask(ctx);
        if (!hidden) return WaitPort(port);
        if (port->mp_MsgList.lh_TailPred != (struct Node *)&port->mp_MsgList)
            return (struct Message *)port->mp_MsgList.lh_Head;
        Wait((1UL << port->mp_SigBit) | hidden);
    }
}

#endif

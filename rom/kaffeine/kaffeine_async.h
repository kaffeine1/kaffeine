/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Owner-thread delivery of optional asynchronous bridge events.
*/
#ifndef KAFFEINE_ASYNC_H
#define KAFFEINE_ASYNC_H

#include "kaffeine_intern.h"
#include "kaffeine_callback.h"
#include "kaffeine_layers.h"

/* Both providers retain their own masks and native ownership. Never store
   this union in io_signals or guest_signals. */
static inline ULONG Kaffeine_AsyncSignalMask(struct KaffeineContext *ctx)
{
    return ctx->io_signals | Kaffeine_LayerSignalMask(ctx);
}

/* Call only on the stable native driver, at safe slice/thunk/wait points.
   A DOS broker has paused that driver and must not pump its events. Native
   layer poll also validates its captured driver before touching pending
   state. NULL tails perform no native operations. */
static inline BOOL Kaffeine_AsyncPoll(struct KaffeineContext *ctx)
{
    if (ctx->broker_call) return TRUE;
    if ((ctx->io_poll && !ctx->io_poll(ctx)) || !Kaffeine_LayerPoll(ctx))
    {
        if (ctx->cpu) Kaffeine_StopGuest(ctx, ctx->cpu, 20);
        else
        {
            ctx->running = FALSE;
            ctx->callback_failed = TRUE;
            ctx->exit_code = 20;
        }
        return FALSE;
    }
    return TRUE;
}

#endif

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The registry of the screens that the 68k programs of this library
    opened. An IND_WRITEEVENT of any program may name such a screen in an
    IEPointerPixel; Intuition's input handler reads that screen while
    input.device handles the event, and native CloseScreen() frees it on the
    task of the program that closes it, which DoSyncAction() does not always
    make wait for an event already being handled. So each screen has a gate,
    an Exec semaphore of its slot:

    - an input event takes it shared, with AttemptSemaphoreShared(), which
      never waits, under the registry lock, only for an OPEN slot of the
      generation that the program's capture of the screen had, and gives it
      back on the same task as soon as its synchronous DoIO() returns;
    - the program that closes the screen makes the slot CLOSING under the
      registry lock, so that no new event takes the gate, then, with no lock
      of its own held, takes the gate exclusively, which waits for exactly
      the events admitted before, and only then calls CloseScreen().

    CLOSING is needed, not only tidy: AROS grants AttemptSemaphoreShared() to
    a semaphore held shared even while an exclusive request waits, so new
    events could otherwise keep the closing program waiting. The wait asks
    no more of input.device than native CloseScreen() itself does, which
    waits for it too. CloseScreen() of a screen without windows therefore
    closes and returns TRUE, as on AmigaOS, and never because of the input
    of another program returns FALSE.

    The registry lock is a leaf: under it only the slots change, and the
    only Exec call is AttemptSemaphoreShared(). A program may look a
    generation up under LockIBase(); the registry lock comes after it. A
    gate is never set up again: InitSemaphore() runs once for every slot
    here, and a slot that serves another screen keeps it as it is. A slot is
    FREE again only after its drain, when no event holds its gate, and the
    slot of a screen that a program left open at its end stays CLOSING, with
    its generation, for as long as that screen may be open.
*/

#include "kaffeine_intern.h"
#include "kaffeine_owned_screens.h"

#include <proto/exec.h>

void Kaffeine_OwnedScreensInit(struct KaffeineLibBase *base)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    int i;

    InitSemaphore(&r->lock);
    for (i = 0; i < KAFFEINE_MAX_OWNED_SCREENS; i++)
    {
        r->slot[i].native = NULL;
        r->slot[i].generation = 0;
        r->slot[i].state = KAFFEINE_OWNED_FREE;
        InitSemaphore(&r->slot[i].gate);
    }
    r->next_generation = 0;
    r->epoch = 0;
    r->ready = TRUE;
}

static BOOL valid(struct KaffeineOwnedScreens *r, LONG slot)
{
    return r->ready && slot >= 0 && slot < KAFFEINE_MAX_OWNED_SCREENS;
}

static LONG owned_reserve(struct KaffeineLibBase *base, ULONG *generation)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    LONG i, found = -1;

    *generation = 0;
    if (!r->ready)
        return -1;
    ObtainSemaphore(&r->lock);
    /* a generation is never given twice */
    if (r->next_generation != (ULONG)~0UL)
        for (i = 0; i < KAFFEINE_MAX_OWNED_SCREENS; i++)
            if (r->slot[i].state == KAFFEINE_OWNED_FREE)
            {
                found = i;
                break;
            }
    if (found >= 0)
    {
        r->slot[found].native = NULL;
        r->slot[found].generation = *generation = ++r->next_generation;
        r->slot[found].state = KAFFEINE_OWNED_RESERVED;
        r->epoch++;
    }
    ReleaseSemaphore(&r->lock);
    return found;
}

static void owned_bind(struct KaffeineLibBase *base, LONG slot, struct Screen *native)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;

    if (!valid(r, slot))
        return;
    ObtainSemaphore(&r->lock);
    if (r->slot[slot].state == KAFFEINE_OWNED_RESERVED)
    {
        r->slot[slot].native = native;
        r->slot[slot].state = KAFFEINE_OWNED_OPEN;
        r->epoch++;
    }
    ReleaseSemaphore(&r->lock);
}

static void owned_unreserve(struct KaffeineLibBase *base, LONG slot)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;

    if (!valid(r, slot))
        return;
    ObtainSemaphore(&r->lock);
    if (r->slot[slot].state == KAFFEINE_OWNED_RESERVED)
    {
        r->slot[slot].native = NULL;
        r->slot[slot].generation = 0;
        r->slot[slot].state = KAFFEINE_OWNED_FREE;
        r->epoch++;
    }
    ReleaseSemaphore(&r->lock);
}

static BOOL owned_closing(struct KaffeineLibBase *base, LONG slot, ULONG generation)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    BOOL ok = FALSE;

    if (!valid(r, slot) || !generation)
        return FALSE;
    ObtainSemaphore(&r->lock);
    if (r->slot[slot].generation == generation &&
        (r->slot[slot].state == KAFFEINE_OWNED_OPEN || r->slot[slot].state == KAFFEINE_OWNED_CLOSING))
    {
        if (r->slot[slot].state != KAFFEINE_OWNED_CLOSING)
        {
            r->slot[slot].state = KAFFEINE_OWNED_CLOSING;
            r->epoch++;
        }
        ok = TRUE;
    }
    ReleaseSemaphore(&r->lock);
    return ok;
}

/* Exec queues the request and clears SIGF_SINGLE under Forbid() before it
   waits, and ReleaseSemaphore() grants and signals under Forbid(): a
   release before the wait is not lost. */
static void owned_drain(struct KaffeineLibBase *base, LONG slot)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;

    if (valid(r, slot))
        ObtainSemaphore(&r->slot[slot].gate);
}

static void owned_finish(struct KaffeineLibBase *base, LONG slot, BOOL closed, BOOL reopen)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;

    if (!valid(r, slot))
        return;
    /* CLOSING still keeps every new event out */
    ReleaseSemaphore(&r->slot[slot].gate);
    ObtainSemaphore(&r->lock);
    if (r->slot[slot].state == KAFFEINE_OWNED_CLOSING)
    {
        if (closed)
        {
            r->slot[slot].native = NULL;
            r->slot[slot].generation = 0;
            r->slot[slot].state = KAFFEINE_OWNED_FREE;
            r->epoch++;
        }
        else if (reopen)
        {
            r->slot[slot].state = KAFFEINE_OWNED_OPEN;
            r->epoch++;
        }
    }
    ReleaseSemaphore(&r->lock);
}

static ULONG owned_generation(struct KaffeineLibBase *base, const struct Screen *native)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    ULONG generation = 0;
    int i;

    if (!r->ready || !native)
        return 0;
    ObtainSemaphore(&r->lock);
    for (i = 0; i < KAFFEINE_MAX_OWNED_SCREENS; i++)
        if (r->slot[i].native == native && r->slot[i].generation > generation &&
            (r->slot[i].state == KAFFEINE_OWNED_OPEN || r->slot[i].state == KAFFEINE_OWNED_CLOSING))
            generation = r->slot[i].generation;
    ReleaseSemaphore(&r->lock);
    return generation;
}

static BOOL owned_hold(struct KaffeineLibBase *base, const struct Screen *native, ULONG generation,
                       LONG *slot)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    BOOL held = FALSE;
    int i;

    *slot = -1;
    if (!r->ready || !native || !generation)
        return FALSE;
    ObtainSemaphore(&r->lock);
    for (i = 0; i < KAFFEINE_MAX_OWNED_SCREENS; i++)
        if (r->slot[i].native == native && r->slot[i].generation == generation)
        {
            if (r->slot[i].state == KAFFEINE_OWNED_OPEN && AttemptSemaphoreShared(&r->slot[i].gate))
            {
                *slot = i;
                held = TRUE;
            }
            break;
        }
    ReleaseSemaphore(&r->lock);
    return held;
}

static void owned_drop(struct KaffeineLibBase *base, LONG slot)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;

    if (valid(r, slot))
        ReleaseSemaphore(&r->slot[slot].gate);
}

static BOOL owned_foreign(struct KaffeineLibBase *base, const struct Screen *native)
{
    struct KaffeineOwnedScreens *r = &base->owned_screens;
    BOOL foreign = TRUE;
    int i;

    if (!r->ready)
        return TRUE;
    ObtainSemaphore(&r->lock);
    for (i = 0; i < KAFFEINE_MAX_OWNED_SCREENS && foreign; i++)
        if (r->slot[i].state == KAFFEINE_OWNED_RESERVED ||
            (r->slot[i].native == native &&
             (r->slot[i].state == KAFFEINE_OWNED_OPEN || r->slot[i].state == KAFFEINE_OWNED_CLOSING)))
            foreign = FALSE;
    ReleaseSemaphore(&r->lock);
    return foreign;
}

static ULONG owned_epoch(struct KaffeineLibBase *base)
{
    return base->owned_screens.epoch;
}

const struct KaffeineOwnedScreenOps Kaffeine_OwnedScreenOps =
{
    owned_reserve, owned_bind, owned_unreserve, owned_closing, owned_drain, owned_finish,
    owned_generation, owned_hold, owned_drop, owned_foreign, owned_epoch
};

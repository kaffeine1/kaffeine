/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The screens that the 68k programs of this library opened, one registry
    for every context. Each has a slot with a generation, which no other
    screen gets again, and a gate: an Exec semaphore that an input event of
    any program holds shared while input.device handles it, and that the
    program that closes the screen takes exclusively before Intuition frees
    it. See kaffeine_owned_screens.c.
*/
#ifndef KAFFEINE_OWNED_SCREENS_H
#define KAFFEINE_OWNED_SCREENS_H

#include <exec/types.h>
#include <exec/semaphores.h>

struct Screen;
struct KaffeineLibBase;

/* Screens of all programs that are open at the same time */
#define KAFFEINE_MAX_OWNED_SCREENS 64

enum
{
    KAFFEINE_OWNED_FREE,     /* no screen */
    KAFFEINE_OWNED_RESERVED, /* OpenScreenTagList() runs, no native yet */
    KAFFEINE_OWNED_OPEN,     /* open; an input event may hold it */
    KAFFEINE_OWNED_CLOSING   /* closing or left open by a program that ended:
                                no new hold, the generation stays */
};

struct KaffeineOwnedScreen
{
    struct Screen *native;  /* compared, never dereferenced here */
    ULONG generation;       /* 0 when FREE */
    UBYTE state;
    /* InitSemaphore() once, with the library: never cleared or set up
       again, also when the slot serves another screen */
    struct SignalSemaphore gate;
};

struct KaffeineOwnedScreens
{
    /* A leaf: nothing under it waits, calls Intuition or does I/O */
    struct SignalSemaphore lock;
    ULONG next_generation;
    /* Changes with every change of a slot; read without the lock, so that a
       program can tell that nothing changed since it last looked */
    ULONG epoch;
    BOOL ready;
    struct KaffeineOwnedScreen slot[KAFFEINE_MAX_OWNED_SCREENS];
};

/* The registry as a context reaches it (ctx->owned_screen_ops), installed
   with a real library base; a context without it registers no screen and
   gives no input event a screen that a program opened. */
struct KaffeineOwnedScreenOps
{
    /* A FREE slot and a new generation for a screen about to open, or -1
       when every slot is in use */
    LONG (*reserve)(struct KaffeineLibBase *, ULONG *generation);
    /* the native screen of a RESERVED slot: it is OPEN now */
    void (*bind)(struct KaffeineLibBase *, LONG slot, struct Screen *native);
    /* a RESERVED slot whose screen did not open is FREE again */
    void (*unreserve)(struct KaffeineLibBase *, LONG slot);
    /* OPEN to CLOSING, also again for one already CLOSING; FALSE when the
       slot does not have that generation */
    BOOL (*closing)(struct KaffeineLibBase *, LONG slot, ULONG generation);
    /* With no lock of the bridge held: wait until every input event that
       held the screen of a CLOSING slot has been handled, and keep the gate
       until finish() */
    void (*drain)(struct KaffeineLibBase *, LONG slot);
    /* After the native CloseScreen() of a drained slot: the gate goes back;
       a closed screen leaves the slot FREE, one that Intuition did not close
       makes it OPEN again (reopen) or leaves it CLOSING */
    void (*finish)(struct KaffeineLibBase *, LONG slot, BOOL closed, BOOL reopen);
    /* The generation of an OPEN or CLOSING screen, the newest one for that
       native, else 0 */
    ULONG (*generation)(struct KaffeineLibBase *, const struct Screen *native);
    /* An input hold of an OPEN screen of exactly that generation, which
       never waits; drop() gives it back on the same task */
    BOOL (*hold)(struct KaffeineLibBase *, const struct Screen *native, ULONG generation,
                 LONG *slot);
    void (*drop)(struct KaffeineLibBase *, LONG slot);
    /* TRUE when the native is in no OPEN or CLOSING slot and no slot is
       RESERVED, whose native might be this one */
    BOOL (*foreign)(struct KaffeineLibBase *, const struct Screen *native);
    ULONG (*epoch)(struct KaffeineLibBase *);
};

#ifdef __cplusplus
extern "C" {
#endif
/* Once, from the library's init */
void Kaffeine_OwnedScreensInit(struct KaffeineLibBase *base);
extern const struct KaffeineOwnedScreenOps Kaffeine_OwnedScreenOps;
#ifdef __cplusplus
}
#endif

#endif

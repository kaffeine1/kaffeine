/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Native probe of the registry of owned screens: the production
    rom/kaffeine/kaffeine_owned_screens.c, linked unchanged, with real Exec
    tasks and semaphores. A holder task takes the gate of an OPEN screen
    shared, as an input event does across its DoIO(); a drainer task makes
    the slot CLOSING and calls the production drain, as CloseScreen() does.

    That the drainer waits is observed, not timed: under Forbid() the probe
    walks the gate's ss_WaitQueue for the drainer's own SemaphoreRequest,
    exclusive (sr_Waiter without SM_SHARED), with the drainer in TS_WAIT on
    SIGF_SINGLE. That is exactly what ObtainSemaphore() leaves (rom/exec/
    semaphores.c: AddTail() to ss_WaitQueue, then Wait(SIGF_SINGLE), which
    sets tc_SigWait and TS_WAIT, rom/exec/wait.c). Forbid() keeps the probe
    from looking between AddTail() and Wait() only on a uniprocessor Exec,
    so the probe refuses to build for an SMP Exec.

    While the drainer waits, the registry refuses a new hold of the CLOSING
    slot, although Exec itself still grants AttemptSemaphoreShared() to the
    gate held shared with an exclusive request queued. The holder then
    drops on its own task; the drainer returns owning the gate exclusively,
    and only after that the probe lets it finish the slot as closed. The old
    generation is refused, a new screen at the same slot and native address
    is held and dropped, and an ordinary failed close opens the slot again
    with its generation.

    Each task signals its end under Forbid(), so it is gone before the probe
    runs again (rom/exec/remtask.c: a task that removes itself never runs
    again), and nothing shared is freed before every task has ended. Waits
    are individually bounded; a wait that ends without its event is a
    failure. If a task did not end, the probe detaches its notifications
    under Forbid() and retains the coordinator, shared memory AND executable
    segment: main must not return and let DOS unload code that the task may
    still execute. It reports and flushes this failed retained state, then
    parks until the isolated VM's outer deadline stops the failed boot.
    That deadline is never a success oracle. Run only in an isolated UP VM.

    The native screens here are opaque addresses: the registry compares them
    and never reads them. This probe is the registry alone; the library,
    IND_WRITEEVENT and CloseScreen() integration is the separate owner and
    injector probe, and neither shows one DoIO() racing one CloseScreen().
*/
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "kaffeine_intern.h"
#include "kaffeine_owned_screens.h"

#if defined(__AROSEXEC_SMP__)
#error "owned_screens_gate observes Exec wait queues under Forbid(), which needs a uniprocessor Exec"
#endif

/* The longest wait for one event, in ticks of Delay(): 10 seconds. The
   VM's own deadline stays the outer net. */
#define WAIT_TICKS 500

struct Probe
{
    struct KaffeineLibBase *base;
    const struct KaffeineOwnedScreenOps *ops;
    struct Screen *native;
    LONG slot;
    ULONG generation;
    /* the task to notify, NULL once the probe has detached; read and
       written only under Forbid() */
    struct Task *volatile coordinator;
    /* the coordinator's signals, as masks */
    ULONG ready, drained, holder_done, drainer_done;
    /* the holder, and the signal that it allocated, -1 before */
    volatile BYTE drop_sig;
    volatile BOOL held, dropped;
    volatile LONG held_slot;
    /* the drainer */
    volatile BYTE finish_sig;
    volatile BOOL closing, returned, finished;
};

static unsigned checks, failures;
static UBYTE native_screen[16];

static BOOL check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    Printf((CONST_STRPTR)"%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

/* One of the coordinator's signals, within WAIT_TICKS; it is cleared */
static BOOL wait_for(ULONG mask)
{
    ULONG tick;

    for (tick = 0; tick <= WAIT_TICKS; ++tick)
    {
        if (SetSignal(0, 0) & mask)
        {
            SetSignal(0, mask);
            return TRUE;
        }
        Delay(1);
    }
    return FALSE;
}

/* Under Forbid(): whether task waits in the queue of gate for exclusive
   ownership, as ObtainSemaphore() queues it. The task is read only when
   its request is found, which keeps it from ending. */
static BOOL exclusive_waiter(struct SignalSemaphore *gate, struct Task *task)
{
    struct MinNode *n;

    for (n = gate->ss_WaitQueue.mlh_Head; n->mln_Succ; n = n->mln_Succ)
        if (((struct SemaphoreRequest *)n)->sr_Waiter == task)
            return task->tc_State == TS_WAIT && (task->tc_SigWait & SIGF_SINGLE);
    return FALSE;
}

static BOOL queue_empty(struct SignalSemaphore *gate)
{
    return IsListEmpty((struct List *)&gate->ss_WaitQueue);
}

/* A notification of the coordinator unless it has detached: the check and
   Signal() under one Forbid(), as the detach is */
static void notify(struct Probe *p, ULONG mask)
{
    Forbid();
    if (p->coordinator)
        Signal(p->coordinator, mask);
    Permit();
}

/* The last notification of a task: under the Forbid() that lasts until the
   task has been removed, so it is gone before the coordinator runs again */
static void notify_end(struct Probe *p, ULONG mask)
{
    Forbid();
    if (p->coordinator)
        Signal(p->coordinator, mask);
}

/* An input hold of the coordinator, given back at once when it was
   admitted, so that no check that fails keeps a share of the gate */
static BOOL try_hold(struct Probe *p, ULONG generation, LONG *slot)
{
    BOOL held = p->ops->hold(p->base, p->native, generation, slot);

    if (held)
        p->ops->drop(p->base, *slot);
    return held;
}

static UBYTE slot_state(struct Probe *p, LONG slot)
{
    UBYTE state;

    ObtainSemaphore(&p->base->owned_screens.lock);
    state = p->base->owned_screens.slot[slot].state;
    ReleaseSemaphore(&p->base->owned_screens.lock);
    return state;
}

/* Takes the gate shared, as an input event does, and drops it on this same
   task once told to */
static void holder_main(void)
{
    struct Probe *p = FindTask(NULL)->tc_UserData;
    BYTE sig = AllocSignal(-1);
    LONG slot = -1;

    if (sig >= 0)
        p->held = p->ops->hold(p->base, p->native, p->generation, &slot);
    p->held_slot = slot;
    p->drop_sig = sig;
    notify(p, p->ready);
    if (sig >= 0)
    {
        Wait(1UL << sig);
        if (p->held)
        {
            p->ops->drop(p->base, slot);
            p->dropped = TRUE;
        }
        FreeSignal(sig);
    }
    notify_end(p, p->holder_done);
}

/* Makes the slot CLOSING and drains it, as CloseScreen() does, then
   finishes it as closed once told to, still owning the gate */
static void drainer_main(void)
{
    struct Probe *p = FindTask(NULL)->tc_UserData;
    BYTE sig = AllocSignal(-1);

    p->finish_sig = sig;
    if (sig >= 0 && (p->closing = p->ops->closing(p->base, p->slot, p->generation)))
    {
        p->ops->drain(p->base, p->slot);
        p->returned = TRUE;
        notify(p, p->drained);
        Wait(1UL << sig);
        p->ops->finish(p->base, p->slot, TRUE, FALSE);
        p->finished = TRUE;
    }
    else
        notify(p, p->drained);
    if (sig >= 0)
        FreeSignal(sig);
    notify_end(p, p->drainer_done);
}

static struct Task *start(const char *name, void (*entry)(void), struct Probe *p)
{
    struct TagItem tags[] =
    {
        { TASKTAG_NAME, (IPTR)name },
        { TASKTAG_PC, (IPTR)entry },
        { TASKTAG_USERDATA, (IPTR)p },
        { TASKTAG_STACKSIZE, 16384 },
        { TASKTAG_PRI, 0 },
        { TAG_DONE, 0 }
    };

    return NewCreateTaskA(tags);
}

static BYTE signal_of(void)
{
    return AllocSignal(-1);
}

int main(void)
{
    struct Probe *p;
    struct KaffeineLibBase *base;
    const struct KaffeineOwnedScreenOps *ops = &Kaffeine_OwnedScreenOps;
    struct SignalSemaphore *gate = NULL;
    struct Task *holder = NULL, *drainer = NULL;
    BOOL holder_gone = TRUE, drainer_gone = TRUE, queued = FALSE;
    BYTE sigs[4] = { -1, -1, -1, -1 };
    LONG slot = -1, other = -1, reused = -1;
    ULONG generation = 0, renewed = 0, tick;
    int i;

    p = AllocMem(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR);
    base = AllocMem(sizeof(*base), MEMF_PUBLIC | MEMF_CLEAR);
    for (i = 0; i < 4; ++i)
        sigs[i] = signal_of();
    if (!check(p && base && sigs[0] >= 0 && sigs[1] >= 0 && sigs[2] >= 0 && sigs[3] >= 0,
               "probe memory and four coordinator signals"))
        goto out;

    Kaffeine_OwnedScreensInit(base);
    p->base = base;
    p->ops = ops;
    p->native = (struct Screen *)native_screen;
    p->coordinator = FindTask(NULL);
    p->ready = 1UL << sigs[0];
    p->drained = 1UL << sigs[1];
    p->holder_done = 1UL << sigs[2];
    p->drainer_done = 1UL << sigs[3];
    p->drop_sig = p->finish_sig = -1;
    p->held_slot = -1;

    slot = ops->reserve(base, &generation);
    if (!check(slot >= 0 && generation == 1, "a slot and generation 1 for a screen"))
        goto out;
    ops->bind(base, slot, p->native);
    p->slot = slot;
    p->generation = generation;
    gate = &base->owned_screens.slot[slot].gate;
    check(slot_state(p, slot) == KAFFEINE_OWNED_OPEN, "bound: OPEN");

    /* 1. the holder takes the gate shared */
    holder = start("owned_screens_gate holder", holder_main, p);
    holder_gone = !holder;
    if (!check(holder != NULL, "holder task started") ||
        !check(wait_for(p->ready), "holder ready"))
        goto release;
    check(p->held && p->held_slot == slot && p->drop_sig >= 0, "holder holds the OPEN slot of its generation");
    Forbid();
    i = gate->ss_Owner == NULL && gate->ss_NestCount == 1 && queue_empty(gate);
    Permit();
    check(i, "gate held shared once, nobody queued");

    /* 2. the drainer makes the slot CLOSING and waits in the gate's queue */
    drainer = start("owned_screens_gate drainer", drainer_main, p);
    drainer_gone = !drainer;
    if (!check(drainer != NULL, "drainer task started"))
        goto release;
    for (tick = 0; tick <= WAIT_TICKS && !queued; ++tick)
    {
        Forbid();
        queued = exclusive_waiter(gate, drainer);
        Permit();
        if (!queued)
            Delay(1);
    }
    check(queued, "drainer queued for the gate exclusively, in TS_WAIT on SIGF_SINGLE");
    check(p->closing && !p->returned && slot_state(p, slot) == KAFFEINE_OWNED_CLOSING,
          "slot CLOSING, drainer not returned");
    check(!(SetSignal(0, 0) & p->drained), "no drained signal while the holder holds");
    other = 99;
    check(!try_hold(p, generation, &other) && other == -1, "CLOSING refuses a new hold");
    {
        ULONG granted = AttemptSemaphoreShared(gate);

        check(granted != 0, "Exec alone would still grant another reader");
        if (granted)
            ReleaseSemaphore(gate);
    }
    Forbid();
    i = queued && exclusive_waiter(gate, drainer) && gate->ss_Owner == NULL && gate->ss_NestCount == 1;
    Permit();
    check(i, "drainer still queued behind the one shared holder");

release:
    /* 3. the holder drops on its own task; the drainer returns owning the gate */
    if (holder && p->drop_sig >= 0)
        Signal(holder, 1UL << p->drop_sig);
    if (holder)
        holder_gone = check(wait_for(p->holder_done), "holder ended");
    if (drainer)
    {
        check(wait_for(p->drained), "drainer returned from the drain");
        check(p->dropped && p->returned, "holder dropped, drain returned");
        Forbid();
        i = gate->ss_Owner == drainer && gate->ss_NestCount == 1 && queue_empty(gate);
        Permit();
        check(i, "drainer owns the gate exclusively, nobody queued");
        check(slot_state(p, slot) == KAFFEINE_OWNED_CLOSING && !p->finished, "still CLOSING before finish");
        /* only a drainer that returned from the drain waits for this; one
           that could not close the slot has ended by itself */
        if (p->returned && p->finish_sig >= 0)
            Signal(drainer, 1UL << p->finish_sig);
        drainer_gone = check(wait_for(p->drainer_done), "drainer ended");
    }
    if (!holder_gone || !drainer_gone || !queued)
        goto out;

    /* the slot is free, its gate free, the old generation refused */
    check(p->finished && slot_state(p, slot) == KAFFEINE_OWNED_FREE &&
          !base->owned_screens.slot[slot].generation && !base->owned_screens.slot[slot].native,
          "finished as closed: FREE");
    Forbid();
    i = gate->ss_Owner == NULL && gate->ss_NestCount == 0 && gate->ss_QueueCount == -1 && queue_empty(gate);
    Permit();
    check(i, "gate free again");
    check(!try_hold(p, generation, &other), "old generation refused");

    /* 4. a new screen at the same slot and native address */
    reused = ops->reserve(base, &renewed);
    if (!check(reused >= 0, "a slot for the new screen"))
        goto out;
    check(reused == slot && renewed == generation + 1, "same slot, next generation");
    ops->bind(base, reused, p->native);
    check(!try_hold(p, generation, &other), "old generation refused at the reused native");
    other = -1;
    check(try_hold(p, renewed, &other), "new generation held and dropped");
    check(other == reused, "held at the reused slot");

    /* 5. a failed close opens the slot again with its generation */
    check(ops->closing(base, reused, renewed), "CLOSING again");
    ops->drain(base, reused);
    Forbid();
    i = gate->ss_Owner == FindTask(NULL) && gate->ss_NestCount == 1;
    Permit();
    check(i, "an unheld gate drains at once to its closer");
    ops->finish(base, reused, FALSE, TRUE);
    check(slot_state(p, reused) == KAFFEINE_OWNED_OPEN && base->owned_screens.slot[reused].generation == renewed,
          "refused close: OPEN, same generation");
    check(try_hold(p, renewed, &other), "held again after the refused close");
    check(ops->closing(base, reused, renewed), "closing for good");
    ops->drain(base, reused);
    ops->finish(base, reused, TRUE, FALSE);
    check(slot_state(p, reused) == KAFFEINE_OWNED_FREE, "closed: FREE");

out:
    Printf((CONST_STRPTR)"owned_screens_gate: %ld checks, %ld failed\n", (LONG)checks, (LONG)failures);
    /* nothing that a task may still use goes while it may, and no task
       signals this process once it has gone */
    if (holder_gone && drainer_gone)
    {
        if (base)
            FreeMem(base, sizeof(*base));
        if (p)
            FreeMem(p, sizeof(*p));
        for (i = 0; i < 4; ++i)
            if (sigs[i] >= 0)
                FreeSignal(sigs[i]);
    }
    else
    {
        Forbid();
        p->coordinator = NULL;
        Permit();
        Printf((CONST_STRPTR)"FAIL owned_screens_gate: unjoined task; retaining coordinator, code and shared memory for VM stop\n");
        Flush(Output());
        /* Returning would unload this executable, including any worker
           that can still wake. The outer VM deadline records a failed
           boot and stops all of these isolated participants together. */
        for (;;)
            Delay(50);
    }
    return failures ? 10 : 0;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Classic 68k tasks of one program, scheduled cooperatively inside its
    Kaffeine_RunCPU().

    The root task is the native Exec task that runs the program; its guest
    identity stays the Process snapshot of ExecBase.ThisTask. A child that
    AddTask() adds is never a native Exec task: it is a guest Task in the
    arena with a complete backend CPU of its own, from the backend's
    factory, so that every private register, the FPU included, stays with
    it. ExecBase.ThisTask names the current guest task.

    Tasks switch only between slices of the outermost run loop, never
    inside a callback or a thunk, and not while the current task holds
    Forbid() or Disable() in the guest ExecBase, unless it waits. Exec's
    rules pick the next one: the highest priority runs, tasks of the same
    priority take turns of KAFFEINE_TASK_QUANTUM slices.

    Signals. The root's signals are those of the native task, as before.
    A child's are its own, with an AllocSignal() of a child taking a free
    bit of the native task as well, so that no native source can collide
    with it; a native signal on such a bit goes to that child. A child that
    waits for a native bit no guest task allocated, such as the port of a
    window that it opened, gets it unless the root waits for it too. When
    every guest task waits, the native task waits for the union.

    A blocked Wait() returns at the thunk at once; its result is written to
    D0 of that task when it is resumed, before it executes again. A child
    ends when it returns to KAFFEINE_TASK_EXIT_ADDR or removes itself, and
    the scheduler destroys its CPU at the next slice boundary, then frees
    the MemLists of its tc_MemEntry that the guest heap made. A fault or a
    refused operation in any task stops the whole program, with its D0.
*/

#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"
#include "kaffeine_workbench.h"

/* The classic Task of the NDK (exec/tasks.h) and its constants */
enum
{
    TC_PRED = 4, TC_TYPE = 8, TC_PRI = 9, TC_NAME = 10, TC_FLAGS = 14,
    TC_STATE = 15, TC_IDNESTCNT = 16, TC_TDNESTCNT = 17, TC_SIGALLOC = 18,
    TC_SIGWAIT = 22, TC_SIGRECVD = 26, TC_SIGEXCEPT = 30, TC_EXCEPTCODE = 42,
    TC_TRAPCODE = 50, TC_SPREG = 54, TC_SPLOWER = 58, TC_SPUPPER = 62,
    TC_SWITCH = 66, TC_LAUNCH = 70, TC_MEMENTRY = 74, TC_SIZE = 92,
    LH_TAIL = 4, LH_TAILPRED = 8, MEMLIST_HEADER = 16,
    G_NT_TASK = 1,
    G_TS_RUN = 2, G_TS_READY = 3, G_TS_WAIT = 4, G_TS_REMOVED = 6,
    /* MemLists of one task that RemTask() frees */
    MAX_MEMENTRY = 32
};
#define SYSTEM_SIGNALS 0x0000FFFFUL  /* the classic SysBase->TaskSigAlloc */
#define TASK_SR        0x2000        /* supervisor: exception frames use the task's stack */

enum { SLOT_FREE, SLOT_READY, SLOT_RUN, SLOT_WAIT, SLOT_REMOVED };

struct GuestTask
{
    ULONG tcb;                  /* guest Task; 0 for a free slot */
    struct KaffeineCPU *cpu;    /* a child's own; the root's is the caller's */
    UBYTE state;
    BYTE  pri;
    UBYTE nest[2];              /* IDNestCnt, TDNestCnt while switched out */
    BOOL  resume;               /* the result of a blocked call is due */
    KaffeineTaskRetry retry;    /* that call, or NULL for Wait() */
    ULONG wait;                 /* the signals that it waits for */
    ULONG recvd;                /* a child's signals; the root's on child bits */
    ULONG alloc;                /* a child's tc_SigAlloc */
    ULONG native;               /* native bits that a child allocated */
    ULONG lower;                /* a child's tc_SPLower */
    ULONG upper;                /* full stack reservation, including live frames */
    ULONG turn;                 /* when it last got the CPU */
};

struct KaffeineScheduler
{
    struct GuestTask root;
    struct GuestTask child[KAFFEINE_MAX_GUEST_TASKS];
    struct GuestTask *current;  /* NULL only while a removed current goes */
    ULONG slices, turns;
    BOOL  reschedule;
};

static BOOL arena(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && size <= ctx->mem_size - addr;
}

static BOOL overlap(ULONG a, ULONG an, ULONG b, ULONG bn)
{
    return an && bn && (a <= b ? b - a < an : a - b < bn);
}

static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!arena(ctx, addr, 1) || !memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return (const char *)ctx->mem + addr;
}

/* In a thunk: the dispatcher puts the 20 into D0. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* Between slices, where Kaffeine_RunCPU() returns D0 of ctx->cpu. */
static void stop(struct KaffeineContext *ctx, const char *why)
{
    refuse(ctx, why);
    if (ctx->cpu)
        Kaffeine_SetD(ctx->cpu, 0, 20);
}

static BOOL execbase(struct KaffeineContext *ctx)
{
    return arena(ctx, ctx->m68k_sysbase, M68K_SIZEOF_EXECBASE);
}

static ULONG this_task(struct KaffeineContext *ctx)
{
    return execbase(ctx) ? m68k_read32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK) : 0;
}

static void set_this_task(struct KaffeineContext *ctx, ULONG tcb)
{
    if (execbase(ctx))
        m68k_write32(ctx, ctx->m68k_sysbase + M68K_EXECBASE_THISTASK, tcb);
}

/* Forbid() or Disable() of the current task, in the guest ExecBase */
static BOOL forbidden(struct KaffeineContext *ctx)
{
    return execbase(ctx) &&
        ((signed char)ctx->mem[ctx->m68k_sysbase + KAFFEINE_EXECBASE_IDNESTCNT] >= 0 ||
         (signed char)ctx->mem[ctx->m68k_sysbase + KAFFEINE_EXECBASE_TDNESTCNT] >= 0);
}

static void nest_save(struct KaffeineContext *ctx, struct GuestTask *t)
{
    if (execbase(ctx))
        memcpy(t->nest, ctx->mem + ctx->m68k_sysbase + KAFFEINE_EXECBASE_IDNESTCNT, 2);
    if (arena(ctx, t->tcb, TC_SIZE))
        memcpy(ctx->mem + t->tcb + TC_IDNESTCNT, t->nest, 2);
}

static void nest_load(struct KaffeineContext *ctx, struct GuestTask *t)
{
    if (execbase(ctx))
        memcpy(ctx->mem + ctx->m68k_sysbase + KAFFEINE_EXECBASE_IDNESTCNT, t->nest, 2);
}

static struct GuestTask *task_at(struct KaffeineScheduler *s, unsigned i)
{
    return i ? &s->child[i - 1] : &s->root;
}

/* The root, or a child that has not been removed */
static struct GuestTask *task_of(struct KaffeineScheduler *s, ULONG tcb)
{
    unsigned i;

    if (!tcb)
        return NULL;
    for (i = 0; i <= KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = task_at(s, i);

        if (t->tcb == tcb && t->state != SLOT_FREE && t->state != SLOT_REMOVED)
            return t;
    }
    return NULL;
}

static unsigned children(struct KaffeineScheduler *s)
{
    unsigned i, n = 0;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        n += s->child[i].state != SLOT_FREE;
    return n;
}

/* Native bits that children allocated */
static ULONG child_bits(struct KaffeineScheduler *s)
{
    ULONG bits = 0;
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state != SLOT_FREE)
            bits |= s->child[i].native;
    return bits;
}

/* Native bits that no guest task allocated, such as those of native ports */
static ULONG unowned_bits(struct KaffeineContext *ctx)
{
    return ~ctx->guest_signals & ~SYSTEM_SIGNALS;
}

/* The guest Task fields that Exec would keep, in BE */
static void mirror(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                   struct GuestTask *t)
{
    static const UBYTE states[] = { 0, G_TS_READY, G_TS_RUN, G_TS_WAIT, G_TS_REMOVED };

    if (!arena(ctx, t->tcb, TC_SIZE))
        return;
    ctx->mem[t->tcb + TC_STATE] = states[t->state];
    if (t == &s->root)
        return;
    m68k_write32(ctx, t->tcb + TC_SIGALLOC, t->alloc);
    m68k_write32(ctx, t->tcb + TC_SIGWAIT, t->wait);
    m68k_write32(ctx, t->tcb + TC_SIGRECVD, t->recvd);
}

/* ExecBase TaskReady and TaskWait as Enqueue() would order them, rebuilt
   from the scheduler, so that guest walks see the other guest tasks. The
   running task is in neither. */
static void publish(struct KaffeineContext *ctx, struct KaffeineScheduler *s)
{
    static const UWORD lists[2] = { M68K_EXECBASE_TASKREADY, M68K_EXECBASE_TASKWAIT };
    static const UBYTE states[2] = { SLOT_READY, SLOT_WAIT };
    struct GuestTask *order[KAFFEINE_MAX_GUEST_TASKS + 1];
    unsigned i, j, k, count;

    if (!execbase(ctx))
        return;
    for (k = 0; k < 2; k++)
    {
        ULONG list = ctx->m68k_sysbase + lists[k], pred = list;

        count = 0;
        for (i = 0; i <= KAFFEINE_MAX_GUEST_TASKS; i++)
        {
            struct GuestTask *t = task_at(s, i);

            if (t->state != states[k] || !arena(ctx, t->tcb, TC_SIZE))
                continue;
            for (j = count; j > 0 && order[j - 1]->pri < t->pri; j--)
                order[j] = order[j - 1];
            order[j] = t;
            count++;
        }
        for (i = 0; i < count; i++)
        {
            m68k_write32(ctx, pred, order[i]->tcb);
            m68k_write32(ctx, order[i]->tcb + TC_PRED, pred);
            pred = order[i]->tcb;
        }
        m68k_write32(ctx, pred, list + LH_TAIL);
        m68k_write32(ctx, list + LH_TAIL, 0);
        m68k_write32(ctx, list + LH_TAILPRED, pred);
    }
}

static void wake(struct KaffeineContext *ctx, struct KaffeineScheduler *s, struct GuestTask *t)
{
    t->state = SLOT_READY;
    s->reschedule = TRUE;
    mirror(ctx, s, t);
}

static void suspend(struct KaffeineContext *ctx, struct KaffeineScheduler *s, struct GuestTask *t)
{
    t->state = SLOT_WAIT;
    t->resume = TRUE;
    s->reschedule = TRUE;
    mirror(ctx, s, t);
}

/* Native bits that would wake a waiting task */
static ULONG wake_bits(struct KaffeineContext *ctx, struct KaffeineScheduler *s)
{
    ULONG owned = child_bits(s), unowned = unowned_bits(ctx), bits = 0;
    unsigned i;

    if (s->root.state == SLOT_WAIT)
        bits |= s->root.wait & ~owned;
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state == SLOT_WAIT)
            bits |= s->child[i].wait & (s->child[i].native | unowned);
    return bits;
}

/* Native signals got: the root's stay native until it takes them, children
   get theirs and the unowned bits they wait for, which leave the native
   task. */
static void deliver(struct KaffeineContext *ctx, struct KaffeineScheduler *s, ULONG got)
{
    ULONG owned = child_bits(s), unowned = unowned_bits(ctx), rootbits = 0, taken = 0;
    unsigned i;

    if (s->root.state == SLOT_WAIT)
    {
        rootbits = got & s->root.wait & ~owned;
        if (rootbits)
            wake(ctx, s, &s->root);
    }
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = &s->child[i];
        ULONG hits;

        if (t->state == SLOT_FREE || t->state == SLOT_REMOVED)
            continue;
        hits = got & t->native;
        if (t->state == SLOT_WAIT)
            hits |= got & t->wait & unowned & ~rootbits & ~taken;
        if (!hits)
            continue;
        taken |= hits;
        t->recvd |= hits;
        if (t->state == SLOT_WAIT && (t->recvd & t->wait))
            wake(ctx, s, t);
        else
            mirror(ctx, s, t);
    }
    if (taken)
        SetSignal(0, taken);
}

static void poll(struct KaffeineContext *ctx, struct KaffeineScheduler *s)
{
    ULONG want = wake_bits(ctx, s) | child_bits(s), got;

    if (want && (got = SetSignal(0, 0) & want))
        deliver(ctx, s, got);
}

/* Take what a task received of mask, as Wait() does on return */
static ULONG take(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                  struct GuestTask *t, ULONG mask)
{
    ULONG got = t->recvd & mask;

    t->recvd &= ~mask;
    if (t == &s->root)
    {
        ULONG native = mask & ~child_bits(s);

        if (native)
            got |= SetSignal(0, native) & native;
    }
    return got;
}

/* Switch to next, saving the current task's state for its next turn */
static void enter(struct KaffeineContext *ctx, struct KaffeineScheduler *s, struct GuestTask *next)
{
    struct GuestTask *cur = s->current;

    if (cur)
        nest_save(ctx, cur);
    if (cur && cur != next)
    {
        if (cur->state == SLOT_RUN)
            cur->state = SLOT_READY;
        if (cur != &s->root && arena(ctx, cur->tcb, TC_SIZE))
            m68k_write32(ctx, cur->tcb + TC_SPREG, Kaffeine_GetA(cur->cpu, 7));
        mirror(ctx, s, cur);
    }
    /* A new turn, also for a task that keeps the CPU after its turn, but
       not for one that keeps it through a reschedule in its turn. */
    if (cur != next)
        next->turn = ++s->turns;
    if (cur != next || s->slices >= KAFFEINE_TASK_QUANTUM)
        s->slices = 0;
    s->current = next;
    s->reschedule = FALSE;
    next->state = SLOT_RUN;
    nest_load(ctx, next);
    set_this_task(ctx, next->tcb);
    ctx->cpu = next->cpu;
    mirror(ctx, s, next);
    publish(ctx, s);
}

/* The current task's blocked call returns now, or it waits again */
static BOOL complete(struct KaffeineContext *ctx, struct KaffeineScheduler *s, struct GuestTask *t)
{
    IPTR result;

    if (!t->resume)
        return TRUE;
    if (t->retry)
    {
        KaffeineTaskRetry retry = t->retry;

        t->retry = NULL;
        t->resume = FALSE;
        /* the signal that woke it, as Wait() in WaitPort() would take it */
        (void)take(ctx, s, t, t->wait);
        result = retry(ctx, t->cpu);
        if (ctx->after_thunk)
        {
            void (*after)(struct KaffeineContext *) = ctx->after_thunk;

            ctx->after_thunk = NULL;
            after(ctx);
        }
        if (ctx->running && t->state == SLOT_WAIT)
            return FALSE;
        Kaffeine_SetD(t->cpu, 0, (ULONG)result);
        mirror(ctx, s, t);
        return TRUE;
    }
    result = take(ctx, s, t, t->wait);
    if (!result)
    {
        t->state = SLOT_WAIT;
        mirror(ctx, s, t);
        return FALSE;
    }
    t->resume = FALSE;
    Kaffeine_SetD(t->cpu, 0, (ULONG)result);
    mirror(ctx, s, t);
    return TRUE;
}

/* Exec's choice: the highest priority; among equals the current task
   until its turn is over, then the one that waited longest. */
static struct GuestTask *pick(struct KaffeineScheduler *s)
{
    struct GuestTask *best = NULL, *cur = s->current;
    BOOL fresh = cur && cur->state == SLOT_RUN && s->slices < KAFFEINE_TASK_QUANTUM;
    unsigned i;

    for (i = 0; i <= KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = task_at(s, i);

        if (t->state != SLOT_READY && !(t == cur && t->state == SLOT_RUN))
            continue;
        if (!best || t->pri > best->pri)
        {
            best = t;
            continue;
        }
        if (t->pri < best->pri || (best == cur && fresh))
            continue;
        if ((t == cur && fresh) || best == cur || (t != cur && t->turn < best->turn))
            best = t;
    }
    return best;
}

/* The MemLists of a classic tc_MemEntry, without following a bad link */
static BOOL mem_entries(struct KaffeineContext *ctx, ULONG tcb, ULONG *nodes, unsigned *count)
{
    ULONG list = tcb + TC_MEMENTRY, node, pred = list;
    unsigned i;

    *count = 0;
    if (!arena(ctx, tcb, TC_SIZE) || m68k_read32(ctx, list + LH_TAIL))
        return FALSE;
    node = m68k_read32(ctx, list);
    while (node != list + LH_TAIL)
    {
        if (*count == MAX_MEMENTRY || (node & 1) || !arena(ctx, node, MEMLIST_HEADER) ||
            m68k_read32(ctx, node + TC_PRED) != pred)
            break;
        for (i = 0; i < *count && nodes[i] != node; i++)
            ;
        if (i < *count)
            break;
        nodes[(*count)++] = node;
        pred = node;
        node = m68k_read32(ctx, node);
    }
    if (node != list + LH_TAIL || m68k_read32(ctx, list + LH_TAILPRED) != pred)
    {
        *count = 0;
        return FALSE;
    }
    return TRUE;
}

/* A task's stack can hold messages still queued for another task. Keep
   such allocations alive instead of invalidating a queue when reaping.
   HeapFreeEntry subsequently checks the native heap ownership record. */
static BOOL entry_unreferenced(struct KaffeineContext *ctx, ULONG entry)
{
    ULONG count, size, i;
    if (!arena(ctx, entry, MEMLIST_HEADER)) return FALSE;
    count = m68k_read16(ctx, entry + 14);
    if (count > 1024) return FALSE;
    size = MEMLIST_HEADER + count * 8;
    if (!Kaffeine_MessageMemory(ctx, entry, size)) return FALSE;
    for (i = 0; i < count; i++)
    {
        ULONG member = m68k_read32(ctx, entry + MEMLIST_HEADER + i * 8);
        ULONG bytes = m68k_read32(ctx, entry + MEMLIST_HEADER + i * 8 + 4);
        if (bytes && !Kaffeine_MessageMemory(ctx, member, bytes)) return FALSE;
    }
    return TRUE;
}

/* A removed child goes: its CPU, which nothing executes now, its slot, and
   the guest heap MemLists of its tc_MemEntry, which may hold its Task and
   stack and are freed last. Others stay allocated: the guest heap frees
   only what it made, never native memory. Retire orphan port signal records
   before releasing bits, so deleting one later cannot free a reused bit. */
static void reap(struct KaffeineContext *ctx, struct KaffeineScheduler *s, struct GuestTask *t)
{
    ULONG nodes[MAX_MEMENTRY], tcb = t->tcb;
    struct KaffeineCPU *cpu = t->cpu;
    unsigned count, i;
    BOOL listed;

    for (i = 0; i < 32; i++)
        if (t->native & (1UL << i))
        {
            unsigned j;
            Kaffeine_AppSignalGone(ctx, (BYTE)i);
            for (j = 0; j < KAFFEINE_MAX_IO_PORTS; j++)
                if (ctx->io_ports[j].guest && ctx->io_ports[j].sigbit == (BYTE)i)
                    ctx->io_ports[j].sigbit = -1;
            SetSignal(0, 1UL << i);
            ctx->guest_signals &= ~(1UL << i);
            FreeSignal((LONG)i);
        }

    if (t == s->current)
    {
        s->current = NULL;
        ctx->cpu = s->root.cpu;
        set_this_task(ctx, s->root.tcb);
    }
    if (arena(ctx, tcb, TC_SIZE))
        ctx->mem[tcb + TC_STATE] = G_TS_REMOVED;
    listed = mem_entries(ctx, tcb, nodes, &count);
    if (listed)
        for (i = 0; i < count; i++)
            if (!entry_unreferenced(ctx, nodes[i]))
            {
                bug("[kaffeine] RemTask(%08lx): task memory is still referenced; "
                    "keeping it until context cleanup\n", (unsigned long)tcb);
                count = 0;
                break;
            }
    memset(t, 0, sizeof(*t));
    publish(ctx, s);
    if (ctx->cpu_factory)
        ctx->cpu_factory->destroy(ctx, cpu);
    else
        bug("[kaffeine] guest task %08lx: no backend to destroy its CPU\n", (unsigned long)tcb);
    if (!listed)
        bug("[kaffeine] RemTask(%08lx): tc_MemEntry is not a valid list, "
            "its memory stays allocated\n", (unsigned long)tcb);
    for (i = 0; i < count; i++)
        Kaffeine_HeapFreeEntry(ctx, nodes[i]);
}

static struct KaffeineScheduler *activate(struct KaffeineContext *ctx, struct KaffeineCPU *cpu)
{
    struct KaffeineScheduler *s =
        (struct KaffeineScheduler *)AllocMem(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);

    if (!s)
        return NULL;
    s->root.tcb = ctx->guest_exec_task;
    s->root.cpu = cpu;
    s->root.state = SLOT_RUN;
    s->root.pri = (BYTE)ctx->mem[ctx->guest_exec_task + TC_PRI];
    s->root.turn = s->turns = 1;
    s->current = &s->root;
    m68k_write16(ctx, KAFFEINE_TASK_EXIT_ADDR, M68K_OP_NOP);
    m68k_write16(ctx, KAFFEINE_TASK_EXIT_ADDR + 2, M68K_OP_NOP);
    ctx->sched = s;
    return s;
}

/* The root alone again: Exec as before guest tasks */
static void deactivate(struct KaffeineContext *ctx)
{
    struct KaffeineScheduler *s = ctx->sched;
    unsigned k;

    if (execbase(ctx))
        for (k = 0; k < 2; k++)
        {
            ULONG list = ctx->m68k_sysbase + (k ? M68K_EXECBASE_TASKWAIT : M68K_EXECBASE_TASKREADY);

            m68k_write32(ctx, list, list + LH_TAIL);
            m68k_write32(ctx, list + LH_TAIL, 0);
            m68k_write32(ctx, list + LH_TAILPRED, list);
        }
    set_this_task(ctx, s->root.tcb);
    if (arena(ctx, s->root.tcb, TC_SIZE))
        ctx->mem[s->root.tcb + TC_STATE] = G_TS_RUN;
    ctx->sched = NULL;
    FreeMem(s, sizeof(*s));
}

static void schedule(struct KaffeineContext *ctx, struct KaffeineScheduler *s)
{
    struct GuestTask *next;
    unsigned i, tries = 0;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state == SLOT_REMOVED)
            reap(ctx, s, &s->child[i]);
    poll(ctx, s);
    for (;;)
    {
        next = pick(s);
        if (!next)
        {
            ULONG want = wake_bits(ctx, s), got;

            if (!want)
            {
                stop(ctx, "every guest task waits for signals that nothing can send");
                return;
            }
            /* No guest task can run: the native task waits for them all. */
            got = Wait(want);
            SetSignal(got, got);
            deliver(ctx, s, got);
            tries = 0;
            continue;
        }
        enter(ctx, s, next);
        if (complete(ctx, s, next) || !ctx->running)
            break;
        if (++tries > 4 * (KAFFEINE_MAX_GUEST_TASKS + 1))
        {
            stop(ctx, "guest tasks wake without making progress");
            return;
        }
    }
    if (ctx->running && s->current == &s->root && !children(s))
        deactivate(ctx);
}

static void sched_boundary(struct KaffeineContext *ctx)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *cur = s->current;

    if (ctx->callback_depth || !ctx->running)
        return;
    if (cur->state == SLOT_RUN)
    {
        if (Kaffeine_GetPC(cur->cpu) == KAFFEINE_TASK_EXIT_ADDR)
        {
            if (cur == &s->root)
            {
                stop(ctx, "the root task reached the end address of a child");
                return;
            }
            /* initialPC returned to the default finalPC: RemTask(NULL) */
            cur->state = SLOT_REMOVED;
        }
        else if ((++s->slices < KAFFEINE_TASK_QUANTUM && !s->reschedule) || forbidden(ctx))
            return;
    }
    schedule(ctx, s);
}

/* The program ends: no child outlives it. Nothing executes, so every CPU
   can go; the guest heap goes with the context. */
static void sched_end(struct KaffeineContext *ctx)
{
    struct KaffeineScheduler *s = ctx->sched;
    unsigned i;

    if (s->current != &s->root)
        nest_load(ctx, &s->root);
    s->current = &s->root;
    ctx->cpu = s->root.cpu;
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = &s->child[i];

        if (t->state == SLOT_FREE)
            continue;
        bug("[kaffeine] guest task %08lx ends with the program\n", (unsigned long)t->tcb);
        if (ctx->cpu_factory)
            ctx->cpu_factory->destroy(ctx, t->cpu);
        memset(t, 0, sizeof(*t));
    }
    deactivate(ctx);
}

/* ── AddTask() ───────────────────────────────────────────────────── */

static BOOL code_address(struct KaffeineContext *ctx, ULONG pc)
{
    return !(pc & 1) && pc >= KAFFEINE_VECTORS_SIZE && arena(ctx, pc, 2);
}

/* Bytes of another guest task: its Task and its entire reserved stack. */
static BOOL claimed(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                    ULONG addr, ULONG size)
{
    unsigned i;

    if (overlap(addr, size, ctx->guest_exec_task, M68K_SIZEOF_PROCESS) ||
        (ctx->stack_bottom < ctx->stack_top &&
         overlap(addr, size, ctx->stack_bottom, ctx->stack_top - ctx->stack_bottom)))
        return TRUE;
    for (i = 0; s && i < KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = &s->child[i];

        if (t->state == SLOT_FREE)
            continue;
        if (overlap(addr, size, t->tcb, TC_SIZE) ||
            overlap(addr, size, t->lower, t->upper - t->lower))
            return TRUE;
    }
    return FALSE;
}

/* -282: AddTask(A1=task, A2=initialPC, A3=finalPC) -> D0=task or NULL.
   The Task, its stack and the code are checked before anything changes;
   a pointer that Exec could not use stops the program, a full table or a
   failed CPU returns NULL. Then the fields as NewAddTask() sets them, the
   stack with finalPC (KAFFEINE_TASK_EXIT_ADDR for NULL) as the return
   address of initialPC, and registers 0 apart from A7 and the PC. */
IPTR Kaffeine_TaskAdd(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct KaffeineCPU *caller = (struct KaffeineCPU *)cpu, *root, *handle;
    struct GuestTask *t = NULL;
    ULONG tcb = THUNK_A(1), initial = THUNK_A(2), final = THUNK_A(3);
    ULONG name, lower, upper, sp, list, alloc;
    UBYTE type;
    unsigned i;

    if (!ctx->cpu_factory || !arena(ctx, ctx->guest_exec_task, M68K_SIZEOF_PROCESS) ||
        ctx->cpu != caller || (s && s->current->cpu != caller))
        return refuse(ctx, "AddTask(): guest tasks need a backend CPU factory and the root Task");
    root = s ? s->root.cpu : caller;
    if ((tcb & 1) || tcb < KAFFEINE_HEAP_BASE || !arena(ctx, tcb, TC_SIZE) ||
        !Kaffeine_GuestBytes(ctx, tcb, TC_SIZE) || claimed(ctx, s, tcb, TC_SIZE))
        return refuse(ctx, "AddTask(): the Task is not free classic Task memory");
    type = ctx->mem[tcb + TC_TYPE];
    if (type && type != G_NT_TASK)
        return refuse(ctx, "AddTask(): only an NT_TASK can be added, not a Process");
    if (m68k_read32(ctx, tcb + TC_SWITCH) || m68k_read32(ctx, tcb + TC_LAUNCH))
        return refuse(ctx, "AddTask(): tc_Switch and tc_Launch are not supported");
    if (m68k_read32(ctx, tcb + TC_EXCEPTCODE) || m68k_read32(ctx, tcb + TC_TRAPCODE))
        return refuse(ctx, "AddTask(): task exception and trap code is not supported");
    name = m68k_read32(ctx, tcb + TC_NAME);
    if (name && !guest_string(ctx, name))
        return refuse(ctx, "AddTask(): the name is not a guest string");
    lower = m68k_read32(ctx, tcb + TC_SPLOWER);
    upper = m68k_read32(ctx, tcb + TC_SPUPPER);
    sp = m68k_read32(ctx, tcb + TC_SPREG);
    if (!sp)
        sp = upper;
    sp &= ~1UL;
    if (lower < KAFFEINE_HEAP_BASE || lower >= upper || !arena(ctx, lower, upper - lower) ||
        sp > upper || sp < lower || sp - lower < KAFFEINE_TASK_MIN_STACK + 4 ||
        overlap(tcb, TC_SIZE, lower, upper - lower) || !Kaffeine_GuestBytes(ctx, lower, upper - lower) ||
        claimed(ctx, s, lower, upper - lower))
        return refuse(ctx, "AddTask(): the stack is not free guest memory of at least 256 bytes");
    if (!initial || !code_address(ctx, initial) || (final && !code_address(ctx, final)))
        return refuse(ctx, "AddTask(): initialPC or finalPC is not guest code");

    for (i = 0; s && i < KAFFEINE_MAX_GUEST_TASKS && s->child[i].state != SLOT_FREE; i++)
        ;
    if (i == KAFFEINE_MAX_GUEST_TASKS)
    {
        bug("[kaffeine] AddTask(%08lx): already %d guest tasks\n", (unsigned long)tcb,
            KAFFEINE_MAX_GUEST_TASKS);
        return 0;
    }
    handle = ctx->cpu_factory->create(ctx);
    if (!handle)
        return 0;
    if (!s && !(s = activate(ctx, caller)))
    {
        ctx->cpu_factory->destroy(ctx, handle);
        return 0;
    }
    t = &s->child[i];

    if (!type)
        ctx->mem[tcb + TC_TYPE] = G_NT_TASK;
    list = tcb + TC_MEMENTRY;
    if (!m68k_read32(ctx, list))
    {
        m68k_write32(ctx, list, list + LH_TAIL);
        m68k_write32(ctx, list + LH_TAIL, 0);
        m68k_write32(ctx, list + LH_TAILPRED, list);
    }
    ctx->mem[tcb + TC_FLAGS] = 0;
    ctx->mem[tcb + TC_IDNESTCNT] = 0xFF;
    ctx->mem[tcb + TC_TDNESTCNT] = 0xFF;
    m68k_write32(ctx, tcb + TC_SIGEXCEPT, 0);
    alloc = m68k_read32(ctx, tcb + TC_SIGALLOC);
    if (!alloc && execbase(ctx))
        alloc = m68k_read32(ctx, ctx->m68k_sysbase + KAFFEINE_EXECBASE_TASKSIGALLOC);
    if (!alloc)
        alloc = SYSTEM_SIGNALS;
    sp -= 4;
    m68k_write32(ctx, sp, final ? final : KAFFEINE_TASK_EXIT_ADDR);
    m68k_write32(ctx, tcb + TC_SPREG, sp);

    for (i = 0; i < 8; i++)
    {
        Kaffeine_SetD(handle, i, 0);
        if (i < 7)
            Kaffeine_SetA(handle, i, 0);
    }
    /* SR first: A7 is then the stack pointer of that mode. */
    Kaffeine_SetSR(handle, (UWORD)(TASK_SR | (Kaffeine_GetSR(root) & 0x0700)));
    Kaffeine_SetA(handle, 7, sp);
    Kaffeine_SetPC(handle, initial);

    t->tcb = tcb;
    t->cpu = handle;
    t->state = SLOT_READY;
    t->pri = (BYTE)ctx->mem[tcb + TC_PRI];
    t->nest[0] = t->nest[1] = 0xFF;
    t->alloc = alloc;
    t->lower = lower;
    t->upper = upper;
    mirror(ctx, s, t);
    if (t->pri > s->current->pri)
        s->reschedule = TRUE;
    publish(ctx, s);
    return tcb;
}

/* ── Other Exec task functions ───────────────────────────────────── */

/* -288: RemTask(A1=task): NULL or the current child ends at the next slice
   boundary, as the thunk returns; another child at once. The root and any
   other Task stop the program, as before guest tasks. */
IPTR Kaffeine_TaskRemove(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t;
    ULONG tcb = THUNK_A(1);

    if (!s)
        return refuse(ctx, "RemTask(): no guest task scheduler");
    t = task_of(s, tcb ? tcb : s->current->tcb);
    if (!t || t == &s->root)
        return refuse(ctx, "RemTask(): only a child that AddTask() made here can be removed");
    if (t == s->current)
    {
        if (ctx->callback_depth)
            return refuse(ctx, "RemTask(): a callback cannot end its own task");
        t->state = SLOT_REMOVED;
        s->reschedule = TRUE;
        mirror(ctx, s, t);
        return 0;
    }
    t->state = SLOT_REMOVED;
    reap(ctx, s, t);
    s->reschedule = TRUE;
    return 0;
}

static ULONG sched_find_named(struct KaffeineContext *ctx, const char *name)
{
    struct KaffeineScheduler *s = ctx->sched;
    unsigned i;

    for (i = 0; s && name && i < KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = &s->child[i];
        const char *candidate;

        if (t->state == SLOT_FREE || t->state == SLOT_REMOVED)
            continue;
        candidate = guest_string(ctx, m68k_read32(ctx, t->tcb + TC_NAME));
        if (candidate && !strcmp(candidate, name))
            return t->tcb;
    }
    return 0;
}

/* -294: FindTask(A1=name), the guest part */
IPTR Kaffeine_TaskFindTask(struct KaffeineContext *ctx, void *cpu)
{
    ULONG at = THUNK_A(1);
    const char *name;

    if (!at)
        return this_task(ctx);
    name = guest_string(ctx, at);
    return name ? sched_find_named(ctx, name) : 0;
}

/* -300: SetTaskPri(A1=task, D0=priority) -> D0=old priority. The root's
   native task changes as before guest tasks. */
IPTR Kaffeine_TaskSetPri(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s ? task_of(s, THUNK_A(1) ? THUNK_A(1) : s->current->tcb) : NULL;
    BYTE pri = (BYTE)THUNK_D(0), old;

    if (!s)
    {
        ULONG task = THUNK_A(1);
        if ((task && task != ctx->guest_exec_task) ||
            !arena(ctx, ctx->guest_exec_task, M68K_SIZEOF_PROCESS))
            return refuse(ctx, "SetTaskPri(): not the root guest task");
        old = SetTaskPri(FindTask(NULL), pri);
        ctx->mem[ctx->guest_exec_task + TC_PRI] = (UBYTE)pri;
        return (IPTR)(LONG)old;
    }
    if (!t)
        return refuse(ctx, "SetTaskPri(): not a guest task of this program");
    old = t->pri;
    if (t == &s->root)
        old = SetTaskPri(FindTask(NULL), pri);
    t->pri = pri;
    if (arena(ctx, t->tcb, TC_SIZE))
        ctx->mem[t->tcb + TC_PRI] = (UBYTE)pri;
    s->reschedule = TRUE;
    publish(ctx, s);
    return (IPTR)(LONG)old;
}

/* -306: SetSignal(D0=newSignals, D1=signalSet) -> D0=old signals */
IPTR Kaffeine_TaskSetSignal(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t;
    ULONG set = THUNK_D(1), signals = THUNK_D(0) & set, old;

    if (!s)
        return refuse(ctx, "SetSignal(): no guest task scheduler");
    t = s->current;
    poll(ctx, s);
    old = t->recvd;
    t->recvd = (t->recvd & ~set) | signals;
    if (t == &s->root)
    {
        ULONG native = ~child_bits(s);

        t->recvd &= ~native;
        old = (old & ~native) | (SetSignal(signals & native, set & native) & native);
        return old;
    }
    mirror(ctx, s, t);
    return old;
}

/* -318: Wait(D0=signalSet) -> D0=signals. What is there returns at once;
   otherwise the task waits and D0 is written when it resumes. */
IPTR Kaffeine_TaskWait(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t;
    ULONG mask = THUNK_D(0), got;

    if (!s || (struct KaffeineCPU *)cpu != s->current->cpu)
        return refuse(ctx, "Wait(): not the CPU of the current guest task");
    t = s->current;
    poll(ctx, s);
    got = take(ctx, s, t, mask);
    if (got)
    {
        mirror(ctx, s, t);
        return got;
    }
    if (ctx->callback_depth)
        return refuse(ctx, "Wait(): a callback cannot wait while guest tasks run");
    t->wait = mask;
    t->retry = NULL;
    suspend(ctx, s, t);
    return 0;
}

/* -324: Signal(A1=task, D0=signalSet). The root's signals are native. */
static void sched_send_signal(struct KaffeineContext *ctx, ULONG task, ULONG set)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = task_of(s, task);
    if (!t) return;
    if (t == &s->root)
    {
        ULONG owned = child_bits(s);

        if (set & ~owned)
            Signal(FindTask(NULL), set & ~owned);
        t->recvd |= set & owned;
        if (t->state == SLOT_WAIT)
        {
            s->reschedule = TRUE;
            if (t->recvd & t->wait)
                wake(ctx, s, t);
        }
        return;
    }
    t->recvd |= set;
    if (t->state == SLOT_WAIT && (t->recvd & t->wait))
        wake(ctx, s, t);
    else
        mirror(ctx, s, t);
}

IPTR Kaffeine_TaskSignal(struct KaffeineContext *ctx, void *cpu)
{
    ULONG task = THUNK_A(1);
    if (!ctx->sched || !task_of(ctx->sched, task))
        return refuse(ctx, "Signal(): not a guest task of this program");
    sched_send_signal(ctx, task, THUNK_D(0));
    return 0;
}

static struct GuestTask *bit_owner(struct KaffeineScheduler *s, ULONG bit)
{
    unsigned i;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state != SLOT_FREE && (s->child[i].native & bit))
            return &s->child[i];
    return NULL;
}

static void sched_signal_allocated(struct KaffeineContext *ctx, LONG sig)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;
    ULONG bit;

    if (sig < 0 || sig > 31)
        return;
    bit = 1UL << sig;
    t->recvd &= ~bit;
    if (t == &s->root)
        return;
    t->native |= bit;
    t->alloc |= bit;
    mirror(ctx, s, t);
}

/* A child frees only its own bits, the root none of a child's. */
static BOOL sched_signal_freeable(struct KaffeineContext *ctx, LONG sig)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current, *owner;
    ULONG bit;

    if (sig < 0 || sig > 31)
        return TRUE;
    bit = 1UL << sig;
    owner = bit_owner(s, bit);
    if (owner != (t == &s->root ? NULL : t))
    {
        bug("[kaffeine] FreeSignal(%ld): not a signal of this guest task\n", (long)sig);
        return FALSE;
    }
    if (owner)
    {
        t->native &= ~bit;
        t->alloc &= ~bit;
        t->recvd &= ~bit;
        mirror(ctx, s, t);
    }
    return TRUE;
}

/* WaitPort() of an empty native port: the task waits for the port's signal
   while the others run; retry, the same thunk, then finds the message. */
static BOOL sched_block_port(struct KaffeineContext *ctx, void *cpu, struct MsgPort *port,
                             KaffeineTaskRetry retry, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;

    if (port->mp_MsgList.lh_TailPred != (struct Node *)&port->mp_MsgList)
        return FALSE;
    if (port->mp_Flags != 0 || port->mp_SigBit > 31 ||
        (t == &s->root ? ((1UL << port->mp_SigBit) & child_bits(s)) != 0 :
         ((1UL << port->mp_SigBit) & (t->native | unowned_bits(ctx))) == 0))
    {
        *result = refuse(ctx, "WaitPort(): native port cannot signal this guest task");
        return TRUE;
    }
    if (ctx->callback_depth || (struct KaffeineCPU *)cpu != t->cpu)
    {
        *result = refuse(ctx, "a callback cannot wait for a port while guest tasks run");
        return TRUE;
    }
    t->wait = 1UL << port->mp_SigBit;
    t->retry = retry;
    suspend(ctx, s, t);
    *result = 0;
    return TRUE;
}

static BOOL sched_known_task(struct KaffeineContext *ctx, ULONG task)
{
    return task_of(ctx->sched, task) != NULL;
}

static BOOL sched_owns_signal(struct KaffeineContext *ctx, ULONG task, ULONG bit)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = task_of(s, task);
    if (!t || !bit || !(ctx->guest_signals & bit)) return FALSE;
    return t == &s->root ? !(child_bits(s) & bit) : (t->native & bit) == bit;
}

/* Queue geometry is checked by Kaffeine_GuestGetMsg before it calls here.
   Only the receiving task can wait for its empty PA_SIGNAL port. */
static BOOL sched_block_guest_port(struct KaffeineContext *ctx, ULONG port,
                                   KaffeineTaskRetry retry, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;
    ULONG owner, bit;
    if (!arena(ctx, port, 34)) return FALSE;
    owner = m68k_read32(ctx, port + 16);
    bit = ctx->mem[port + 15];
    if (ctx->mem[port + 14] || bit > 31 || owner != t->tcb ||
        !sched_owns_signal(ctx, owner, 1UL << bit)) return FALSE;
    if (ctx->callback_depth)
    {
        *result = refuse(ctx, "a callback cannot wait for a guest port");
        return TRUE;
    }
    t->wait = 1UL << bit;
    t->retry = retry;
    suspend(ctx, s, t);
    *result = 0;
    return TRUE;
}

const struct KaffeineTaskOps Kaffeine_GuestTaskOps =
{
    Kaffeine_TaskAdd, Kaffeine_TaskRemove, sched_find_named, Kaffeine_TaskSetPri,
    Kaffeine_TaskSetSignal, Kaffeine_TaskWait, Kaffeine_TaskSignal,
    sched_signal_allocated, sched_signal_freeable, sched_block_port,
    sched_boundary, sched_end, sched_known_task, sched_owns_signal,
    sched_send_signal, sched_block_guest_port
};

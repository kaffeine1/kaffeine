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

    CreateProc() of the root makes a child that is a guest Process: Process,
    stack, name and SegArray in one guest heap MemList of its tc_MemEntry,
    the seglist the caller's. Its SIGB_DOS is a guest signal of that child
    alone, never a native bit, for its pr_MsgPort. It has no DOS context:
    no streams, current directory or CLI, and no dos.library call of it
    reaches native DOS, which would act for the root's native process.
*/

#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
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
    G_NT_TASK = 1, G_NT_MSGPORT = 4, G_NT_MESSAGE = 5, G_NT_MEMORY = 10, G_NT_PROCESS = 13,
    G_TS_RUN = 2, G_TS_READY = 3, G_TS_WAIT = 4, G_TS_REMOVED = 6,
    /* MemLists of one task that RemTask() frees */
    MAX_MEMENTRY = 32,
    /* The classic Process past its Task, dos/dosextens.h of the NDK */
    MP_FLAGS = 14, MP_SIGBIT = 15, MP_SIGTASK = 16, MP_MSGLIST = 20, LH_TYPE = 12,
    PR_LOCALVARS = 208,
    /* dos.library Exit() */
    DOS_EXIT_LVO = 144
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
    ULONG size;                 /* a child's reserved TCB: TC_SIZE, or a Process */
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

/* A child of CreateProc(): a guest Process without DOS context */
static BOOL is_process(struct KaffeineScheduler *s, const struct GuestTask *t)
{
    return t && t != &s->root && t->size == M68K_SIZEOF_PROCESS;
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

/* Native bits that no guest task allocated, such as those of native ports,
   but not the private ones of asynchronous I/O */
static ULONG unowned_bits(struct KaffeineContext *ctx)
{
    return ~ctx->guest_signals & ~SYSTEM_SIGNALS & ~ctx->io_signals;
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

/* Completed asynchronous I/O to its guest ports first, whose signals then
   wake their tasks. FALSE when that stopped the program. */
static BOOL poll(struct KaffeineContext *ctx, struct KaffeineScheduler *s)
{
    ULONG want, got;

    if (ctx->io_poll && !ctx->io_poll(ctx))
        return FALSE;
    want = wake_bits(ctx, s) | child_bits(s);
    if (want && (got = SetSignal(0, 0) & want))
        deliver(ctx, s, got);
    return TRUE;
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
    /* A copied timer request there is the heap's I/O release hook's to
       decide about: an idle one goes, one in flight keeps the memory. */
    if (!Kaffeine_ReleasableMemory(ctx, entry, size)) return FALSE;
    for (i = 0; i < count; i++)
    {
        ULONG member = m68k_read32(ctx, entry + MEMLIST_HEADER + i * 8);
        ULONG bytes = m68k_read32(ctx, entry + MEMLIST_HEADER + i * 8 + 4);
        if (bytes && !Kaffeine_ReleasableMemory(ctx, member, bytes)) return FALSE;
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
    BOOL polled = TRUE;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state == SLOT_REMOVED)
            reap(ctx, s, &s->child[i]);
    if (!poll(ctx, s))
    {
        stop(ctx, "a completed I/O request cannot reach its guest port");
        return;
    }
    for (;;)
    {
        next = pick(s);
        if (!next)
        {
            ULONG want, got;

            /* What completed since may wake a task without a native wait. */
            if (!polled)
            {
                polled = TRUE;
                if (!poll(ctx, s))
                {
                    stop(ctx, "a completed I/O request cannot reach its guest port");
                    return;
                }
                continue;
            }
            polled = FALSE;
            want = wake_bits(ctx, s);
            if (!want)
            {
                stop(ctx, "every guest task waits for signals that nothing can send");
                return;
            }
            /* No guest task can run: the native task waits for them all,
               and for the private bits of I/O in flight, which the next
               poll turns into guest port signals. */
            got = Wait(want | ctx->io_signals) & want;
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

/* Bytes of another guest task: its Task, all of a Process, and its entire
   reserved stack. */
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
        if (overlap(addr, size, t->tcb, t->size) ||
            overlap(addr, size, t->lower, t->upper - t->lower))
            return TRUE;
    }
    return FALSE;
}

/* A new child in slot t: its CPU at initial in supervisor mode, all other
   registers 0 apart from A7, then the state that the scheduler keeps. */
static void start_guest(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                        struct GuestTask *t, struct KaffeineCPU *handle, struct KaffeineCPU *root,
                        ULONG tcb, ULONG size, ULONG initial, ULONG sp, ULONG lower, ULONG upper,
                        ULONG alloc)
{
    unsigned i;

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
    t->size = size;
    mirror(ctx, s, t);
    if (t->pri > s->current->pri)
        s->reschedule = TRUE;
    publish(ctx, s);
}

static ULONG initial_sigalloc(struct KaffeineContext *ctx, ULONG alloc)
{
    if (!alloc && execbase(ctx))
        alloc = m68k_read32(ctx, ctx->m68k_sysbase + KAFFEINE_EXECBASE_TASKSIGALLOC);
    return alloc ? alloc : SYSTEM_SIGNALS;
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
    alloc = initial_sigalloc(ctx, m68k_read32(ctx, tcb + TC_SIGALLOC));
    sp -= 4;
    m68k_write32(ctx, sp, final ? final : KAFFEINE_TASK_EXIT_ADDR);
    m68k_write32(ctx, tcb + TC_SPREG, sp);

    start_guest(ctx, s, t, handle, root, tcb, TC_SIZE, initial, sp, lower, upper, alloc);
    return tcb;
}

/* ── CreateProc() ────────────────────────────────────────────────── */

/* The first segment of a seglist: a chain of at most KAFFEINE_PROC_MAX_SEGS
   [next BPTR][code] segments in the guest heap, without a cycle, whose
   first has code at seg + 4. Nothing here follows that code. */
static ULONG seglist_start(struct KaffeineContext *ctx, ULONG bptr)
{
    ULONG seen[KAFFEINE_PROC_MAX_SEGS];
    unsigned n = 0, j;

    while (bptr)
    {
        ULONG seg;

        if (n == KAFFEINE_PROC_MAX_SEGS || bptr >= 0x40000000UL)
            return 0;
        seg = bptr << 2;
        if (seg < KAFFEINE_HEAP_BASE || !arena(ctx, seg, 8))
            return 0;
        for (j = 0; j < n; j++)
            if (seen[j] == seg)
                return 0;
        seen[n++] = seg;
        bptr = m68k_read32(ctx, seg);
    }
    return n && code_address(ctx, seen[0] + 4) ? seen[0] : 0;
}

/* CreateProc(D1=name, D2=pri, D3=seglist, D4=stackSize) of the root -> D0
   pr_MsgPort. Everything is checked before anything changes; what Exec or
   DOS could not use stops the program, a full table, a failed CPU or a
   guest heap without room returns 0 with ERROR_NO_FREE_STORE and leaves
   nothing allocated. The Process starts as NewAddTask() starts a Task, at
   seg + 4 with the end address and the stack size on its stack, D0 0, A0
   an empty argument string and A6 SysBase. */
IPTR Kaffeine_ProcCreate(struct KaffeineContext *ctx, void *cpu, LONG *ioerr)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct KaffeineCPU *caller = (struct KaffeineCPU *)cpu, *root, *handle;
    struct GuestTask *t;
    ULONG bptr = THUNK_D(3), seg, stack, entry, proc, lower, upper, copy, segarray, sp, list, port;
    LONG request = (LONG)THUNK_D(4);
    ULONG spec[8];
    const char *text;
    char name[KAFFEINE_PROC_MAX_NAME + 1];
    size_t length = 0;
    unsigned i, slot;

    *ioerr = 0;
    if (!ctx->cpu_factory || !arena(ctx, ctx->guest_exec_task, M68K_SIZEOF_PROCESS) ||
        ctx->cpu != caller || (s && s->current->cpu != caller))
        return refuse(ctx, "CreateProc(): guest processes need a backend CPU factory and the root Task");
    if (s && s->current != &s->root)
    {
        /* As before CreateProc(): only the root starts processes. */
        bug("[kaffeine] CreateProc(): only the root task can create a process\n");
        *ioerr = ERROR_NOT_IMPLEMENTED;
        return 0;
    }
    root = s ? s->root.cpu : caller;
    text = guest_string(ctx, THUNK_D(1));
    if (!text || (length = strlen(text)) > KAFFEINE_PROC_MAX_NAME)
        return refuse(ctx, "CreateProc(): the name is not a guest string of at most 255 bytes");
    /* A copy now: allocation below may reuse free memory that held it. */
    memcpy(name, text, length + 1);
    seg = seglist_start(ctx, bptr);
    if (!seg)
        return refuse(ctx, "CreateProc(): the seglist is not a bounded chain of guest code");
    stack = request > 0 ? (ULONG)request : 0;
    if (stack > KAFFEINE_PROC_MAX_STACK)
    {
        bug("[kaffeine] CreateProc(%s): a stack of %lu bytes is more than the guest heap gives\n",
            name, (unsigned long)stack);
        *ioerr = ERROR_NO_FREE_STORE;
        return 0;
    }
    stack = (stack + 3) & ~3UL;
    if (stack < KAFFEINE_TASK_MIN_STACK + 8)
        stack = KAFFEINE_TASK_MIN_STACK + 8;

    for (slot = 0; s && slot < KAFFEINE_MAX_GUEST_TASKS && s->child[slot].state != SLOT_FREE; slot++)
        ;
    if (slot == KAFFEINE_MAX_GUEST_TASKS)
    {
        bug("[kaffeine] CreateProc(%s): already %d guest tasks\n", name, KAFFEINE_MAX_GUEST_TASKS);
        *ioerr = ERROR_NO_FREE_STORE;
        return 0;
    }
    handle = ctx->cpu_factory->create(ctx);
    if (!handle)
    {
        *ioerr = ERROR_NO_FREE_STORE;
        return 0;
    }
    /* The Process, its stack, its name, and its SegArray with a zero
       longword after it, the empty argument string */
    spec[0] = spec[2] = spec[4] = spec[6] = MEMF_PUBLIC | MEMF_CLEAR;
    spec[1] = M68K_SIZEOF_PROCESS;
    spec[3] = stack;
    spec[5] = (ULONG)length + 1;
    spec[7] = 20;
    entry = Kaffeine_HeapAllocList(ctx, spec, 4);
    if (entry & 0x80000000UL)
    {
        ctx->cpu_factory->destroy(ctx, handle);
        *ioerr = ERROR_NO_FREE_STORE;
        return 0;
    }
    proc = m68k_read32(ctx, entry + MEMLIST_HEADER);
    lower = m68k_read32(ctx, entry + MEMLIST_HEADER + 8);
    copy = m68k_read32(ctx, entry + MEMLIST_HEADER + 16);
    segarray = m68k_read32(ctx, entry + MEMLIST_HEADER + 24);
    upper = lower + stack;
    /* What the heap made must be guest memory that no task holds, and the
       seglist must have survived it unchanged. */
    for (i = 0; i < 4; i++)
    {
        ULONG member = m68k_read32(ctx, entry + MEMLIST_HEADER + 8 * i), bytes = spec[2 * i + 1];

        if ((member & 3) || member < KAFFEINE_HEAP_BASE || !arena(ctx, member, bytes) ||
            !Kaffeine_GuestBytes(ctx, member, bytes) || claimed(ctx, s, member, bytes) ||
            overlap(member, bytes, seg, 8))
            break;
    }
    if (i < 4 || claimed(ctx, s, entry, MEMLIST_HEADER + 32) ||
        overlap(entry, MEMLIST_HEADER + 32, seg, 8) || seglist_start(ctx, bptr) != seg)
    {
        Kaffeine_HeapFreeEntry(ctx, entry);
        ctx->cpu_factory->destroy(ctx, handle);
        return refuse(ctx, "CreateProc(): the guest heap gave memory that is not free for a process");
    }
    if (!s && !(s = activate(ctx, caller)))
    {
        Kaffeine_HeapFreeEntry(ctx, entry);
        ctx->cpu_factory->destroy(ctx, handle);
        *ioerr = ERROR_NO_FREE_STORE;
        return 0;
    }
    t = &s->child[slot];

    /* The Task, as AddTask() completes it, owning the MemList */
    ctx->mem[entry + TC_TYPE] = G_NT_MEMORY;
    list = proc + TC_MEMENTRY;
    m68k_write32(ctx, list, entry);
    m68k_write32(ctx, list + LH_TAIL, 0);
    m68k_write32(ctx, list + LH_TAILPRED, entry);
    m68k_write32(ctx, entry, list + LH_TAIL);
    m68k_write32(ctx, entry + TC_PRED, list);
    ctx->mem[proc + TC_TYPE] = G_NT_PROCESS;
    ctx->mem[proc + TC_PRI] = (UBYTE)(BYTE)THUNK_D(2);
    memcpy(ctx->mem + copy, name, length + 1);
    m68k_write32(ctx, proc + TC_NAME, copy);
    ctx->mem[proc + TC_IDNESTCNT] = 0xFF;
    ctx->mem[proc + TC_TDNESTCNT] = 0xFF;
    sp = upper - 8;
    m68k_write32(ctx, sp, KAFFEINE_TASK_EXIT_ADDR);
    m68k_write32(ctx, sp + 4, stack);
    m68k_write32(ctx, proc + TC_SPREG, sp);
    m68k_write32(ctx, proc + TC_SPLOWER, lower);
    m68k_write32(ctx, proc + TC_SPUPPER, upper);
    /* pr_MsgPort: PA_SIGNAL on the child's own SIGB_DOS */
    port = proc + M68K_PR_MSGPORT;
    ctx->mem[port + TC_TYPE] = G_NT_MSGPORT;
    ctx->mem[port + MP_FLAGS] = 0;
    ctx->mem[port + MP_SIGBIT] = SIGB_DOS;
    m68k_write32(ctx, port + MP_SIGTASK, proc);
    m68k_write32(ctx, port + MP_MSGLIST, port + MP_MSGLIST + LH_TAIL);
    m68k_write32(ctx, port + MP_MSGLIST + LH_TAILPRED, port + MP_MSGLIST);
    ctx->mem[port + MP_MSGLIST + LH_TYPE] = G_NT_MESSAGE;
    /* SegArray { 3, 0, 0, seglist }: no BCPL segments, no global vector */
    m68k_write32(ctx, segarray, 3);
    m68k_write32(ctx, segarray + 12, bptr);
    m68k_write32(ctx, proc + M68K_PR_SEGLIST, segarray >> 2);
    m68k_write32(ctx, proc + M68K_PR_STACKSIZE, stack);
    m68k_write32(ctx, proc + M68K_PR_STACKBASE, upper >> 2);
    m68k_write32(ctx, proc + PR_LOCALVARS, proc + PR_LOCALVARS + LH_TAIL);
    m68k_write32(ctx, proc + PR_LOCALVARS + LH_TAILPRED, proc + PR_LOCALVARS);

    start_guest(ctx, s, t, handle, root, proc, M68K_SIZEOF_PROCESS, seg + 4, sp, lower, upper,
                initial_sigalloc(ctx, 0) | SIGF_DOS);
    Kaffeine_SetA(handle, 0, segarray + 16);
    Kaffeine_SetA(handle, 6, ctx->m68k_sysbase);
    bug("[kaffeine] CreateProc(%s): guest process %08lx, port %08lx, stack %lu\n",
        name, (unsigned long)proc, (unsigned long)port, (unsigned long)stack);
    return port;
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
    if (!poll(ctx, s))
        return 20;
    /* The root's private I/O bits are the bridge's, never shown or changed */
    if (t == &s->root)
    {
        set &= ~ctx->io_signals;
        signals &= set;
    }
    old = t->recvd;
    t->recvd = (t->recvd & ~set) | signals;
    if (t == &s->root)
    {
        ULONG native = ~child_bits(s) & ~ctx->io_signals;

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
    if (!poll(ctx, s))
        return 20;
    /* The root never waits for, nor takes, the private bits of I/O */
    if (t == &s->root)
        mask &= ~ctx->io_signals;
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

        set &= ~ctx->io_signals;
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

static BOOL sched_process_port(struct KaffeineContext *ctx, ULONG port, ULONG owner)
{
    struct KaffeineScheduler *s = ctx->sched;

    return s && is_process(s, task_of(s, owner)) && port == owner + M68K_PR_MSGPORT;
}

/* Before the thunk of every dos.library call: a CreateProc() child has no
   DOS context of its own, and native DOS would act for the root's process. */
static BOOL sched_process_dos(struct KaffeineContext *ctx, void *cpu, ULONG lvo, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;

    if (!is_process(s, t))
        return FALSE;
    if (lvo == DOS_EXIT_LVO && !ctx->callback_depth && (struct KaffeineCPU *)cpu == t->cpu)
    {
        /* As RemTask(NULL): this child alone ends at the slice boundary. */
        bug("[kaffeine] Exit() of guest process %08lx ends that process\n",
            (unsigned long)t->tcb);
        t->state = SLOT_REMOVED;
        s->reschedule = TRUE;
        mirror(ctx, s, t);
        *result = 0;
        return TRUE;
    }
    bug("[kaffeine] dos.library LVO -%lu of guest process %08lx\n",
        (unsigned long)lvo, (unsigned long)t->tcb);
    *result = refuse(ctx, "a CreateProc() process has no DOS context for dos.library");
    return TRUE;
}

/* Queue geometry is checked by Kaffeine_GuestGetMsg before it calls here.
   Only the receiving task can wait for its empty PA_SIGNAL port; a child
   Process also for its pr_MsgPort, on its own SIGB_DOS. */
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
        !(sched_owns_signal(ctx, owner, 1UL << bit) ||
          (bit == SIGB_DOS && sched_process_port(ctx, port, owner)))) return FALSE;
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

static BOOL sched_is_root(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;

    return s && s->current == &s->root && (struct KaffeineCPU *)cpu == s->root.cpu;
}

const struct KaffeineTaskOps Kaffeine_GuestTaskOps =
{
    Kaffeine_TaskAdd, Kaffeine_TaskRemove, sched_find_named, Kaffeine_TaskSetPri,
    Kaffeine_TaskSetSignal, Kaffeine_TaskWait, Kaffeine_TaskSignal,
    sched_signal_allocated, sched_signal_freeable, sched_block_port,
    sched_boundary, sched_end, sched_known_task, sched_owns_signal,
    sched_send_signal, sched_block_guest_port,
    Kaffeine_ProcCreate, sched_process_port, sched_process_dos,
    sched_is_root
};

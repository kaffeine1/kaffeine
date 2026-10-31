/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

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

    Typed CreateNewProc() instead imports code into this same arena and
    gives its guest Process a private native DOS broker. The driver waits
    while that broker executes one selected compiled thunk; no guest CPU
    callback runs there. Guest Exec waits and ports remain cooperative.
    A typed child keeps the domain alive after the root returns, without
    changing the program-end lifetime of classic-only AddTask/CreateProc.
*/

#include <aros/debug.h>
#include "kaffeine_async.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_offsets.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"
#include "kaffeine_messages.h"
#include "kaffeine_workbench.h"
#include "kaffeine_process.h"
#include "kaffeine_dos_broker.h"
#include "kaffeine_domain_modules.h"

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

enum { SLOT_FREE, SLOT_READY, SLOT_RUN, SLOT_WAIT, SLOT_REMOVED, SLOT_RECOVERY };

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
    ULONG io_request;           /* private WaitIO retry identity; no signal bits */
    ULONG recvd;                /* a child's signals; the root's on child bits */
    ULONG alloc;                /* a child's tc_SigAlloc */
    ULONG native;               /* native bits that a child allocated */
    ULONG lower;                /* a child's tc_SPLower */
    ULONG upper;                /* full stack reservation, including live frames */
    ULONG turn;                 /* when it last got the CPU */
    ULONG size;                 /* a child's reserved TCB: TC_SIZE, or a Process */
    /* Native-only lifetime owners: never copied into the guest arena. */
    struct KaffeineDosBroker *broker;
    struct KaffeineDomainModule *module;
    ULONG dos_lvo;
    BOOL dos_pending;
    ULONG packet_port, packet_msg; /* private synchronous broker arguments */
    BOOL packet_file, packet_pending;
    ULONG cli;                  /* guest CLI cache of this DOS owner */
    ULONG argptr, argsize;
    ULONG startup_entry;        /* private staged MemList; never read from guest links */
    char program_name[256];
};

struct KaffeineScheduler
{
    struct GuestTask root;
    struct GuestTask child[KAFFEINE_MAX_GUEST_TASKS];
    struct GuestTask *current;  /* NULL only while a removed current goes */
    ULONG slices, turns;
    BOOL  reschedule;
    BOOL root_done, root_drained;
    LONG root_result;
};

static void packet_clear(struct GuestTask *);

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

        if (t->tcb == tcb && t->state != SLOT_FREE && t->state != SLOT_REMOVED &&
            t->state != SLOT_RECOVERY)
            return t;
    }
    return NULL;
}

/* A child Process; only the typed path gives it a native DOS broker. */
static BOOL is_process(struct KaffeineScheduler *s, const struct GuestTask *t)
{
    return t && t != &s->root && t->size == M68K_SIZEOF_PROCESS;
}

/* AddTask() runs in its root DOS context as before. A typed Process owns
   its own guest Cli()/program-name view, matching its private native DOS. */
static struct GuestTask *dos_owner(struct KaffeineScheduler *s, struct GuestTask *t)
{
    return t && t->broker ? t : &s->root;
}

static void dos_save(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                     struct GuestTask *t)
{
    struct GuestTask *owner = dos_owner(s, t);
    owner->cli = ctx->m68k_cli;
    owner->argptr = ctx->m68k_argptr;
    owner->argsize = ctx->m68k_argsize;
    memcpy(owner->program_name, ctx->program_name, sizeof(owner->program_name));
    if (arena(ctx, owner->tcb, M68K_SIZEOF_PROCESS))
        m68k_write32(ctx, owner->tcb + M68K_PR_CLI, owner->cli >> 2);
}

static void dos_load(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                     struct GuestTask *t)
{
    struct GuestTask *owner = dos_owner(s, t);
    ctx->m68k_cli = owner->cli;
    ctx->m68k_argptr = owner->argptr;
    ctx->m68k_argsize = owner->argsize;
    ctx->guest_exec_task = owner->tcb;
    memcpy(ctx->program_name, owner->program_name, sizeof(ctx->program_name));
}

/* Startup and retirement may need native DOS before/after the child CPU's
   lifetime. No guest instruction or dispatcher executes in this scope. A
   nested startup restores the parent's broker owner as well as its view. */
struct DosScope
{
    struct GuestTask *previous;
    struct KaffeineCPU *cpu;
    struct Task *broker_owner;
    void (*after_thunk)(struct KaffeineContext *);
    ULONG this_task;
    BOOL broker_call;
};

static void dos_scope_begin(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                            struct GuestTask *t, struct KaffeineCPU *cpu,
                            struct DosScope *scope)
{
    scope->previous = s->current;
    scope->cpu = ctx->cpu;
    scope->this_task = this_task(ctx);
    scope->broker_call = ctx->broker_call;
    scope->broker_owner = ctx->broker_owner;
    scope->after_thunk = ctx->after_thunk;
    dos_save(ctx, s, scope->previous);
    dos_load(ctx, s, t);
    set_this_task(ctx, t->tcb);
    if (cpu) ctx->cpu = cpu;
    ctx->broker_call = TRUE;
    ctx->after_thunk = NULL;
}

static void dos_scope_end(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                          struct GuestTask *t, const struct DosScope *scope)
{
    dos_save(ctx, s, t);
    dos_load(ctx, s, scope->previous);
    ctx->cpu = scope->cpu;
    set_this_task(ctx, scope->this_task);
    ctx->broker_call = scope->broker_call;
    ctx->broker_owner = scope->broker_owner;
    ctx->after_thunk = scope->after_thunk;
}

static BOOL finish_process(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                           struct GuestTask *t)
{
    struct DosScope scope;
    BOOL okay;
    packet_clear(t); /* no packet RPC survives its synchronous acknowledgement */
    if (!t->broker) return TRUE;
    dos_scope_begin(ctx, s, t, NULL, &scope);
    okay = Kaffeine_DosBrokerFinish(t->broker, ctx, Kaffeine_ProcessDropGuestDos);
    dos_scope_end(ctx, s, t, &scope);
    return okay;
}

/* No guest resumes while retirement lacks an ACK. IsIdle is metadata-only:
   never put a different guest view under a worker that may still use ctx.
   The sole-driver contract excludes new submissions during this wait. */
static BOOL finish_process_wait(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                                struct GuestTask *t)
{
    BOOL clean = TRUE;
    if (!t->broker) return TRUE;
    while (!Kaffeine_DosBrokerIsIdle(t->broker) || !finish_process(ctx, s, t))
    {
        if (clean)
        {
            refuse(ctx, "a Process retirement awaits its DOS broker acknowledgement");
            clean = FALSE;
        }
        Delay(1);
    }
    if (!clean)
        Kaffeine_StopGuest(ctx, ctx->cpu, 20);
    return clean;
}

static IPTR drop_guest_dos(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return Kaffeine_ProcessDropGuestDos(ctx, NULL);
}

static unsigned children(struct KaffeineScheduler *s)
{
    unsigned i, n = 0;

    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        n += s->child[i].state != SLOT_FREE && s->child[i].state != SLOT_RECOVERY;
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
    return ~ctx->guest_signals & ~SYSTEM_SIGNALS & ~Kaffeine_AsyncSignalMask(ctx);
}

/* The guest Task fields that Exec would keep, in BE */
static void mirror(struct KaffeineContext *ctx, struct KaffeineScheduler *s,
                   struct GuestTask *t)
{
    static const UBYTE states[] = { 0, G_TS_READY, G_TS_RUN, G_TS_WAIT, G_TS_REMOVED, G_TS_REMOVED };

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
    if (Kaffeine_LayerInfoLocked(ctx))
    {
        Kaffeine_StopGuest(ctx, t->cpu, 20);
        return;
    }
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

    got &= ~Kaffeine_LayerSignalMask(ctx);
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

        if (t->state == SLOT_FREE || t->state == SLOT_REMOVED || t->state == SLOT_RECOVERY)
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

    if (!Kaffeine_AsyncPoll(ctx))
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
    ULONG got;

    /* Internal legacy WaitIO port retries may consume their private IO
       bit; no guest path may observe the separate layer control bit. */
    mask &= ~Kaffeine_LayerSignalMask(ctx);
    got = t->recvd & mask;
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
    {
        nest_save(ctx, cur);
        dos_save(ctx, s, cur);
    }
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
    dos_load(ctx, s, next);
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
    BOOL clean;
    if (ctx->layer_ops && ctx->layer_ops->task_removed)
        ctx->layer_ops->task_removed(ctx, tcb);
    clean = finish_process_wait(ctx, s, t);

    if (t == s->current)
        dos_save(ctx, s, t);

    /* Quarantine native requests before the task's signals or stack can
       be reused. Late replies must never write into that old stack. */
    if (ctx->io_task_removed)
        ctx->io_task_removed(ctx, tcb);
    if (ctx->idcmp_task_gone) ctx->idcmp_task_gone(ctx, tcb);

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
        dos_load(ctx, s, &s->root);
        set_this_task(ctx, s->root.tcb);
    }
    if (!clean)
        Kaffeine_StopGuest(ctx, ctx->cpu, 20);
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
    if (ctx->cpu_factory)
        ctx->cpu_factory->destroy(ctx, cpu);
    else
        bug("[kaffeine] guest task %08lx: no backend to destroy its CPU\n", (unsigned long)tcb);
    /* The driver is outside every thunk; no broker call or CPU can still
       use the module. A busy module remains on the domain's cleanup list. */
    if (t->module)
        (void)Kaffeine_DomainUnload(ctx, t->module);
    memset(t, 0, sizeof(*t));
    publish(ctx, s);
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
    s->root.cli = ctx->m68k_cli;
    s->root.argptr = ctx->m68k_argptr;
    s->root.argsize = ctx->m68k_argsize;
    memcpy(s->root.program_name, ctx->program_name, sizeof(s->root.program_name));
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

    /* Self-removal can end an Info scope. Release the current owner before
       an earlier removed slot can send a broker cleanup RPC. */
    if (s->current && s->current->state == SLOT_REMOVED && Kaffeine_LayerInfoLocked(ctx) &&
        ctx->layer_ops->task_removed)
        ctx->layer_ops->task_removed(ctx, s->current->tcb);
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state == SLOT_REMOVED)
            reap(ctx, s, &s->child[i]);
    if (!ctx->running)
        return;
    if (s->root_done && !children(s))
    {
        s->root_drained = TRUE;
        s->current = &s->root;
        ctx->cpu = s->root.cpu;
        nest_load(ctx, &s->root);
        dos_load(ctx, s, &s->root);
        set_this_task(ctx, s->root.tcb);
        Kaffeine_SetD(s->root.cpu, 0, (ULONG)s->root_result);
        ctx->exit_code = s->root_result;
        ctx->running = FALSE;
        return;
    }
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
                BOOL io_waiter = s->root.state == SLOT_WAIT && s->root.io_request;
                for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
                    if (s->child[i].state == SLOT_WAIT && s->child[i].io_request)
                        io_waiter = TRUE;
                /* A foreign paint producer can wake a virtual break-bit
                   waiter even when no guest wait mask has a native bit. */
                if ((!io_waiter || !ctx->io_signals) && !Kaffeine_LayerSignalMask(ctx))
                {
                    stop(ctx, "every guest task waits for signals that nothing can send");
                    return;
                }
            }
            /* No guest task can run: the native task waits for them all,
               and for the private bits of I/O in flight, which the next
               poll turns into guest port signals. */
            got = Wait(want | Kaffeine_AsyncSignalMask(ctx)) & want;
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
        else if (Kaffeine_LayerInfoLocked(ctx) ||
                 (++s->slices < KAFFEINE_TASK_QUANTUM && !s->reschedule) || forbidden(ctx))
            return;
    }
    schedule(ctx, s);
}

/* Normal domain drain or a domain-wide fault. The keeper calls this only
   after the driver has stopped executing every CPU and thunk. */
static void sched_end(struct KaffeineContext *ctx)
{
    struct KaffeineScheduler *s = ctx->sched;
    unsigned i;

    if (ctx->layer_ops && ctx->layer_ops->release_locks)
        ctx->layer_ops->release_locks(ctx);
    if (s->current != &s->root)
    {
        dos_save(ctx, s, s->current);
        nest_load(ctx, &s->root);
    }
    s->current = &s->root;
    ctx->cpu = s->root.cpu;
    dos_load(ctx, s, &s->root);
    if (!s->root_done && ctx->layer_ops && ctx->layer_ops->task_removed)
        ctx->layer_ops->task_removed(ctx, s->root.tcb);
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
    {
        struct GuestTask *t = &s->child[i];

        if (t->state == SLOT_FREE)
            continue;
        /* Root's preregistered native records must acknowledge staged
           rollback before this slot, backend or shared arena can go. */
        while (t->state == SLOT_RECOVERY)
        {
            Kaffeine_StopGuest(ctx, ctx->cpu, 20);
            if (ctx->domain_drain) (void)ctx->domain_drain(ctx);
            if (t->state == SLOT_RECOVERY) Delay(1);
        }
        if (t->state == SLOT_FREE)
            continue;
        bug("[kaffeine] guest task %08lx ends with the program\n", (unsigned long)t->tcb);
        if (ctx->layer_ops && ctx->layer_ops->task_removed)
            ctx->layer_ops->task_removed(ctx, t->tcb);
        (void)finish_process_wait(ctx, s, t);
        if (ctx->io_task_removed)
            ctx->io_task_removed(ctx, t->tcb);
        if (ctx->idcmp_task_gone) ctx->idcmp_task_gone(ctx, t->tcb);
        if (ctx->cpu_factory)
            ctx->cpu_factory->destroy(ctx, t->cpu);
        if (t->module)
            (void)Kaffeine_DomainUnload(ctx, t->module);
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

    if (overlap(addr, size, s ? s->root.tcb : ctx->guest_exec_task, M68K_SIZEOF_PROCESS) ||
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

/* The typed parser and importer completed all native-object validation.
   This last transaction reserves only guest startup state and one CPU;
   broker/module remain the caller's on every failure. */
ULONG Kaffeine_SchedStartProcess(struct KaffeineContext *ctx, void *cpu,
                                const struct KaffeineProcessStart *spec,
                                ULONG guest_bptr, ULONG initial,
                                struct KaffeineDosBroker *broker,
                                struct KaffeineDomainModule *owned_module,
                                SIPTR *error)
{
    struct KaffeineScheduler *s = ctx ? ctx->sched : NULL;
    struct KaffeineCPU *caller = (struct KaffeineCPU *)cpu, *handle, *root;
    struct GuestTask *t;
    const char *args, *command;
    const char *end;
    ULONG sizes[12], members[6], entry, proc, lower, upper, name, arguments, segarray, cli = 0;
    ULONG stack, sp, list, port, count, i, j, slot, length, arglen, commandlen;
    struct DosScope scope;
    SIPTR local_error, native_error = 0;
    IPTR initialized = 0, dropped = 0;
    BOOL newly_active = !s, okay;

    if (!error) error = &local_error;
    *error = ERROR_NOT_IMPLEMENTED;
    if (!ctx || !spec || !broker || !owned_module || !caller || !ctx->cpu_factory ||
        !arena(ctx, ctx->guest_exec_task, M68K_SIZEOF_PROCESS) ||
        ctx->cpu != caller || ctx->callback_depth ||
        (s && (!s->current || s->current->cpu != caller || s->current->state != SLOT_RUN ||
               (is_process(s, s->current) && !s->current->broker))))
        return 0;
    *error = ERROR_BAD_NUMBER;
    if (!spec->name || !(end = (const char *)memchr(spec->name, 0, KAFFEINE_PROCESS_MAX_NAME + 1)))
        return 0;
    length = (ULONG)(end - spec->name);
    args = spec->arguments ? spec->arguments : "";
    end = (const char *)memchr(args, 0, KAFFEINE_PROCESS_MAX_ARGS + 1);
    if (!end) return 0;
    arglen = (ULONG)(end - args);
    command = spec->command ? spec->command : spec->name;
    end = (const char *)memchr(command, 0, KAFFEINE_PROCESS_MAX_NAME + 1);
    if (!end) return 0;
    commandlen = (ULONG)(end - command);
    if (spec->stack_size < KAFFEINE_TASK_MIN_STACK ||
        spec->stack_size > KAFFEINE_PROCESS_MAX_STACK ||
        !guest_bptr || guest_bptr > 0x3FFFFFFFUL ||
        initial != (guest_bptr << 2) + 4 || !code_address(ctx, initial) ||
        !arena(ctx, guest_bptr << 2, 8))
        return 0;
    stack = (spec->stack_size + 3) & ~3UL;
    if (stack < KAFFEINE_TASK_MIN_STACK + 8)
        stack = KAFFEINE_TASK_MIN_STACK + 8;
    for (slot = 0; s && slot < KAFFEINE_MAX_GUEST_TASKS && s->child[slot].state != SLOT_FREE; slot++)
        ;
    *error = ERROR_NO_FREE_STORE;
    if (slot == KAFFEINE_MAX_GUEST_TASKS) return 0;
    handle = ctx->cpu_factory->create(ctx);
    if (!handle) return 0;

    /* One tracked MemList: Process, stack, name, argument string,
       SegArray and (when requested) the private guest CLI plus BSTRs. */
    count = spec->cli ? 6 : 5;
    sizes[1] = M68K_SIZEOF_PROCESS;
    sizes[3] = stack;
    sizes[5] = length + 1;
    sizes[7] = arglen + 1;
    sizes[9] = 16;
    sizes[11] = M68K_SIZEOF_CLI + 3 * M68K_CLI_STRING_SLOT;
    for (i = 0; i < count; i++)
        sizes[2 * i] = MEMF_PUBLIC | MEMF_CLEAR;
    entry = Kaffeine_HeapAllocList(ctx, sizes, (UWORD)count);
    if (entry & 0x80000000UL)
    {
        ctx->cpu_factory->destroy(ctx, handle);
        return 0;
    }
    if ((entry & 3) || !arena(ctx, entry, MEMLIST_HEADER + 8 * count) ||
        claimed(ctx, s, entry, MEMLIST_HEADER + 8 * count))
        goto fail;
    for (i = 0; i < count; i++)
    {
        ULONG bytes = sizes[2 * i + 1];
        members[i] = m68k_read32(ctx, entry + MEMLIST_HEADER + 8 * i);
        if ((members[i] & 3) || members[i] < KAFFEINE_HEAP_BASE ||
            !arena(ctx, members[i], bytes) || !Kaffeine_GuestBytes(ctx, members[i], bytes) ||
            claimed(ctx, s, members[i], bytes) ||
            overlap(members[i], bytes, guest_bptr << 2, 8) ||
            overlap(members[i], bytes, entry, MEMLIST_HEADER + 8 * count))
            goto fail;
        for (j = 0; j < i; j++)
            if (overlap(members[i], bytes, members[j], sizes[2 * j + 1]))
                goto fail;
    }
    if (overlap(entry, MEMLIST_HEADER + 8 * count, guest_bptr << 2, 8))
        goto fail;
    if (!s && !(s = activate(ctx, caller)))
        goto fail;
    root = s->root.cpu;
    t = &s->child[slot];
    proc = members[0]; lower = members[1]; name = members[2];
    arguments = members[3]; segarray = members[4]; upper = lower + stack;
    if (spec->cli) cli = members[5];

    ctx->mem[entry + TC_TYPE] = G_NT_MEMORY;
    list = proc + TC_MEMENTRY;
    m68k_write32(ctx, list, entry);
    m68k_write32(ctx, list + LH_TAIL, 0);
    m68k_write32(ctx, list + LH_TAILPRED, entry);
    m68k_write32(ctx, entry, list + LH_TAIL);
    m68k_write32(ctx, entry + TC_PRED, list);
    ctx->mem[proc + TC_TYPE] = G_NT_PROCESS;
    ctx->mem[proc + TC_PRI] = (UBYTE)spec->priority;
    memcpy(ctx->mem + name, spec->name, length + 1);
    memcpy(ctx->mem + arguments, args, arglen + 1);
    m68k_write32(ctx, proc + TC_NAME, name);
    ctx->mem[proc + TC_IDNESTCNT] = ctx->mem[proc + TC_TDNESTCNT] = 0xFF;
    sp = upper - 8;
    m68k_write32(ctx, sp, KAFFEINE_TASK_EXIT_ADDR);
    m68k_write32(ctx, sp + 4, stack);
    m68k_write32(ctx, proc + TC_SPREG, sp);
    m68k_write32(ctx, proc + TC_SPLOWER, lower);
    m68k_write32(ctx, proc + TC_SPUPPER, upper);
    port = proc + M68K_PR_MSGPORT;
    ctx->mem[port + TC_TYPE] = G_NT_MSGPORT;
    ctx->mem[port + MP_SIGBIT] = SIGB_DOS;
    m68k_write32(ctx, port + MP_SIGTASK, proc);
    m68k_write32(ctx, port + MP_MSGLIST, port + MP_MSGLIST + LH_TAIL);
    m68k_write32(ctx, port + MP_MSGLIST + LH_TAILPRED, port + MP_MSGLIST);
    ctx->mem[port + MP_MSGLIST + LH_TYPE] = G_NT_MESSAGE;
    m68k_write32(ctx, segarray, 3);
    m68k_write32(ctx, segarray + 12, guest_bptr);
    m68k_write32(ctx, proc + M68K_PR_SEGLIST, segarray >> 2);
    m68k_write32(ctx, proc + M68K_PR_STACKSIZE, stack);
    m68k_write32(ctx, proc + M68K_PR_STACKBASE, upper >> 2);
    m68k_write32(ctx, proc + M68K_PR_ARGUMENTS, arguments);
    m68k_write32(ctx, proc + M68K_PR_CLI, cli >> 2);
    m68k_write32(ctx, proc + PR_LOCALVARS, proc + PR_LOCALVARS + LH_TAIL);
    m68k_write32(ctx, proc + PR_LOCALVARS + LH_TAILPRED, proc + PR_LOCALVARS);
    if (cli)
    {
        ULONG bstr = cli + M68K_SIZEOF_CLI;
        ctx->mem[bstr] = (UBYTE)commandlen;
        memcpy(ctx->mem + bstr + 1, command, commandlen + 1);
        m68k_write32(ctx, cli + M68K_CLI_COMMANDNAME, bstr >> 2);
        m68k_write32(ctx, cli + M68K_CLI_SETNAME, (bstr + M68K_CLI_STRING_SLOT) >> 2);
        m68k_write32(ctx, cli + M68K_CLI_PROMPT, (bstr + 2 * M68K_CLI_STRING_SLOT) >> 2);
        m68k_write32(ctx, cli + M68K_CLI_DEFAULTSTACK, stack >> 2);
        m68k_write32(ctx, cli + M68K_CLI_MODULE, guest_bptr);
    }
    t->broker = broker;
    t->module = owned_module;
    t->tcb = proc;
    t->cpu = handle;
    t->cli = cli;
    t->argptr = arguments;
    t->argsize = arglen;
    t->state = SLOT_RECOVERY;
    t->size = M68K_SIZEOF_PROCESS;
    t->lower = lower;
    t->upper = upper;
    t->startup_entry = entry;
    memcpy(t->program_name, command, commandlen + 1);

    /* The broker supplies standard streams and installed lock snapshots
       for this Process before C startup can inspect pr_CIS/pr_COS. It
       owns the native references; only guest identities reach the TCB. */
    if (!Kaffeine_DosBrokerIsIdle(broker))
    {
        Kaffeine_StopGuest(ctx, caller, 20);
        return 0;
    }
    dos_scope_begin(ctx, s, t, handle, &scope);
    okay = Kaffeine_DosBrokerCall(broker, ctx, handle, 0,
                                  Kaffeine_ProcessInitGuestDos, &initialized, &native_error);
    dos_scope_end(ctx, s, t, &scope);
    if (!okay || initialized != 1)
    {
        *error = native_error ? native_error : ERROR_NO_FREE_STORE;
        /* Idempotent partial-setup rollback, without consuming broker.
           The caller still owns it and the module when we return zero. */
        okay = Kaffeine_DosBrokerIsIdle(broker);
        if (okay)
        {
            dos_scope_begin(ctx, s, t, handle, &scope);
            okay = Kaffeine_DosBrokerCall(broker, ctx, handle, 0,
                                          drop_guest_dos, &dropped, &native_error);
            dos_scope_end(ctx, s, t, &scope);
        }
        if (!okay)
        {
            /* No allocation on this failure path: the reserved slot keeps
               every byte/CPU alive until the caller's recovery record gets
               a Finish ACK on the stopped domain's driver. */
            Kaffeine_StopGuest(ctx, caller, 20);
            return 0;
        }
        memset(t, 0, sizeof(*t));
        if (newly_active)
            deactivate(ctx);
        goto fail;
    }
    start_guest(ctx, s, t, handle, root, proc, M68K_SIZEOF_PROCESS, initial, sp, lower, upper,
                initial_sigalloc(ctx, 0) | SIGF_DOS);
    Kaffeine_SetD(handle, 0, arglen);
    Kaffeine_SetA(handle, 0, arguments);
    Kaffeine_SetA(handle, 6, ctx->m68k_sysbase);
    t->startup_entry = 0;
    *error = 0;
    return proc;

fail:
    Kaffeine_HeapFreeEntry(ctx, entry);
    ctx->cpu_factory->destroy(ctx, handle);
    return 0;
}

static struct GuestTask *recovery_start(struct KaffeineContext *ctx,
                                       struct KaffeineDosBroker *broker)
{
    struct KaffeineScheduler *s = ctx ? ctx->sched : NULL;
    unsigned i;
    if (!s || !broker) return NULL;
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; ++i)
        if (s->child[i].state == SLOT_RECOVERY && s->child[i].broker == broker)
            return &s->child[i];
    return NULL;
}

BOOL Kaffeine_SchedHasRecoveryStart(struct KaffeineContext *ctx, struct KaffeineDosBroker *broker)
{
    return recovery_start(ctx, broker) != NULL;
}

BOOL Kaffeine_SchedRecoverStart(struct KaffeineContext *ctx, struct KaffeineDosBroker *broker)
{
    struct GuestTask *t = recovery_start(ctx, broker);
    ULONG entry, tcb;
    struct KaffeineCPU *cpu;

    /* IsIdle must precede even a temporary DOS view: an unknown busy job
       may still use this shared Context. No guest/coordinator can submit
       work during the caller's stopped, serial-driver recovery pass. */
    if (!t || !ctx->cpu_factory || !ctx->cpu_factory->destroy || !Kaffeine_DosBrokerIsIdle(broker))
        return FALSE;
    if (!finish_process(ctx, ctx->sched, t))
        return FALSE;
    entry = t->startup_entry;
    tcb = t->tcb;
    cpu = t->cpu;
    /* Broker is consumed now. Restore a live parent view before destroying
       this CPU; the native caller still owns the module and unloads next. */
    if (ctx->io_task_removed) ctx->io_task_removed(ctx, tcb);
    if (ctx->idcmp_task_gone) ctx->idcmp_task_gone(ctx, tcb);
    ctx->cpu_factory->destroy(ctx, cpu);
    if (entry) Kaffeine_HeapFreeEntry(ctx, entry);
    memset(t, 0, sizeof(*t));
    if (ctx->callback_failed && ctx->exit_code == 20 && ctx->cpu)
        Kaffeine_SetD(ctx->cpu, 0, 20);
    /* Keep scheduler allocation until the driver's ordinary end/boundary,
       so an enclosing drain/end never retains a freed scheduler pointer. */
    return TRUE;
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
    if (Kaffeine_LayerInfoLocked(ctx))
        return refuse(ctx, "RemTask(): another task cannot retire inside a LayerInfo lock");
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

        if (t->state == SLOT_FREE || t->state == SLOT_REMOVED || t->state == SLOT_RECOVERY)
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
    ULONG set = THUNK_D(1) & ~Kaffeine_AsyncSignalMask(ctx);
    ULONG signals = THUNK_D(0) & set, old;

    if (!s)
        return refuse(ctx, "SetSignal(): no guest task scheduler");
    t = s->current;
    if (!poll(ctx, s))
        return 20;
    /* The root's private I/O bits are the bridge's, never shown or changed */
    if (t == &s->root)
    {
        set &= ~Kaffeine_AsyncSignalMask(ctx);
        signals &= set;
    }
    old = t->recvd & ~Kaffeine_AsyncSignalMask(ctx);
    t->recvd &= ~Kaffeine_AsyncSignalMask(ctx);
    t->recvd = (t->recvd & ~set) | signals;
    if (t == &s->root)
    {
        ULONG native = ~child_bits(s) & ~Kaffeine_AsyncSignalMask(ctx);

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
    ULONG mask = THUNK_D(0) & ~Kaffeine_AsyncSignalMask(ctx), got;

    if (Kaffeine_LayerInfoLocked(ctx))
        return refuse(ctx, "Wait(): while holding a LayerInfo lock");
    if (!s || (struct KaffeineCPU *)cpu != s->current->cpu)
        return refuse(ctx, "Wait(): not the CPU of the current guest task");
    t = s->current;
    if (!poll(ctx, s))
        return 20;
    /* The root never waits for, nor takes, the private bits of I/O */
    if (t == &s->root)
        mask &= ~Kaffeine_AsyncSignalMask(ctx);
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
    set &= ~Kaffeine_AsyncSignalMask(ctx);
    if (t == &s->root)
    {
        ULONG owned = child_bits(s);

        set &= ~Kaffeine_AsyncSignalMask(ctx);
        if (set & ~owned)
            /* A packet reply may be appended by a DOS broker while the
               driver waits. The root's native signals still belong here. */
            Signal(ctx->exec_task ? ctx->exec_task : FindTask(NULL), set & ~owned);
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
        (Kaffeine_LayerSignalMask(ctx) & (1UL << port->mp_SigBit)) ||
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
    if (!t || !bit || (bit & Kaffeine_AsyncSignalMask(ctx)) ||
        !(ctx->guest_signals & bit)) return FALSE;
    return t == &s->root ? !(child_bits(s) & bit) : (t->native & bit) == bit;
}

static BOOL sched_can_signal(struct KaffeineContext *ctx, ULONG task, ULONG bits)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t;
    ULONG allowed;

    if (!ctx->running || !bits || (bits & Kaffeine_AsyncSignalMask(ctx))) return FALSE;
    if (!s)
    {
        if (!ctx->running || !task || task != ctx->guest_exec_task) return FALSE;
        allowed = (ctx->guest_signals & ~SYSTEM_SIGNALS) | KAFFEINE_TASK_BREAK_SIGNALS;
    }
    else
    {
        t = task_of(s, task);
        if (!t) return FALSE;
        if (t == &s->root)
            allowed = (ctx->guest_signals & ~child_bits(s) & ~SYSTEM_SIGNALS) |
                      KAFFEINE_TASK_BREAK_SIGNALS;
        else
            allowed = (t->native & ~SYSTEM_SIGNALS) | (t->alloc & KAFFEINE_TASK_BREAK_SIGNALS);
    }
    return !(bits & ~allowed);
}

static BOOL sched_process_port(struct KaffeineContext *ctx, ULONG port, ULONG owner)
{
    struct KaffeineScheduler *s = ctx->sched;

    return s && is_process(s, task_of(s, owner)) && port == owner + M68K_PR_MSGPORT;
}

BOOL Kaffeine_SchedFinishRoot(struct KaffeineContext *ctx, void *cpu, LONG result)
{
    struct KaffeineScheduler *s = ctx ? ctx->sched : NULL;
    unsigned i;

    if (!s || s->root_done || s->current != &s->root || s->root.cpu != cpu ||
        s->root.state != SLOT_RUN || ctx->callback_depth || !children(s))
        return FALSE;
    /* Classic AddTask/CreateProc alone retain their program-end lifetime.
       A typed Process is what gives this context a persistent domain. */
    for (i = 0; i < KAFFEINE_MAX_GUEST_TASKS; i++)
        if (s->child[i].state != SLOT_FREE && s->child[i].state != SLOT_REMOVED &&
            s->child[i].state != SLOT_RECOVERY &&
            s->child[i].broker)
            break;
    if (i == KAFFEINE_MAX_GUEST_TASKS)
        return FALSE;
    if (ctx->layer_ops && ctx->layer_ops->task_removed)
        ctx->layer_ops->task_removed(ctx, s->root.tcb);
    dos_save(ctx, s, &s->root);
    s->root_done = TRUE;
    s->root_result = result;
    s->root.state = SLOT_REMOVED;
    s->reschedule = TRUE;
    if (ctx->io_task_removed)
        ctx->io_task_removed(ctx, s->root.tcb);
    if (ctx->idcmp_task_gone) ctx->idcmp_task_gone(ctx, s->root.tcb);
    mirror(ctx, s, &s->root);
    publish(ctx, s);
    if (ctx->root_complete)
        ctx->root_complete(ctx, result);
    return TRUE;
}

static BOOL sched_root_result(struct KaffeineContext *ctx, LONG *result)
{
    struct KaffeineScheduler *s = ctx->sched;

    if (!s || !s->root_done || !s->root_drained)
        return FALSE;
    *result = s->root_result;
    return TRUE;
}

static ULONG sched_root_task(struct KaffeineContext *ctx)
{
    struct KaffeineScheduler *s = ctx->sched;
    return s && s->root.state != SLOT_REMOVED ? s->root.tcb : 0;
}

/* Before the thunk of every dos.library call. The original CreateProc()
   child still has no DOS context. Only a typed Process with a private
   broker can proceed to a selected compiled thunk through dos_call. */
static BOOL sched_process_dos(struct KaffeineContext *ctx, void *cpu, ULONG lvo, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;

    if (Kaffeine_LayerInfoLocked(ctx))
    {
        *result = refuse(ctx, "DOS: while holding a LayerInfo lock");
        return TRUE;
    }
    if (lvo == DOS_EXIT_LVO && ctx->callback_depth)
    {
        *result = refuse(ctx, "a callback cannot exit its Process");
        return TRUE;
    }
    if (t == &s->root && lvo == DOS_EXIT_LVO &&
        Kaffeine_SchedFinishRoot(ctx, cpu, (LONG)Kaffeine_GetD(cpu, 1)))
    {
        *result = (IPTR)(LONG)Kaffeine_GetD(cpu, 1);
        return TRUE;
    }
    if (!is_process(s, t))
        return FALSE;
    t->dos_pending = FALSE;
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
    if (t->broker && lvo && lvo != DOS_EXIT_LVO && !ctx->broker_call &&
        (struct KaffeineCPU *)cpu == t->cpu && t->state == SLOT_RUN)
    {
        t->dos_lvo = lvo;
        t->dos_pending = TRUE;
        return FALSE;
    }
    bug("[kaffeine] dos.library LVO -%lu of guest process %08lx\n",
        (unsigned long)lvo, (unsigned long)t->tcb);
    *result = refuse(ctx, t->broker ? "a Process DOS call has no safe broker route"
                                  : "a CreateProc() process has no DOS context for dos.library");
    return TRUE;
}

/* Match the dispatcher's manual-before-generated selection. The thunk
   pointer comes from native tables; no guest function pointer is called. */
static BOOL selected_dos_thunk(struct KaffeineContext *ctx, ULONG lvo, KaffeineThunkFn thunk)
{
    unsigned lib, i;
    for (lib = 0; lib < ctx->num_libs; lib++)
    {
        const struct M68KFakeLibBase *base = &ctx->libs[lib];
        if (!base->dos) continue;
        for (i = 0; i < base->num_thunks; i++)
            if (base->thunks[i].lvo == lvo && base->thunks[i].thunk)
                return base->thunks[i].thunk == thunk;
        for (i = 0; i < base->num_gen_thunks; i++)
            if (base->gen_thunks[i].lvo == lvo && base->gen_thunks[i].thunk)
                return base->gen_thunks[i].thunk == thunk;
    }
    return FALSE;
}

static BOOL sched_dos_call(struct KaffeineContext *ctx, void *cpu,
                           KaffeineThunkFn thunk, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s ? s->current : NULL;
    SIPTR error = 0;
    BOOL okay;

    if (!t || !t->broker)
        return FALSE;
    if (!t->dos_pending || !thunk || !selected_dos_thunk(ctx, t->dos_lvo, thunk) ||
        t->cpu != (struct KaffeineCPU *)cpu || ctx->cpu != t->cpu ||
        t->state != SLOT_RUN || ctx->broker_call)
    {
        t->dos_pending = FALSE;
        *result = refuse(ctx, "dos.library did not select a safe compiled broker thunk");
        return TRUE;
    }
    t->dos_pending = FALSE;
    ctx->broker_call = TRUE;
    okay = Kaffeine_DosBrokerCall(t->broker, ctx, cpu, t->dos_lvo, thunk, result, &error);
    ctx->broker_call = FALSE;
    if (!okay)
    {
        bug("[kaffeine] DOS broker transport failed (%ld)\n", (long)error);
        *result = refuse(ctx, "a Process DOS broker call failed");
    }
    return TRUE;
}

static void packet_clear(struct GuestTask *t)
{
    t->packet_port = t->packet_msg = 0;
    t->packet_file = t->packet_pending = FALSE;
}

static IPTR packet_refuse(struct KaffeineContext *ctx, const char *why)
{
    Kaffeine_StopGuest(ctx, ctx->cpu, 20);
    return refuse(ctx, why);
}

/* Only private registry identities choose either adapter. Its existing
   StandardPacket/queue/handle validation still runs before native DOS. */
static BOOL packet_adapter(struct KaffeineContext *ctx, ULONG port, BOOL file)
{
    unsigned i;
    if (!port) return FALSE;
    if (file && ctx->file_packet)
        for (i = 0; i < KAFFEINE_MAX_FILES; i++)
            if (ctx->files[i].guest && ctx->files[i].handler == port) return TRUE;
    if (!file && ctx->handler_packet)
        for (i = 0; i < KAFFEINE_MAX_HANDLERS; i++)
            if (ctx->handlers[i].guest == port) return TRUE;
    return FALSE;
}

/* Fixed native code, never a callback supplied by guest memory. No guest CPU
   instruction, dispatcher, guest Wait or scheduler switch runs here. Native
   DOS may wait synchronously while the driver waits for its acknowledgement. */
static IPTR packet_thunk(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s ? s->current : NULL;
    IPTR result;
    if (!t || !t->broker || !t->packet_pending || t->state != SLOT_RUN ||
        ctx->cpu != t->cpu || cpu != t->cpu || ctx->guest_exec_task != t->tcb ||
        this_task(ctx) != t->tcb || ctx->callback_depth || !ctx->broker_call ||
        !ctx->broker_owner || ctx->broker_owner != FindTask(NULL) ||
        ctx->broker_owner == ctx->exec_task || Kaffeine_LayerInfoLocked(ctx) ||
        !packet_adapter(ctx, t->packet_port, t->packet_file))
        return packet_refuse(ctx, "a Process packet has no safe broker owner");
    result = t->packet_file ? ctx->file_packet(ctx, t->packet_port, t->packet_msg)
                            : ctx->handler_packet(ctx, t->packet_port, t->packet_msg);
    if (!ctx->running && ctx->exit_code == 20)
        Kaffeine_StopGuest(ctx, t->cpu, 20);
    return result;
}

static BOOL sched_packet_call(struct KaffeineContext *ctx, ULONG port, ULONG msg,
                              BOOL file, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s ? s->current : NULL;
    SIPTR error = 0;
    BOOL okay;
    if (!s || !is_process(s, t)) return FALSE;
    if (!t->broker || t->packet_pending || !ctx->running || ctx->callback_depth ||
        ctx->cpu != t->cpu || t->state != SLOT_RUN ||
        ctx->guest_exec_task != t->tcb || this_task(ctx) != t->tcb ||
        Kaffeine_LayerInfoLocked(ctx) || !packet_adapter(ctx, port, file) ||
        !ctx->exec_task ||
        (ctx->broker_call ? (!ctx->broker_owner || ctx->broker_owner != FindTask(NULL) ||
                             ctx->broker_owner == ctx->exec_task)
                          : (ctx->broker_owner || FindTask(NULL) != ctx->exec_task)))
    {
        *result = packet_refuse(ctx, "a Process packet has no safe broker route");
        return TRUE;
    }
    t->packet_port = port;
    t->packet_msg = msg;
    t->packet_file = file;
    t->packet_pending = TRUE;
    if (ctx->broker_call)
    {
        /* SendPkt/DoPkt is already inside this Process's selected DOS call.
           Reuse that exact native owner instead of submitting to itself. */
        *result = packet_thunk(ctx, t->cpu);
    }
    else
    {
        ctx->broker_call = TRUE;
        okay = Kaffeine_DosBrokerCall(t->broker, ctx, t->cpu, 0,
                                      packet_thunk, result, &error);
        ctx->broker_call = FALSE;
        if (!okay)
        {
            bug("[kaffeine] packet broker transport failed (%ld)\n", (long)error);
            *result = packet_refuse(ctx, "a Process packet broker call failed");
        }
    }
    packet_clear(t);
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
          (bit == SIGB_DOS &&
           (sched_process_port(ctx, port, owner) ||
            (t == &s->root && port == s->root.tcb + M68K_PR_MSGPORT))))) return FALSE;
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

static ULONG sched_caller_task(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s ? s->current : NULL;

    return t && t->state == SLOT_RUN && t->cpu == (struct KaffeineCPU *)cpu ? t->tcb : 0;
}

static BOOL sched_block_io(struct KaffeineContext *ctx, void *cpu, ULONG request,
                           KaffeineTaskRetry retry, IPTR *result)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = s->current;
    unsigned i;

    if (ctx->callback_depth || !sched_caller_task(ctx, cpu))
    {
        *result = refuse(ctx, "WaitIO(): not the current guest task's CPU");
        return TRUE;
    }
    for (i = 0; i < KAFFEINE_MAX_IO_REQUESTS; i++)
    {
        const struct KaffeineIORequest *e = &ctx->io_requests[i];
        if (e->guest != request || e->orphan)
            continue;
        if ((e->owner != t->tcb && (e->owner || t != &s->root)) || !e->native)
            break;
        if (!e->pending)
            return FALSE;
        t->io_request = request;
        t->wait = 0;
        t->retry = retry;
        suspend(ctx, s, t);
        *result = 0;
        return TRUE;
    }
    *result = refuse(ctx, "WaitIO(): no pending request of the current guest task");
    return TRUE;
}

static void sched_io_completed(struct KaffeineContext *ctx, ULONG request, ULONG owner)
{
    struct KaffeineScheduler *s = ctx->sched;
    struct GuestTask *t = owner ? task_of(s, owner) : &s->root;

    if (t && t->state == SLOT_WAIT && t->io_request == request)
    {
        t->io_request = 0;
        t->state = SLOT_READY;
        mirror(ctx, s, t);
    }
}

const struct KaffeineTaskOps Kaffeine_GuestTaskOps =
{
    Kaffeine_TaskAdd, Kaffeine_TaskRemove, sched_find_named, Kaffeine_TaskSetPri,
    Kaffeine_TaskSetSignal, Kaffeine_TaskWait, Kaffeine_TaskSignal,
    sched_signal_allocated, sched_signal_freeable, sched_block_port,
    sched_boundary, sched_end, sched_known_task, sched_owns_signal,
    sched_send_signal, sched_block_guest_port,
    Kaffeine_ProcCreate, sched_process_port, sched_process_dos,
    sched_is_root, sched_caller_task, sched_block_io, sched_io_completed,
    sched_dos_call, Kaffeine_SchedFinishRoot, sched_root_result,
    sched_root_task, sched_can_signal, sched_packet_call
};

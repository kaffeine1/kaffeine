/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Cooperative scheduler for classic 68k tasks that a program adds with
    AddTask(), and processes that it makes with CreateProc(). See
    kaffeine_sched.c.
*/
#ifndef KAFFEINE_SCHED_H
#define KAFFEINE_SCHED_H

#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

struct MsgPort;

/* Children besides the root task, the native task that runs the program */
#define KAFFEINE_MAX_GUEST_TASKS 16
/* Backend slices in one round-robin turn between tasks of equal priority */
#define KAFFEINE_TASK_QUANTUM    4096
/* Smallest stack from the initial tc_SPReg down to tc_SPLower. A child runs
   in supervisor mode on that stack, so exception frames land there too. */
#define KAFFEINE_TASK_MIN_STACK  256
/* CreateProc(): the largest stack that the guest heap gives a Process, the
   longest name and the most seglist entries that it accepts */
#define KAFFEINE_PROC_MAX_STACK  (256 * 1024)
#define KAFFEINE_PROC_MAX_NAME   255
#define KAFFEINE_PROC_MAX_SEGS   64

/* ExecBase TDNestCnt and IDNestCnt: the guest bytes are the Forbid() and
   Disable() nesting of the current guest task, -1 when it allows switches,
   as the FORBID macro of exec/ables.i also changes them in place. */
#define KAFFEINE_EXECBASE_IDNESTCNT    294
#define KAFFEINE_EXECBASE_TDNESTCNT    295
#define KAFFEINE_EXECBASE_TASKSIGALLOC 316

/* A backend that runs guest tasks makes one complete CPU instance for each,
   with all of its private state, the FPU included, over the context's arena.
   create returns it after reset, in supervisor mode, ready for the register
   operations, or NULL. destroy is never called while that CPU executes or
   one of its thunks runs. */
struct KaffeineCPUFactory
{
    struct KaffeineCPU *(*create)(struct KaffeineContext *ctx);
    void (*destroy)(struct KaffeineContext *ctx, struct KaffeineCPU *cpu);
};

/* A blocked call that runs again, as a thunk of the same task, when the
   task resumes; it may block again. */
typedef IPTR (*KaffeineTaskRetry)(struct KaffeineContext *ctx, void *cpu);

/* Entry points behind ctx->task_ops. Other files reach the scheduler only
   through them, so that they link without it. Except add_task/set_task_pri/
   create_proc, they are called only while ctx->sched is set. */
struct KaffeineTaskOps
{
    IPTR  (*add_task)(struct KaffeineContext *ctx, void *cpu);       /* -282 */
    IPTR  (*rem_task)(struct KaffeineContext *ctx, void *cpu);       /* -288 */
    ULONG (*find_named)(struct KaffeineContext *ctx, const char *name);
    IPTR  (*set_task_pri)(struct KaffeineContext *ctx, void *cpu);   /* -300 */
    IPTR  (*set_signal)(struct KaffeineContext *ctx, void *cpu);     /* -306 */
    IPTR  (*wait)(struct KaffeineContext *ctx, void *cpu);           /* -318 */
    IPTR  (*signal)(struct KaffeineContext *ctx, void *cpu);         /* -324 */
    /* After a native AllocSignal() of the guest, and before a native
       FreeSignal(), which goes ahead only for TRUE. */
    void  (*signal_allocated)(struct KaffeineContext *ctx, LONG sig);
    BOOL  (*signal_freeable)(struct KaffeineContext *ctx, LONG sig);
    /* For an empty native port: block the current task until the port's
       signal arrives, then run retry; *result is the D0 for the thunk to
       return now. FALSE, when the port has a message, changes nothing. */
    BOOL  (*block_port)(struct KaffeineContext *ctx, void *cpu, struct MsgPort *port,
                        KaffeineTaskRetry retry, IPTR *result);
    /* Kaffeine_RunCPU(): after each slice, and once when it ends. */
    void  (*boundary)(struct KaffeineContext *ctx);
    void  (*end)(struct KaffeineContext *ctx);
    /* Guest queues may target any live task of this context, but only
       signal bits that that task allocated through the bridge. */
    BOOL  (*known_task)(struct KaffeineContext *ctx, ULONG task);
    BOOL  (*owns_signal)(struct KaffeineContext *ctx, ULONG task, ULONG bit);
    void  (*send_signal)(struct KaffeineContext *ctx, ULONG task, ULONG signals);
    BOOL  (*block_guest_port)(struct KaffeineContext *ctx, ULONG port,
                              KaffeineTaskRetry retry, IPTR *result);
    /* dos.library -138: CreateProc(D1=name, D2=pri, D3=seglist, D4=stack)
       of the root -> pr_MsgPort of a guest Process, or 0 with the IoErr()
       in *ioerr; what Exec could not use stops the program. */
    IPTR  (*create_proc)(struct KaffeineContext *ctx, void *cpu, LONG *ioerr);
    /* pr_MsgPort of owner, a live CreateProc() child, whose SIGB_DOS is a
       guest signal of that child alone */
    BOOL  (*process_port)(struct KaffeineContext *ctx, ULONG port, ULONG owner);
    /* Before any dos.library call, lvo, of the current task: FALSE lets it
       go to its thunk. A CreateProc() child has no DOS context, so its
       Exit() ends that child alone and every other call stops the program;
       *result is D0 then. */
    BOOL  (*process_dos)(struct KaffeineContext *ctx, void *cpu, ULONG lvo, IPTR *result);
    /* Whether cpu is the root's and the root is the current task, from the
       scheduler's own state, never from the guest ThisTask */
    BOOL  (*is_root)(struct KaffeineContext *ctx, void *cpu);
    /* Trusted current live task, only for its own CPU; 0 for any other
       caller. Independent of the writable guest ExecBase.ThisTask. */
    ULONG (*caller_task)(struct KaffeineContext *ctx, void *cpu);
    /* WaitIO blocks on a private request identity, never on a guest or
       native signal bit. Completion wakes only its captured live owner. */
    BOOL  (*block_io)(struct KaffeineContext *ctx, void *cpu, ULONG request,
                      KaffeineTaskRetry retry, IPTR *result);
    void  (*io_completed)(struct KaffeineContext *ctx, ULONG request, ULONG owner);
};

#ifdef __cplusplus
extern "C" {
#endif

extern const struct KaffeineTaskOps Kaffeine_GuestTaskOps;

/* The thunks behind Kaffeine_GuestTaskOps, for host dispatch tests. Apart
   from Kaffeine_TaskAdd they need ctx->sched. Kaffeine_TaskFindTask is the
   guest part of FindTask(): ThisTask, or a child by name. */
IPTR Kaffeine_TaskAdd(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskRemove(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskFindTask(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskSetPri(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskSetSignal(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskWait(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_TaskSignal(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ProcCreate(struct KaffeineContext *ctx, void *cpu, LONG *ioerr);

#ifdef __cplusplus
}
#endif

/* -132: Forbid(), -138: Permit(). D0 is 0, as before guest tasks. Without
   a guest ExecBase there is nothing to count. */
static inline UBYTE *Kaffeine_TaskNesting(struct KaffeineContext *ctx)
{
    if (!ctx->mem || !ctx->m68k_sysbase || ctx->m68k_sysbase >= ctx->mem_size ||
        ctx->mem_size - ctx->m68k_sysbase <= KAFFEINE_EXECBASE_TDNESTCNT)
        return NULL;
    return ctx->mem + ctx->m68k_sysbase + KAFFEINE_EXECBASE_TDNESTCNT;
}

static inline IPTR Kaffeine_TaskForbid(struct KaffeineContext *ctx, void *cpu)
{
    UBYTE *nest = Kaffeine_TaskNesting(ctx);

    (void)cpu;
    if (nest && (signed char)*nest < 127)
        ++*nest;
    return 0;
}

static inline IPTR Kaffeine_TaskPermit(struct KaffeineContext *ctx, void *cpu)
{
    UBYTE *nest = Kaffeine_TaskNesting(ctx);

    (void)cpu;
    if (nest && (signed char)*nest >= 0)
        --*nest;
    return 0;
}

/* Interrupt nesting belongs to the emulated CPU. It must never disable
   interrupts of the native task while guest code runs. */
static inline IPTR Kaffeine_TaskDisable(struct KaffeineContext *ctx, void *cpu)
{
    UBYTE *nest = Kaffeine_TaskNesting(ctx);
    (void)cpu;
    if (nest && (signed char)nest[-1] < 127)
        ++nest[-1];
    return 0;
}

static inline IPTR Kaffeine_TaskEnable(struct KaffeineContext *ctx, void *cpu)
{
    UBYTE *nest = Kaffeine_TaskNesting(ctx);
    (void)cpu;
    if (nest && (signed char)nest[-1] >= 0)
        --nest[-1];
    return 0;
}

#endif /* KAFFEINE_SCHED_H */

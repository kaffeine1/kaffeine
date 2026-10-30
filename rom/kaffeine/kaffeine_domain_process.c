/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "kaffeine_intern.h"
#include "kaffeine_process.h"
#include "kaffeine_domain_process.h"
#include "kaffeine_callback.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_dos_broker.h"
#include "kaffeine_sched.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_offsets.h"

/* DosEntry saves these identities before entering the worker, then closes
 * the saved values according to the Process's live PRF_CLOSE* flags. Neither
 * SelectInput nor SelectOutput transfers that exit ownership to a new file.
 * Records exist only between this worker's Init and acknowledged Drop. The
 * driver and its synchronous brokers are the sole serial Context callers. */
struct KaffeineProcessStreams
{
    struct KaffeineProcessStreams *next;
    struct Process *owner;
    BPTR startup[3];
};

static const ULONG stream_close_flags[3] = {
    PRF_CLOSEINPUT, PRF_CLOSEOUTPUT, PRF_CLOSEERROR
};

static BOOL streams_valid(struct KaffeineContext *ctx)
{
    struct KaffeineProcessStreams *record;
    struct Task *caller = FindTask(NULL);
    if (!caller || (ctx->broker_call ? caller != ctx->broker_owner :
                   caller != ctx->exec_task)) return FALSE;
    for (record = ctx->process_streams; record; record = record->next)
        if (!record->owner || record->owner->pr_Task.tc_Node.ln_Type != NT_PROCESS)
            return FALSE;
    return TRUE;
}

/* Disarm every saved alias before an explicit close or one owner's exit.
 * Other captured workers are idle under the serial Context protocol. Clear
 * their selected native aliases too, so Input/Output cannot reimport a file
 * that this close frees. The current Close path updates its guest fields. */
static void streams_detach(struct Process *owner, BPTR native)
{
    if (owner->pr_CIS == native) owner->pr_CIS = (BPTR)0;
    if (owner->pr_COS == native) owner->pr_COS = (BPTR)0;
    if (owner->pr_CES == native) owner->pr_CES = (BPTR)0;
}

static void streams_disarm(struct KaffeineContext *ctx, BPTR native,
                           struct Process *keep_owner, ULONG keep_flag,
                           struct Process *close_caller)
{
    struct KaffeineProcessStreams *record;
    unsigned i;
    for (record = ctx->process_streams; record; record = record->next)
    {
        struct Process *owner = record->owner;
        for (i = 0; i < 3; ++i)
            if (record->startup[i] == native)
            {
                if (owner != keep_owner || stream_close_flags[i] != keep_flag)
                    owner->pr_Flags &= ~stream_close_flags[i];
                record->startup[i] = (BPTR)0;
            }
        if (owner == close_caller) continue;
        streams_detach(owner, native);
    }
    /* The driver can select an imported child's original too. Its native
     * Process remains live until all child cleanup has acknowledged. */
    if (ctx->exec_task && ctx->exec_task != (struct Task *)close_caller &&
        ctx->exec_task->tc_Node.ln_Type == NT_PROCESS)
        streams_detach((struct Process *)ctx->exec_task, native);
}

static BOOL process_file_close(struct KaffeineContext *ctx, BPTR native)
{
    SIPTR error = IoErr();
    if (!streams_valid(ctx)) return FALSE;
    if (native)
        streams_disarm(ctx, native, NULL, 0, (struct Process *)FindTask(NULL));
    SetIoErr(error);
    return TRUE;
}

IPTR Kaffeine_ProcessInitGuestDos(struct KaffeineContext *ctx, void *cpu)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct KaffeineProcessStreams *record;
    SIPTR error = IoErr();
    ULONG current = 0, home = 0, input, output, errors;
    ULONG task = ctx->guest_exec_task;
    (void)cpu;
    if (!ctx->broker_call || ctx->broker_owner != (struct Task *)me ||
        !task || task >= ctx->mem_size || ctx->mem_size - task < M68K_SIZEOF_PROCESS ||
        !me || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return 0;
    for (record = ctx->process_streams; record; record = record->next)
        if (record->owner == me) return 0; /* never recapture replacements */
    record = AllocVec(sizeof(*record), MEMF_PUBLIC | MEMF_CLEAR);
    if (!record) { SetIoErr(ERROR_NO_FREE_STORE); return 0; }
    record->owner = me;
    record->startup[0] = Input();
    record->startup[1] = Output();
    record->startup[2] = me->pr_CES;
    record->next = ctx->process_streams;
    ctx->process_streams = record;
    ctx->process_file_close = process_file_close;
    SetIoErr(error);
    input = Kaffeine_FileImport(ctx, record->startup[0], FALSE);
    if (record->startup[0] && !input) return 0;
    output = Kaffeine_FileImport(ctx, record->startup[1], FALSE);
    if (record->startup[1] && !output) return 0;
    errors = Kaffeine_FileImport(ctx, record->startup[2], FALSE);
    if (record->startup[2] && !errors) return 0;
    if (!Kaffeine_LockGetInstalled(ctx, KAFFEINE_LOCK_CURRENT, &current) ||
        !Kaffeine_LockGetInstalled(ctx, KAFFEINE_LOCK_PROGRAM, &home)) return 0;
    m68k_write32(ctx, task + M68K_PR_CIS, input);
    m68k_write32(ctx, task + M68K_PR_COS, output);
    m68k_write32(ctx, task + M68K_PR_CES, errors);
    m68k_write32(ctx, task + M68K_PR_CURRENTDIR, current);
    m68k_write32(ctx, task + M68K_PR_HOMEDIR, home);
    return 1;
}

IPTR Kaffeine_ProcessDropGuestDos(struct KaffeineContext *ctx, void *cpu)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct KaffeineProcessStreams **link, *record;
    SIPTR error = IoErr();
    unsigned i;
    (void)cpu;
    if (!ctx->broker_call || ctx->broker_owner != (struct Task *)me ||
        !me || me->pr_Task.tc_Node.ln_Type != NT_PROCESS ||
        !streams_valid(ctx)) return 0;
    link = &ctx->process_streams;
    while (*link && (*link)->owner != me) link = &(*link)->next;
    record = *link;
    if (record)
    {
        for (i = 0; i < 3; ++i)
        {
            BPTR native = record->startup[i];
            if (!native || !(me->pr_Flags & stream_close_flags[i])) continue;
            /* Exactly one native exit slot closes a shared original. Clear
             * all other saved aliases before its storage can be reused. */
            streams_disarm(ctx, native, me, stream_close_flags[i], NULL);
            Kaffeine_FileDropNative(ctx, native, FALSE);
        }
        *link = record->next;
        FreeVec(record);
        if (!ctx->process_streams) ctx->process_file_close = NULL;
    }
    Kaffeine_LockOwnerDrop(ctx, &me->pr_Task);
    SetIoErr(error);
    return 1;
}

/* Allocated before either a copied module or a native broker exists. A
 * refused cleanup can therefore retain ownership without allocating while
 * rolling back an already failing startup. The record is native-only. */
struct KaffeineDomainStartRecovery
{
    struct KaffeineDomainStartRecovery *next;
    struct KaffeineDosBroker *broker;
    struct KaffeineDomainModule *module;
};

static BOOL finish_start(struct KaffeineContext *ctx,
                         struct KaffeineDomainStartRecovery *record)
{
    BOOL old_broker_call, okay;

    if (!record->broker) return TRUE;
    /* No Context view may change while a previous RPC is outstanding.
     * The sole serial coordinator prevents a new submission between this
     * metadata probe and the scheduler's scoped Finish. */
    if (!Kaffeine_DosBrokerIsIdle(record->broker)) return FALSE;
    if (Kaffeine_SchedHasRecoveryStart(ctx, record->broker))
        okay = Kaffeine_SchedRecoverStart(ctx, record->broker);
    else
    {
        /* No child DOS view was published on this path. Drop only aliases
         * owned by this broker before native DOS closes its own resources. */
        old_broker_call = ctx->broker_call;
        ctx->broker_call = TRUE;
        okay = Kaffeine_DosBrokerFinish(record->broker, ctx,
                                        Kaffeine_ProcessDropGuestDos);
        ctx->broker_call = old_broker_call;
    }
    if (!okay) return FALSE;
    record->broker = NULL; /* consumed only by the acknowledged Finish */
    return TRUE;
}

static void release_start(struct KaffeineContext *ctx,
                          struct KaffeineDomainStartRecovery *record)
{
    /* A busy module remains owned by DomainUnload's Context registry. No
     * broker callback or staged CPU can refer to it after finish_start. */
    if (record->module) Kaffeine_DomainUnload(ctx, record->module);
    FreeVec(record);
}

BOOL Kaffeine_DomainDrainStarts(struct KaffeineContext *ctx)
{
    struct KaffeineDomainStartRecovery *record;
    SIPTR saved_error = IoErr();
    BOOL complete = FALSE;

    if (!ctx) goto done;
    if (!ctx->domain_start_recovery) { complete = TRUE; goto done; }
    /* Only the driver, outside guest execution and every nested broker
     * callback, can retry a refused startup. Keep all live resources until
     * it receives the native owner's acknowledgement. */
    if (FindTask(NULL) != ctx->exec_task || ctx->running ||
        ctx->broker_call || ctx->broker_owner || ctx->callback_depth)
        goto done;
    while ((record = ctx->domain_start_recovery) != NULL)
    {
        if (!finish_start(ctx, record)) goto done;
        ctx->domain_start_recovery = record->next;
        release_start(ctx, record);
    }
    ctx->domain_drain = NULL;
    complete = TRUE;
done:
    if (!complete) Delay(1);
    SetIoErr(saved_error);
    return complete;
}

ULONG Kaffeine_DomainStartProcess(struct KaffeineContext *ctx,
                                 const struct KaffeineProcessStart *spec)
{
    struct KaffeineDomainImage image = {0};
    struct KaffeineDomainStartRecovery *record = NULL;
    struct KaffeineDosBrokerStart start;
    IPTR relocs = 0;
    struct TagItem query[] = { { GSLI_68KRelocs, (IPTR)&relocs }, { TAG_DONE, 0 } };
    SIPTR error = ERROR_BAD_NUMBER, saved_error = IoErr();
    BPTR duplicate = BNULL;
    ULONG task = 0;

    if (!ctx || !ctx->cpu || !ctx->cpu_factory || !ctx->exec_task || !spec ||
        !spec->seglist || ctx->callback_depth) goto failed;
    if (ctx->domain_start_recovery)
    {
        error = ERROR_BREAK;
        goto failed;
    }
    record = AllocVec(sizeof(*record), MEMF_PUBLIC | MEMF_CLEAR);
    if (!record) { error = ERROR_NO_FREE_STORE; goto failed; }
    if (!GetSegListInfo(spec->seglist, query) || !relocs)
    {
        error = ERROR_INVALID_RESIDENT_LIBRARY;
        goto failed;
    }
    if (!Kaffeine_DomainLoad(ctx, spec->seglist,
                            (const struct HunkRelocTable *)relocs, &image))
    {
        error = IoErr();
        goto failed;
    }
    record->module = image.module;
    if (spec->home_dir)
    {
        duplicate = DupLock(spec->home_dir);
        if (!duplicate) { error = IoErr(); goto failed; }
    }
    start.name = spec->name;
    start.command = spec->command;
    start.arguments = spec->arguments;
    start.backend = spec->backend;
    start.home_dir = duplicate;
    start.stack_size = spec->stack_size;
    start.cli = spec->cli;
    start.copy_vars = spec->copy_vars;
    start.priority = spec->priority;
    record->broker = Kaffeine_DosBrokerStart(ctx->exec_task, &start, &error);
    if (!record->broker) goto failed;
    duplicate = BNULL; /* broker consumed only our extra reference */
    task = Kaffeine_SchedStartProcess(ctx, ctx->cpu, spec, image.seglist,
                                     image.entry, record->broker, image.module, &error);
    if (!task) goto failed;
    /* Scheduler publication consumed broker/module. CPUs cannot run before
     * this thunk returns; native sources are now independent of both copies. */
    FreeVec(record);
    if (spec->home_dir) UnLock(spec->home_dir);
    UnLoadSeg(spec->seglist);
    SetIoErr(saved_error);
    return task;

failed:
    if (duplicate) UnLock(duplicate);
    if (record)
    {
        if (finish_start(ctx, record))
            release_start(ctx, record);
        else
        {
            record->next = ctx->domain_start_recovery;
            ctx->domain_start_recovery = record;
            ctx->domain_drain = Kaffeine_DomainDrainStarts;
            Kaffeine_StopGuest(ctx, ctx->cpu, 20);
        }
    }
    SetIoErr(error ? error : ERROR_NO_FREE_STORE);
    return 0;
}

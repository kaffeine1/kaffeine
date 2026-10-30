/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_dos_broker.h"

#ifndef KAFFEINE_DOS_BROKER_START_TICKS
#define KAFFEINE_DOS_BROKER_START_TICKS 250
#endif
#define NATIVE_STACK_MIN (128 * 1024)

enum BrokerState { STARTING, READY, COMMITTED, ABORTED, FINISHED };
enum RequestKind { BROKER_CALL, BROKER_STOP };

struct BrokerRequest
{
    struct Message message;
    enum RequestKind kind;
    struct KaffeineContext *ctx;
    void *cpu;
    KaffeineDosBrokerThunk thunk;
    ULONG lvo;
    IPTR result;
};

struct KaffeineDosBroker
{
    struct SignalSemaphore lock;
    unsigned refs;
    enum BrokerState state;
    BOOL busy;
    struct Task *driver, *native;
    struct MsgPort *inbox;
    struct BrokerRequest *request;
    struct Library *base;
    SIPTR startup_error;
    BOOL cli;
    char name[KAFFEINE_DOS_BROKER_MAX_NAME + 1];
    char command[KAFFEINE_DOS_BROKER_MAX_NAME + 1];
    char arguments[KAFFEINE_DOS_BROKER_MAX_ARGS + 1];
    char backend[6];
};

static BOOL bounded(const char *s, ULONG limit)
{
    ULONG i;
    if (!s) return FALSE;
    for (i = 0; i <= limit; ++i) if (!s[i]) return TRUE;
    return FALSE;
}

/* Never free a record while one of its semaphore operations still needs it. */
static void drop(struct KaffeineDosBroker *b)
{
    BOOL last;
    ObtainSemaphore(&b->lock);
    last = !--b->refs;
    ReleaseSemaphore(&b->lock);
    if (last) FreeVec(b);
}

static void reply_port(struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_IGNORE;
    NEWLIST(&port->mp_MsgList);
}

static void pause_tick(void)
{
    SIPTR saved = IoErr();
    Delay(1);
    SetIoErr(saved);
}

static void await_reply(struct MsgPort *port, struct Message *message)
{
    /* This stack-local queue is never published to a guest. PA_IGNORE uses
     * no private signal on the driver (or on a parent broker doing Start).
     * GetMsg/ReplyMsg provide the native queue's publication ordering. */
    while (GetMsg(port) != message) pause_tick();
}

static void execute(struct BrokerRequest *request)
{
    struct KaffeineContext *ctx = request->ctx;
    SIPTR error;
    /* The driver is paused for this whole interval. Native ownership is a
     * separate private capability: never substitute the Exec-I/O driver. */
    ctx->broker_owner = FindTask(NULL);
    request->result = request->thunk(ctx, request->cpu);
    error = IoErr();
    if (ctx->after_thunk)
    {
        void (*after)(struct KaffeineContext *) = ctx->after_thunk;
        ctx->after_thunk = NULL;
        after(ctx);
    }
    /* Cleanup is native-owner work too, but cannot replace the thunk's DOS
     * error (e.g. UnLock/Close during settlement). */
    SetIoErr(error);
    ctx->broker_owner = NULL;
}

static LONG worker(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct KaffeineDosBroker *b = me->pr_Task.tc_UserData;
    struct Library *base = b->base;
    struct MsgPort *inbox = NULL;
    struct Message *stop_reply = NULL;
    unsigned ticks;
    BOOL committed;

    ObtainSemaphore(&b->lock);
    b->native = &me->pr_Task;
    committed = b->state != ABORTED;
    ReleaseSemaphore(&b->lock);
    if (!committed) goto done;

    inbox = CreateMsgPort();
    if (!inbox)
    {
        ObtainSemaphore(&b->lock);
        b->startup_error = ERROR_NO_FREE_STORE;
        ReleaseSemaphore(&b->lock);
        goto done;
    }
    if (!SetVar("KAFFEINE_CPU", b->backend, -1, GVF_LOCAL_ONLY) ||
        (b->cli && !SetProgramName(b->command)))
    {
        SIPTR error = IoErr();
        ObtainSemaphore(&b->lock);
        b->startup_error = error ? error : ERROR_NO_FREE_STORE;
        ReleaseSemaphore(&b->lock);
        goto done;
    }
    ObtainSemaphore(&b->lock);
    b->inbox = inbox;
    if (b->state == STARTING) b->state = READY;
    ReleaseSemaphore(&b->lock);

    for (ticks = 0; ticks < KAFFEINE_DOS_BROKER_START_TICKS; ++ticks)
    {
        ObtainSemaphore(&b->lock);
        committed = b->state == COMMITTED;
        if (committed || b->state == ABORTED)
        {
            ReleaseSemaphore(&b->lock);
            break;
        }
        ReleaseSemaphore(&b->lock);
        pause_tick();
    }
    ObtainSemaphore(&b->lock);
    committed = b->state == COMMITTED;
    if (!committed) b->state = ABORTED;
    ReleaseSemaphore(&b->lock);
    if (!committed) goto done;

    for (;;)
    {
        struct Message *message;
        WaitPort(inbox);
        while ((message = GetMsg(inbox)))
        {
            struct BrokerRequest *request;
            ObtainSemaphore(&b->lock);
            request = b->request;
            /* The only accepted native message is the exact outstanding
             * private request. Never select a callback from message bytes
             * supplied by a guest or an unrelated native sender. */
            if (!request || message != &request->message ||
                message->mn_Length != sizeof(*request))
            {
                ReleaseSemaphore(&b->lock);
                continue;
            }
            ReleaseSemaphore(&b->lock);
            if (request->kind == BROKER_STOP)
            {
                if (request->thunk) execute(request);
                stop_reply = message;
                goto done;
            }
            execute(request);
            ReplyMsg(message);
        }
    }

done:
    me->pr_Task.tc_UserData = NULL;
    ObtainSemaphore(&b->lock);
    b->inbox = NULL;
    b->native = NULL;
    b->state = FINISHED;
    ReleaseSemaphore(&b->lock);
    if (inbox) DeleteMsgPort(inbox);
    /* The resident/noexpunge library stays executable while this controlled
     * ARM NP_Entry returns. Native DOS then owns its CLI/streams/dir cleanup. */
    CloseLibrary(base);
    drop(b); /* worker reference; Start/Stop still owns the other reference */
    if (stop_reply) ReplyMsg(stop_reply);
    return RETURN_OK;
}

struct KaffeineDosBroker *Kaffeine_DosBrokerStart(
    struct Task *driver, const struct KaffeineDosBrokerStart *spec, SIPTR *out_error)
{
    SIPTR saved = IoErr(), error = ERROR_NO_FREE_STORE;
    struct KaffeineDosBroker *b = NULL;
    struct Library *base = NULL;
    struct Process *created;
    BPTR home = BNULL;
    unsigned ticks;
    BOOL success = FALSE;
    const char *command = spec && spec->command ? spec->command : spec ? spec->name : NULL;
    const char *arguments = spec && spec->arguments ? spec->arguments : "";

    if (!driver || !spec || !bounded(spec->name, KAFFEINE_DOS_BROKER_MAX_NAME) ||
        !bounded(command, KAFFEINE_DOS_BROKER_MAX_NAME) ||
        !bounded(arguments, KAFFEINE_DOS_BROKER_MAX_ARGS) ||
        !bounded(spec->backend, 5) ||
        (strcmp(spec->backend, "moira") && strcmp(spec->backend, "jit")) ||
        spec->stack_size < 256 || spec->stack_size > KAFFEINE_DOS_BROKER_MAX_STACK)
    {
        error = ERROR_BAD_NUMBER;
        goto failure;
    }
    base = OpenLibrary("kaffeine.library", 0);
    if (!base) { error = ERROR_INVALID_RESIDENT_LIBRARY; goto failure; }
    if (spec->home_dir)
    {
        home = DupLock(spec->home_dir);
        if (!home) { error = IoErr(); goto failure; }
    }
    b = AllocVec(sizeof(*b), MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) goto failure;
    InitSemaphore(&b->lock);
    b->refs = 2; /* starter/owner plus the not-yet-created native worker */
    b->state = STARTING;
    b->driver = driver;
    b->base = base;
    b->startup_error = ERROR_BREAK;
    b->cli = spec->cli;
    strcpy(b->name, spec->name);
    strcpy(b->command, command);
    strcpy(b->arguments, arguments);
    strcpy(b->backend, spec->backend);
    {
        struct TagItem tags[] = {
            { NP_Entry, (IPTR)worker },
            { NP_UserData, (IPTR)b },
            { NP_Name, (IPTR)b->name },
            { NP_StackSize, spec->stack_size > NATIVE_STACK_MIN ? spec->stack_size : NATIVE_STACK_MIN },
            { NP_Priority, (IPTR)(LONG)spec->priority },
            { NP_Cli, spec->cli },
            { NP_CopyVars, spec->copy_vars },
            { NP_Arguments, (IPTR)b->arguments },
            { home ? NP_HomeDir : TAG_IGNORE, (IPTR)home },
            { NP_ConsoleTask, 0 },
            { NP_WindowPtr, 0 },
            { NP_FreeSeglist, TRUE },
            { TAG_DONE, 0 }
        };
        /* Omitted stream tags make three private native OpenNIL handles.
         * Omitted CurrentDir makes native DOS duplicate the caller's CWD. */
        created = CreateNewProc(tags);
    }
    /* A non-NULL value may be stale or only a success sentinel. Do not read
     * the returned Process; worker publishes its own stable inbox/identity. */
    if (!created)
    {
        error = IoErr();
        drop(b); /* CNP failure: there is no worker */
        drop(b);
        b = NULL;
        goto failure;
    }
    home = BNULL; /* native DOS owns the distinct duplicate */
    base = NULL;  /* worker balances the private library reference */
    error = ERROR_BREAK;
    for (ticks = 0; ticks < KAFFEINE_DOS_BROKER_START_TICKS; ++ticks)
    {
        ObtainSemaphore(&b->lock);
        if (b->state == READY)
        {
            b->state = COMMITTED;
            success = TRUE;
            ReleaseSemaphore(&b->lock);
            break;
        }
        if (b->state == FINISHED || b->state == ABORTED)
        {
            error = b->startup_error;
            ReleaseSemaphore(&b->lock);
            break;
        }
        ReleaseSemaphore(&b->lock);
        pause_tick();
    }
    if (!success)
    {
        ObtainSemaphore(&b->lock);
        if (b->state != FINISHED) b->state = ABORTED;
        ReleaseSemaphore(&b->lock);
        drop(b);
        b = NULL;
        goto failure;
    }
    if (spec->home_dir) UnLock(spec->home_dir);
    if (out_error) *out_error = 0;
    SetIoErr(saved);
    return b;

failure:
    if (home) UnLock(home);
    if (base) CloseLibrary(base);
    if (out_error) *out_error = error;
    SetIoErr(saved);
    return NULL;
}

static BOOL submit(struct KaffeineDosBroker *b, struct BrokerRequest *request)
{
    BOOL accepted = FALSE;
    ObtainSemaphore(&b->lock);
    if (b->state == COMMITTED && b->native && b->inbox && !b->busy)
    {
        b->busy = TRUE;
        b->request = request;
        /* Port lifetime cannot end between the state check and PutMsg.
         * No semaphore is held over a DOS operation or the reply wait. */
        PutMsg(b->inbox, &request->message);
        accepted = TRUE;
    }
    ReleaseSemaphore(&b->lock);
    return accepted;
}

static BOOL caller(struct KaffeineDosBroker *b, struct KaffeineContext *ctx,
                   BOOL internal)
{
    struct Task *task = FindTask(NULL);
    if (ctx && ctx->exec_task != b->driver) return FALSE;
    if (task == b->driver) return TRUE;
    /* A parent broker may coordinate only a private initialization/final
     * cleanup helper while its driver waits for that parent callback. The
     * parent then waits for this child callback: the Context stays serial. */
    return internal && ctx && ctx->broker_call && ctx->broker_owner == task;
}

BOOL Kaffeine_DosBrokerCall(struct KaffeineDosBroker *b,
    struct KaffeineContext *ctx, void *cpu, ULONG lvo,
    KaffeineDosBrokerThunk thunk, IPTR *result, SIPTR *out_error)
{
    SIPTR saved = IoErr(), error = ERROR_BAD_NUMBER;
    struct MsgPort reply;
    struct BrokerRequest request;
    struct Task *previous_owner;
    BOOL success = FALSE;
    if (result) *result = 0;
    if (!b || !ctx || !cpu || !thunk || !result ||
        !caller(b, ctx, lvo == 0)) goto done;
    previous_owner = ctx->broker_owner;
    reply_port(&reply);
    memset(&request, 0, sizeof(request));
    request.message.mn_ReplyPort = &reply;
    request.message.mn_Length = sizeof(request);
    request.kind = BROKER_CALL;
    request.ctx = ctx;
    request.cpu = cpu;
    request.lvo = lvo;
    request.thunk = thunk;
    error = ERROR_BREAK;
    if (!submit(b, &request)) goto done;
    await_reply(&reply, &request.message);
    ctx->broker_owner = previous_owner;
    *result = request.result;
    ObtainSemaphore(&b->lock);
    b->request = NULL;
    b->busy = FALSE;
    ReleaseSemaphore(&b->lock);
    success = TRUE;
    error = 0;
done:
    if (out_error) *out_error = error;
    SetIoErr(saved);
    return success;
}

BOOL Kaffeine_DosBrokerFinish(struct KaffeineDosBroker *b,
    struct KaffeineContext *ctx, KaffeineDosBrokerThunk cleanup)
{
    SIPTR saved = IoErr();
    struct MsgPort reply;
    struct BrokerRequest request;
    struct Task *previous_owner = NULL;
    BOOL success = FALSE;
    if (!b || (!ctx != !cleanup) || !caller(b, ctx, cleanup != NULL)) goto done;
    if (ctx) previous_owner = ctx->broker_owner;
    reply_port(&reply);
    memset(&request, 0, sizeof(request));
    request.message.mn_ReplyPort = &reply;
    request.message.mn_Length = sizeof(request);
    request.kind = BROKER_STOP;
    request.ctx = ctx;
    request.thunk = cleanup;
    if (!submit(b, &request)) goto done;
    await_reply(&reply, &request.message);
    if (ctx) ctx->broker_owner = previous_owner;
    /* The worker released all private execution resources before replying.
     * This last owner reference keeps the record alive through that reply. */
    drop(b);
    success = TRUE;
done:
    SetIoErr(saved);
    return success;
}

BOOL Kaffeine_DosBrokerStop(struct KaffeineDosBroker *b)
{
    return Kaffeine_DosBrokerFinish(b, NULL, NULL);
}

BOOL Kaffeine_DosBrokerIsIdle(struct KaffeineDosBroker *b)
{
    BOOL idle;
    if (!b) return FALSE;
    ObtainSemaphore(&b->lock);
    idle = b->state == COMMITTED && b->native && b->inbox && !b->busy;
    ReleaseSemaphore(&b->lock);
    return idle;
}

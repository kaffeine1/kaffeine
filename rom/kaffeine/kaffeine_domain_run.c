/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_domain_run.h"
#include "kaffeine_filehandle.h"

#ifndef KAFFEINE_DOMAIN_START_TICKS
#define KAFFEINE_DOMAIN_START_TICKS 250
#endif

enum DriverState { DRIVER_STARTING, DRIVER_READY, DRIVER_COMMITTED, DRIVER_ABORTED, DRIVER_FINISHED };

struct DomainDriver
{
    struct SignalSemaphore lock;
    unsigned refs;
    enum DriverState state;
    struct KaffeineContext *ctx;
    struct Library *base;
    LONG (*run)(struct KaffeineContext *);
    void (*dispose)(struct KaffeineContext *);
    BOOL root_done, cli;
    LONG result;
    SIPTR error;
    char command[256];
};

static void release(struct DomainDriver *driver)
{
    BOOL last;
    ObtainSemaphore(&driver->lock);
    last = --driver->refs == 0;
    ReleaseSemaphore(&driver->lock);
    if (last)
    {
        FreeVec(driver);
    }
}

static void root_complete(struct KaffeineContext *ctx, LONG result)
{
    struct DomainDriver *driver = ctx->root_completion_data;
    unsigned i;
    /* The caller may close its streams as soon as the gate opens. Retire
       their borrowed identities first, without closing caller-owned handles. */
    for (i = 0; i < 3; ++i)
        Kaffeine_FileDropBorrowed(ctx, ctx->root_streams[i]);
    ObtainSemaphore(&driver->lock);
    if (!driver->root_done)
    {
        driver->result = result;
        driver->root_done = TRUE;
    }
    ReleaseSemaphore(&driver->lock);
}

static LONG driver_entry(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct DomainDriver *driver = me->pr_Task.tc_UserData;
    struct Library *base = driver->base;
    struct KaffeineContext *ctx;
    LONG result = RETURN_FAIL;
    BOOL committed;
    unsigned ticks;

    if (driver->cli && driver->command[0] && !SetProgramName(driver->command))
    {
        SIPTR error = IoErr();
        ObtainSemaphore(&driver->lock);
        driver->error = error ? error : ERROR_NO_FREE_STORE;
        driver->state = DRIVER_ABORTED;
        ReleaseSemaphore(&driver->lock);
        goto done;
    }
    ObtainSemaphore(&driver->lock);
    if (driver->state == DRIVER_STARTING) driver->state = DRIVER_READY;
    ReleaseSemaphore(&driver->lock);
    for (ticks = 0; ticks < KAFFEINE_DOMAIN_START_TICKS; ++ticks)
    {
        ObtainSemaphore(&driver->lock);
        committed = driver->state == DRIVER_COMMITTED;
        if (committed || driver->state == DRIVER_ABORTED)
        {
            ReleaseSemaphore(&driver->lock);
            break;
        }
        ReleaseSemaphore(&driver->lock);
        Delay(1);
    }
    ObtainSemaphore(&driver->lock);
    committed = driver->state == DRIVER_COMMITTED;
    if (!committed) driver->state = DRIVER_ABORTED;
    ctx = committed ? driver->ctx : NULL;
    ReleaseSemaphore(&driver->lock);
    if (ctx)
    {
        result = driver->run(ctx);
        root_complete(ctx, result);
        ctx->root_complete = NULL;
        ctx->root_completion_data = NULL;
        driver->dispose(ctx);
        ObtainSemaphore(&driver->lock);
        driver->ctx = NULL;
        ReleaseSemaphore(&driver->lock);
    }
done:
    me->pr_Task.tc_UserData = NULL;
    ObtainSemaphore(&driver->lock);
    driver->state = DRIVER_FINISHED;
    ReleaseSemaphore(&driver->lock);
    release(driver);
    /* The library is resident (noexpunge), including this final return. */
    CloseLibrary(base);
    return result;
}

BOOL Kaffeine_RunDomain(struct KaffeineContext *ctx, LONG (*run)(struct KaffeineContext *),
                       void (*dispose)(struct KaffeineContext *), LONG *result)
{
    struct DomainDriver *driver;
    struct Process *me = (struct Process *)FindTask(NULL), *created;
    BPTR current, directory = BNULL;
    SIPTR error = ERROR_NO_FREE_STORE, saved_error;
    BOOL committed = FALSE, done;
    unsigned ticks;

    if (!ctx || !run || !dispose || !result || !me ||
        me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return FALSE;
    }
    saved_error = IoErr();
    driver = AllocVec(sizeof(*driver), MEMF_PUBLIC | MEMF_CLEAR);
    if (!driver) goto failed;
    InitSemaphore(&driver->lock);
    driver->refs = 1;
    driver->base = OpenLibrary("kaffeine.library", 0);
    if (!driver->base) { error = ERROR_INVALID_RESIDENT_LIBRARY; goto no_worker; }
    current = CurrentDir(BNULL);
    CurrentDir(current);
    if (current)
    {
        directory = DupLock(current);
        if (!directory) { error = IoErr(); goto no_worker; }
    }
    driver->ctx = ctx;
    driver->run = run;
    driver->dispose = dispose;
    driver->cli = me->pr_CLI != BNULL;
    memcpy(driver->command, ctx->program_name, sizeof(driver->command));
    driver->command[sizeof(driver->command) - 1] = 0;
    driver->error = ERROR_BREAK;
    driver->state = DRIVER_STARTING;
    driver->refs = 2;
    {
        struct TagItem tags[] = {
            { NP_Entry, (IPTR)driver_entry },
            { NP_UserData, (IPTR)driver },
            { NP_Name, (IPTR)"kaffeine" },
            { NP_StackSize, 128 * 1024 },
            { NP_Synchronous, FALSE },
            { NP_FreeSeglist, TRUE },
            { NP_Cli, driver->cli },
            { NP_CopyVars, TRUE },
            { NP_Input, (IPTR)Input() },
            { NP_CloseInput, FALSE },
            { NP_Output, (IPTR)Output() },
            { NP_CloseOutput, FALSE },
            { NP_Error, (IPTR)me->pr_CES },
            { NP_CloseError, FALSE },
            { directory ? NP_CurrentDir : TAG_IGNORE, (IPTR)directory },
            { TAG_DONE, 0 }
        };
        created = CreateNewProc(tags);
    }
    /* Native DOS may have already retired a non-NULL Process. Its returned
       pointer is an outcome only; the gate and its references are private. */
    if (!created)
    {
        error = IoErr();
        driver->refs = 1;
        goto no_worker;
    }
    directory = BNULL; /* native DOS owns the duplicate, including on abort */
    for (ticks = 0; ticks < KAFFEINE_DOMAIN_START_TICKS; ++ticks)
    {
        ObtainSemaphore(&driver->lock);
        if (driver->state == DRIVER_READY)
        {
            ctx->root_completion_data = driver;
            ctx->root_complete = root_complete;
            driver->state = DRIVER_COMMITTED;
            committed = TRUE;
        }
        done = committed || driver->state == DRIVER_ABORTED || driver->state == DRIVER_FINISHED;
        error = driver->error;
        ReleaseSemaphore(&driver->lock);
        if (done) break;
        Delay(1);
    }
    if (!committed)
    {
        ObtainSemaphore(&driver->lock);
        if (driver->state != DRIVER_FINISHED) driver->state = DRIVER_ABORTED;
        driver->ctx = NULL;
        ReleaseSemaphore(&driver->lock);
        release(driver);
        SetIoErr(error);
        return FALSE;
    }
    for (;;)
    {
        ObtainSemaphore(&driver->lock);
        done = driver->root_done;
        if (done) *result = driver->result;
        ReleaseSemaphore(&driver->lock);
        if (done) break;
        Delay(1);
    }
    release(driver);
    SetIoErr(saved_error);
    return TRUE;

no_worker:
    if (directory) UnLock(directory);
    if (driver->base) CloseLibrary(driver->base);
    release(driver);
failed:
    SetIoErr(error);
    return FALSE;
}

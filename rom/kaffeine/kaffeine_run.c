/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    kaffeine.library — RunHunk / RunFile entry points
*/

#include <aros/debug.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <string.h>
#include <stdio.h>

#include "kaffeine_intern.h"
#include "kaffeine_shadow.h"
#include "kaffeine_thunks.h"
#include "kaffeine_readargs.h"
#include "kaffeine_thunks_locale.h"
#include "kaffeine_thunks_dos.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_menus.h"
#include "kaffeine_screens.h"
#include "kaffeine_window.h"
#include "kaffeine_rastport.h"
#include "kaffeine_icon.h"
#include "kaffeine_layers.h"
#include "kaffeine_sched.h"
#include "kaffeine_regions.h"

/* The signals, ports and open devices of the guest belong to the native
   task that runs it, as does a lock of the list of public screens, an Exec
   semaphore: release them there, before that task ends. */
static LONG execute_owned(struct KaffeineContext *ctx)
{
    LONG result;

    ctx->exec_task = FindTask(NULL);
    result = Kaffeine_Execute(ctx);
    Kaffeine_WindowsCleanup(ctx);
    Kaffeine_MenusCleanup(ctx);
    Kaffeine_GadgetsCleanup(ctx);
    Kaffeine_VisualInfosCleanup(ctx);
    Kaffeine_FileCleanup(ctx);
    Kaffeine_IOCleanup(ctx);
    Kaffeine_ScreensCleanup(ctx);
    /* after the windows and screens, which retire their 68k Layers first */
    Kaffeine_LayersCleanup(ctx);
    /* after the layers, whose clip Regions they may be */
    Kaffeine_RegionsCleanup(ctx);
    return result;
}

/* ── Emulator child process entry point ────────────────────────── */

static LONG emu_proc_entry(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct KaffeineContext *ctx = (struct KaffeineContext *)me->pr_Task.tc_UserData;

    if (!ctx) return -1;

    ctx->exit_code = execute_owned(ctx);
    return ctx->exit_code;
}

/* ── Common setup + execution for both RunHunk and RunFile ─────── */

static LONG setup_and_execute(struct KaffeineContext *ctx, ULONG entry,
                              CONST_STRPTR argPtr, ULONG argSize,
                              ULONG stackSize, const char *programName,
                              struct KaffeineLibBase *KaffeineBase)
{
    ULONG m68k_sysbase, m68k_argptr;
    LONG result = -1;

    /* Set up fake exec.library base (lib_id=0) */
    m68k_sysbase = Kaffeine_SetupLibBase(ctx, 0, "exec.library",
                                         KAFFEINE_MAX_LVO,
                                         kaffeine_thunks_exec,
                                         kaffeine_thunks_exec_count,
                                         kaffeine_thunks_exec_gen,
                                         kaffeine_thunks_exec_gen_count);
    if (!m68k_sysbase)
        return -1;

    /* Set up fake dos.library base (lib_id=1) */
    Kaffeine_SetupLibBase(ctx, 1, "dos.library",
                          KAFFEINE_MAX_LVO,
                          kaffeine_thunks_dos,
                          kaffeine_thunks_dos_count,
                          kaffeine_thunks_dos_gen,
                          kaffeine_thunks_dos_gen_count);

    /* Write SysBase pointer at address 4 (AbsExecBase) */
    m68k_write32(ctx, 4, m68k_sysbase);

    /* Copy argument string into containment space */
    m68k_argptr = 0;
    if (argPtr && argSize > 0)
    {
        m68k_argptr = Kaffeine_HeapAlloc(ctx, argSize + 1, 0);
        if (m68k_argptr)
            CopyMem((APTR)argPtr, m68k_to_host(ctx, m68k_argptr), argSize);
    }

    if (programName)
    {
        snprintf(ctx->program_name, sizeof(ctx->program_name), "%s", programName);
    }

    /* Publish startup state before building the one current-task snapshot. */
    ctx->entry_point  = entry;
    ctx->m68k_argptr  = m68k_argptr;
    ctx->m68k_argsize = argSize;
    ctx->m68k_sysbase = m68k_sysbase;

    /* Init ExecBase shadow data fields */
    shadow_init_execbase(ctx, m68k_sysbase, SysBase);

    /* AddTask() children run on the guest task scheduler */
    ctx->task_ops = &Kaffeine_GuestTaskOps;

    /* Auto-open libraries referenced by C startup tables */
    Kaffeine_AutoOpenLibs(ctx);

    /* If stackSize is 0, run directly (non-threaded) */
    if (stackSize == 0)
        return execute_owned(ctx);

    /* Spawn emulator in its own AROS process.
     * NP_Synchronous=TRUE makes us block until the child exits.
     */
    {
        BPTR curdir = CurrentDir(BNULL);
        CurrentDir(curdir);  /* restore */

        BPTR dirlock = DupLock(curdir);

        struct TagItem tags[] = {
            { NP_Entry,       (IPTR)emu_proc_entry },
            { NP_Name,        (IPTR)"kaffeine" },
            { NP_Synchronous, TRUE },
            { NP_UserData,    (IPTR)ctx },
            { NP_FreeSeglist, FALSE },
            { NP_Input,       (IPTR)Input() },
            { NP_CloseInput,  FALSE },
            { NP_Output,      (IPTR)Output() },
            { NP_CloseOutput, FALSE },
            { NP_CurrentDir,  (IPTR)dirlock },
            { TAG_DONE,       0 }
        };

        struct Process *proc = CreateNewProc(tags);
        if (proc)
        {
            result = ctx->exit_code;
        }
        else
        {
            UnLock(dirlock);
            result = -1;
        }
    }

    return result;
}

/*****************************************************************************

    NAME */
#include <proto/kaffeine.h>

        AROS_LH4(LONG, RunHunk,

/*  SYNOPSIS */
        AROS_LHA(BPTR,        segList,   D1),
        AROS_LHA(ULONG,       stackSize, D2),
        AROS_LHA(CONST_STRPTR, argPtr,   D3),
        AROS_LHA(ULONG,       argSize,   D4),

/*  LOCATION */
        struct KaffeineLibBase *, KaffeineBase, 5, Kaffeine)

/*  FUNCTION
        Execute an m68k AmigaOS hunk binary under emulation.
        All library calls from the m68k code are trapped and
        dispatched to native AROS implementations.

    INPUTS
        segList   - Loaded m68k hunk segment list (from LoadSeg)
        stackSize - Stack size for the emulated process
        argPtr    - Command arguments string
        argSize   - Length of arguments

    RESULT
        Return code from the m68k program, or -1 on failure. A seglist
        without the relocation record kept by LoadSeg() (GSLI_68KRelocs)
        is refused.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct KaffeineContext *ctx;
    IPTR relocs = 0;
    struct TagItem segtags[] =
    {
        { GSLI_68KRelocs, (IPTR)&relocs },
        { TAG_DONE,       0             }
    };
    ULONG entry;
    LONG result = -1;
    char program[256];

    /* LoadSeg() relocated the segments for their native addresses. */
    if (!GetSegListInfo(segList, segtags) || !relocs)
    {
        D(bug("[kaffeine] RunHunk: no relocation record for seglist %p\n", segList));
        return -1;
    }

    ctx = Kaffeine_CreateContext(KaffeineBase);
    if (!ctx)
        return -1;
    Kaffeine_GadgetsSetup(ctx);
    Kaffeine_LayersSetup(ctx);

    entry = Kaffeine_LoadHunks(ctx, segList, (const struct HunkRelocTable *)relocs);
    if (entry)
    {
        /* The Shell names its CLI's command before RunCommand(); that name
           is what GetProgramName() and a C startup's argv[0] report. */
        if (!GetProgramName(program, sizeof(program)))
            program[0] = 0;
        result = setup_and_execute(ctx, entry, argPtr, argSize,
                                   stackSize, program[0] ? program : NULL, KaffeineBase);
    }

    Kaffeine_ReadArgsCleanup(ctx);
    Kaffeine_LocaleCleanup(ctx);
    Kaffeine_MatchCleanup(ctx);
    Kaffeine_ExAllCleanup(ctx);
    Kaffeine_MenusCleanup(ctx);
    Kaffeine_GadgetsCleanup(ctx);
    Kaffeine_FontsCleanup(ctx);
    Kaffeine_IconsCleanup(ctx);
    Kaffeine_LayersCleanup(ctx);
    Kaffeine_RegionsCleanup(ctx);
    Kaffeine_DestroyContext(ctx);
    return result;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************
    RunFile — load and execute an m68k hunk binary from a file path
*****************************************************************************/
#include <proto/kaffeine.h>

        AROS_LH4(LONG, RunFile,
        AROS_LHA(CONST_STRPTR, fileName, D1),
        AROS_LHA(ULONG,       stackSize, D2),
        AROS_LHA(CONST_STRPTR, argPtr,   D3),
        AROS_LHA(ULONG,       argSize,   D4),
        struct KaffeineLibBase *, KaffeineBase, 6, Kaffeine)
{
    AROS_LIBFUNC_INIT

    struct KaffeineContext *ctx;
    BPTR fh;
    LONG fileSize;
    UBYTE *fileData;
    ULONG entry;
    LONG result = -1;

    fh = Open(fileName, MODE_OLDFILE);
    if (!fh) return -1;

    Seek(fh, 0, OFFSET_END);
    fileSize = Seek(fh, 0, OFFSET_BEGINNING);
    if (fileSize <= 0) { Close(fh); return -1; }

    fileData = (UBYTE *)AllocMem(fileSize, MEMF_ANY);
    if (!fileData) { Close(fh); return -1; }

    if (Read(fh, fileData, fileSize) != fileSize)
    { FreeMem(fileData, fileSize); Close(fh); return -1; }
    Close(fh);

    ctx = Kaffeine_CreateContext(KaffeineBase);
    if (!ctx) { FreeMem(fileData, fileSize); return -1; }
    Kaffeine_GadgetsSetup(ctx);
    Kaffeine_LayersSetup(ctx);

    entry = Kaffeine_LoadHunksFromMemory(ctx, fileData, fileSize);
    FreeMem(fileData, fileSize);

    if (entry)
        result = setup_and_execute(ctx, entry, argPtr, argSize,
                                   stackSize, fileName, KaffeineBase);

    Kaffeine_ReadArgsCleanup(ctx);
    Kaffeine_LocaleCleanup(ctx);
    Kaffeine_MatchCleanup(ctx);
    Kaffeine_ExAllCleanup(ctx);
    Kaffeine_MenusCleanup(ctx);
    Kaffeine_GadgetsCleanup(ctx);
    Kaffeine_FontsCleanup(ctx);
    Kaffeine_IconsCleanup(ctx);
    Kaffeine_LayersCleanup(ctx);
    Kaffeine_RegionsCleanup(ctx);
    Kaffeine_DestroyContext(ctx);
    return result;

    AROS_LIBFUNC_EXIT
}

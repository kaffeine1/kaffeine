/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Non-CLI command launches must not wait for an implicit WBStartup. */
#include <exec/types.h>
#include <exec/semaphores.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <workbench/startup.h>
#include <aros/libcall.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <stdio.h>
#include <string.h>

#if defined(__AROSEXEC_SMP__)
#error This retirement fixture requires uniprocessor Exec
#endif
APTR KernelBase;
static struct Library *emulator;
static BPTR code;
static struct SignalSemaphore guard;
static struct MsgPort *live_port;
static unsigned mode, checks, failures;
static BOOL done;
static IPTR result;
static const char *names[] = {"kaffeine-command-direct", "kaffeine-command-nested", "kaffeine-command-dosentry"};

static BOOL check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    return ok;
}

static void child_exit(IPTR rc, IPTR data)
{
    (void)data;
    ObtainSemaphore(&guard);
    result = rc; done = TRUE; live_port = NULL;
    ReleaseSemaphore(&guard);
}

static LONG child_entry(void)
{
    if (mode == 1)
    {
        struct Process *me = (struct Process *)FindTask(NULL);
        struct Message *startup;
        ObtainSemaphore(&guard);
        live_port = &me->pr_MsgPort;
        ReleaseSemaphore(&guard);
        WaitPort(&me->pr_MsgPort);
        startup = GetMsg(&me->pr_MsgPort);
        if (!startup) return 20;
        ReplyMsg(startup);
        /* The native GUI owner already consumed its own startup. The
           nested command has no second Workbench message to receive. */
        return RunCommand(code, 65536, NULL, 0);
    }
    return AROS_LC4(LONG, RunHunk,
        AROS_LCA(BPTR, code, D1), AROS_LCA(ULONG, 65536, D2),
        AROS_LCA(CONST_STRPTR, NULL, D3), AROS_LCA(ULONG, 0, D4),
        struct Library *, emulator, 5, Kaffeine);
}

static BOOL retired(unsigned slot)
{
    BOOL ended;
    ObtainSemaphore(&guard); ended = done; ReleaseSemaphore(&guard);
    return ended && !FindTask(names[slot]);
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "jit";
    struct MsgPort *reply = NULL;
    struct WBStartup startup;
    IPTR hunk = 0, relocs = 0;
    struct TagItem info[] = {{GSLI_68KHUNK, (IPTR)&hunk}, {GSLI_68KRelocs, (IPTR)&relocs}, {TAG_DONE, 0}};
    unsigned slot, tick;
    BOOL started = FALSE, sent = FALSE;
    InitSemaphore(&guard);
    memset(&startup, 0, sizeof(startup));
    if (!check(!strcmp(backend, "moira") || !strcmp(backend, "jit"), "explicit backend")) goto finish;
    KernelBase = OpenResource("kernel.resource");
    if (!check(KernelBase && KrnGetCPUCount() == 1, "one active Exec CPU for retirement")) goto finish;
    emulator = OpenLibrary("kaffeine.library", 0);
    if (!check(emulator != NULL, "pin emulator through all command owners' retirement")) goto finish;
    if (!check(SetVar("KAFFEINE_CPU", backend, strlen(backend), GVF_LOCAL_ONLY), "copy a local backend into each owner")) goto finish;
    code = LoadSeg("SYS:EMU68/wbstartup_untaken");
    if (!check(code && GetSegListInfo(code, info) == 2 && hunk && relocs, "registered original Rts HUNK with relocation metadata")) goto finish;
    reply = CreateMsgPort();
    if (!check(reply != NULL, "native startup reply port")) goto finish;
    startup.sm_Message.mn_ReplyPort = reply;
    startup.sm_Message.mn_Length = sizeof(startup);
    startup.sm_Segment = code;
    for (slot = 0; slot < 3; ++slot)
    {
        struct Process *created;
        struct TagItem tags[] = {
            {NP_Entry, slot < 2 ? (IPTR)child_entry : 0},
            {NP_Seglist, slot == 2 ? (IPTR)code : 0}, {NP_FreeSeglist, FALSE},
            {NP_Name, (IPTR)names[slot]}, {NP_StackSize, 65536},
            {NP_Cli, FALSE}, {NP_CopyVars, TRUE},
            {NP_Input, (IPTR)BNULL}, {NP_Output, (IPTR)BNULL},
            {NP_ExitCode, (IPTR)child_exit}, {NP_ExitData, 0}, {TAG_DONE, 0}};
        if (!check(!FindTask(names[slot]), "unique native owner name available")) break;
        mode = slot; done = FALSE; live_port = NULL; result = 0; sent = FALSE;
        created = CreateNewProc(tags);
        if (!check(created != NULL, "create a non-CLI command owner")) break;
        started = TRUE;
        /* Never dereference CreateNewProc's possibly stale returned pointer. */
        for (tick = 0; tick < 1000 && !retired(slot); ++tick)
        {
            if (slot == 1 && !sent)
            {
                ObtainSemaphore(&guard);
                if (live_port)
                {
                    startup.sm_Process = live_port;
                    PutMsg(live_port, &startup.sm_Message);
                    sent = TRUE;
                }
                ReleaseSemaphore(&guard);
            }
            Delay(1);
        }
        if (!check(retired(slot), "command returns without waiting for another startup"))
            for (;;) Delay(50); /* outer VM deadline; keep all native data alive */
        started = FALSE;
        check(result == 7, "native exit callback receives the original HUNK result");
        if (slot == 1)
        {
            struct Message *message = GetMsg(reply);
            check(sent && message == &startup.sm_Message && message->mn_Node.ln_Type == NT_REPLYMSG && !GetMsg(reply),
                  "GUI owner consumed and replied its own startup exactly once before RunCommand");
        }
        else
            check(!GetMsg(reply), "command launch supplied no Workbench startup");
    }
finish:
    if (!started)
    {
        if (reply) DeleteMsgPort(reply);
        if (code) UnLoadSeg(code);
        if (emulator) CloseLibrary(emulator);
    }
    printf("wbstartup_command_native: %u checks, %u failures\n", checks, failures);
    return failures ? 20 : 0;
}

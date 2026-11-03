/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Native controller for the original, NDK-free WBStartup HUNK. */
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
#error This name-based retirement fixture requires uniprocessor Exec
#endif
APTR KernelBase;
static struct Library *emulator;
static struct SignalSemaphore guard;
static struct MsgPort *live_port;
static struct Task *live_task;
static BOOL done, forced_reply;
static LONG result;
static BPTR code;
static const char child_name[] = "kaffeine-wbstartup-fixture";
static unsigned checks, failures;

static BOOL check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    return ok;
}

/* Publish only while this native task is alive. Wait uses Exec alone: no
   DOS call may consume the process-port message before RunHunk takes it. */
static LONG child_entry(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct Message *left;
    LONG rc;
    ObtainSemaphore(&guard);
    live_port = &me->pr_MsgPort;
    live_task = &me->pr_Task;
    ReleaseSemaphore(&guard);
    Wait(SIGF_SINGLE);
    rc = AROS_LC4(LONG, RunHunk,
        AROS_LCA(BPTR, code, D1), AROS_LCA(ULONG, 65536, D2),
        AROS_LCA(CONST_STRPTR, NULL, D3), AROS_LCA(ULONG, 0, D4),
        struct Library *, emulator, 5, Kaffeine);
    /* Baseline cleanup is deliberately detectable. It cannot turn a
       missing bridge into a pass or leave a queued pointer to dead data. */
    left = GetMsg(&me->pr_MsgPort);
    if (left) ReplyMsg(left);
    ObtainSemaphore(&guard);
    forced_reply = left != NULL;
    result = rc;
    done = TRUE;
    live_port = NULL;
    live_task = NULL;
    ReleaseSemaphore(&guard);
    return rc;
}

static BOOL write_bytes(const char *name, const char *bytes)
{
    BPTR file = Open(name, MODE_NEWFILE);
    LONG size = strlen(bytes), n;
    if (!file) return FALSE;
    n = Write(file, bytes, size);
    return Close(file) && n == size;
}

static BOOL retired(void)
{
    BOOL ended;
    ObtainSemaphore(&guard);
    ended = done;
    ReleaseSemaphore(&guard);
    return ended && !FindTask(child_name);
}

static int run_case(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "moira";
    const char *payload = argc > 2 ? argv[2] : "SYS:EMU68/wbstartup_guest";
    BOOL cleanup = argc > 3 && !strcmp(argv[3], "cleanup");
    const char *folders[3] = {"SYS:EMU68", "SYS:EMU68/wbstartup-a", "SYS:EMU68/wbstartup-b"};
    const char *names[3] = {"wbstartup_guest", "first", "second"};
    struct WBStartup startup, snapshot;
    struct WBArg args[3], saved_args[3];
    struct MsgPort *reply = NULL;
    struct Message *message = NULL;
    struct Process *created;
    IPTR hunk = 0, relocs = 0;
    struct TagItem info[] = {{GSLI_68KHUNK, (IPTR)&hunk}, {GSLI_68KRelocs, (IPTR)&relocs}, {TAG_DONE, 0}};
    unsigned i, tick;
    BOOL started = FALSE, sent = FALSE;
    UBYTE bytes[13];
    LONG n = -1;
    BPTR file;
    done = forced_reply = FALSE;
    live_port = NULL; live_task = NULL; result = -1; code = BNULL; emulator = NULL;
    InitSemaphore(&guard);
    memset(&startup, 0, sizeof(startup));
    memset(args, 0, sizeof(args));
    if (!check(!strcmp(backend, "moira") || !strcmp(backend, "jit"), "explicit backend")) goto finish;
    KernelBase = OpenResource("kernel.resource");
    if (!check(KernelBase && KrnGetCPUCount() == 1, "one active Exec CPU for retirement")) goto finish;
    if (!check(!FindTask(child_name), "private native child name available")) goto finish;
    /* A local variable is copied into the native wrapper and domain driver;
       do not change the machine's global ENV selection. */
    if (!check(SetVar("KAFFEINE_CPU", backend, strlen(backend), GVF_LOCAL_ONLY), "select copied local backend")) goto finish;
    emulator = OpenLibrary("kaffeine.library", 0);
    if (!check(emulator != NULL, "pin kaffeine.library through native retirement")) goto finish;
    code = LoadSeg(payload);
    if (!check(code && GetSegListInfo(code, info) == 2 && hunk && relocs, "registered 68k HUNK and relocation record")) goto finish;
    for (i = 0; i < 3; ++i)
    {
        BPTR dir = i ? CreateDir(folders[i]) : BNULL;
        if (dir) UnLock(dir);
        args[i].wa_Lock = Lock(folders[i], ACCESS_READ);
        args[i].wa_Name = (STRPTR)names[i];
        if (!check(args[i].wa_Lock != BNULL, "separate native argument lock")) goto finish;
    }
    if (!check(write_bytes("SYS:EMU68/wbstartup-a/first", "ALFA") &&
               write_bytes("SYS:EMU68/wbstartup-b/second", "BETA"), "distinct per-argument directory data")) goto finish;
    DeleteFile("SYS:EMU68/wbstartup-result.bin");
    reply = CreateMsgPort();
    if (!check(reply != NULL, "private native reply port")) goto finish;
    startup.sm_Message.mn_ReplyPort = reply;
    startup.sm_Message.mn_Length = sizeof(startup);
    startup.sm_Segment = code;
    startup.sm_NumArgs = 3;
    startup.sm_ArgList = args;
    created = CreateNewProcTags(NP_Entry, (IPTR)child_entry, NP_Name, (IPTR)child_name,
        NP_StackSize, 65536, NP_Cli, FALSE, NP_CopyVars, TRUE, TAG_DONE);
    if (!check(created != NULL, "start a non-CLI native owner")) goto finish;
    started = TRUE;
    /* Never dereference the returned Process pointer; it could be stale. */
    for (tick = 0; tick < 250 && !sent; ++tick)
    {
        ObtainSemaphore(&guard);
        if (live_port && live_task)
        {
            startup.sm_Process = live_port;
            snapshot = startup;
            memcpy(saved_args, args, sizeof(args));
            PutMsg(live_port, &startup.sm_Message);
            Signal(live_task, SIGF_SINGLE);
            sent = TRUE;
        }
        ReleaseSemaphore(&guard);
        if (!sent) Delay(1);
    }
    check(sent, "send startup to a gated live owner before RunHunk");
    for (tick = 0; tick < 1000 && !retired(); ++tick) Delay(1);
    if (!check(retired(), "native owner fully retires within the bounded case"))
    {
        /* Static callback code, locks, HUNK and message must stay alive.
           The VM runner's outer deadline terminates this failed test. */
        for (;;) Delay(50);
    }
    message = GetMsg(reply);
    check(sent && message == &startup.sm_Message &&
          message->mn_Node.ln_Type == NT_REPLYMSG && !GetMsg(reply), "exactly one real native reply");
    check(!forced_reply, "reply came from the bridge, not baseline cleanup");
    check(result == (cleanup ? 7 : 0), "68k payload returned its expected code");
    /* Exec owns the Message header links/type on send/reply. Everything
       after that header, and all WBArg entries, must remain untouched. */
    check(!memcmp((UBYTE *)&startup + sizeof(struct Message),
                  (UBYTE *)&snapshot + sizeof(struct Message), sizeof(startup) - sizeof(struct Message)) &&
          !memcmp(args, saved_args, sizeof(args)), "native startup payload and arguments unchanged");
    file = Open("SYS:EMU68/wbstartup-result.bin", MODE_OLDFILE);
    if (file) { n = Read(file, bytes, sizeof(bytes)); if (!Close(file)) n = -1; }
    if (cleanup)
        check(n == -1 && IoErr() == ERROR_OBJECT_NOT_FOUND,
              "cleanup-only payload leaves no normal startup result");
    else
        check(n == 12 && !memcmp(bytes, "WBS1\0\0\0\177\0\0\0\3", 12),
              "independent bytes prove layout, names, guest seglist and both lock-backed reads");
finish:
    if (!started || retired())
    {
        if (reply) DeleteMsgPort(reply);
        for (i = 0; i < 3; ++i) if (args[i].wa_Lock) UnLock(args[i].wa_Lock);
        if (code) UnLoadSeg(code);
        if (emulator) CloseLibrary(emulator);
    }
    printf("wbstartup_native: %u checks, %u failures\n", checks, failures);
    return failures ? 20 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && (!strcmp(argv[2], "cleanup-suite") || !strcmp(argv[2], "all-suite")))
    {
        char *case_args[] = {argv[0], argv[1], "SYS:EMU68/wbstartup_untaken", "cleanup"};
        BOOL all = !strcmp(argv[2], "all-suite");
        if (all) run_case(2, case_args);
        run_case(4, case_args);
        case_args[2] = "SYS:EMU68/wbstartup_unreplied";
        run_case(4, case_args);
        if (all)
        {
            struct TagItem cli[] = {{SYS_Asynch, FALSE}, {TAG_DONE, 0}};
            check(SystemTagList("SYS:EMU68/hello_short", cli) == 0,
                  "ordinary CLI HUNK path still executes relocated DATA and BSS");
            printf("wbstartup_native: %u checks, %u failures\n", checks, failures);
        }
        return failures ? 20 : 0;
    }
    return run_case(argc, argv);
}

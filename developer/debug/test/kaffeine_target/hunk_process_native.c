/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Native AROS companion: start two original 68k modules through DOS.
   This bypasses the refused guest CreateNewProc thunk deliberately.
   The parent pins kaffeine.library until both native owners have retired. */
#include <exec/types.h>
#include <exec/semaphores.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <stdio.h>
#include <string.h>

#if defined(__AROSEXEC_SMP__)
#error This process-retirement probe requires a uniprocessor Exec build
#endif

APTR KernelBase;

static struct SignalSemaphore state_lock;
static struct Child
{
    BOOL started, done;
    IPTR result;
    BPTR input, output;
} children[2];
static unsigned checks, failures;
static const char *names[2] = {"kaffeine-hunk-test-0", "kaffeine-hunk-test-1"};
static const char *drawers[2] = {"SYS:EMU68/hunk-process-0", "SYS:EMU68/hunk-process-1"};

static BOOL check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    return ok;
}

/* DosEntry's non-m68k exit callback has native IPTR arguments. No callback
   or native data pointer is supplied by an emulated caller in this test. */
static void child_exit(IPTR result, IPTR data)
{
    struct Child *child = (struct Child *)data;
    ObtainSemaphore(&state_lock);
    child->result = result;
    child->input = Input();
    child->output = Output();
    child->done = TRUE;
    ReleaseSemaphore(&state_lock);
}

static BOOL write_file(const char *path, const void *bytes, LONG size)
{
    BPTR file = Open(path, MODE_NEWFILE);
    LONG count;
    BOOL closed;
    if (!file) return FALSE;
    count = Write(file, bytes, size);
    closed = Close(file);
    return count == size && closed;
}

static LONG read_file(unsigned slot, const char *leaf, UBYTE *bytes, LONG size)
{
    char path[128];
    BPTR file;
    LONG count;
    snprintf(path, sizeof(path), "%s/%s", drawers[slot], leaf);
    file = Open(path, MODE_OLDFILE);
    if (!file) return -1;
    count = Read(file, bytes, size);
    return Close(file) ? count : -1;
}

static BOOL save_backend(UBYTE *bytes, LONG capacity, LONG *length, BOOL *present)
{
    BPTR file = Open("ENV:KAFFEINE_CPU", MODE_OLDFILE);
    LONG count = 0, n;
    UBYTE extra;
    *present = file != BNULL;
    *length = 0;
    if (!file) return IoErr() == ERROR_OBJECT_NOT_FOUND;
    while (count < capacity)
    {
        n = Read(file, bytes + count, capacity - count);
        if (n <= 0)
        {
            BOOL closed = Close(file);
            *length = count;
            return n == 0 && closed;
        }
        count += n;
    }
    n = Read(file, &extra, 1);
    *length = count;
    return Close(file) && n == 0;
}

static BOOL prepare(unsigned slot)
{
    static const char *leaves[] = {"input", "ready", "release", "result"};
    char path[128], input[] = "input0\n";
    BPTR lock = CreateDir(drawers[slot]);
    unsigned i;
    if (!lock) lock = Lock(drawers[slot], ACCESS_READ);
    if (!lock) return FALSE;
    UnLock(lock);
    for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); ++i)
    {
        snprintf(path, sizeof(path), "%s/%s", drawers[slot], leaves[i]);
        DeleteFile(path);
        lock = Lock(path, ACCESS_READ);
        if (lock) { UnLock(lock); return FALSE; }
        if (IoErr() != ERROR_OBJECT_NOT_FOUND) return FALSE;
    }
    input[5] += slot;
    snprintf(path, sizeof(path), "%s/input", drawers[slot]);
    return write_file(path, input, sizeof(input) - 1);
}

static void release_children(void)
{
    unsigned i;
    for (i = 0; i < 2; ++i)
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/release", drawers[i]);
        check(write_file(path, "go\n", 3), "release the bounded guest barrier");
    }
}

static BOOL retired(void)
{
    unsigned i;
    for (i = 0; i < 2; ++i)
    {
        BOOL done;
        ObtainSemaphore(&state_lock);
        done = children[i].done;
        ReleaseSemaphore(&state_lock);
        /* Compare only presence; a returned Process pointer may be stale.
           ExitCode precedes DOS cleanup, so its flag alone is not a join. */
        if (children[i].started && (!done || FindTask(names[i]) != NULL))
            return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "moira";
    const char *payload = argc > 2 ? argv[2] : "SYS:EMU68/hunk_process_child";
    char arguments[2][32];
    UBYTE old_backend[4096], restored[4096];
    LONG old_length = 0;
    BOOL old_present = FALSE, environment_changed = FALSE, overlap = FALSE;
    struct Library *pinned;
    BPTR parent_input = Input(), parent_output = Output();
    unsigned i, tick;

    if (!check(!strcmp(backend, "moira") || !strcmp(backend, "jit"),
               "select an explicit backend")) goto finish;
    KernelBase = OpenResource("kernel.resource");
    if (!check(KernelBase && KrnGetCPUCount() == 1,
               "require exactly one active guest CPU for the name-based retirement check")) goto finish;
    InitSemaphore(&state_lock);
    if (!check(!FindTask(names[0]) && !FindTask(names[1]),
               "the fixture's child names are not already in use")) goto finish;
    pinned = OpenLibrary("kaffeine.library", 0);
    if (!check(pinned != NULL, "keep the emulator library open through child retirement")) goto finish;

    /* CopyVars=FALSE needs a global backend in the disposable VM. Snapshot
       only ENV bytes/presence: GetVar can truncate or fall back to ENVARC. */
    if (!check(save_backend(old_backend, sizeof(old_backend), &old_length, &old_present),
               "snapshot the exact prior ENV backend without ENVARC fallback")) goto close_library;
    environment_changed = TRUE;
    if (!check(SetVar("KAFFEINE_CPU", backend, strlen(backend), GVF_GLOBAL_ONLY),
               "set the disposable VM's global backend")) goto close_library;

    for (i = 0; i < 2; ++i)
    {
        BPTR seg = BNULL, dir = BNULL, home = BNULL;
        IPTR hunk = 0, relocs = 0;
        struct Process *process;
        struct TagItem info[] = {{GSLI_68KHUNK, (IPTR)&hunk},
            {GSLI_68KRelocs, (IPTR)&relocs}, {TAG_DONE, 0}};
        struct TagItem tags[] = {
            {NP_Seglist, 0}, {NP_FreeSeglist, TRUE},
            {NP_Name, (IPTR)names[i]}, {NP_CommandName, (IPTR)"hunk_process_child"},
            {NP_StackSize, 65536}, {NP_CopyVars, FALSE}, {NP_Cli, TRUE},
            {NP_CurrentDir, 0}, {NP_HomeDir, 0},
            {NP_Arguments, (IPTR)arguments[i]},
            {NP_ExitCode, (IPTR)child_exit}, {NP_ExitData, (IPTR)&children[i]},
            {TAG_DONE, 0}
        };
        if (!check(prepare(i), "prepare distinct child directory and input")) break;
        seg = LoadSeg(payload);
        if (!check(seg && GetSegListInfo(seg, info) == 2 && hunk && relocs,
                   "load a registered 68k hunk with relocation metadata"))
        { if (seg) UnLoadSeg(seg); break; }
        dir = Lock(drawers[i], ACCESS_READ);
        home = dir ? DupLock(dir) : BNULL;
        if (!check(dir && home, "acquire separate child directory references"))
        { if (home) UnLock(home); if (dir) UnLock(dir); UnLoadSeg(seg); break; }
        snprintf(arguments[i], sizeof(arguments[i]), "%u %s", i, backend);
        tags[0].ti_Data = (IPTR)seg;
        tags[7].ti_Data = (IPTR)dir;
        tags[8].ti_Data = (IPTR)home;
        process = CreateNewProc(tags);
        if (!check(process != NULL, "DOS starts the registered hunk process"))
        { UnLock(home); UnLock(dir); UnLoadSeg(seg); break; }
        children[i].started = TRUE;
        memset(arguments[i], 'X', sizeof(arguments[i]) - 1);
        arguments[i][sizeof(arguments[i]) - 1] = 0;
        /* Successful creation transfers both directory locks and seglist.
           The returned pointer is never dereferenced or retained. */
    }

    for (tick = 0; tick < 500; ++tick)
    {
        UBYTE ready0, ready1;
        if (read_file(0, "ready", &ready0, 1) == 1 && ready0 == 0 &&
            read_file(1, "ready", &ready1, 1) == 1 && ready1 == 1)
        {
            ObtainSemaphore(&state_lock);
            overlap = !children[0].done && !children[1].done;
            ReleaseSemaphore(&state_lock);
            break;
        }
        if (retired()) break;
        Delay(1);
    }
    check(overlap, "both private guest contexts reach the barrier before either exits");
    release_children();
    for (tick = 0; tick < 1000 && !retired(); ++tick) Delay(1);
    if (!check(retired(), "both native owners retire after the guest returns"))
    {
        /* Keep callback code/data and the pinned library alive on failure.
           The VM runner has its own deadline and shuts the test VM down. */
        for (;;) Delay(50);
    }
    for (i = 0; i < 2; ++i)
    {
        UBYTE result[65];
        BOOL bytes = read_file(i, "result", result, sizeof(result)) == 64;
        unsigned j;
        if (bytes)
        {
            bytes = !memcmp(result, "KHF1", 4) && result[4] == i && result[5] == 0x1F;
            for (j = 8; j < 64; ++j) bytes &= result[j] == 0x30 + i;
        }
        check(children[i].started && children[i].done && children[i].result == 7 + i,
              "the native exit callback receives the child's distinct return code");
        check(children[i].input && children[i].output &&
              children[i].input != parent_input && children[i].output != parent_output,
              "unspecified child streams are independent of the parent's streams");
        check(bytes, "the persisted child result retains its private data and checks");
    }
    check(children[0].input != children[1].input && children[0].output != children[1].output,
          "the two children have distinct native streams");
    check(Input() == parent_input && Output() == parent_output,
          "child retirement preserves the parent's selected streams");

close_library:
    if (environment_changed)
    {
        LONG length;
        BOOL present, ok;
        if (old_present)
            ok = write_file("ENV:KAFFEINE_CPU", old_backend, old_length);
        else
            ok = DeleteFile("ENV:KAFFEINE_CPU") || IoErr() == ERROR_OBJECT_NOT_FOUND;
        check(ok && save_backend(restored, sizeof(restored), &length, &present) &&
              present == old_present && (!present || (length == old_length &&
              !memcmp(restored, old_backend, length))),
              "restore the exact prior ENV backend presence and bytes");
    }
    CloseLibrary(pinned);
finish:
    printf("hunk_process_native: %u checks, %u failed\n", checks, failures);
    fflush(stdout);
    return failures ? 20 : 0;
}

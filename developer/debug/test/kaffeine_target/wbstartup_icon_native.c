/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Actual native Workbench TOOL launch, without the CLI tooltype. */
#include <exec/types.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include <proto/kernel.h>
#include <stdio.h>
#include <string.h>

#if defined(__AROSEXEC_SMP__)
#error This name-based retirement probe requires uniprocessor Exec
#endif
APTR KernelBase;
struct Library *IconBase, *WorkbenchBase;
static unsigned checks, failures;
static BOOL check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    return ok;
}
static BOOL write_bytes(const char *name, const void *bytes, LONG length)
{
    BPTR file = Open(name, MODE_NEWFILE);
    LONG n;
    if (!file) return FALSE;
    n = Write(file, bytes, length);
    return Close(file) && n == length;
}
static BOOL snapshot_env(UBYTE *bytes, LONG capacity, LONG *length, BOOL *present)
{
    BPTR file = Open("ENV:KAFFEINE_CPU", MODE_OLDFILE);
    LONG n, total = 0;
    UBYTE extra;
    *present = file != BNULL; *length = 0;
    if (!file) return IoErr() == ERROR_OBJECT_NOT_FOUND;
    while (total < capacity)
    {
        n = Read(file, bytes + total, capacity - total);
        if (n <= 0) { *length = total; return Close(file) && n == 0; }
        total += n;
    }
    n = Read(file, &extra, 1); *length = total;
    return Close(file) && n == 0;
}
int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "jit";
    const char *tool = "SYS:EMU68/wbstartup_guest";
    const char *result = "SYS:EMU68/wbstartup-result.bin";
    const char *folders[] = {"SYS:EMU68/wbstartup-a", "SYS:EMU68/wbstartup-b"};
    BPTR locks[2] = {BNULL, BNULL}, file;
    struct Library *emulator = NULL;
    struct DiskObject *icon = NULL;
    STRPTR *types;
    UBYTE prior[4096], restored[4096], output[13];
    LONG prior_length = 0, restored_length = 0, n = -1;
    BOOL prior_present = FALSE, restored_present = FALSE, changed = FALSE, launched = FALSE;
    unsigned i, tick;
    struct TagItem tags[] = {
        {WBOPENA_ArgLock, 0}, {WBOPENA_ArgName, (IPTR)"first"},
        {WBOPENA_ArgLock, 0}, {WBOPENA_ArgName, (IPTR)"second"}, {TAG_DONE, 0}};
    if (!check(!strcmp(backend, "moira") || !strcmp(backend, "jit"), "explicit expected backend")) goto finish;
    KernelBase = OpenResource("kernel.resource");
    if (!check(KernelBase && KrnGetCPUCount() == 1, "one active Exec CPU for retirement")) goto finish;
    if (!check(!FindTask("wbstartup_guest"), "Workbench tool is not already active")) goto finish;
    emulator = OpenLibrary("kaffeine.library", 0);
    IconBase = OpenLibrary("icon.library", 44);
    WorkbenchBase = OpenLibrary("workbench.library", 44);
    if (!check(emulator && IconBase && WorkbenchBase &&
               (emulator->lib_Version > 1 || emulator->lib_Revision >= 1) &&
               (WorkbenchBase->lib_Version > 45 ||
                (WorkbenchBase->lib_Version == 45 && WorkbenchBase->lib_Revision >= 7)),
               "pin compatible startup ABI and native icon/Workbench libraries")) goto finish;
    for (i = 0; i < 2; ++i)
    {
        BPTR dir = CreateDir(folders[i]);
        if (dir) UnLock(dir);
        locks[i] = Lock(folders[i], ACCESS_READ);
        if (!check(locks[i] != BNULL, "acquire native project directory")) goto finish;
    }
    tags[0].ti_Data = (IPTR)locks[0]; tags[2].ti_Data = (IPTR)locks[1];
    if (!check(write_bytes("SYS:EMU68/wbstartup-a/first", "ALFA", 4) &&
               write_bytes("SYS:EMU68/wbstartup-b/second", "BETA", 4), "prepare distinct project files")) goto finish;
    DeleteFile(result);
    file = Lock(result, ACCESS_READ);
    if (file) UnLock(file);
    if (!check(!file && IoErr() == ERROR_OBJECT_NOT_FOUND, "no stale result can satisfy the probe")) goto finish;
    icon = GetDefDiskObject(WBTOOL);
    if (!check(icon != NULL, "obtain native TOOL icon")) goto finish;
    types = icon->do_ToolTypes;
    icon->do_ToolTypes = NULL;
    icon->do_StackSize = 65536;
    i = PutDiskObject((STRPTR)tool, icon);
    icon->do_ToolTypes = types;
    FreeDiskObject(icon); icon = NULL;
    if (!check(i != 0, "write TOOL icon with no tooltypes")) goto finish;
    icon = GetDiskObject((STRPTR)tool);
    if (!check(icon && icon->do_Type == WBTOOL &&
               !FindToolType(icon->do_ToolTypes, "CLI"), "reread icon and verify CLI is absent")) goto finish;
    FreeDiskObject(icon); icon = NULL;
    if (!check(snapshot_env(prior, sizeof(prior), &prior_length, &prior_present), "snapshot exact global backend without ENVARC fallback")) goto finish;
    changed = TRUE;
    if (!check(SetVar("KAFFEINE_CPU", backend, strlen(backend), GVF_GLOBAL_ONLY), "select disposable machine's backend for Workbench")) goto finish;
    launched = OpenWorkbenchObjectA((STRPTR)tool, tags);
    if (!check(launched, "native Workbench accepts the non-CLI TOOL and project arguments")) goto finish;
    for (tick = 0; tick < 1000; ++tick)
    {
        file = Open(result, MODE_OLDFILE);
        if (file) { n = Read(file, output, sizeof(output)); if (!Close(file)) n = -1; }
        if (n == 12 && !FindTask("wbstartup_guest")) break;
        Delay(1);
    }
    if (!check(tick < 1000, "Workbench tool produces a fresh result and its native owner retires"))
    {
        /* Native Workbench may still own messages/locks. Keep caller data
           and library references alive until the external VM deadline. */
        for (;;) Delay(50);
    }
    check(n == 12 && !memcmp(output, "WBS1\0\0\0\177\0\0\0\3", 12),
          "actual non-CLI DOS launch passes all classic startup checks");
finish:
    if (changed)
    {
        BOOL ok = prior_present ? write_bytes("ENV:KAFFEINE_CPU", prior, prior_length) : DeleteFile("ENV:KAFFEINE_CPU");
        check(ok && snapshot_env(restored, sizeof(restored), &restored_length, &restored_present) &&
              prior_present == restored_present && prior_length == restored_length &&
              !memcmp(prior, restored, prior_length), "restore exact prior global backend");
    }
    if (icon) FreeDiskObject(icon);
    for (i = 0; i < 2; ++i) if (locks[i]) UnLock(locks[i]);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (IconBase) CloseLibrary(IconBase);
    if (emulator) CloseLibrary(emulator);
    printf("wbstartup_icon_native: %u checks, %u failures\n", checks, failures);
    return failures ? 20 : 0;
}

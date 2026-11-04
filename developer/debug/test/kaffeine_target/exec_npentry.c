/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Guest-only entry, Process user data, DOS ownership and Forbid retirement. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"
static struct Task *parent;
static ULONG done;
static volatile LONG passed, parent_leaves;
static ULONG token = 0x51a7bcde;
static LONG child(void)
{
    struct Task *me = FindTask(NULL);
    BPTR lock;
    Wait(SIGBREAKF_CTRL_F);
    passed = me->tc_UserData == (APTR)&token && *(ULONG *)me->tc_UserData == token;
    lock = Lock((CONST_STRPTR)"RAM:", ACCESS_READ);
    passed = passed && lock != 0;
    if (lock) UnLock(lock);
    SetIoErr(0x1234);
    Forbid(); Signal(parent, done);
    return 0;
}
static LONG after_root(void)
{
    BPTR file;
    static const char proof[] = "NP_ENTRY_CHILD_OK\n";
    if (!parent_leaves) return 10;
    file = Open((CONST_STRPTR)"SYS:EMU68/np-entry-after-root.tmp", MODE_NEWFILE);
    if (!file) return 10;
    if (Write(file, (APTR)proof, sizeof(proof)-1) != sizeof(proof)-1) { Close(file); return 10; }
    Close(file);
    return Rename((CONST_STRPTR)"SYS:EMU68/np-entry-after-root.tmp",
                  (CONST_STRPTR)"SYS:EMU68/np-entry-after-root.txt") ? 0 : 10;
}
int main(void)
{
    struct Process *process;
    LONG bit, priority, ioerr;
    parent = FindTask(NULL);
    priority = SetTaskPri(parent, 2);
    bit = AllocSignal(-1);
    if (!check(bit >= 0, "allocates completion signal")) goto end;
    done = 1UL << bit;
    SetIoErr(0x4321); ioerr = IoErr();
    process = CreateNewProcTags(NP_Entry,(ULONG)child, NP_Name,(ULONG)"entry-fixture-child",
              NP_Priority,0, NP_StackSize,16384, TAG_DONE);
    if (check(process != NULL, "creates guest NP_Entry Process with its own DOS broker"))
    {
        process->pr_Task.tc_UserData = (APTR)&token;
        SetIoErr(0x4321);
        Signal((struct Task *)process, SIGBREAKF_CTRL_F);
        Wait(done);
        ioerr = IoErr(); /* Capture before the fixture's own DOS output. */
        check(passed == 1, "child receives late tc_UserData and makes a native DOS call");
        check(ioerr == 0x4321, "child DOS error does not overwrite parent DOS error");
        check(FindTask((CONST_STRPTR)"entry-fixture-child") == NULL,
              "child returns while Forbid and retires before parent resumes");
    }
    check(!CreateNewProcTags(NP_Entry,((ULONG)child)|1, TAG_DONE), "refuses an odd guest code entry");
    check(!CreateNewProcTags(NP_Entry,(ULONG)child, NP_Cli,TRUE,TAG_DONE), "refuses unsupported entry CLI mode");
    check(!CreateNewProcTags(NP_Entry,(ULONG)child, NP_ExitCode,(ULONG)child,TAG_DONE), "refuses a guest exit callback");
    DeleteFile((CONST_STRPTR)"SYS:EMU68/np-entry-after-root.txt");
    process = CreateNewProcTags(NP_Entry,(ULONG)after_root, NP_Name,(ULONG)"entry-fixture-drain",
              NP_Priority,0, NP_StackSize,16384, TAG_DONE);
    check(process != NULL, "creates a guest child that remains alive when root returns");
    parent_leaves = 1;
    FreeSignal(bit);
end:
    /* Keep the root above its last child until its normal return. */
    (void)priority;
    return test_done("exec_npentry");
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Break of the CLI that launched a 68k program reaches the program. It
    runs in a Kaffeine driver process of its own while that CLI waits, and
    Status COMMAND names the CLI, not the driver. The program clears its
    break signals, starts exec_break_forward-helper, which lies next to it,
    with Run, and the helper sends CTRL-C and then CTRL-D with Break to the
    CLI that Status COMMAND gives for this command. The program waits up to
    30 seconds for both and returns 0 only when both came and neither
    CTRL-E nor CTRL-F did. The helper writes its witnesses with the
    execution number N, 1 from the Shell and 2 through Run, once
    NAME-shell-rc.txt exists.

    The launching script shows that no break was left on that CLI: a CTRL-D
    still pending there would stop the script after the Shell execution,
    and a CTRL-C the Copy that follows the Run execution.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "m68ktest.h"

#define BREAKS (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F)
#define WANTED (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D)

int main(void)
{
    char name[256], path[300], command[700];
    ULONG got = 0;
    LONG polls, rc, n;
    BPTR lock;

    if (!check(GetProgramName((STRPTR)name, sizeof(name)) && name[0], "GetProgramName() names the command"))
        return test_done("exec_break_forward");

    /* the execution number, as the runner names its files */
    strcpy(path, name);
    strcat(path, "-shell-rc.txt");
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    n = lock ? 2 : 1;
    if (lock)
        UnLock(lock);

    /* only the breaks that the helper sends count */
    SetSignal(0, BREAKS);
    strcpy(command, "Run >NIL: <NIL: Execute \"");
    strcat(command, name);
    strcat(command, "-helper\" \"");
    strcat(command, name);
    strcat(command, n == 2 ? "\" 2" : "\" 1");
    rc = SystemTagList((CONST_STRPTR)command, NULL);
    check_long("Run of the helper", rc, 0);

    for (polls = 0; polls < 300 && (got & WANTED) != WANTED; polls++)
    {
        got |= SetSignal(0, BREAKS) & BREAKS;
        if ((got & WANTED) != WANTED)
            Delay(5);
    }
    check((got & SIGBREAKF_CTRL_C) != 0, "CTRL-C from Break of the launching CLI");
    check((got & SIGBREAKF_CTRL_D) != 0, "CTRL-D from Break of the launching CLI");
    check(!(got & (SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F)), "no CTRL-E or CTRL-F");
    return test_done("exec_break_forward");
}

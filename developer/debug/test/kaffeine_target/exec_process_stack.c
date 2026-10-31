/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original probe of the guest Process stack description. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include "m68ktest.h"
int main(void)
{
    struct Process *proc=(struct Process*)FindTask(NULL);
    volatile ULONG local=0;
    ULONG lower,upper;
    if(!check(proc!=NULL&&proc->pr_Task.tc_Node.ln_Type==NT_PROCESS,"current task is a Process"))
        return test_done("exec_process_stack");
    lower=(ULONG)proc->pr_Task.tc_SPLower;upper=(ULONG)proc->pr_Task.tc_SPUpper;
    check(lower!=0&&upper>lower,"Task describes a nonempty guest stack");
    check((ULONG)&local>=lower&&(ULONG)&local+sizeof(local)<=upper,"local variable lies within declared stack");
    check(proc->pr_StackSize>=16384,"Process advertises at least 16 KiB of reserved stack");
    check(proc->pr_StackSize>0&&(ULONG)proc->pr_StackSize==upper-lower,"Process size equals the Task stack interval");
    check(upper!=0&&(ULONG)BADDR(proc->pr_StackBase)==upper,"Process stack base is a BPTR to the high end");
    check(FindTask(NULL)==(struct Task*)proc&&SysBase->ThisTask==(struct Task*)proc,"stack description retains current task identity");
    return test_done("exec_process_stack");
}

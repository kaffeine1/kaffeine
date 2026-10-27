/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original classic Exec task/signal test. Parent and child code share their
   data but have independent CPU state and stacks. Wait temporarily permits
   scheduling inside Forbid, and child return must not end the parent. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"

static struct Task worker, returning_worker;
static ULONG worker_stack[512], return_stack[512];
static struct Task *parent;
static ULONG parent_mask;
static volatile ULONG phase, identity_ok, named_ok, received, nesting_after_wait;
static volatile ULONG remove_returned, returned_body;

static void worker_entry(void)
{
    Forbid();
    identity_ok = FindTask(NULL) == &worker && SysBase->ThisTask == &worker;
    named_ok = FindTask((CONST_STRPTR)"kaffeine-task-worker") == &worker;
    phase = 1;
    Signal(parent,parent_mask);
    received = Wait(SIGF_SINGLE);
    nesting_after_wait = (UBYTE)SysBase->TDNestCnt;
    Permit();
    phase = 2;
    Signal(parent,parent_mask);
    RemTask(NULL);
    remove_returned = 1;
}

static void returning_entry(void)
{
    returned_body = 1;
    Signal(parent,parent_mask);
}

static void init_task(struct Task *task, ULONG *stack, const char *name)
{
    memset(task,0,sizeof(*task));
    task->tc_Node.ln_Type=NT_TASK;
    task->tc_Node.ln_Pri=5;
    task->tc_Node.ln_Name=(STRPTR)name;
    task->tc_SPLower=stack;
    task->tc_SPUpper=stack+512;
    task->tc_SPReg=stack+512;
    task->tc_MemEntry.lh_Head=(struct Node *)&task->tc_MemEntry.lh_Tail;
    task->tc_MemEntry.lh_Tail=NULL;
    task->tc_MemEntry.lh_TailPred=(struct Node *)&task->tc_MemEntry.lh_Head;
    task->tc_MemEntry.lh_Type=NT_MEMORY;
}

int main(void)
{
    LONG sig=AllocSignal(-1);
    ULONG before_permit;
    parent=FindTask(NULL);
    if(!check(sig>=0 && parent!=NULL,"parent has task identity and an allocated signal"))
        return test_done("exec_guest_tasks");
    parent_mask=1UL<<sig;
    init_task(&worker,worker_stack,"kaffeine-task-worker");
    Forbid();
    AddTask(&worker,(APTR)worker_entry,NULL);
    before_permit=phase;
    Permit();
    check(before_permit==0,"Forbid prevents a newly added child from running immediately");
    Wait(parent_mask);
    check(phase==1,"child executes and signals its parent");
    check(identity_ok && named_ok,"child sees its own task identity through Exec and FindTask");
    check(FindTask(NULL)==parent && SysBase->ThisTask==parent,"parent identity restored after task switch");
    check(FindTask((CONST_STRPTR)"kaffeine-task-worker")==&worker,"parent can find its waiting child");
    Signal(&worker,SIGF_SINGLE);
    Wait(parent_mask);
    check(phase==2 && received==SIGF_SINGLE,"Wait inside Forbid permits another task to signal the child");
    check(nesting_after_wait==0,"Wait restores the child's Forbid nesting after waking");
    check(!remove_returned,"RemTask(NULL) does not return to removed child code");
    check(FindTask((CONST_STRPTR)"kaffeine-task-worker")==NULL,"removed child disappears from task lookup");
    init_task(&returning_worker,return_stack,"kaffeine-task-return");
    AddTask(&returning_worker,(APTR)returning_entry,NULL);
    Wait(parent_mask);
    check(returned_body && FindTask(NULL)==parent,"a returning child leaves parent execution intact");
    check(FindTask((CONST_STRPTR)"kaffeine-task-return")==NULL,"normal child return removes that task");
    FreeSignal(sig);
    return test_done("exec_guest_tasks");
}

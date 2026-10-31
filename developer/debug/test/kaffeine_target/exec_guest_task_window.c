/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   An original child-task IDCMP test. No input is needed: Intuition sends
   ticks to the active window. The child waits inside Forbid(), while its
   parent waits for completion, then replies and closes its own window. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
static struct Task worker, *parent;
static ULONG stack[512], done_mask;
static volatile ULONG opened, owner_ok, tick, resumed, removed_returned;

static void child(void)
{
    struct Window *win;
    Forbid();
    win=OpenWindowTags(NULL,WA_Left,40,WA_Top,40,WA_Width,320,WA_Height,160,
                      WA_Title,(ULONG)"Guest task IDCMP",WA_Activate,TRUE,
                      WA_IDCMP,IDCMP_INTUITICKS,TAG_DONE);
    opened=win!=NULL;
    if(win && win->UserPort)
    {
        struct IntuiMessage *msg;
        owner_ok=win->UserPort->mp_SigTask==FindTask(NULL);
        while(!tick)
        {
            Wait(1UL<<win->UserPort->mp_SigBit);
            resumed++;
            while((msg=(struct IntuiMessage *)GetMsg(win->UserPort)))
            {
                if(msg->Class==IDCMP_INTUITICKS && msg->IDCMPWindow==win)
                    tick=1;
                ReplyMsg(&msg->ExecMessage);
            }
        }
    }
    Permit();
    if(win) CloseWindow(win);
    Signal(parent,done_mask);
    RemTask(NULL);
    removed_returned=1;
}

int main(void)
{
    LONG sig;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    if(!check(IntuitionBase!=NULL,"Intuition opens"))
        return test_done("exec_guest_task_window");
    sig=AllocSignal(-1);
    parent=FindTask(NULL);
    if(!check(sig>=0 && parent!=NULL,"parent can wait for child completion"))
    {
        if(sig>=0) FreeSignal(sig);
        CloseLibrary((struct Library *)IntuitionBase);
        return test_done("exec_guest_task_window");
    }
    done_mask=1UL<<sig;
    memset(&worker,0,sizeof(worker));
    worker.tc_Node.ln_Type=NT_TASK;
    worker.tc_Node.ln_Pri=5;
    worker.tc_Node.ln_Name=(STRPTR)"kaffeine-task-window";
    worker.tc_SPLower=stack;
    worker.tc_SPUpper=worker.tc_SPReg=stack+512;
    worker.tc_MemEntry.lh_Head=(struct Node *)&worker.tc_MemEntry.lh_Tail;
    worker.tc_MemEntry.lh_TailPred=(struct Node *)&worker.tc_MemEntry.lh_Head;
    worker.tc_MemEntry.lh_Type=NT_MEMORY;
    AddTask(&worker,(APTR)child,NULL);
    Wait(done_mask);
    check(opened,"child opens a native-backed window");
    check(owner_ok,"IDCMP port identifies the guest child as signal owner");
    check(tick && resumed,"native Intuition tick wakes a child waiting inside Forbid");
    check(FindTask(NULL)==parent,"parent resumes with its own task identity");
    check(!removed_returned && FindTask((CONST_STRPTR)"kaffeine-task-window")==NULL,
          "child closes its window and removes itself");
    FreeSignal(sig);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("exec_guest_task_window");
}

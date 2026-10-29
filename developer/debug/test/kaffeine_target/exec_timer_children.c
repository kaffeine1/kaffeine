/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original child-task timer protocol, also built for native AROS ARM. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/nodes.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"
static struct Task workers[3];
static ULONG stacks[3][4096] __attribute__((aligned(16)));
static struct Task *parent;
static ULONG parent_mask;
static volatile ULONG done,progress,answer[2];
static void timer_worker(void)
{
    unsigned i=FindTask(NULL)==&workers[0]?0:1;
    struct MsgPort *port=CreateMsgPort();
    struct timerequest req;
    ULONG ok=0;
    memset(&req,0,sizeof(req));
    if(port && !OpenDevice((CONST_STRPTR)TIMERNAME,i?UNIT_VBLANK:UNIT_MICROHZ,(struct IORequest*)&req,0))
    {
        ok|=1;
        req.tr_node.io_Message.mn_ReplyPort=port;
        req.tr_node.io_Command=TR_ADDREQUEST;
        req.tr_time.tv_secs=1;req.tr_time.tv_micro=i?500000:0;
        SendIO((struct IORequest*)&req);
        if(!WaitIO((struct IORequest*)&req)&&!GetMsg(port))ok|=2;
        req.tr_time.tv_secs=0;req.tr_time.tv_micro=i?100000:50000;
        SendIO((struct IORequest*)&req);
        if(WaitPort(port)==&req.tr_node.io_Message && GetMsg(port)==&req.tr_node.io_Message &&
           !WaitIO((struct IORequest*)&req) && !GetMsg(port))ok|=4;
        req.tr_time.tv_secs=2;req.tr_time.tv_micro=0;
        SendIO((struct IORequest*)&req);
        AbortIO((struct IORequest*)&req);
        if(WaitIO((struct IORequest*)&req)==IOERR_ABORTED && !GetMsg(port))ok|=8;
        if(req.tr_node.io_Message.mn_Length==0)ok|=16;
        CloseDevice((struct IORequest*)&req);
    }
    if(port)DeleteMsgPort(port);
    Forbid();answer[i]=ok;done|=1UL<<i;Signal(parent,parent_mask);Permit();
    RemTask(NULL);
}
static void pulse_worker(void)
{
    ULONG n;for(n=0;n<1000;n++)progress++;
    RemTask(NULL);
}
static void init_task(unsigned i,BYTE pri)
{
    struct Task *t=&workers[i];memset(t,0,sizeof(*t));
    t->tc_Node.ln_Type=NT_TASK;t->tc_Node.ln_Pri=pri;
    t->tc_Node.ln_Name=(STRPTR)(i==0?"kaffeine-timer-a":i==1?"kaffeine-timer-b":"kaffeine-timer-progress");
    t->tc_SPLower=stacks[i];t->tc_SPUpper=stacks[i]+4096;t->tc_SPReg=stacks[i]+4096;
    t->tc_MemEntry.lh_Head=(struct Node*)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_TailPred=(struct Node*)&t->tc_MemEntry.lh_Head;
    t->tc_MemEntry.lh_Type=NT_MEMORY;
}
int main(void)
{
    struct timerequest root;
    struct MsgPort *port=NULL;
    LONG sig=AllocSignal(-1);
    BOOL opened=FALSE;
    parent=FindTask(NULL);memset(&root,0,sizeof(root));
    if(!check(sig>=0&&parent!=NULL,"root has its identity and join signal"))goto out;
    parent_mask=1UL<<sig;
    port=CreateMsgPort();if(!check(port!=NULL,"root reply port"))goto out;
    if(!check(!OpenDevice((CONST_STRPTR)TIMERNAME,UNIT_MICROHZ,(struct IORequest*)&root,0),"root timer"))goto out;
    opened=TRUE;
    Forbid();
    init_task(0,1);init_task(1,1);init_task(2,0);
    AddTask(&workers[0],(APTR)timer_worker,NULL);
    AddTask(&workers[1],(APTR)timer_worker,NULL);
    AddTask(&workers[2],(APTR)pulse_worker,NULL);
    Permit();
    root.tr_node.io_Message.mn_ReplyPort=port;root.tr_node.io_Command=TR_ADDREQUEST;
    root.tr_time.tv_micro=500000;SendIO((struct IORequest*)&root);
    check(!WaitIO((struct IORequest*)&root),"root waits cooperatively while child timers are in flight");
    while((done&3)!=3)Wait(parent_mask);
    check(progress==1000,"another child executes while timers wait");
    check(answer[0]==31,"first child: WaitIO, queued reply, abort, preserved zero length");
    check(answer[1]==31,"second child: independent unit, queue, completion and abort");
    check(FindTask(NULL)==parent,"root identity survives child timer execution");
    check(!FindTask((CONST_STRPTR)"kaffeine-timer-a")&&!FindTask((CONST_STRPTR)"kaffeine-timer-b"),"both timer tasks are removed");
    check(!GetMsg(port),"root WaitIO leaves no queued completion");
out:
    if(opened)CloseDevice((struct IORequest*)&root);
    if(port)DeleteMsgPort(port);
    if(sig>=0)FreeSignal(sig);
    return test_done("exec_timer_children");
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original task-to-task message exchange through real classic Exec LVOs.
   WaitPort must yield to the other task even inside Forbid. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"

static struct Task worker, *parent;
static ULONG stack[512], ready_mask;
static struct MsgPort *volatile child_port;
static volatile ULONG owner_ok, peek_ok, get_ok, removed_returned;
static struct Packet { struct Message message; ULONG value; } packet;

static void child(void)
{
    struct MsgPort *port=CreateMsgPort();
    child_port=port;
    if(port)
    {
        struct Message *msg;
        owner_ok=port->mp_SigTask==FindTask(NULL);
        Forbid();
        Signal(parent,ready_mask);
        msg=WaitPort(port);
        peek_ok=msg==&packet.message && port->mp_MsgList.lh_Head==(struct Node *)msg;
        msg=GetMsg(port);
        get_ok=msg==&packet.message && GetMsg(port)==NULL;
        if(msg==&packet.message)
        {
            packet.value=42;
            ReplyMsg(msg);
        }
        Permit();
        DeleteMsgPort(port);
    }
    else Signal(parent,ready_mask);
    RemTask(NULL);
    removed_returned=1;
}

int main(void)
{
    struct MsgPort *reply=CreateMsgPort();
    LONG sig=AllocSignal(-1);
    struct Message *message;
    parent=FindTask(NULL);
    if(!check(reply && sig>=0 && parent,"parent creates reply port and readiness signal"))
    {
        if(reply) DeleteMsgPort(reply);
        if(sig>=0) FreeSignal(sig);
        return test_done("exec_guest_messages");
    }
    ready_mask=1UL<<sig;
    memset(&worker,0,sizeof(worker));
    worker.tc_Node.ln_Type=NT_TASK;
    worker.tc_Node.ln_Pri=5;
    worker.tc_Node.ln_Name=(STRPTR)"kaffeine-task-messages";
    worker.tc_SPLower=stack;
    worker.tc_SPUpper=worker.tc_SPReg=stack+512;
    worker.tc_MemEntry.lh_Head=(struct Node *)&worker.tc_MemEntry.lh_Tail;
    worker.tc_MemEntry.lh_TailPred=(struct Node *)&worker.tc_MemEntry.lh_Head;
    worker.tc_MemEntry.lh_Type=NT_MEMORY;
    packet.message.mn_ReplyPort=reply;
    packet.message.mn_Length=sizeof(packet);
    AddTask(&worker,(APTR)child,NULL);
    Wait(ready_mask);
    if(check(child_port!=NULL,"child creates its own signal port"))
    {
        PutMsg(child_port,&packet.message);
        message=WaitPort(reply);
        check(message==&packet.message,"reply wakes the parent waiting for its own port");
        check(GetMsg(reply)==message && GetMsg(reply)==NULL,"parent receives one reply");
        check(packet.value==42,"child processed the shared payload");
        check(owner_ok && peek_ok && get_ok,"child port ownership and WaitPort/GetMsg are correct");
        check(packet.message.mn_Node.ln_Type==NT_REPLYMSG,"ReplyMsg marks the message as a reply");
        check(!removed_returned && FindTask((CONST_STRPTR)"kaffeine-task-messages")==NULL,
              "child deletes its port and removes itself before parent continues");
    }
    DeleteMsgPort(reply);
    FreeSignal(sig);
    return test_done("exec_guest_messages");
}

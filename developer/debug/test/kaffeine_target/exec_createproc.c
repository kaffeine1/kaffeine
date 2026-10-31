/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original classic CreateProc service-process protocol using a caller-owned
   68k JMP seglist. No guest function is handed to native Exec. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"
struct JumpSegment { BPTR next; UWORD jump; ULONG entry; } __attribute__((packed,aligned(2)));
static struct JumpSegment segment;
static struct Packet { struct Message msg; ULONG value; } packet;
static struct Process *volatile child_process;
static volatile ULONG child_identity,child_stack,child_port,child_nesting,replies,use_exit,exit_returned;
static void child(void)
{
    struct Process *p=(struct Process*)FindTask(NULL);
    unsigned i;
    child_process=p;
    child_identity=p&&p->pr_Task.tc_Node.ln_Type==NT_PROCESS&&SysBase->ThisTask==&p->pr_Task;
    child_stack=p&&p->pr_StackSize==4096&&
        (ULONG)p->pr_Task.tc_SPUpper-(ULONG)p->pr_Task.tc_SPLower==4096&&
        (ULONG)p->pr_StackBase==((ULONG)p->pr_Task.tc_SPUpper>>2);
    child_port=p&&p->pr_MsgPort.mp_SigTask==&p->pr_Task&&p->pr_MsgPort.mp_SigBit==SIGB_DOS;
    p->pr_WindowPtr=(APTR)-1;
    for(i=0;i<2;i++)
    {
        struct Message *m;
        WaitPort(&p->pr_MsgPort);m=GetMsg(&p->pr_MsgPort);
        if(m==&packet.msg) { packet.value++;replies++; }
        if(i==1) { Forbid();child_nesting=(UBYTE)SysBase->TDNestCnt; }
        if(m)ReplyMsg(m);
    }
    if(use_exit) { Exit(7);exit_returned=1; }
}
int main(void)
{
    struct Task *parent=FindTask(NULL);
    struct MsgPort *reply=CreateMsgPort(),*port;
    struct Message *m;
    ULONG avail;
    unsigned i,pass;
    if(!check(reply&&parent,"parent creates a reply port"))goto done;
    segment.next=0;segment.jump=0x4EF9;segment.entry=(ULONG)child;
    memset(&packet,0,sizeof(packet));packet.msg.mn_ReplyPort=reply;packet.msg.mn_Length=sizeof(packet);
    avail=AvailMem(MEMF_PUBLIC);
    for(pass=0;pass<2;pass++)
    {
    child_process=NULL;replies=0;packet.value=0;use_exit=pass;
    Forbid();port=CreateProc((CONST_STRPTR)"kaffeine-proc-service",0,(BPTR)((ULONG)&segment>>2),4096);
    check(port!=NULL,"CreateProc returns a guest process port");
    check(!child_process,"Forbid holds the new child until Permit");
    Permit();
    if(!port)goto done;
    check(FindTask((CONST_STRPTR)"kaffeine-proc-service")==
          (struct Task*)((UBYTE*)port-92),"parent finds the service process by name");
    for(i=0;i<2;i++)
    {
        PutMsg(port,&packet.msg);m=WaitPort(reply);
        check(m==&packet.msg,"service reply wakes parent");
        check(GetMsg(reply)==&packet.msg&&GetMsg(reply)==NULL,"reply is delivered exactly once");
    }
    check(child_identity&&child_stack&&child_port,"child has Process identity, stack and virtual DOS port");
    check(replies==2&&packet.value==2,"service processes both shared messages");
    check(child_nesting==0&&(UBYTE)SysBase->TDNestCnt==0xFF,"final Forbid ends with the child");
    check(FindTask((CONST_STRPTR)"kaffeine-proc-service")==NULL&&FindTask(NULL)==parent,
          "normal child return preserves parent execution and identity");
    check(AvailMem(MEMF_PUBLIC)==avail,"child-owned guest memory is reclaimed");
    check(!exit_returned,"child Exit never returns into removed code");
    }
done:
    if(reply)DeleteMsgPort(reply);
    return test_done("exec_createproc");
}

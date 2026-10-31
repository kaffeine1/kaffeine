/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original asynchronous timer/copy protocol. Same valid caller code is
   compiled natively for ARM as an independent reference. */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"
static struct timerequest copies[3];
#if defined(__mc68000__) || defined(__m68k__)
static struct Task worker;
static ULONG worker_stack[1024];
static struct Task *parent;
static ULONG parent_mask;
static volatile ULONG ticks,worker_stop;
static BOOL worker_added;
static LONG worker_signal=-1;
static void worker_entry(void)
{
    Signal(parent,parent_mask);
    for(;;)
    {
        Wait(SIGF_SINGLE);
        if(worker_stop)
        { Forbid();Signal(parent,parent_mask);RemTask(NULL); }
        ticks++;
        Signal(parent,parent_mask);
    }
}
static BOOL start_worker(void)
{
    worker_signal=AllocSignal(-1);parent=FindTask(NULL);
    if(!check(worker_signal>=0&&parent!=NULL,"scheduler probe has its parent signal"))return FALSE;
    parent_mask=1UL<<worker_signal;
    memset(&worker,0,sizeof(worker));worker.tc_Node.ln_Type=NT_TASK;
    worker.tc_Node.ln_Name=(STRPTR)"kaffeine-async-worker";
    worker.tc_SPLower=worker_stack;worker.tc_SPUpper=worker_stack+1024;worker.tc_SPReg=worker_stack+1024;
    worker.tc_MemEntry.lh_Head=(struct Node*)&worker.tc_MemEntry.lh_Tail;
    worker.tc_MemEntry.lh_TailPred=(struct Node*)&worker.tc_MemEntry.lh_Head;
    worker.tc_MemEntry.lh_Type=NT_MEMORY;
    AddTask(&worker,(APTR)worker_entry,NULL);worker_added=TRUE;
    Wait(parent_mask);
    return check(FindTask((CONST_STRPTR)"kaffeine-async-worker")==&worker,
                 "guest worker is alive while the root uses timers");
}
static void stop_worker(void)
{
    if(worker_added)
    {
        worker_stop=TRUE;Signal(&worker,SIGF_SINGLE);Wait(parent_mask);
        check(FindTask((CONST_STRPTR)"kaffeine-async-worker")==NULL,"timer worker is removed before parent cleanup");
    }
    if(worker_signal>=0)FreeSignal(worker_signal);
}
#endif
static void delay_request(struct timerequest *r,ULONG secs,ULONG micro)
{
    r->tr_node.io_Command=TR_ADDREQUEST;
    r->tr_time.tv_secs=secs;r->tr_time.tv_micro=micro;
    SendIO((struct IORequest*)r);
}
static BOOL empty(struct MsgPort *p)
{ return p->mp_MsgList.lh_Head==(struct Node*)&p->mp_MsgList.lh_Tail; }
static BOOL now(struct timerequest *r,struct timeval *out)
{
    r->tr_node.io_Command=TR_GETSYSTIME;
    if(DoIO((struct IORequest*)r))return FALSE;
    *out=r->tr_time;return TRUE;
}
int main(int argc,char **argv)
{
    struct MsgPort *p=CreateMsgPort();
    struct timerequest *source=p?(struct timerequest*)CreateIORequest(p,sizeof(*source)):NULL;
    struct timeval start,submitted;
    BOOL opened=FALSE;
    ULONG seen=0,total=0,elapsed;
    unsigned i;
    if(!check(source!=NULL,"create source timer and reply port"))goto done;
    opened=OpenDevice((CONST_STRPTR)"timer.device",UNIT_MICROHZ,(struct IORequest*)source,0)==0;
    if(!check(opened,"open timer.device MICROHZ"))goto done;
#if defined(__mc68000__) || defined(__m68k__)
    if(argc>1&&!strcmp(argv[1],"tasks")&&!start_worker())goto done;
#endif
    for(i=0;i<3;i++)memcpy(&copies[i],source,sizeof(*source));
    if(!check(now(source,&start),"source gets system time"))goto done;
    delay_request(&copies[0],1,0);
    check(now(source,&submitted),"source synchronous request works while a copy is pending");
    elapsed=(submitted.tv_secs-start.tv_secs)*1000000+submitted.tv_micro-start.tv_micro;
    check(elapsed<750000,"SendIO returns before the one-second timer expires");
    check(CheckIO((struct IORequest*)&copies[0])==NULL,"copied request is genuinely pending");
#if defined(__mc68000__) || defined(__m68k__)
    if(worker_added)Signal(&worker,SIGF_SINGLE);
#endif
    check(WaitIO((struct IORequest*)&copies[0])==0,"WaitIO completes the copied request");
#if defined(__mc68000__) || defined(__m68k__)
    if(worker_added)
    {
        Wait(parent_mask);
        check(ticks==1,"another guest task ran while copied WaitIO blocked the root");
        delay_request(source,0,20000);Signal(&worker,SIGF_SINGLE);
        check(WaitIO((struct IORequest*)source)==0&&empty(p),"original request WaitIO resumes through the guest scheduler");
        Wait(parent_mask);
        check(ticks==2,"guest worker also runs during original-request WaitIO");
    }
#endif
    check(empty(p)&&copies[0].tr_node.io_Message.mn_Node.ln_Type==NT_REPLYMSG,
          "pending WaitIO consumes only its own reply");
    delay_request(&copies[0],2,0);
    AbortIO((struct IORequest*)&copies[0]);
    check(WaitIO((struct IORequest*)&copies[0])==IOERR_ABORTED&&empty(p),
          "cancelled WaitIO returns IOERR_ABORTED and drains its reply");
    delay_request(&copies[0],0,300000);
    delay_request(&copies[1],0,100000);
    delay_request(&copies[2],0,500000);
    while(total<3)
    {
        struct Message *m;
        WaitPort(p);
        while((m=GetMsg(p)))
        {
            for(i=0;i<3;i++)if(m==&copies[i].tr_node.io_Message)break;
            if(!check(i<3,"reply identifies one of the three copied requests"))goto done;
            check(!(seen&(1UL<<i)),"each copied request replies once");
            check(copies[i].tr_node.io_Error==0&&m->mn_Node.ln_Type==NT_REPLYMSG,
                  "asynchronous reply carries native completion status");
            seen|=1UL<<i;total++;
        }
    }
    check(seen==7&&empty(p),"shared reply queue delivers all copied requests");
    for(i=0;i<3;i++)check(WaitIO((struct IORequest*)&copies[i])==0,
                          "WaitIO accepts a reply already removed with GetMsg");
    {
        struct timerequest *heapcopy=(struct timerequest*)AllocMem(sizeof(*heapcopy),MEMF_PUBLIC);
        if(check(heapcopy!=NULL,"allocate a temporary copied request"))
        {
            memcpy(heapcopy,source,sizeof(*source));delay_request(heapcopy,0,20000);
            check(WaitIO((struct IORequest*)heapcopy)==0&&empty(p),"temporary copy finishes before its storage is freed");
            FreeMem(heapcopy,sizeof(*heapcopy));
        }
    }
    check(now(source,&submitted),"source remains usable after copy completion and release");
done:
#if defined(__mc68000__) || defined(__m68k__)
    stop_worker();
#endif
    for(i=0;i<3;i++)
        if(copies[i].tr_node.io_Device)
        {
            if(!CheckIO((struct IORequest*)&copies[i]))AbortIO((struct IORequest*)&copies[i]);
            WaitIO((struct IORequest*)&copies[i]);
        }
    if(opened)CloseDevice((struct IORequest*)source);
    if(source)DeleteIORequest((struct IORequest*)source);
    if(p)DeleteMsgPort(p);
    return test_done("exec_timer_async");
}

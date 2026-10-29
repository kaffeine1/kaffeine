/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Several native I/O opens must not consume one hidden signal per open. */
#include <devices/timer.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"
struct CheckedTimer { UBYTE before[8]; struct timerequest request; UBYTE after[8]; };
static struct CheckedTimer copied;
int main(void)
{
    struct MsgPort *reply=NULL;
    struct timerequest *requests[3]={NULL,NULL,NULL};
    BYTE signals[14];
    unsigned count=0,opened=0,i;
    BOOL safe=TRUE;
    memset(signals,-1,sizeof(signals));
    reply=CreateMsgPort();
    if(!check(reply!=NULL,"create one guest-visible reply port"))goto done;
    /* Together with the port, reserve thirteen ordinary signal bits. */
    for(i=0;i<12;i++)
    {
        signals[count]=AllocSignal(-1);
        if(signals[count]<0)break;
        count++;
    }
    if(!check(count==12,"reserve the ordinary application signal budget"))goto done;
    for(i=0;i<3;i++)
    {
        requests[i]=(struct timerequest*)CreateIORequest(reply,sizeof(*requests[i]));
        if(!requests[i]||OpenDevice(TIMERNAME,UNIT_MICROHZ,&requests[i]->tr_node,0))break;
        opened++;
    }
    if(!check(opened==3,"open three original timer requests within the signal budget"))goto done;
    signals[count]=AllocSignal(-1);
    if(!check(signals[count]>=0,"I/O opens leave another application signal available"))goto done;
    count++;
    memset(&copied,0xA5,sizeof(copied));copied.request=*requests[0];
    copied.request.tr_node.io_Command=TR_ADDREQUEST;
    copied.request.tr_time.tv_secs=0;copied.request.tr_time.tv_micro=30000;
    for(i=0;i<2;i++)
    {
        requests[i]->tr_node.io_Command=TR_ADDREQUEST;
        requests[i]->tr_time.tv_secs=0;requests[i]->tr_time.tv_micro=1000+i*1000;
        SendIO(&requests[i]->tr_node);
    }
    SendIO(&copied.request.tr_node);
    check(!WaitIO(&copied.request.tr_node),"a copied timer shares completion resources with the originals");
    check(!WaitIO(&requests[0]->tr_node)&&!WaitIO(&requests[1]->tr_node),
          "waiting for the copy preserves both original completions");
    signals[count]=AllocSignal(-1);
    if(check(signals[count]>=0,"a live copied timer leaves the final application signal available"))count++;
    for(i=0;i<8;i++)if(copied.before[i]!=0xA5||copied.after[i]!=0xA5)safe=FALSE;
    check(safe,"shared completions preserve the copied request guards");
done:
    for(i=0;i<opened;i++)CloseDevice(&requests[i]->tr_node);
    for(i=0;i<3;i++)if(requests[i])DeleteIORequest(&requests[i]->tr_node);
    while(count)FreeSignal(signals[--count]);
    if(reply)DeleteMsgPort(reply);
    return test_done("exec_io_signal_budget");
}

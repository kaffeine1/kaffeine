/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original timer protocol using stack storage without CreateIORequest. */
#include <exec/types.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"
int main(void)
{
    struct MsgPort *port=NULL;
    struct timerequest req;
    struct Message *reply;
    LONG error;
    BOOL opened=FALSE;
    memset(&req,0,sizeof(req));
    port=CreateMsgPort();
    if(!check(port!=NULL,"create a reply port"))goto done;
    error=OpenDevice((CONST_STRPTR)TIMERNAME,UNIT_VBLANK,(struct IORequest*)&req,0);
    if(!check(error==0,"open a timer in cleared stack storage"))goto done;
    opened=TRUE;
    Printf("stack timer mn_Length after OpenDevice: %ld\n",(LONG)req.tr_node.io_Message.mn_Length);
    req.tr_node.io_Message.mn_ReplyPort=port;
    req.tr_node.io_Command=TR_ADDREQUEST;
    req.tr_time.tv_secs=0;req.tr_time.tv_micro=50000;
    SendIO((struct IORequest*)&req);
    reply=WaitPort(port);
    check(reply==&req.tr_node.io_Message,"stack timer reaches its port");
    check(GetMsg(port)==&req.tr_node.io_Message,"take the exact stack request");
    check(WaitIO((struct IORequest*)&req)==0,"WaitIO accepts the completed stack request");
    check(req.tr_node.io_Message.mn_Length==0,"unused message length remains unchanged");
    check(GetMsg(port)==NULL,"completion is delivered once");
done:
    if(opened)CloseDevice((struct IORequest*)&req);
    if(port)DeleteMsgPort(port);
    return test_done("exec_timer_stack");
}

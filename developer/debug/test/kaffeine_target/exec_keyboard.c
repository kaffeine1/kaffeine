/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original keyboard matrix probe; the same valid operations run natively
   on ARM as a reference. Input comes from the runner's USB keyboard. */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/keyboard.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
static UBYTE guarded[80];
static BOOL guards(unsigned length)
{
    unsigned i;
    for(i=0;i<8;i++)if(guarded[i]!=0xA5)return FALSE;
    for(i=8+length;i<sizeof(guarded);i++)if(guarded[i]!=0xA5)return FALSE;
    return TRUE;
}
static LONG matrix(struct IOStdReq *r,ULONG length)
{
    r->io_Command=KBD_READMATRIX;r->io_Length=length;r->io_Data=guarded+8;
    return DoIO((struct IORequest*)r);
}
static BOOL marker(void)
{
    char program[128],path[256],*base,*p;
    BPTR f;BOOL second;
    if(!GetProgramName((STRPTR)program,sizeof(program)))return FALSE;
    base=program;
    for(p=program;*p;p++)if(*p==':'||*p=='/')base=p+1;
    strcpy(path,"SYS:EMU68/");strcat(path,base);strcat(path,"-shell-rc.txt");
    f=Open((CONST_STRPTR)path,MODE_OLDFILE);second=f!=0;if(f)Close(f);
    strcpy(path,"SYS:EMU68/");strcat(path,base);strcat(path,second?"-keys-2.txt":"-keys-1.txt");
    f=Open((CONST_STRPTR)path,MODE_NEWFILE);if(!f)return FALSE;
    Write(f,(APTR)"ready\n",6);Close(f);return TRUE;
}
int main(int argc,char **argv)
{
    struct MsgPort *port=CreateMsgPort();
    struct IOStdReq *r=port?(struct IOStdReq*)CreateIORequest(port,sizeof(*r)):NULL;
    struct IOStdReq *input=port?(struct IOStdReq*)CreateIORequest(port,sizeof(*input)):NULL;
    struct Window *w=NULL;
    BOOL opened=FALSE,input_open=FALSE,pressed=FALSE,released=FALSE,raw_down=FALSE,raw_up=FALSE;
    unsigned i;

    if(!check(r&&input,"create device requests and reply port"))goto done;
    opened=OpenDevice((CONST_STRPTR)"keyboard.device",0,(struct IORequest*)r,0)==0;
    if(!check(opened,"open keyboard.device unit zero"))goto done;
    input_open=OpenDevice((CONST_STRPTR)"input.device",0,(struct IORequest*)input,0)==0;
    check(input_open,"open input.device unit zero");
    memset(guarded,0xA5,sizeof(guarded));
    check(matrix(r,13)==0&&r->io_Actual==13,"13-byte matrix read succeeds");
    check(guards(13)&&r->io_Data==guarded+8&&r->io_Length==13,"matrix preserves guards and request inputs");
    memset(guarded,0xA5,sizeof(guarded));
    check(matrix(r,0)==0&&r->io_Actual==0&&guards(0),"empty matrix read writes no bytes");
    memset(guarded,0xA5,sizeof(guarded));
    check(matrix(r,64)==0&&r->io_Actual==32&&guards(32),"long read is capped at the native 32-byte matrix");
    IntuitionBase=(struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",33);
    if(IntuitionBase)w=OpenWindowTags(NULL,WA_Width,240,WA_Height,100,WA_Activate,TRUE,WA_IDCMP,IDCMP_RAWKEY,TAG_DONE);
    if(!check(w!=NULL,"active window receives test keyboard input"))goto done;
    Delay(5);
    if(!check(marker(),"request USB keyboard input"))goto done;
    for(i=0;i<300&&(!raw_up||(pressed&&!released));i++)
    {
        struct IntuiMessage *msg;
        if(matrix(r,32)!=0)break;
        while((msg=(struct IntuiMessage*)GetMsg(w->UserPort))) { if(msg->Code==0x20)raw_down=TRUE;if(msg->Code==0xA0)raw_up=TRUE;ReplyMsg((struct Message*)msg); }
        if(guarded[8+4]&1)pressed=TRUE;
        else if(pressed)released=TRUE;
        Delay(1);
    }
    check(raw_down&&raw_up,"real A key press and release reaches IDCMP");
    PutStr((CONST_STRPTR)(pressed&&released?"matrix key tracking: yes\n":"matrix key tracking: no\n"));
    /* The current ARM USB image delivers RAWKEY events while its native
       matrix stays zero. Keep that platform capability visible separately. */
    if(argc>1&&!strcmp(argv[1],"matrix"))
        check(pressed&&released,"matrix tracks the real A key press and release");
    check(guards(32),"key polling preserves trailing guards");
done:
    if(w)CloseWindow(w);
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(input_open)CloseDevice((struct IORequest*)input);
    if(input)DeleteIORequest((struct IORequest*)input);
    if(opened)CloseDevice((struct IORequest*)r);
    if(r)DeleteIORequest((struct IORequest*)r);
    if(port)DeleteMsgPort(port);
    return test_done("exec_keyboard");
}

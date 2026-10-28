/* Original DOS packet protocol test; the same source builds for native AROS and classic 68k. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static unsigned checks,failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
static void transfer(BPTR file,struct MsgPort *reply,struct StandardPacket *sp,
                     LONG action,char *buffer,struct MsgPort *unused,LONG want)
{
    struct FileHandle *fh=(struct FileHandle *)BADDR(file);
    struct Message *message;
    sp->sp_Msg.mn_Node.ln_Name=(char *)&sp->sp_Pkt;
    sp->sp_Msg.mn_ReplyPort=unused;
    sp->sp_Msg.mn_Length=sizeof(*sp);
    sp->sp_Pkt.dp_Link=&sp->sp_Msg;
    sp->sp_Pkt.dp_Port=reply;
    sp->sp_Pkt.dp_Type=action;
    sp->sp_Pkt.dp_Arg1=fh->fh_Args;
    sp->sp_Pkt.dp_Arg2=(intptr_t)(uintptr_t)buffer;
    sp->sp_Pkt.dp_Arg3=1;
    PutMsg((struct MsgPort *)fh->fh_Type,&sp->sp_Msg);
    WaitPort(reply);
    message=GetMsg(reply);
    CHECK(message==&sp->sp_Msg);
    CHECK(sp->sp_Pkt.dp_Res1==want);
    CHECK(sp->sp_Pkt.dp_Res2==0);
    CHECK(sp->sp_Msg.mn_ReplyPort==unused);
    CHECK(GetMsg(reply)==NULL);
}
int main(void)
{
    const char *path="T:kaffeine-packet-reply.test";
    struct MsgPort *port=CreateMsgPort();
    struct StandardPacket *sp=AllocVec(sizeof(*sp),MEMF_PUBLIC|MEMF_CLEAR);
    BPTR file=0;
    unsigned i;
    char byte=0;
    CHECK(port!=NULL); CHECK(sp!=NULL);
    if(!port||!sp) goto done;
    file=Open((CONST_STRPTR)path,MODE_NEWFILE); CHECK(file!=0);
    if(!file) goto done;
    for(i=0;i<3;i++) {
        struct MsgPort *unused=i==0?NULL:i==1?port:(struct MsgPort *)(uintptr_t)0x12345678;
        byte=(char)('A'+i);
        transfer(file,port,sp,ACTION_WRITE,&byte,unused,1);
    }
    CHECK(Close(file)); file=0;
    file=Open((CONST_STRPTR)path,MODE_OLDFILE); CHECK(file!=0);
    if(!file) goto done;
    for(i=0;i<3;i++) {
        struct MsgPort *unused=i==0?NULL:i==1?port:(struct MsgPort *)(uintptr_t)0x12345678;
        byte=0;
        transfer(file,port,sp,ACTION_READ,&byte,unused,1);
        CHECK(byte==(char)('A'+i));
    }
    CHECK(Close(file)); file=0;
    CHECK(DeleteFile((CONST_STRPTR)path));
done:
    if(file) Close(file);
    if(sp) FreeVec(sp);
    if(port) DeleteMsgPort(port);
    printf("file_packet_reply: %u checks, %u failed\n",checks,failed);
    return failed?10:0;
}

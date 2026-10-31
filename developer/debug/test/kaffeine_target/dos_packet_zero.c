/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original cleared StandardPackets sent directly to filesystem handlers. */
#include <dos/dosextens.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <string.h>
#include "m68ktest.h"
#ifndef MKBADDR
#define MKBADDR(p) ((BPTR)((ULONG)(p)>>2))
#endif
#ifdef __AROS__
typedef SIPTR PacketArg;
#else
typedef LONG PacketArg;
#endif
#define NAME "kaffeine-packet-zero"
struct CheckedPacket { UBYTE before[8]; struct StandardPacket packet; UBYTE after[8]; };
static struct CheckedPacket checked;
static struct InfoData info __attribute__((aligned(4)));
static struct FileInfoBlock fib __attribute__((aligned(4)));
static struct DateStamp stamp;
static UBYTE filename[64] __attribute__((aligned(4)));

static BOOL guards(void)
{
    unsigned i;
    for(i=0;i<8;i++)if(checked.before[i]!=0xA5||checked.after[i]!=0xA5)return FALSE;
    return TRUE;
}
static LONG send(struct MsgPort *handler,struct MsgPort *reply,LONG action,
                 PacketArg a,PacketArg b,PacketArg c,PacketArg d)
{
    struct StandardPacket *p=&checked.packet;
    struct Message *received;
    memset(&checked,0xA5,sizeof(checked));memset(p,0,sizeof(*p));
    p->sp_Msg.mn_Node.ln_Name=(char*)&p->sp_Pkt;
    p->sp_Pkt.dp_Link=&p->sp_Msg;p->sp_Pkt.dp_Port=reply;p->sp_Pkt.dp_Type=action;
    p->sp_Pkt.dp_Arg1=a;p->sp_Pkt.dp_Arg2=b;p->sp_Pkt.dp_Arg3=c;p->sp_Pkt.dp_Arg4=d;
    PutMsg(handler,&p->sp_Msg);
    received=WaitPort(reply);
    check(received==&p->sp_Msg&&GetMsg(reply)==&p->sp_Msg,"direct PutMsg returns the original packet through its reply queue");
    check(p->sp_Pkt.dp_Link==&p->sp_Msg&&p->sp_Msg.mn_Node.ln_Name==(char*)&p->sp_Pkt,
          "packet link and node-name identity survive the reply");
    check((p->sp_Msg.mn_Length==0||p->sp_Msg.mn_Length==sizeof(*p))&&guards(),
          "reply keeps a valid packet extent and surrounding guards");
    return p->sp_Pkt.dp_Res1;
}
int main(void)
{
    struct MsgPort *handler,*reply=NULL;
    BPTR lock=0,fh;
    BSTR name;
    handler=DeviceProc("RAM:");reply=CreateMsgPort();lock=Lock("RAM:",SHARED_LOCK);
    if(!check(handler&&reply&&lock,"filesystem handler, reply port and directory lock"))goto done;
    check(send(handler,reply,ACTION_IS_FILESYSTEM,0,0,0,0)!=0,"filesystem query accepts a cleared packet with length zero");
    memset(&info,0,sizeof(info));
    check(send(handler,reply,ACTION_DISK_INFO,(PacketArg)MKBADDR(&info),0,0,0)&&info.id_BytesPerBlock>0,
          "disk-info packet writes the native result through the checked output");
    fh=Open("RAM:" NAME,MODE_NEWFILE);
    if(check(fh!=0,"create the metadata fixture"))Close(fh);
#ifdef AROS_FAST_BSTR
    strcpy((char*)filename,NAME);
#else
    filename[0]=sizeof(NAME)-1;memcpy(filename+1,NAME,sizeof(NAME));
#endif
    name=(BSTR)MKBADDR(filename);DateStamp(&stamp);
    check(send(handler,reply,ACTION_SET_DATE,0,(PacketArg)lock,(PacketArg)name,(PacketArg)&stamp)!=0,
          "post-copy date packet accepts length zero");
    fh=Lock("RAM:" NAME,SHARED_LOCK);
    check(fh&&Examine(fh,&fib)&&fib.fib_Date.ds_Days==stamp.ds_Days&&
          fib.fib_Date.ds_Minute==stamp.ds_Minute&&fib.fib_Date.ds_Tick==stamp.ds_Tick,
          "the date operation really changes the file metadata");
    if(fh)UnLock(fh);
done:
    if(lock)UnLock(lock);
    if(reply)DeleteMsgPort(reply);
    DeleteFile("RAM:" NAME);
    return test_done("dos_packet_zero");
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original-daemon nonempty volume-list acceptance with independent SYS identity.
   Loopback only; no input, SETUP, external networking or physical target. */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/keymap.h>
#include <devices/inputevent.h>
#include <string.h>
#include "socket_fixture_abi.h"
#include "m68ktest.h"
struct Library *SocketBase;
static const char daemon_name[]="amigent-viewer-fixture";
static const char key[]="public-loopback-fixture-key";
static UWORD u16(const UBYTE *p){return (UWORD)p[0]<<8|p[1];}
static ULONG u32(const UBYTE *p){return (ULONG)p[0]<<24|(ULONG)p[1]<<16|(ULONG)p[2]<<8|p[3];}
static void put16(UBYTE *p,UWORD x){p[0]=x>>8;p[1]=x;}
static BOOL receive(LONG fd,void *data,LONG n)
{UBYTE *p=data;while(n){LONG k=recv(fd,p,n,0);if(k<=0)return FALSE;p+=k;n-=k;}return TRUE;}
static BOOL transmit(LONG fd,const void *data,LONG n)
{const UBYTE *p=data;while(n){LONG k=send(fd,p,n,0);if(k<=0)return FALSE;p+=k;n-=k;}return TRUE;}
static BOOL request(LONG fd,UBYTE cmd,UBYTE arg,UWORD x,UWORD y,UWORD w,UWORD h)
{UBYTE b[10];b[0]=cmd;b[1]=arg;put16(b+2,x);put16(b+4,y);put16(b+6,w);put16(b+8,h);return transmit(fd,b,10);}
static LONG connection(void)
{
    struct KSocketIn a;struct KSocketTime tv={5,0};LONG fd,i;UBYTE hello[8],s;
    memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(44125);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    for(i=0;i<40;++i){fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        if(!connect(fd,(struct KSocketAddress *)&a,sizeof(a)))break;CloseSocket(fd);Delay(2);
    }
    if(i==40)return -1;
    if(receive(fd,hello,8)&&!memcmp(hello,"AMGT",4)&&u16(hello+4)==3&&(hello[7]&1)&&
       request(fd,17,0,sizeof(key)-1,0,0,0)&&transmit(fd,key,sizeof(key)-1)&&receive(fd,&s,1)&&!s)return fd;
    CloseSocket(fd);return -1;
}
static BOOL volumes_reply(LONG fd,const char *root)
{
    UBYTE h[5];static char text[16385];ULONG n;char *at;BOOL found=FALSE;
    if(!request(fd,31,0,0,0,0,0)||!receive(fd,h,5)||h[0]||(n=u32(h+1))>=sizeof(text)||!receive(fd,text,n))return FALSE;
    text[n]=0;Printf("VOLUMES: %ld bytes\n%s",(LONG)n,(LONG)text);Flush(Output());
    for(at=text;*at;){if(!strncmp(at,root,strlen(root))&&at[strlen(root)]=='\t')found=TRUE;
        at=strchr(at,'\n');if(!at)break;++at;}
    return n>0&&found;
}
int main(void)
{
    struct Process *daemon=NULL;BPTR segment=0,sys=0;LONG fd=-1,i,old=SetTaskPri(FindTask(NULL),-5);
    UBYTE status;char path[512],*colon;
    SocketBase=OpenLibrary("bsdsocket.library",4);
    if(!check(SocketBase!=NULL,"open loopback socket library"))goto end;
    sys=Lock("SYS:",SHARED_LOCK);
    if(!check(sys&&NameFromLock(sys,path,sizeof(path)),"independent native SYS lock resolves real volume name"))goto end;
    UnLock(sys);sys=0;colon=strchr(path,':');
    if(!check(colon!=NULL,"SYS native name has volume separator"))goto end;
    colon[1]=0;
    segment=LoadSeg("SYS:EMU68/amigentd");
    if(!check(segment!=0,"load original released daemon"))goto end;
    daemon=CreateNewProcTags(NP_Seglist,segment,NP_FreeSeglist,TRUE,NP_Name,(ULONG)daemon_name,
      NP_CommandName,(ULONG)"amigentd",NP_Arguments,(ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
      NP_Cli,TRUE,NP_StackSize,65536,NP_CopyVars,FALSE,TAG_DONE);
    if(!check(daemon!=NULL,"start unmodified daemon for file browser metadata"))goto end;
    segment=0;fd=connection();if(!check(fd>=0,"authenticate command connection"))goto end;
    if(!check(request(fd,20,0,0,0,0,0)&&receive(fd,&status,1)&&!status,"enter original command channel"))goto end;
    for(i=0;i<3;++i)
        if(!check(volumes_reply(fd,path),"original VOLUMES includes mounted SYS volume with Info fields"))goto end;
    check(request(fd,6,0,0,0,0,0)&&receive(fd,&status,1)&&!status,"daemon remains responsive after repeated complete volume lists");
end:
    if(sys)UnLock(sys);
    if(fd>=0)CloseSocket(fd);
    if(daemon){struct Task *live=FindTask((CONST_STRPTR)daemon_name);if(live)Signal(live,SIGBREAKF_CTRL_C);
        for(i=0;i<100&&FindTask((CONST_STRPTR)daemon_name);++i)Delay(1);
        check(!FindTask((CONST_STRPTR)daemon_name),"daemon and reader stop normally");}
    if(segment&&!FindTask((CONST_STRPTR)daemon_name))UnLoadSeg(segment);
    if(SocketBase)CloseLibrary(SocketBase);SetTaskPri(FindTask(NULL),old);
    return test_done("net_amigent_volumes");
}

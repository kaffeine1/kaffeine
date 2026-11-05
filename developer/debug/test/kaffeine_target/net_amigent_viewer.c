/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Finite original-viewer request pattern regression, native Workbench screen.
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
static BOOL reply_frame(LONG fd)
{
    UBYTE h[13],data[16384];ULONG count,i,n;UBYTE kind;
    if(!receive(fd,&kind,1))return FALSE;
    if(kind!=2)return FALSE;
    if(!receive(fd,h,11))return FALSE;
    count=u32(h+7);if(count>1024)return FALSE;
    for(i=0;i<count;++i){
        if(!receive(fd,h,13)||(n=u32(h+9))>sizeof(data))return FALSE;
        if(!receive(fd,data,n))return FALSE;
    }
    return TRUE;
}
static BOOL text_reply(LONG fd,UBYTE cmd,const char *field)
{
    UBYTE h[5];static char text[16385];ULONG n;
    if(!request(fd,cmd,0,0,0,0,0)||!receive(fd,h,5)||h[0]||(n=u32(h+1))>=sizeof(text)||!receive(fd,text,n))return FALSE;
    text[n]=0;Printf("metadata command %ld returned %ld bytes\n",(LONG)cmd,(LONG)n);Flush(Output());
    return !field || (n>0&&strstr(text,field)!=NULL);
}
int main(void)
{
    struct Process *daemon=NULL;BPTR segment=0;LONG fd=-1,channel=-1,i,batch,old=SetTaskPri(FindTask(NULL),-5);
    UBYTE s,h[13];UWORD width,height,cols,rows,total;
    SocketBase=OpenLibrary("bsdsocket.library",4);
    if(!check(SocketBase!=NULL,"open loopback socket library"))goto end;
    segment=LoadSeg((CONST_STRPTR)"SYS:EMU68/amigentd");
    if(!check(segment!=0,"load original released daemon"))goto end;
    daemon=CreateNewProcTags(NP_Seglist,segment,NP_FreeSeglist,TRUE,NP_Name,(ULONG)daemon_name,
      NP_CommandName,(ULONG)"amigentd",NP_Arguments,(ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
      NP_Cli,TRUE,NP_StackSize,65536,NP_CopyVars,FALSE,TAG_DONE);
    if(!check(daemon!=NULL,"start independent unmodified daemon"))goto end;
    segment=0;fd=connection();if(!check(fd>=0,"authenticate screen connection"))goto end;
    if(!check(request(fd,18,0,0x0123,0x4567,0,0)&&receive(fd,&s,1),"original CLIENT ownership exchange"))goto end;
    /* Exactly the original protocol-v3 probe: FRAME planar offscreen and INFO. */
    if(!check(request(fd,2,2,0x7fff,0x7fff,1,1)&&request(fd,1,0,0,0,0,0)&&receive(fd,h,13),
      "pipelined planar capability probe and INFO"))goto end;
    if(!check(h[0]==0&&u32(h+1)==0,"native RTG returns classic empty fallback format"))goto end;
    width=u16(h+5);height=u16(h+7);cols=(width+127)/128;rows=(height+63)/64;total=cols*rows;
    if(!check(width>0&&height>0&&width<=1920&&height<=1080&&total<=300,"bounded native screen geometry"))goto end;
    if(!check(request(fd,6,0,0,0,0,0)&&receive(fd,&s,1)&&!s,"PING before feed"))goto end;
    for(batch=0;batch<total;batch+=2){
        UWORD j,send_count=total-batch<2?total-batch:2;
        Printf("viewer batch %ld of %ld\n",batch,(LONG)total);Flush(Output());
        if(batch==0 && !request(fd,1,0,0,0,0,0))break;
        for(j=0;j<send_count;++j){
            UWORD t=batch+j,x=(t%cols)*128,y=(t/cols)*64;
            UWORD w=width-x<128?width-x:128,hgt=height-y<64?height-y:64;
            if(!request(fd,2,7,x,y,w,hgt))break;
        }
        if(j!=send_count)break;
        if(batch==0 && !receive(fd,h,8))break;
        for(j=0;j<send_count;++j)if(!reply_frame(fd))break;
        if(j!=send_count)break;
    }
    check(batch>=total,"complete first small-region viewer sweep with two frames in flight");
    check(request(fd,6,0,0,0,0,0)&&receive(fd,&s,1)&&!s,"daemon remains responsive after full viewer sweep");
    channel=connection();if(!check(channel>=0,"authenticate viewer metadata command connection"))goto end;
    if(!check(request(channel,20,0,0,0,0,0)&&receive(channel,&s,1)&&!s,"open viewer metadata command channel"))goto end;
    check(text_reply(channel,30,"chip_free="),"original viewer SYSINFO returns real memory and OS metadata");
    check(text_reply(channel,31,NULL),"original viewer VOLUMES completes without terminating the daemon");
    check(request(fd,6,0,0,0,0,0)&&receive(fd,&s,1)&&!s,"screen remains responsive after viewer metadata");

end:
    if(channel>=0)CloseSocket(channel);
    if(fd>=0)CloseSocket(fd);
    if(daemon){struct Task *live=FindTask((CONST_STRPTR)daemon_name);if(live)Signal(live,SIGBREAKF_CTRL_C);
        for(i=0;i<100&&FindTask((CONST_STRPTR)daemon_name);++i)Delay(1);
        check(!FindTask((CONST_STRPTR)daemon_name),"daemon and reader stop normally");}
    if(segment&&!FindTask((CONST_STRPTR)daemon_name))UnLoadSeg(segment);
    if(SocketBase)CloseLibrary(SocketBase);SetTaskPri(FindTask(NULL),old);
    return test_done("net_amigent_viewer");
}

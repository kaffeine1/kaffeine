/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Independent protocol acceptance for original Amigent: owned GUI, tile,
   command and file operations. Loopback only, no SETUP or physical target. */
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
static const char daemon_name[]="amigent-native-fixture";
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
static BOOL status(LONG fd)
{
    UBYTE s,b[2];char error[256];UWORD n;
    if(!receive(fd,&s,1))return FALSE;
    if(!s)return TRUE;
    if(receive(fd,b,2) && (n=u16(b))<sizeof(error) && receive(fd,error,n))
    {error[n]=0;Printf("command status %ld: %s\n",(LONG)s,(ULONG)error);}
    return FALSE;
}
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
static BOOL execute(LONG fd)
{
    const char command[]="Echo AMIGENT_EXEC_OK";UBYTE b[8];char out[512];ULONG n;
    if(!request(fd,21,0,sizeof(command)-1,0,5,0)||!transmit(fd,command,sizeof(command)-1)||!status(fd)||
       !receive(fd,b,8)||(n=u32(b+4))>=sizeof(out)||!receive(fd,out,n))return FALSE;
    out[n]=0;return u32(b)==0 && strstr(out,"AMIGENT_EXEC_OK")!=NULL;
}


static BOOL okay(LONG fd,UBYTE cmd,UBYTE arg,UWORD x,UWORD y)
{UBYTE s=255;return request(fd,cmd,arg,x,y,0,0)&&receive(fd,&s,1)&&s==0;}
static BOOL read_proof(const char *path,char *out,ULONG size)
{
    BPTR f=Open((CONST_STRPTR)path,MODE_OLDFILE);LONG n;if(!f)return FALSE;
    n=Read(f,out,size-1);Close(f);if(n<0)return FALSE;out[n]=0;return TRUE;
}
static LONG number(const char *text,const char *field)
{const char *p=strstr(text,field);LONG n=0;if(!p)return -1;p+=strlen(field);while(*p>='0'&&*p<='9')n=n*10+*p++-'0';return n;}
static BOOL marker(LONG fd,UWORD x,UWORD y)
{
    UBYTE header[4],pixels[32*32*3];ULONG i;
    if(!request(fd,13,0,x,y,32,32)||!receive(fd,header,4)||u16(header)!=32||u16(header+2)!=32||!receive(fd,pixels,sizeof(pixels)))return FALSE;
    for(i=0;i<sizeof(pixels);i+=3)if(pixels[i]!=42||pixels[i+1]!=99||pixels[i+2]!=177)return FALSE;
    return TRUE;
}
int main(void)
{

    struct Process *daemon=NULL;BPTR segment=0;LONG fd=-1,channel=-1,i,left,top;
    LONG old_priority=SetTaskPri(FindTask(NULL),-5);BOOL witness=FALSE;
    static const char nonce[]="0123456789abcdef0123456789abcdef";
    char proof[1024];UBYTE status_byte;
    SocketBase=OpenLibrary("bsdsocket.library",4);
    if(!check(SocketBase!=NULL,"open witness protocol socket library"))goto end;
    if(!check(SystemTags((CONST_STRPTR)"Run >NIL: <NIL: SYS:EMU68/AmigentInputWitness 0123456789abcdef0123456789abcdef",TAG_DONE)==0,
      "start finite native input witness without a guest window"))goto end;
    witness=TRUE;
    for(i=0;i<100;++i){if(read_proof("RAM:KaffeineAmigentWitness.ready",proof,sizeof(proof))&&strstr(proof,nonce))break;Delay(1);}
    if(!check(i<100&&strstr(proof,"\"active\":true")&&strstr(proof,"Kaffeine Amigent Input Check"),
      "fresh closed native witness proves focus and own-window geometry"))goto end;
    left=number(proof,"\"left\":");top=number(proof,"\"top\":");
    if(!check(left>=0&&top>=0,"parse owned native window position"))goto end;
    segment=LoadSeg((CONST_STRPTR)"SYS:EMU68/amigentd");
    if(!check(segment!=0,"load original released daemon"))goto end;
    daemon=CreateNewProcTags(NP_Seglist,segment,NP_FreeSeglist,TRUE,NP_Name,(ULONG)daemon_name,
      NP_CommandName,(ULONG)"amigentd",NP_Arguments,(ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
      NP_Cli,TRUE,NP_StackSize,65536,NP_CopyVars,FALSE,TAG_DONE);
    if(!check(daemon!=NULL,"start independent original daemon"))goto end;
    segment=0;
    fd=connection();if(!check(fd>=0,"authenticate screen connection"))goto end;

    check(marker(fd,number(proof,"\"marker_x\":"),number(proof,"\"marker_y\":")),
      "Amigent RGB capture matches every native marker pixel on 24bit Workbench");
    channel=connection();if(!check(channel>=0,"authenticate separate native command channel"))goto end;
    status_byte=255;if(!check(request(channel,20,0,0,0,0,0)&&receive(channel,&status_byte,1)&&!status_byte,
      "obtain native command-channel child"))goto end;
    check(execute(channel),"native Shell EXEC output is exact while main connection remains usable");
    if(!check(okay(fd,3,0,left+120,top+130)&&okay(fd,4,1,0,0)&&okay(fd,4,0,0,0),
      "send finite mouse move and click only inside witnessed native window"))goto end;
    for(i=0;i<10;++i)if(!okay(fd,7,0,(UBYTE)"Amigent68k"[i],0))break;
    if(!check(i==10,"original CHAR types finite expected native witness text"))goto end;
    for(i=0;i<100;++i){if(read_proof("RAM:KaffeineAmigentWitness.result",proof,sizeof(proof))&&strstr(proof,nonce))break;Delay(1);}
    check(i<100&&strstr(proof,"\"text_exact\":true")&&strstr(proof,"\"moved\":true")&&
      strstr(proof,"\"down\":true")&&strstr(proof,"\"up\":true")&&strstr(proof,"\"mouse_x\":120")&&strstr(proof,"\"mouse_y\":130"),
      "independent native task witnesses exact text and mouse effects then closes its window");
end:
    if(channel>=0)CloseSocket(channel);if(fd>=0)CloseSocket(fd);
    if(daemon){struct Task *live=FindTask((CONST_STRPTR)daemon_name);if(live)Signal(live,SIGBREAKF_CTRL_C);
      for(i=0;i<100&&FindTask((CONST_STRPTR)daemon_name);++i)Delay(1);
      check(!FindTask((CONST_STRPTR)daemon_name),"daemon and its command/reader children stop normally");}
    if(segment&&!FindTask((CONST_STRPTR)daemon_name))UnLoadSeg(segment);

    if(witness){struct MsgPort *p;Forbid();p=FindPort((CONST_STRPTR)"KaffeineAmigentInputWitness");
      if(p)Signal(p->mp_SigTask,SIGBREAKF_CTRL_C);Permit();
      for(i=0;i<100;++i){Forbid();p=FindPort((CONST_STRPTR)"KaffeineAmigentInputWitness");Permit();if(!p)break;Delay(1);}
      check(i<100,"native witness is closed and its private port retired");}
    DeleteFile((CONST_STRPTR)"RAM:KaffeineAmigentWitness.ready");DeleteFile((CONST_STRPTR)"RAM:KaffeineAmigentWitness.result");
    if(SocketBase)CloseLibrary(SocketBase);SetTaskPri(FindTask(NULL),old_priority);
    return test_done("net_amigent_native");
}

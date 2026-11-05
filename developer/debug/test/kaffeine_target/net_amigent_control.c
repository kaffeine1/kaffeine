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
#include <string.h>
#include "socket_fixture_abi.h"
#include "m68ktest.h"
struct Library *SocketBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static const char daemon_name[]="amigent-control-fixture";
static const char key[]="public-loopback-fixture-key";
static const char folder[]="RAM:Amigent-Protocol-Check";
static const char file[]="RAM:Amigent-Protocol-Check/first.txt";
static const char renamed[]="RAM:Amigent-Protocol-Check/renamed.txt";
static const char proof[]="exact command-channel roundtrip\n";
static struct IntuiText button_label={1,0,JAM1,0,0,NULL,(UBYTE *)"protocol-button",NULL};
static struct Gadget button={NULL,12,24,120,18,GFLG_GADGHNONE,GACT_RELVERIFY,GTYP_BOOLGADGET,
    NULL,NULL,&button_label,0,NULL,7,NULL};
static struct IntuiText item_label={1,0,JAM1,0,0,NULL,(UBYTE *)"Protocol item",NULL};
static struct MenuItem item={NULL,0,0,120,12,ITEMTEXT|ITEMENABLED|HIGHCOMP,0,&item_label,NULL,0,NULL,0};
static struct Menu menu={NULL,0,0,100,12,MENUENABLED,(BYTE *)"Protocol menu",&item,0,0,0,0};
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
static BOOL path_command(LONG fd,UBYTE cmd,const char *p)
{return request(fd,cmd,0,strlen(p),0,0,0)&&transmit(fd,p,strlen(p))&&status(fd);}
static BOOL blob(LONG fd,char *out,ULONG cap,ULONG *length)
{UBYTE b[4];ULONG n;if(!receive(fd,b,4)||(n=u32(b))>=cap||!receive(fd,out,n))return FALSE;out[n]=0;if(length)*length=n;return TRUE;}
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
static BOOL classic_tile(LONG fd)
{
    UBYTE b[12],data[8192];ULONG n,i,total=0;
    if(!request(fd,2,1,64,64,64,64)||!receive(fd,b,4)||u32(b)!=1||!receive(fd,b,12)||
       u16(b)!=64||u16(b+2)!=64||u16(b+4)!=64||u16(b+6)!=64||(n=u32(b+8))>sizeof(data)||!receive(fd,data,n))return FALSE;
    if(n==4096){for(i=0;i<n;++i)if(data[i]!=153)return FALSE;return TRUE;}
    if(n%2)return FALSE;
    for(i=0;i<n;i+=2){if(data[i+1]!=153)return FALSE;total+=(ULONG)data[i]+1;}
    return total==4096;
}
static BOOL extended_probe(LONG fd,UWORD w,UWORD h)
{
    UBYTE b[12];
    return request(fd,2,6,0x7fff,0x7fff,1,1)&&receive(fd,b,12)&&b[0]==2&&b[1]==8&&
      u16(b+4)==w&&u16(b+6)==h&&u32(b+8)==0;
}
static BOOL text_reply(LONG fd,UBYTE cmd,char *out,ULONG cap)
{return request(fd,cmd,0,0,0,0,0)&&blob(fd,out,cap,NULL);}
static BOOL files(LONG fd)
{
    char out[4096];ULONG n;UBYTE s;
    if(!check(path_command(fd,28,folder),"command channel creates an owned RAM directory"))return FALSE;
    if(!check(request(fd,23,0,strlen(file),0,0,sizeof(proof)-1)&&transmit(fd,file,strlen(file))&&
              transmit(fd,proof,sizeof(proof)-1)&&status(fd),"WRITE transfers exact fixture bytes"))return FALSE;
    check(path_command(fd,22,file)&&blob(fd,out,sizeof(out),&n)&&n==sizeof(proof)-1&&!memcmp(out,proof,n),
          "READ returns every byte written by the independent channel client");
    check(request(fd,27,0,strlen(file),strlen(renamed),0,0)&&transmit(fd,file,strlen(file))&&
          transmit(fd,renamed,strlen(renamed))&&status(fd),"RENAME moves only the owned fixture file");
    check(path_command(fd,24,folder)&&blob(fd,out,sizeof(out),NULL)&&strstr(out,"renamed.txt"),
          "DIR describes the renamed fixture file");
    check(path_command(fd,25,renamed)&&blob(fd,out,sizeof(out),NULL)&&strstr(out,"renamed.txt"),
          "STAT describes the owned fixture file");
    check(path_command(fd,26,renamed),"DELETE removes the owned fixture file");
    check(path_command(fd,26,folder),"DELETE removes the empty owned directory");
    s=255;check(request(fd,6,0,0,0,0,0)&&receive(fd,&s,1)&&s==0,"channel stays responsive after file operations");
    return TRUE;
}
static BOOL execute(LONG fd)
{
    const char command[]="Echo AMIGENT_EXEC_OK";UBYTE b[8];char out[512];ULONG n;
    if(!request(fd,21,0,sizeof(command)-1,0,5,0)||!transmit(fd,command,sizeof(command)-1)||!status(fd)||
       !receive(fd,b,8)||(n=u32(b+4))>=sizeof(out)||!receive(fd,out,n))return FALSE;
    out[n]=0;return u32(b)==0 && strstr(out,"AMIGENT_EXEC_OK")!=NULL;
}
int main(void)
{
    struct Screen *screen=NULL;struct Window *window=NULL;struct Process *daemon=NULL;
    BPTR segment=0;LONG fd=-1,channel=-1,i,old_priority=SetTaskPri(FindTask(NULL),-5);BOOL strip=FALSE;
    char out[16384];UBYTE s;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);SocketBase=OpenLibrary("bsdsocket.library",4);
    if(!check(IntuitionBase&&GfxBase&&SocketBase,"open control fixture libraries"))goto end;
    screen=OpenScreenTags(NULL,SA_DisplayID,0x00100001,SA_Width,640,SA_Height,480,SA_Depth,8,
      SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_PubName,(ULONG)"KAFFEINE-AMIGENT-CONTROL",SA_Title,(ULONG)"Protocol screen",TAG_DONE);
    if(!check(screen!=NULL,"open owned public control screen"))goto end;
    PubScreenStatus(screen,0);ScreenToFront(screen);
    SetRGB32(&screen->ViewPort,2,0x2a2a2a2a,0x63636363,0xb1b1b1b1);SetAPen(&screen->RastPort,2);RectFill(&screen->RastPort,64,64,127,127);
    window=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)screen,WA_Left,200,WA_Top,180,WA_Width,320,WA_Height,180,
      WA_Title,(ULONG)"Amigent protocol window",WA_Gadgets,(ULONG)&button,WA_Activate,TRUE,
      WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_SIMPLE_REFRESH,
      WA_IDCMP,IDCMP_RAWKEY|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE,TAG_DONE);
    if(!check(window!=NULL,"open owned window with a classic labelled gadget"))goto end;
    strip=SetMenuStrip(window,&menu);if(!check(strip,"install owned classic menu strip"))goto end;
    for(i=0;i<50;++i){ULONG lock=LockIBase(0);struct Window *active=IntuitionBase->ActiveWindow;
      BOOL ready=active&&active->Title&&!strcmp((char *)active->Title,"Amigent protocol window");
      UnlockIBase(lock);if(ready)break;Delay(1);}
    if(!check(i<50,"observe active native window before menu queries"))goto end;
    segment=LoadSeg((CONST_STRPTR)"SYS:EMU68/amigentd");
    if(!check(segment!=0,"load original released daemon"))goto end;
    daemon=CreateNewProcTags(NP_Seglist,segment,NP_FreeSeglist,TRUE,NP_Name,(ULONG)daemon_name,
      NP_CommandName,(ULONG)"amigentd",NP_Arguments,(ULONG)"PORT 44125 KEY SYS:EMU68/amigent-test.key QUIET\n",
      NP_Cli,TRUE,NP_StackSize,65536,NP_CopyVars,FALSE,TAG_DONE);
    if(!check(daemon!=NULL,"start independent original daemon"))goto end;
    segment=0;
    fd=connection();if(!check(fd>=0,"authenticate screen connection"))goto end;
    check(text_reply(fd,15,out,sizeof(out))&&strstr(out,"Amigent protocol window")&&strstr(out,"protocol-button"),
      "UITREE contains real native window and classic gadget labels");
    check(text_reply(fd,16,out,sizeof(out))&&strstr(out,"Protocol menu")&&strstr(out,"Protocol item"),
      "MENUS contains real active window menu/item text");
    check(extended_probe(fd,screen->Width,screen->Height),"original client's empty extended-frame probe returns RTG format 2");
    check(classic_tile(fd),"classic FRAME RGB332 tile decodes to all 4096 known pixels");
    channel=connection();if(!check(channel>=0,"authenticate separate command channel"))goto end;
    s=255;if(!check(request(channel,20,0,0,0,0,0)&&receive(channel,&s,1)&&!s,"obtain command-channel child"))goto end;
    if(!files(channel))goto end;
    check(execute(channel),"EXEC starts a native Shell and captures exact Echo output");
end:
    if(channel>=0)CloseSocket(channel);if(fd>=0)CloseSocket(fd);
    if(daemon){struct Task *live=FindTask((CONST_STRPTR)daemon_name);if(live)Signal(live,SIGBREAKF_CTRL_C);
      for(i=0;i<100&&FindTask((CONST_STRPTR)daemon_name);++i)Delay(1);
      check(!FindTask((CONST_STRPTR)daemon_name),"daemon and its command/reader children stop normally");}
    if(segment&&!FindTask((CONST_STRPTR)daemon_name))UnLoadSeg(segment);
    if(window){if(strip)ClearMenuStrip(window);CloseWindow(window);}
    if(screen)check(CloseScreen(screen),"close owned control screen after children stop");
    if(SocketBase)CloseLibrary(SocketBase);if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);SetTaskPri(FindTask(NULL),old_priority);
    return test_done("net_amigent_control");
}

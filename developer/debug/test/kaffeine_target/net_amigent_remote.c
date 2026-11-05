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
struct Library *SocketBase, *KeymapBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static const char daemon_name[]="amigent-remote-fixture";
static const char key[]="public-loopback-fixture-key";
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
static BOOL moved,down,up,keys[256];static WORD mx,my,bx,by;
static UBYTE previous[2],qualifiers[2],typed[128];static ULONG typed_length;
static void pump(struct Window *w)
{
    struct IntuiMessage *m;struct InputEvent ev;char out[16];LONG n;
    while((m=(struct IntuiMessage *)GetMsg(w->UserPort))) {
        if(m->Class==IDCMP_MOUSEMOVE){moved=TRUE;mx=m->MouseX;my=m->MouseY;}
        if(m->Class==IDCMP_MOUSEBUTTONS) {
            if(m->Code==SELECTDOWN){down=TRUE;bx=m->MouseX;by=m->MouseY;}
            if(m->Code==SELECTUP)up=TRUE;
        }
        if(m->Class==IDCMP_RAWKEY) {
            keys[m->Code&255]=TRUE;
            memset(&ev,0,sizeof(ev));ev.ie_Class=IECLASS_RAWKEY;ev.ie_Code=m->Code;ev.ie_Qualifier=m->Qualifier;
            ev.ie_Prev1DownCode=previous[0];ev.ie_Prev1DownQual=qualifiers[0];
            ev.ie_Prev2DownCode=previous[1];ev.ie_Prev2DownQual=qualifiers[1];
            n=MapRawKey(&ev,out,sizeof(out),NULL);
            if(n>0 && typed_length+(ULONG)n<sizeof(typed)){memcpy(typed+typed_length,out,n);typed_length+=n;}
            if(!(m->Code&IECODE_UP_PREFIX)) {
                previous[1]=previous[0];qualifiers[1]=qualifiers[0];
                previous[0]=m->Code;qualifiers[0]=m->Qualifier;
            }
        }
        ReplyMsg((struct Message *)m);
    }
}
static void settle(struct Window *w,ULONG ticks)
{ULONG i;for(i=0;i<ticks;++i){pump(w);Delay(1);}pump(w);}
static BOOL input(LONG fd,struct Window *w)
{
    static const UBYTE text[]={'a','Z','7',0xe9};ULONG i;
    if(!check(okay(fd,5,1,0x63,IEQUALIFIER_CONTROL)&&okay(fd,5,0,0x63,0),
      "original Amigent sends harmless qualifier down and up"))return FALSE;
    settle(w,10);
    if(!check(keys[0x63]&&keys[0xe3],"qualifier events witness owned window focus before other input"))return FALSE;
    check(okay(fd,5,1,0x50,0)&&okay(fd,5,0,0x50,0),"original RAWKEY accepts F1 down and up");
    settle(w,5);check(keys[0x50]&&keys[0xd0],"real IDCMP F1 events arrive");
    check(okay(fd,3,0,w->LeftEdge+50,w->TopEdge+70),"original MOVE absolute accepted");
    settle(w,5);check(moved&&mx==50&&my==70,"absolute move reaches exact owned-window coordinates");
    check(okay(fd,4,1,0,0)&&okay(fd,4,0,0,0),"original BUTTON click accepted");
    settle(w,5);check(down&&up&&bx==50&&by==70,"both real click events arrive at the witnessed position");
    moved=FALSE;check(okay(fd,14,0,4,4),"original relative move accepted");
    settle(w,5);check(moved&&mx==54&&my==74,"relative movement is observed exactly");
    typed_length=0;
    for(i=0;i<sizeof(text);++i)if(!okay(fd,7,0,text[i],0))return check(FALSE,"original CHAR converts ASCII and Latin1 dead keys");
    settle(w,10);
    return check(typed_length==sizeof(text)&&!memcmp(typed,text,sizeof(text)),
      "original CHAR produces exact aZ7 and e-acute through real raw-key/dead-key events");
}
static ULONG screen_handle(LONG fd,const char *name)
{
    UBYTE b[14];UWORD n,count,i;char title[512];ULONG handle=0;
    if(!request(fd,11,0,0,0,0,0)||!receive(fd,b,2))return 0;
    count=u16(b);if(count>64)return 0;
    for(i=0;i<count;++i){if(!receive(fd,b,14)||(n=u16(b+12))>=sizeof(title)||!receive(fd,title,n))return 0;
        title[n]=0;if(!strcmp(title,name))handle=u32(b);}
    return handle;
}
static BOOL front_title(const char *name)
{
    ULONG key=LockIBase(0);struct Screen *s=IntuitionBase->FirstScreen;
    BOOL equal=s&&s->Title&&!strcmp((char *)s->Title,name);UnlockIBase(key);return equal;
}
static BOOL switch_screen(LONG fd,const char *name)
{
    ULONG h=screen_handle(fd,name),i;
    if(!h||!okay(fd,12,0,h>>16,h&65535))return FALSE;
    for(i=0;i<40;++i){if(front_title(name))return TRUE;Delay(1);}return FALSE;
}

int main(void)
{
    struct Screen *screen=NULL,*other=NULL;struct Window *window=NULL;struct Process *daemon=NULL;
    BPTR segment=0;LONG fd=-1,channel=-1,i,old_priority=SetTaskPri(FindTask(NULL),-5);BOOL strip=FALSE;
    UBYTE s;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);SocketBase=OpenLibrary("bsdsocket.library",4);
    KeymapBase=OpenLibrary("keymap.library",37);
    if(!check(IntuitionBase&&GfxBase&&SocketBase&&KeymapBase,"open control fixture libraries"))goto end;
    screen=OpenScreenTags(NULL,SA_DisplayID,0x00100001,SA_Width,640,SA_Height,480,SA_Depth,8,
      SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_PubName,(ULONG)"KAFFEINE-AMIGENT-CONTROL",SA_Title,(ULONG)"Protocol screen",TAG_DONE);
    if(!check(screen!=NULL,"open owned public control screen"))goto end;
    PubScreenStatus(screen,0);ScreenToFront(screen);
    SetRGB32(&screen->ViewPort,2,0x2a2a2a2a,0x63636363,0xb1b1b1b1);SetAPen(&screen->RastPort,2);RectFill(&screen->RastPort,64,64,127,127);
    window=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)screen,WA_Left,200,WA_Top,180,WA_Width,320,WA_Height,180,
      WA_Title,(ULONG)"Amigent protocol window",WA_Gadgets,(ULONG)&button,WA_Activate,TRUE,
      WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_SIMPLE_REFRESH,
      WA_ReportMouse,TRUE,WA_RMBTrap,TRUE,WA_IDCMP,IDCMP_RAWKEY|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE,TAG_DONE);
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
    if(!input(fd,window))goto end;
    other=OpenScreenTags(NULL,SA_DisplayID,0x00100001,SA_Width,640,SA_Height,480,SA_Depth,8,
      SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_PubName,(ULONG)"KAFFEINE-AMIGENT-OTHER",SA_Title,(ULONG)"Remote other screen",TAG_DONE);
    if(!check(other!=NULL,"open second owned screen for real depth switching"))goto end;
    PubScreenStatus(other,0);
    check(switch_screen(fd,"Remote other screen"),"original SCREEN_FRONT really switches to second native screen");
    check(switch_screen(fd,"Protocol screen"),"original SCREEN_FRONT returns to first native screen");
    channel=connection();if(!check(channel>=0,"authenticate separate command channel"))goto end;
    s=255;if(!check(request(channel,20,0,0,0,0,0)&&receive(channel,&s,1)&&!s,"obtain command-channel child"))goto end;
    check(execute(channel),"EXEC starts a native Shell and captures exact Echo output");
end:
    if(channel>=0)CloseSocket(channel);if(fd>=0)CloseSocket(fd);
    if(daemon){struct Task *live=FindTask((CONST_STRPTR)daemon_name);if(live)Signal(live,SIGBREAKF_CTRL_C);
      for(i=0;i<100&&FindTask((CONST_STRPTR)daemon_name);++i)Delay(1);
      check(!FindTask((CONST_STRPTR)daemon_name),"daemon and its command/reader children stop normally");}
    if(segment&&!FindTask((CONST_STRPTR)daemon_name))UnLoadSeg(segment);
    if(window){if(strip)ClearMenuStrip(window);CloseWindow(window);}
    if(other)check(CloseScreen(other),"close second screen after child helpers stop");
    if(screen)check(CloseScreen(screen),"close owned control screen after children stop");
    if(KeymapBase)CloseLibrary(KeymapBase);
    if(SocketBase)CloseLibrary(SocketBase);if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);SetTaskPri(FindTask(NULL),old_priority);
    return test_done("net_amigent_remote");
}

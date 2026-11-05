/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Finite native owned-window witness for remote input. No other window,
   shared palette, application, boot configuration or input is modified. */
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <devices/inputevent.h>
#include <graphics/gfxbase.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/keymap.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase, *KeymapBase;
static const char port_name[]="KaffeineAmigentInputWitness";
static const char title[]="Kaffeine Amigent Input Check";
static const char expected[]="Amigent68k";
static const char ready_path[]="RAM:KaffeineAmigentWitness.ready";
static const char result_path[]="RAM:KaffeineAmigentWitness.result";
static BOOL front(struct Window *w)
{ULONG key=LockIBase(0);BOOL active=IntuitionBase->ActiveWindow==w;UnlockIBase(key);return active;}
static BOOL proof(struct Window *w,const char *nonce,BOOL done,BOOL good,BOOL moved,BOOL down,BOOL up,LONG mx,LONG my)
{
    BPTR file=Open((CONST_STRPTR)(done?result_path:ready_path),MODE_NEWFILE);LONG result;
    if(!file)return FALSE;
    result=FPrintf(file,(CONST_STRPTR)"{\"nonce\":\"%s\",\"title\":\"%s\",\"active\":%s,\"left\":%ld,\"top\":%ld,\"width\":%ld,\"height\":%ld,\"marker_x\":%ld,\"marker_y\":%ld,\"marker_size\":32,\"marker_rgb\":\"2a63b1\",\"text_exact\":%s,\"moved\":%s,\"down\":%s,\"up\":%s,\"mouse_x\":%ld,\"mouse_y\":%ld}\n",
      nonce,title,front(w)?"true":"false",(LONG)w->LeftEdge,(LONG)w->TopEdge,(LONG)w->Width,(LONG)w->Height,
      (LONG)w->LeftEdge+80,(LONG)w->TopEdge+80,good?"true":"false",moved?"true":"false",down?"true":"false",up?"true":"false",mx,my);
    return Close(file)&&result>0;
}
int main(int argc,char **argv)
{
    struct MsgPort *port=NULL;struct Window *w=NULL;struct IntuiMessage *m;
    struct InputEvent ev;char text[128]={0},out[16];UBYTE previous[2]={0},qualifier[2]={0};
    ULONG count=0,ticks;LONG n,rc=10,mx=-1,my=-1;BOOL duplicate,good=FALSE,moved=FALSE,down=FALSE,up=FALSE;
    if(argc!=2||strlen(argv[1])!=32||strspn(argv[1],"0123456789abcdef")!=32)return 10;
    Forbid();duplicate=FindPort((CONST_STRPTR)port_name)!=NULL;Permit();if(duplicate)return 10;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",39);CyberGfxBase=OpenLibrary((CONST_STRPTR)"cybergraphics.library",41);
    KeymapBase=OpenLibrary((CONST_STRPTR)"keymap.library",37);
    if(!IntuitionBase||!GfxBase||!CyberGfxBase||!KeymapBase)goto done;
    port=CreateMsgPort();if(!port)goto done;port->mp_Node.ln_Name=(char *)port_name;AddPort(port);
    DeleteFile((CONST_STRPTR)ready_path);DeleteFile((CONST_STRPTR)result_path);
    w=OpenWindowTags(NULL,WA_Left,320,WA_Top,200,WA_Width,480,WA_Height,220,WA_Title,(IPTR)title,
      WA_Activate,TRUE,WA_ReportMouse,TRUE,WA_RMBTrap,TRUE,
      WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_SIMPLE_REFRESH,
      WA_IDCMP,IDCMP_RAWKEY|IDCMP_MOUSEMOVE|IDCMP_MOUSEBUTTONS,TAG_DONE);
    if(!w)goto done;
    for(ticks=0;ticks<50&&!front(w);++ticks)Delay(1);
    if(!front(w))goto done;
    if(FillPixelArray(w->RPort,80,80,32,32,0xff2a63b1)!=1024)goto done;
    if(!proof(w,argv[1],FALSE,FALSE,FALSE,FALSE,FALSE,-1,-1))goto done;
    for(ticks=0;ticks<3000;++ticks) {
      while((m=(struct IntuiMessage *)GetMsg(w->UserPort))) {
        if(m->Class==IDCMP_MOUSEMOVE){moved=TRUE;mx=m->MouseX;my=m->MouseY;}
        if(m->Class==IDCMP_MOUSEBUTTONS){if(m->Code==SELECTDOWN)down=TRUE;if(m->Code==SELECTUP)up=TRUE;}
        if(m->Class==IDCMP_RAWKEY) {
          memset(&ev,0,sizeof(ev));ev.ie_Class=IECLASS_RAWKEY;ev.ie_Code=m->Code;ev.ie_Qualifier=m->Qualifier;
          ev.ie_Prev1DownCode=previous[0];ev.ie_Prev1DownQual=qualifier[0];ev.ie_Prev2DownCode=previous[1];ev.ie_Prev2DownQual=qualifier[1];
          n=MapRawKey(&ev,(STRPTR)out,sizeof(out),NULL);
          if(n>0&&count+(ULONG)n<sizeof(text)){memcpy(text+count,out,n);count+=n;}
          if(!(m->Code&IECODE_UP_PREFIX)){previous[1]=previous[0];qualifier[1]=qualifier[0];previous[0]=m->Code;qualifier[0]=m->Qualifier;}
        }
        ReplyMsg((struct Message *)m);
      }
      good=count==sizeof(expected)-1&&!memcmp(text,expected,sizeof(expected)-1);
      if(good&&moved&&down&&up&&mx==120&&my==130){rc=0;break;}
      if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C)break;
      Delay(1);
    }
    if(!proof(w,argv[1],TRUE,good,moved,down,up,mx,my))rc=10;
done:
    if(w)CloseWindow(w);
    if(port){RemPort(port);DeleteMsgPort(port);}
    if(KeymapBase)CloseLibrary(KeymapBase);if(CyberGfxBase)CloseLibrary(CyberGfxBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

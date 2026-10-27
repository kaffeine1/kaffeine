/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Owned-screen lifecycle through the classic 68k Intuition ABI. */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;
static ULONG indexed_mode(void)
{
    ULONG id=INVALID_ID; unsigned i;
    for(i=0;i<256;i++) {
        struct DimensionInfo dim; struct DisplayInfo disp;
        id=NextDisplayInfo(id); if(id==INVALID_ID)break;
        memset(&dim,0,sizeof(dim)); memset(&disp,0,sizeof(disp));
        if(GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,id) &&
           GetDisplayInfoData(NULL,(UBYTE *)&disp,sizeof(disp),DTAG_DISP,id) &&
           !disp.NotAvailable && dim.MaxDepth>=3 && dim.MaxDepth<=8 &&
           dim.Nominal.MaxX-dim.Nominal.MinX+1>=320 &&
           dim.Nominal.MaxY-dim.Nominal.MinY+1>=200 &&
           !(disp.PropertyFlags & (DIPF_IS_HAM|DIPF_IS_DUALPF))) return id;
    }
    return INVALID_ID;
}
static void screen_fields(struct Screen *s, ULONG id)
{
    check(s->Width>=320 && s->Height>=200,"owned screen covers requested geometry");
    check((s->Flags & SCREENTYPE)==CUSTOMSCREEN,"owned screen keeps custom type");
    check(s->RastPort.Font!=NULL,"owned screen RastPort has a guest font");
    check(s->Font!=NULL && s->Font->ta_YSize==8,"owned screen TextAttr has requested height");
    if(s->Font)check_str("owned screen font name",(char *)s->Font->ta_Name,"topaz.font");
    check(s->BitMap.Depth>=3 && s->BitMap.Depth<=8,"owned screen bitmap depth is indexed");
    check_long("owned ViewPort retains selected mode",GetVPModeID(&s->ViewPort),id);
    SetAPen(&s->RastPort,2);RectFill(&s->RastPort,32,32,47,47);
    check_long("drawing to owned RastPort preserves pen index",ReadPixel(&s->RastPort,40,40),2);
}
int main(int argc,char **argv)
{
    ULONG id; unsigned pass;
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    GadToolsBase=OpenLibrary("gadtools.library",39);
    if(!check(IntuitionBase && GfxBase && GadToolsBase,"open graphics, Intuition and GadTools"))goto done;
    id=indexed_mode(); if(!check(id!=INVALID_ID,"select a genuine indexed native mode"))goto done;
    if(argc>1 && !strcmp(argv[1],"leave")) {
        struct TextAttr font={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
        struct Screen *s=OpenScreenTags(NULL,SA_DisplayID,id,SA_Width,320,
            SA_Height,200,SA_Depth,3,SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,
            SA_Font,(ULONG)&font,SA_PubName,(ULONG)"KAFFEINE-OWNED-TEARDOWN",TAG_DONE);
        if(check(s!=NULL,"open owned public screen for teardown")) {
            struct Screen *locked; struct Window *w;
            PubScreenStatus(s,0);
            locked=LockPubScreen((STRPTR)"KAFFEINE-OWNED-TEARDOWN");
            check(locked==s,"public lock shares the owned screen identity");
            if(locked)UnlockPubScreen(NULL,locked);
            w=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)s,WA_Left,16,WA_Top,16,
                WA_Width,128,WA_Height,80,WA_Flags,WFLG_BORDERLESS|WFLG_SIMPLE_REFRESH,
                WA_IDCMP,0,TAG_DONE);
            check(w!=NULL && w->WScreen==s,"leave an owned window and screen for bridge cleanup");
            check(GetVisualInfoA(s,NULL)!=NULL,"leave an owned VisualInfo for ordered cleanup");
        }
        goto done;
    }
    for(pass=0;pass<2;pass++) {
        struct NewScreen ns; struct Screen *s;
        struct TextAttr font={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
        UWORD pens[]={0,1,1,2,1,3,1,0,2,1,2,~0};
        struct Rectangle clip={0,0,319,199};
        struct {ULONG before,error,after;} result={0x11223344,0xdeadbeef,0x55667788};
        struct TagItem more[]={{SA_DisplayID,id},{SA_Font,(ULONG)&font},{SA_ErrorCode,(ULONG)&result.error},{SA_Pens,(ULONG)pens},{SA_DClip,(ULONG)&clip},{TAG_DONE,0}};
        struct TagItem tags[]={{TAG_IGNORE,0x12345678},{TAG_SKIP,1},{SA_Depth,99},{TAG_MORE,(ULONG)more}};
        memset(&ns,0,sizeof(ns));ns.Width=320;ns.Height=200;ns.Depth=3;ns.DetailPen=1;ns.BlockPen=2;
        ns.Type=CUSTOMSCREEN|SCREENQUIET;ns.Font=&font;
        s=OpenScreenTagList(&ns,tags);
        if(!check(s!=NULL,"open owned screen through bounded chained tags"))break;
        check(result.before==0x11223344 && result.after==0x55667788,"SA_ErrorCode preserves surrounding words");
        screen_fields(s,id);
        {
            APTR vi=GetVisualInfoA(s,NULL);
            if(check(vi!=NULL,"own a native VisualInfo for the screen")) {
                check(!CloseScreen(s),"bridge retains screen while VisualInfo is live");
                FreeVisualInfo(vi);
                check_long("freeing VisualInfo preserves the screen identity",GetVPModeID(&s->ViewPort),id);
            }
        }
        {
            struct Window *w=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)s,
                WA_Left,16,WA_Top,16,WA_Width,128,WA_Height,80,
                WA_Flags,WFLG_BORDERLESS|WFLG_SIMPLE_REFRESH,WA_IDCMP,0,TAG_DONE);
            if(check(w!=NULL,"open window on the owned screen")) {
                check(w->WScreen==s,"window uses the same owned screen identity");
                check(!CloseScreen(s),"native refuses screen close with an open window");
                check_long("refused close preserves the owned ViewPort",GetVPModeID(&s->ViewPort),id);
                CloseWindow(w);
            }
        }
        check(CloseScreen(s),"successfully close owned indexed screen");
    }
    {
        struct ExtNewScreen ns; struct Screen *s;
        struct TextAttr font={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
        struct TagItem tags[]={{SA_DisplayID,id},{TAG_DONE,0}};
        memset(&ns,0,sizeof(ns));ns.Width=320;ns.Height=200;ns.Depth=3;
        ns.Type=CUSTOMSCREEN|SCREENQUIET|NS_EXTENDED;ns.Font=&font;ns.Extension=tags;
        s=OpenScreen((struct NewScreen *)&ns);
        if(check(s!=NULL,"OpenScreen uses the extended NewScreen tag list")) {
            check_long("extended screen mode identity",GetVPModeID(&s->ViewPort),id);
            check(CloseScreen(s),"close extended owned screen");
        }
    }
    {
        ULONG error=0xdeadbeef; struct Screen *bad;
        struct TagItem tags[]={{SA_DisplayID,0x7fff0000},{SA_Width,320},{SA_Height,200},{SA_Depth,3},{SA_Type,CUSTOMSCREEN},{SA_ErrorCode,(ULONG)&error},{TAG_DONE,0}};
        bad=OpenScreenTagList(NULL,tags);
        check(bad==NULL,"unavailable display mode fails without screen ownership");
        check(error!=0 && error!=0xdeadbeef,"native open failure writes a classic error code");
        if(bad)CloseScreen(bad);
    }
done:
    if(GadToolsBase)CloseLibrary(GadToolsBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_owned_screen");
}

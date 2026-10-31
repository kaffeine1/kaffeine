/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original standalone planar/owned-screen bitmap transfer oracle. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
static ULONG pixel(struct BitMap *bm,ULONG x,ULONG y)
{
    ULONG p,value=0;
    for(p=0;p<bm->Depth;p++)
        if (((UBYTE *)bm->Planes[p])[y*bm->BytesPerRow+(x>>3)] & (0x80U>>(x&7))) value|=1UL<<p;
    return value;
}
int main(void)
{
    struct Screen *screen=NULL;
    struct BitMap bm;
    ULONG mode=INVALID_ID,p,pass,bytes=16*96;
    memset(&bm,0,sizeof(bm));
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    if(!check(GfxBase && IntuitionBase,"open graphics and intuition"))goto done;
    while((mode=NextDisplayInfo(mode))!=INVALID_ID) {
        struct DimensionInfo dim;
        memset(&dim,0,sizeof(dim));
        if(GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,mode)>0 &&
           dim.MaxDepth==8 && dim.Nominal.MaxX>=159 && dim.Nominal.MaxY>=119) break;
    }
    if(!check(mode!=INVALID_ID,"find a native indexed display mode"))goto done;
    InitBitMap(&bm,3,128,96);
    for(p=0;p<3;p++) {
        bm.Planes[p]=AllocMem(bytes,MEMF_PUBLIC|MEMF_CLEAR);
        if(!check(bm.Planes[p]!=NULL,"allocate independent planar storage"))goto done;
    }
    check_long("standalone bitmap width attribute",GetBitMapAttr(&bm,BMA_WIDTH),128);
    check_long("standalone bitmap height attribute",GetBitMapAttr(&bm,BMA_HEIGHT),96);
    check_long("standalone bitmap depth attribute",GetBitMapAttr(&bm,BMA_DEPTH),3);
    check_long("standalone bitmap flags attribute",GetBitMapAttr(&bm,BMA_FLAGS),bm.Flags & BMF_STANDARD);
    for(pass=0;pass<2;pass++) {
        struct BitMap *display;
        for(p=0;p<3;p++)memset(bm.Planes[p],p<2?0xFF:0,bytes);
        screen=OpenScreenTags(NULL,SA_DisplayID,mode,SA_Width,160,SA_Height,120,
                             SA_Depth,3,SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_Behind,TRUE,TAG_DONE);
        if(!check(screen!=NULL,"open an owned indexed screen behind Workbench"))goto done;
        display=screen->RastPort.BitMap;
        if(!check(display!=NULL,"owned Screen RastPort exposes a usable bitmap identity"))goto done;
        check(display->Rows>=120 && display->Depth>=3,"screen bitmap reports native dimensions and depth");
        check(GetBitMapAttr(display,BMA_WIDTH)>=160,"screen bitmap width comes from native graphics");
        check(GetBitMapAttr(display,BMA_HEIGHT)>=120,"screen bitmap height comes from native graphics");
        check(GetBitMapAttr(display,BMA_DEPTH)>=3,"screen bitmap depth comes from native graphics");
        check_long("screen bitmap flags preserve documented native bits",GetBitMapAttr(display,BMA_FLAGS),
                   display->Flags & (BMF_DISPLAYABLE|BMF_INTERLEAVED|BMF_STANDARD));
        Printf("OBS screen bitmap flags: attribute=%ld snapshot=%ld\n",
               (LONG)GetBitMapAttr(display,BMA_FLAGS),(LONG)display->Flags);
        check_long("AROS planar to indexed native return",BltBitMap(&bm,4,5,display,20,30,16,12,0xC0,0xFF,NULL),8);
        WaitBlit();
        check_long("planar source remains intact",pixel(&bm,4,5),3);
        check_long("planar copy reaches the indexed screen",ReadPixel(&screen->RastPort,20,30),3);
        check_long("planar copy reaches its last destination pixel",ReadPixel(&screen->RastPort,35,41),3);
        SetAPen(&screen->RastPort,5);RectFill(&screen->RastPort,20,30,35,41);
        check(screen->RastPort.BitMap==display,"bitmap identity survives RastPort refresh after drawing");
        check_long("AROS indexed to planar native return",BltBitMap(display,20,30,&bm,40,40,16,12,0xC0,0xFF,NULL),8);
        WaitBlit();
        check_long("indexed screen copy reaches original planar storage",pixel(&bm,40,40),5);
        check_long("indexed copy reaches its last planar pixel",pixel(&bm,55,51),5);
        check_long("copy preserves planar storage outside destination",pixel(&bm,56,51),3);
        check_long("AROS masked screen to planar return",BltBitMap(display,20,30,&bm,60,60,1,1,0xC0,2,NULL),8);
        WaitBlit();
        check_long("masked screen copy preserves other planes",pixel(&bm,60,60),1);
        check_long("AROS planar clear native return",BltBitMap(&bm,0,0,&bm,40,40,16,12,0,0xFF,NULL),8);
        WaitBlit();
        check_long("unregistered planar bitmap supports a direct clear",pixel(&bm,40,40),0);
        check(CloseScreen(screen)!=0,"close the owned screen after its blits");screen=NULL;
    }
done:
    if(screen)CloseScreen(screen);
    for(p=0;p<3;p++)if(bm.Planes[p])FreeMem(bm.Planes[p],bytes);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_bitmap_transfer");
}

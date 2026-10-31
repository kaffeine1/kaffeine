/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original DrawInfo scalar, font, pens and nested-lifetime oracle. */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
int main(void)
{
    struct Screen *screen=NULL;
    struct DrawInfo *one=NULL,*two=NULL;
    ULONG mode=INVALID_ID,pass,i;
    GfxBase=(struct GfxBase *)OpenLibrary("graphics.library",39);
    IntuitionBase=(struct IntuitionBase *)OpenLibrary("intuition.library",39);
    if(!check(GfxBase && IntuitionBase,"open graphics and intuition"))goto done;
    while((mode=NextDisplayInfo(mode))!=INVALID_ID) {
        struct DimensionInfo dim;
        memset(&dim,0,sizeof(dim));
        if(GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,mode) && dim.MaxDepth==8)break;
    }
    if(!check(mode!=INVALID_ID,"find an indexed display mode"))goto done;
    for(pass=0;pass<2;pass++) {
        UWORD pens[64];
        screen=OpenScreenTags(NULL,SA_DisplayID,mode,SA_Width,320,SA_Height,200,
                             SA_Depth,3,SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_Behind,TRUE,TAG_DONE);
        if(!check(screen!=NULL,"open an owned indexed screen"))goto done;
        one=GetScreenDrawInfo(screen);
        if(!check(one!=NULL,"get the screen DrawInfo"))goto done;
        check(one->dri_Version>=1,"DrawInfo has a supported public version");
        check(one->dri_Depth>=3 && one->dri_Depth<=8,"DrawInfo reports indexed screen depth");
        check(one->dri_NumPens>0 && one->dri_NumPens<=64,"DrawInfo reports a bounded pen count");
        check(one->dri_Font!=NULL,"DrawInfo exposes a typed public font");
        if(one->dri_Font) {
            check(one->dri_Font==screen->RastPort.Font,"DrawInfo and initial screen RastPort share font identity");
            check(one->dri_Font->tf_XSize>0 && one->dri_Font->tf_YSize>0,"DrawInfo font metrics remain readable");
        }
        check(one->dri_Pens!=NULL,"DrawInfo exposes readable public pen values");
        if(one->dri_Pens && one->dri_NumPens<=64)
            for(i=0;i<one->dri_NumPens;i++)pens[i]=one->dri_Pens[i];
        two=GetScreenDrawInfo(screen);
        if(!check(two!=NULL,"get a nested DrawInfo reference"))goto done;
        check(two==one,"nested DrawInfo gets preserve identity");
        check(two->dri_Depth==one->dri_Depth && two->dri_NumPens==one->dri_NumPens,"nested snapshots preserve scalar metadata");
        if(two->dri_Pens && one->dri_Pens && one->dri_NumPens<=64) {
            BOOL same=TRUE;
            for(i=0;i<one->dri_NumPens;i++)if(two->dri_Pens[i]!=pens[i])same=FALSE;
            check(same,"nested DrawInfo gets preserve exact pen values");
        }
        FreeScreenDrawInfo(screen,two);two=NULL;
        check(one->dri_Depth>=3 && one->dri_Font!=NULL && one->dri_Pens!=NULL,"one reference survives freeing its nested peer");
        FreeScreenDrawInfo(screen,one);one=NULL;
        check(TextLength(&screen->RastPort,"Original",8)>0,"screen text metrics survive DrawInfo retirement");
        check(CloseScreen(screen)!=0,"close the screen after the last DrawInfo free");screen=NULL;
    }
done:
    if(two)FreeScreenDrawInfo(screen,two);
    if(one)FreeScreenDrawInfo(screen,one);
    if(screen)CloseScreen(screen);
    if(IntuitionBase)CloseLibrary((struct Library *)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_drawinfo");
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original screen-owned shared-pen protocol, also built as native ARM reference. */
#include <exec/types.h>
#include <intuition/screens.h>
#include <graphics/view.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
int main(void)
{
    struct Screen *s=NULL,*pub=NULL;
    struct ColorMap *cm;
    LONG first=-1,second=-1,borrowed=-1;
    struct { ULONG rgb[3]; UBYTE guard[16]; } out;
    struct TagItem tags[]={ {OBP_Precision,PRECISION_EXACT},{OBP_FailIfBad,TRUE},{TAG_DONE,0} };
    unsigned i;
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    IntuitionBase=(struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    if(!check(GfxBase&&IntuitionBase,"open graphics and Intuition"))goto done;
    s=OpenScreenTags(NULL,SA_Width,320,SA_Height,200,SA_Depth,8,SA_Type,CUSTOMSCREEN,SA_SharePens,TRUE,TAG_DONE);
    if(!check(s&&s->ViewPort.ColorMap,"open indexed screen with a ColorMap"))goto done;
    cm=s->ViewPort.ColorMap;
    first=ObtainBestPenA(cm,0x11111111,0x22222222,0x33333333,tags);
    if(!check(first>=0,"obtain an exact shared color"))goto done;
    second=ObtainBestPenA(cm,0x11111111,0x22222222,0x33333333,tags);
    check(second==first,"same exact color shares the pen");
    memset(&out,0xA5,sizeof(out));GetRGB32(cm,(ULONG)first,1,out.rgb);
    check(out.rgb[0]==0x11111111&&out.rgb[1]==0x22222222&&out.rgb[2]==0x33333333,
          "pen contains the requested 32-bit RGB values");
    for(i=0;i<sizeof(out.guard);i++)if(out.guard[i]!=0xA5)break;
    check(i==sizeof(out.guard),"color query preserves trailing storage");
    ReleasePen(cm,(ULONG)first);first=-1;
    if(second>=0)
    {
        memset(&out,0xA5,sizeof(out));GetRGB32(cm,(ULONG)second,1,out.rgb);
        check(out.rgb[0]==0x11111111,"shared pen survives the first release");
        ReleasePen(cm,(ULONG)second);second=-1;
    }
    pub=LockPubScreen(NULL);
    if(check(pub&&pub->ViewPort.ColorMap,"lock the public screen"))
    {
        struct TagItem loose[]={ {OBP_FailIfBad,FALSE},{TAG_DONE,0} };
        borrowed=ObtainBestPenA(pub->ViewPort.ColorMap,0,0,0,loose);
        PutStr((CONST_STRPTR)(borrowed>=0?"public pen available: yes\n":"public pen available: no\n"));
        if(borrowed>=0) { ReleasePen(pub->ViewPort.ColorMap,(ULONG)borrowed);borrowed=-1; }
        UnlockPubScreen(NULL,pub);pub=NULL;
    }
    if(check(CloseScreen(s),"screen closes after all own pens are released"))s=NULL;
done:
    if(pub)
    { if(borrowed>=0)ReleasePen(pub->ViewPort.ColorMap,(ULONG)borrowed);UnlockPubScreen(NULL,pub); }
    if(s)
    { if(first>=0)ReleasePen(s->ViewPort.ColorMap,(ULONG)first);if(second>=0)ReleasePen(s->ViewPort.ColorMap,(ULONG)second);CloseScreen(s); }
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_screen_pens");
}

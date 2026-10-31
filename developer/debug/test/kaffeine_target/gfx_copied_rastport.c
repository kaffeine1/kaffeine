/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original window-RastPort copy, independent state and layer clipping. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <graphics/gels.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
#if defined(__mc68000__) || defined(__m68k__)
typedef ULONG QATagPtr;
#else
typedef IPTR QATagPtr;
#endif
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
int main(void)
{
    struct Screen *s=NULL;
    struct Window *w=NULL,*cover=NULL;
    struct RastPort copy;
    struct GelsInfo gels;
    struct VSprite head,tail;
    static const UBYTE text[]="Kaffeine";
    ULONG lit=0;
    WORD x,y;
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    IntuitionBase=(struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    if(!check(GfxBase&&IntuitionBase,"open graphics and Intuition"))goto done;
    s=OpenScreenTags(NULL,SA_Width,320,SA_Height,240,SA_Depth,8,SA_Type,CUSTOMSCREEN,TAG_DONE);
    if(!check(s!=NULL,"open indexed screen"))goto done;
    w=OpenWindowTags(NULL,WA_CustomScreen,(QATagPtr)s,WA_Left,10,WA_Top,20,WA_Width,200,
                     WA_Height,150,WA_Borderless,TRUE,WA_Activate,TRUE,WA_SimpleRefresh,TRUE,TAG_DONE);
    if(!check(w&&w->RPort,"open a window with its own layer"))goto done;
    SetABPenDrMd(w->RPort,0,0,JAM2);RectFill(w->RPort,0,0,199,149);
    SetABPenDrMd(w->RPort,1,0,JAM1);Move(w->RPort,2,2);
    memset(&gels,0,sizeof(gels));
    memset(&head,0,sizeof(head));memset(&tail,0,sizeof(tail));
    InitGels(&head,&tail,&gels);
    CopyMem(w->RPort,&copy,sizeof(copy));
    copy.GelsInfo=&gels;
    check(copy.Layer==w->RPort->Layer&&copy.BitMap==w->RPort->BitMap&&copy.Font==w->RPort->Font,
          "copy retains window layer, bitmap and font identities");
    SetABPenDrMd(&copy,3,0,JAM1);Move(&copy,10,30);Text(&copy,text,8);
    check(copy.cp_x==10+TextLength(w->RPort,text,8)&&copy.cp_y==30&&copy.GelsInfo==&gels,
          "text advances the copy and preserves its unused GelsInfo");
    for(y=10;y<=30;y++)for(x=10;x<100;x++)if(ReadPixel(w->RPort,x,y)==3)lit++;
    check(lit>0,"text drawn through the copy is visible through the original RastPort");
    check(GetAPen(w->RPort)==1&&GetBPen(w->RPort)==0&&GetDrMd(w->RPort)==JAM1&&
          w->RPort->cp_x==2&&w->RPort->cp_y==2,
          "copy drawing leaves the original pens, mode and position unchanged");
    cover=OpenWindowTags(NULL,WA_CustomScreen,(QATagPtr)s,WA_Left,65,WA_Top,75,WA_Width,40,
                         WA_Height,40,WA_Borderless,TRUE,WA_Activate,TRUE,WA_SimpleRefresh,TRUE,TAG_DONE);
    if(!check(cover&&cover->RPort,"open a covering window"))goto done;
    SetABPenDrMd(cover->RPort,1,0,JAM2);RectFill(cover->RPort,0,0,39,39);
    SetAPen(&copy,2);RectFill(&copy,40,40,70,70);
    check(ReadPixel(w->RPort,42,42)==2&&copy.GelsInfo==&gels,"copied drawing reaches the uncovered part of its window");
    check(ReadPixel(cover->RPort,5,5)==1,"copied drawing respects clipping of the covering window");
    check(GetAPen(w->RPort)==1&&w->RPort->cp_x==2&&w->RPort->cp_y==2,
          "later copied drawing still preserves the original state");
done:
    if(cover)CloseWindow(cover);
    if(w)CloseWindow(w);
    if(s)check(CloseScreen(s),"screen closes after both windows");
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_copied_rastport");
}

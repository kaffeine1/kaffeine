/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original classic Image/Border chain rendering; same native ARM reference. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
static UBYTE data[8] __attribute__((aligned(2)))={0xF0,0,0x0F,0,0xAA,0,0x55,0};
int main(void)
{
    struct Screen *s=NULL;
    struct RastPort *rp;
    struct Image second={25,10,4,3,0,NULL,0,5,NULL};
    struct Image first={10,10,8,4,1,(UWORD*)data,1,2,&second};
    struct Image before_first,before_second;
    WORD xy[]={0,0,6,0,6,5},xy2[]={0,0,0,4};
    struct Border b2={10,0,4,0,JAM1,2,xy2,NULL};
    struct Border b1={-2,2,6,0,JAM1,3,xy,&b2};
    unsigned x,y;
    BOOL image_ok=TRUE,solid_ok=TRUE;
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    IntuitionBase=(struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    if(!check(GfxBase&&IntuitionBase,"open graphics and Intuition"))goto done;
    s=OpenScreenTags(NULL,SA_Width,320,SA_Height,200,SA_Depth,8,SA_Type,CUSTOMSCREEN,TAG_DONE);
    if(!check(s!=NULL,"open indexed screen"))goto done;
    rp=&s->RastPort;SetABPenDrMd(rp,0,0,JAM2);RectFill(rp,0,0,319,199);
    SetABPenDrMd(rp,7,4,JAM1);Move(rp,2,3);
    memcpy(&before_first,&first,sizeof(first));memcpy(&before_second,&second,sizeof(second));
    DrawImage(rp,&first,5,7);
    for(y=0;y<4;y++)for(x=0;x<8;x++)
        if(ReadPixel(rp,15+x,17+y)!=((data[y*2]&(0x80U>>x))?3:2))image_ok=FALSE;
    for(y=0;y<3;y++)for(x=0;x<4;x++)if(ReadPixel(rp,30+x,17+y)!=5)solid_ok=FALSE;
    check(image_ok,"ImageData planar bytes and PlaneOnOff yield every expected pixel");
    check(solid_ok,"next depth-zero image draws its solid color");
    check(!memcmp(&first,&before_first,sizeof(first))&&!memcmp(&second,&before_second,sizeof(second)),
          "DrawImage leaves both caller-owned descriptors unchanged");
    check(GetAPen(rp)==7&&GetBPen(rp)==4&&GetDrMd(rp)==JAM1&&rp->cp_x==2&&rp->cp_y==3,
          "DrawImage preserves the RastPort state");
    DrawBorder(rp,&b1,60,40);
    check(ReadPixel(rp,58,42)==6&&ReadPixel(rp,64,42)==6&&ReadPixel(rp,64,47)==6,
          "Border uses signed origins and native WORD coordinate pairs");
    check(ReadPixel(rp,70,40)==4&&ReadPixel(rp,70,44)==4,"next Border uses its own coordinates and pen");
    check(GetAPen(rp)==7&&GetBPen(rp)==4&&GetDrMd(rp)==JAM1,"DrawBorder restores pens and drawing mode");
done:
    if(s)check(CloseScreen(s),"close rendered screen");
    if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("intui_classic_draw");
}

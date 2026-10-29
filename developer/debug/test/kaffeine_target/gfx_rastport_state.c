/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original classic RastPort state and AllocRaster protocol. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
int main(void)
{
    struct { UBYTE before[16];struct RastPort rp;UBYTE after[16]; } g;
    PLANEPTR planes[2]={NULL,NULL};
    struct TextFont *font;
    unsigned i,p;
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    if(!check(GfxBase!=NULL,"open graphics.library"))goto done;
    memset(&g,0xA5,sizeof(g));InitRastPort(&g.rp);font=g.rp.Font;
    check(font!=NULL,"InitRastPort supplies a default font");
    SetAPen(&g.rp,5);SetBPen(&g.rp,2);SetDrMd(&g.rp,JAM1);
    check(GetAPen(&g.rp)==5&&g.rp.FgPen==5,"SetAPen and GetAPen agree on caller-owned storage");
    check(GetBPen(&g.rp)==2&&g.rp.BgPen==2,"SetBPen and GetBPen agree on caller-owned storage");
    check(GetDrMd(&g.rp)==JAM1&&g.rp.DrawMode==JAM1,"SetDrMd and GetDrMd agree");
    SetABPenDrMd(&g.rp,1,3,JAM2);
    check(GetAPen(&g.rp)==1&&GetBPen(&g.rp)==3&&GetDrMd(&g.rp)==JAM2,
          "SetABPenDrMd sets both pens and drawing mode");
    Move(&g.rp,-5,9);
    check(g.rp.cp_x==-5&&g.rp.cp_y==9,"Move preserves signed classic coordinates");
    check(g.rp.Font==font&&!g.rp.Layer&&!g.rp.BitMap&&!g.rp.AreaPtrn&&!g.rp.TmpRas,
          "state calls preserve pointer identities and empty drawing attachments");
    for(i=0;i<16;i++)if(g.before[i]!=0xA5||g.after[i]!=0xA5)break;
    check(i==16,"classic state calls preserve adjacent storage");
    for(p=0;p<2;p++)
    {
        planes[p]=AllocRaster(32,16);
        if(!check(planes[p]!=NULL,"AllocRaster supplies a classic image plane"))goto done;
        memset(planes[p],0,64);
    }
    check(planes[0]!=planes[1],"separate raster allocations have separate storage");
done:
    for(p=0;p<2;p++)if(planes[p])FreeRaster(planes[p],32,16);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_rastport_state");
}

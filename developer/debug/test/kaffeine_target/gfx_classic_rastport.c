/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original classic RastPort state and planar-image protocol. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"
struct GfxBase *GfxBase;
static const UBYTE pattern[8]={0xF0,0x0F,0xAA,0x55,0x00,0xFF,0x81,0x18};
static UBYTE expected[2][64];
static ULONG pixel(unsigned x,unsigned y)
{
    ULONG n=x>=1&&x<=30&&y>=1&&y<=14?1:0;
    if((x==0&&y==0)||(x==31&&y==15)||(x==6&&y==5))n=2;
    if(x>=8&&x<24&&y>=6&&y<10)
        n=(pattern[(y-6)*2+(x-8)/8]&(0x80U>>((x-8)&7)))?3:0;
    return n;
}
int main(int argc,char **argv)
{
    struct { UBYTE before[16];struct RastPort rp;UBYTE after[16]; } g;
    struct BitMap bm;
    PLANEPTR planes[2]={NULL,NULL};
    struct TextFont *font;
    unsigned i,x,y,p;
    BOOL bytes_ok=TRUE,pixels_ok=TRUE,state=argc>1&&!strcmp(argv[1],"state");
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
    if(state)goto done;
    InitBitMap(&bm,2,32,16);bm.Planes[0]=planes[0];bm.Planes[1]=planes[1];
    g.rp.BitMap=&bm;SetABPenDrMd(&g.rp,1,0,JAM2);RectFill(&g.rp,1,1,30,14);
    SetAPen(&g.rp,2);
    check(WritePixel(&g.rp,0,0)==0&&WritePixel(&g.rp,31,15)==0&&WritePixel(&g.rp,6,5)==0,
          "WritePixel draws in caller-owned planar storage");
    SetABPenDrMd(&g.rp,3,0,JAM2);BltTemplate((APTR)pattern,0,2,&g.rp,8,6,16,4);
    for(y=0;y<16;y++)for(x=0;x<32;x++)
    {
        ULONG n=pixel(x,y);
        for(p=0;p<2;p++)if(n&(1UL<<p))expected[p][y*4+x/8]|=0x80U>>(x&7);
        if(ReadPixel(&g.rp,x,y)!=(LONG)n)pixels_ok=FALSE;
    }
    for(p=0;p<2;p++)if(memcmp(planes[p],expected[p],64))bytes_ok=FALSE;
    check(bytes_ok,"rectangle, individual pixels and template match independent planar bytes");
    check(pixels_ok,"ReadPixel agrees with all 512 expected indexed pixels");
    check(g.rp.Font==font&&g.rp.BitMap==&bm&&!g.rp.Layer,
          "planar drawing preserves the caller's attachments");
done:
    for(p=0;p<2;p++)if(planes[p])FreeRaster(planes[p],32,16);
    if(GfxBase)CloseLibrary((struct Library*)GfxBase);
    return test_done("gfx_classic_rastport");
}

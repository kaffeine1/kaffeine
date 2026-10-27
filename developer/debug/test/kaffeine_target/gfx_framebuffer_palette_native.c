/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Native AROS Raspberry framebuffer proof (compile for AArch64, not m68k).
    Opens a genuine LUT8 screen behind Workbench, draws before showing it,
    checks the physical RGBX framebuffer, then changes a live palette entry
    without redrawing the indices. Kernel framebuffer attributes are required.
*/
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#define __NOLIBBASE__
#include <aros/kernel.h>
#include <proto/kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned checks,failed;
static void check(int ok,const char *text)
{++checks;if(!ok)++failed;printf("%s %s\n",ok?"ok  ":"FAIL",text);fflush(stdout);}
static int pixel(volatile UBYTE *fb,ULONG pitch,unsigned x,unsigned y,UBYTE r,UBYTE g,UBYTE b)
{
    volatile UBYTE *p=fb+(size_t)y*pitch+x*4;
    if(p[0]==r && p[1]==g && p[2]==b)return 1;
    printf("pixel(%u,%u)=%u,%u,%u expected=%u,%u,%u\n",x,y,p[0],p[1],p[2],r,g,b);fflush(stdout);return 0;
}
int main(void)
{
    struct KernelBase *KernelBase=OpenResource((CONST_STRPTR)"kernel.resource");
    IPTR address=KernelBase?(IPTR)KrnGetSystemAttr(KATTR_FrameBuffer):0;
    ULONG width=KernelBase?KrnGetSystemAttr(KATTR_FrameBufferWidth):0;
    ULONG height=KernelBase?KrnGetSystemAttr(KATTR_FrameBufferHeight):0;
    ULONG pitch=KernelBase?KrnGetSystemAttr(KATTR_FrameBufferPitch):0;
    ULONG id=0xffffffffUL,indexed=0xffffffffUL,truecolor=0xffffffffUL;
    unsigned i;
    struct Screen *scr=NULL;
    volatile UBYTE *fb=(volatile UBYTE *)address;
    check(address && address!=(IPTR)-1 && width>=128 && width<=16384 && height>=64 && height<=16384 && pitch>=width*4 && pitch<=65536,"valid native RGBX framebuffer attributes");
    if(failed)goto done;
    for(i=0;i<256;i++)
    {
        struct DimensionInfo dim;
        id=NextDisplayInfo(id);if(id==0xffffffffUL)break;
        memset(&dim,0,sizeof(dim));
        if(!GetDisplayInfoData(NULL,(UBYTE *)&dim,sizeof(dim),DTAG_DIMS,id))continue;
        printf("mode %08x maxdepth=%u\n",(unsigned)id,dim.MaxDepth);fflush(stdout);
        if(dim.MaxDepth>8 && truecolor==0xffffffffUL)truecolor=id;
        if(dim.MaxDepth==8 && indexed==0xffffffffUL)indexed=id;
    }
    check(truecolor!=0xffffffffUL,"original truecolor mode remains available");
    check(indexed!=0xffffffffUL,"driver enumerates a genuine eight-bit mode");
    if(indexed==0xffffffffUL)goto done;
    scr=OpenScreenTags(NULL,SA_DisplayID,indexed,SA_Width,width,SA_Height,height,
                      SA_Depth,8,SA_Type,CUSTOMSCREEN,SA_Quiet,TRUE,SA_Behind,TRUE,TAG_DONE);
    check(scr!=NULL,"open LUT8 screen behind Workbench");
    if(!scr)goto done;
    check(GetBitMapAttr(scr->RastPort.BitMap,BMA_DEPTH)==8,"allocated bitmap has eight-bit storage depth");
    check(scr->ViewPort.ColorMap && scr->ViewPort.ColorMap->Count==256,"screen owns 256 palette entries");
    SetRGB32(&scr->ViewPort,1,0xffffffffUL,0,0);
    SetRGB32(&scr->ViewPort,2,0,0xffffffffUL,0);
    SetRGB32(&scr->ViewPort,3,0,0,0xffffffffUL);
    SetAPen(&scr->RastPort,1);RectFill(&scr->RastPort,32,32,47,47);
    SetAPen(&scr->RastPort,2);RectFill(&scr->RastPort,64,32,79,47);
    SetAPen(&scr->RastPort,3);RectFill(&scr->RastPort,96,32,111,47);
    check(ReadPixel(&scr->RastPort,40,40)==1 && ReadPixel(&scr->RastPort,72,40)==2 && ReadPixel(&scr->RastPort,104,40)==3,"source pixels retain their palette indices");
    ScreenToFront(scr);Delay(5);
    check(pixel(fb,pitch,40,40,255,0,0),"pre-rendered red visible in physical framebuffer");
    check(pixel(fb,pitch,72,40,0,255,0),"pre-rendered green visible in physical framebuffer");
    check(pixel(fb,pitch,104,40,0,0,255),"pre-rendered blue visible in physical framebuffer");
    SetRGB32(&scr->ViewPort,1,0xffffffffUL,0xffffffffUL,0);Delay(5);
    check(ReadPixel(&scr->RastPort,40,40)==1,"palette update does not rewrite source indices");
    check(pixel(fb,pitch,40,40,255,255,0),"live palette update recolors physical pixels without redraw");
    check(pixel(fb,pitch,72,40,0,255,0) && pixel(fb,pitch,104,40,0,0,255),"unmodified palette entries preserve physical colors");
done:
    if(scr)check(CloseScreen(scr),"close native indexed screen");
    printf("gfx_framebuffer_palette_native: %u checks, %u failed\n",checks,failed);fflush(stdout);
    return failed?10:0;
}

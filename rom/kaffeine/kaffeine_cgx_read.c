/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Typed read-only CGX operations; native writes only private scratch. */
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <string.h>
#include <stdint.h>
#include "kaffeine_intuibase.h"
#include "kaffeine_screens.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks_gfx.h"

static struct BitMap *bitmap(struct KaffeineContext *ctx, ULONG guest)
{
    struct BitMap *bm=Kaffeine_IBaseBitmap(ctx,guest);struct Screen *s=NULL;int kind;
    if(bm)return bm;
    kind=Kaffeine_ScreenBitMapQuery(ctx,guest,&bm);
    if(kind==KAFFEINE_SCREEN_QUERY_BORROWED)return bm;
    if(Kaffeine_ScreenBitMapOf(ctx,guest,&s)==KAFFEINE_SCREEN_BITMAP_OWNED && s)return s->RastPort.BitMap;
    return NULL;
}
IPTR Kaffeine_GetCyberMapAttr(struct KaffeineContext *ctx, void *cpu)
{
    struct BitMap *bm=bitmap(ctx,THUNK_A(0));ULONG attribute=THUNK_D(0);
    if(!bm)return 0;
    switch(attribute)
    {
    case CYBRMATTR_XMOD:case CYBRMATTR_BPPIX:case CYBRMATTR_PIXFMT:
    case CYBRMATTR_WIDTH:case CYBRMATTR_HEIGHT:case CYBRMATTR_DEPTH:
    case CYBRMATTR_ISCYBERGFX:case CYBRMATTR_ISLINEARMEM:
        return (ULONG)GetCyberMapAttr(bm,attribute);
    default: return ~0UL; /* no display/native-memory pointer attribute */
    }
}
static ULONG pixel_bytes(ULONG format)
{
    switch(format)
    {
    case RECTFMT_RGB:case RECTFMT_BGR24:return 3;
    case RECTFMT_RGBA:case RECTFMT_ARGB:case RECTFMT_BGRA32:case RECTFMT_ABGR32:
    case RECTFMT_0RGB32:case RECTFMT_BGR032:case RECTFMT_RGB032:case RECTFMT_0BGR32:return 4;
    case RECTFMT_RGB15:case RECTFMT_BGR15:case RECTFMT_RGB15PC:case RECTFMT_BGR15PC:
    case RECTFMT_RGB16:case RECTFMT_BGR16:case RECTFMT_RGB16PC:case RECTFMT_BGR16PC:return 2;
    default:return 0;
    }
}
IPTR Kaffeine_ReadPixelArray(struct KaffeineContext *ctx, void *cpu)
{
    ULONG dst=THUNK_A(0),rpguest=THUNK_A(1),dx=(UWORD)THUNK_D(0),dy=(UWORD)THUNK_D(1),stride=(UWORD)THUNK_D(2);
    ULONG x=(UWORD)THUNK_D(3),y=(UWORD)THUNK_D(4),w=(UWORD)THUNK_D(5),h=(UWORD)THUNK_D(6),format=(UBYTE)THUNK_D(7);
    ULONG bpp=pixel_bytes(format),row,offset,span,i;uint64_t start,extent,total;
    struct RastPort *native,copy;UBYTE *scratch;ULONG result;
    if(!w || !h)return 0;
    if(!bpp || w>65535/bpp)return 0;
    row=w*bpp;start=(uint64_t)dy*stride+(uint64_t)dx*bpp;
    extent=start+(uint64_t)(h-1)*stride+row;total=(uint64_t)row*h;
    if(stride<(dx+w)*bpp || extent>UINT32_MAX || total>16*1024*1024 ||
       !Kaffeine_ProgramMemory(ctx,dst,(ULONG)extent))return 0;
    native=Kaffeine_IBaseRastPort(ctx,rpguest);
    if(!native)native=Kaffeine_RastPortIn(ctx,rpguest);
    if(!native || !native->BitMap || !GetCyberMapAttr(native->BitMap,CYBRMATTR_ISCYBERGFX))return 0;
    if((uint64_t)x+w>GetBitMapAttr(native->BitMap,BMA_WIDTH) ||
       (uint64_t)y+h>GetBitMapAttr(native->BitMap,BMA_HEIGHT))return 0;
    scratch=AllocVec((ULONG)total,MEMF_PUBLIC);if(!scratch)return 0;
    offset=(ULONG)start;span=(ULONG)extent;(void)span;
    /* Preserve untouched clipped pixels, destination prefixes and padding. */
    for(i=0;i<h;++i)memcpy(scratch+i*row,ctx->mem+dst+offset+i*stride,row);
    copy=*native;
    result=ReadPixelArray(scratch,0,0,row,&copy,x,y,w,h,format);
    if(result<=w*h)
        for(i=0;i<h;++i)memcpy(ctx->mem+dst+offset+i*stride,scratch+i*row,row);
    else result=0;
    FreeVec(scratch);return result;
}

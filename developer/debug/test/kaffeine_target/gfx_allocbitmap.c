/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   OS bitmap ownership and pixel round trips, with no custom-chip access. */
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>

struct GfxBase *GfxBase;
static unsigned checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static void round_trip(ULONG flags)
{
    struct BitMap *a = AllocBitMap(1024, 768, 3, BMF_CLEAR | flags, NULL);
    struct BitMap *b = a ? AllocBitMap(1024, 768, 3, BMF_CLEAR, a) : NULL;
    struct RastPort ra, rb;
    unsigned p, x, y;
    int exact = 1;
    CHECK(a && b);
    if (!a || !b) goto done;
    CHECK(GetBitMapAttr(a, BMA_WIDTH) >= 1024 && GetBitMapAttr(a, BMA_HEIGHT) >= 768);
    CHECK(GetBitMapAttr(a, BMA_DEPTH) >= 3);
    if (GetBitMapAttr(a, BMA_FLAGS) & BMF_STANDARD)
    {
        CHECK(a->BytesPerRow >= 128 && a->Rows >= 768 && a->Depth >= 3);
        for (p=0;p<3;p++)
            for (y=0;y<768;y++)
                for (x=0;x<128;x++)
                    if (a->Planes[p][y * a->BytesPerRow + x]) exact = 0;
        CHECK(exact);
        a->Planes[0][0] = 0x80; /* direct classic plane access */
    }
    InitRastPort(&ra); InitRastPort(&rb);
    ra.BitMap = a; rb.BitMap = b;
    SetAPen(&ra, 5);
    WritePixel(&ra, 1023, 767);
    CHECK(ReadPixel(&ra, 1023, 767) == 5);
    CHECK(BltBitMap(a,0,0,b,0,0,1024,768,0xC0,7,NULL) == 3);
    WaitBlit();
    CHECK(ReadPixel(&rb, 1023, 767) == 5);
    if (GetBitMapAttr(a, BMA_FLAGS) & BMF_STANDARD)
        CHECK(ReadPixel(&rb, 0, 0) == 1);
done:
    if (b) FreeBitMap(b);
    if (a) FreeBitMap(a);
}

static void pen_arrays(void)
{
    struct BitMap *b=AllocBitMap(48,8,3,BMF_CLEAR,NULL);
    struct BitMap *scratch=b ? AllocBitMap(32,1,3,BMF_CLEAR,b) : NULL;
    struct RastPort rp,temp,other;
    struct BitMap *copy=NULL;
    UBYTE input[96],output[96];
    unsigned x,y;
    int exact=1;
    CHECK(b && scratch);
    if (!b || !scratch) goto done;
    InitRastPort(&rp); InitRastPort(&temp);
    rp.BitMap=b; temp.BitMap=scratch;
    for (x=0;x<96;x++) input[x]=x&7;
    memset(output,0xa5,sizeof(output));
    CHECK(WritePixelArray8(&rp,3,2,19,4,input,&temp)==51);
    CHECK(ReadPixelArray8(&rp,3,2,19,4,output,&temp)==51);
    for (y=0;y<3;y++) for(x=0;x<32;x++)
        if (output[y*32+x] != (x<17 ? input[y*32+x] : 0xa5)) exact=0;
    CHECK(exact);
    CHECK(WritePixelLine8(&rp,1,6,17,input,&temp)==17);
    CHECK(ReadPixelLine8(&rp,1,6,17,output,&temp)==17);
    CHECK(!memcmp(input,output,17));
    copy=AllocBitMap(48,8,3,BMF_CLEAR,b);
    CHECK(copy != NULL);
    if (copy)
    {
        InitRastPort(&other);other.BitMap=copy;
        ClipBlit(&rp,3,2,&other,2,1,17,3,0xc0);
        memset(output,0xa5,sizeof(output));
        CHECK(ReadPixelArray8(&other,2,1,18,3,output,&temp)==51);
        exact=1;
        for (y=0;y<3;y++) for(x=0;x<32;x++)
            if (output[y*32+x] != (x<17 ? input[y*32+x] : 0xa5)) exact=0;
        CHECK(exact);
        ClipBlit(&rp,3,2,&rp,4,2,17,3,0xc0);
        memset(output,0xa5,sizeof(output));
        CHECK(ReadPixelArray8(&rp,4,2,20,4,output,&temp)==51);
        exact=1;
        for (y=0;y<3;y++) for(x=0;x<32;x++)
            if (output[y*32+x] != (x<17 ? input[y*32+x] : 0xa5)) exact=0;
        CHECK(exact);
        CHECK((GetBitMapAttr(copy,BMA_FLAGS)&BMF_STANDARD)!=0);
        if(GetBitMapAttr(copy,BMA_FLAGS)&BMF_STANDARD)
        {
            for(x=0;x<copy->Depth;x++)memset(copy->Planes[x],0,copy->BytesPerRow*copy->Rows);
            BltBitMapRastPort(b,4,2,&other,2,1,17,3,0xc0);
            memset(output,0xa5,sizeof(output));
            CHECK(ReadPixelArray8(&other,2,1,18,3,output,&temp)==51);
            exact=1;
            for(y=0;y<3;y++)for(x=0;x<32;x++)
                if(output[y*32+x]!=(x<17?input[y*32+x]:0xa5))exact=0;
            CHECK(exact);
        }
    }
done:
    if (copy) FreeBitMap(copy);
    if (scratch) FreeBitMap(scratch);
    if (b) FreeBitMap(b);
}

int main(void)
{
    struct BitMap *deep, *display;
    struct RastPort rp;
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    CHECK(GfxBase != NULL);
    if (GfxBase)
    {
        round_trip(0);
        round_trip(BMF_INTERLEAVED | BMF_MINPLANES);
        pen_arrays();
        deep = AllocBitMap(32, 16, 24, BMF_CLEAR, NULL);
        CHECK(deep != NULL);
        if (deep)
        {
            CHECK(GetBitMapAttr(deep, BMA_DEPTH) >= 24);
            CHECK(!(GetBitMapAttr(deep, BMA_FLAGS) & BMF_STANDARD));
            FreeBitMap(deep);
        }
        display = AllocBitMap(32,16,3,BMF_CLEAR | BMF_DISPLAYABLE,NULL);
        CHECK(display != NULL);
        if (display)
        {
            InitRastPort(&rp);
            rp.BitMap = display;
            SetAPen(&rp,5);
            CHECK(WritePixel(&rp,7,5) != -1);
            CHECK(ReadPixel(&rp,7,5) == 5);
            FreeBitMap(display);
        }
        CHECK(AllocBitMap(0,16,1,0,NULL) == NULL);
        CloseLibrary((struct Library *)GfxBase);
    }
    printf("gfx_allocbitmap: %u checks, %u failed\n", checks, failed);
    return failed ? 10 : 0;
}

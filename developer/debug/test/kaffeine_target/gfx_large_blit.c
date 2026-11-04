/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Original OS-call large planar rectangle probe; no custom-chip access. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>

struct GfxBase *GfxBase;
static unsigned checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static int bit(const UBYTE *p, ULONG row, unsigned x, unsigned y)
{ return (p[y * row + x / 8] >> (7 - (x & 7))) & 1; }
static void put(UBYTE *p, ULONG row, unsigned x, unsigned y, int value)
{
    UBYTE mask = (UBYTE)(0x80U >> (x & 7));
    if (value) p[y * row + x / 8] |= mask;
    else p[y * row + x / 8] &= (UBYTE)~mask;
}
static void rectangle(UWORD width, UWORD height, WORD sx, WORD dx, WORD w)
{
    struct BitMap src, dst;
    ULONG row = ((width + 15UL) / 16) * 2, size = row * height, i;
    UBYTE *s = AllocMem(size + 16, MEMF_CHIP | MEMF_PUBLIC);
    UBYTE *d = AllocMem(size + 16, MEMF_CHIP | MEMF_PUBLIC);
    UBYTE *expected = AllocMem(size, MEMF_PUBLIC);
    unsigned x, y;
    CHECK(s && d && expected);
    if (!s || !d || !expected) goto done;
    memset(s, 0xa5, size + 16); memset(d, 0x5a, size + 16);
    memset(expected, 0x5a, size);
    for (i = 0; i < size; ++i) s[8 + i] = (UBYTE)(i * 37U + 17U);
    InitBitMap(&src, 1, width, height); InitBitMap(&dst, 1, width, height);
    src.Planes[0] = s + 8; dst.Planes[0] = d + 8;
    CHECK(src.BytesPerRow == row && src.Rows == height && src.Depth == 1);
    for (y = 0; y < height; ++y)
        for (x = 0; x < (unsigned)w; ++x)
            put(expected, row, dx + x, y, bit(s + 8, row, sx + x, y));
    CHECK(BltBitMap(&src, sx, 0, &dst, dx, 0, w, height, 0xc0, 1, NULL) == 1);
    WaitBlit();
    CHECK(!memcmp(d + 8, expected, size));
    for (i = 0; i < size; ++i)
        if (s[8 + i] != (UBYTE)(i * 37U + 17U)) break;
    CHECK(i == size);
    for (i = 0; i < 8; ++i)
        if (s[i] != 0xa5 || s[size + 8 + i] != 0xa5 ||
            d[i] != 0x5a || d[size + 8 + i] != 0x5a) break;
    CHECK(i == 8);
done:
    if (expected) FreeMem(expected, size);
    if (d) FreeMem(d, size + 16);
    if (s) FreeMem(s, size + 16);
}
int main(void)
{
    PLANEPTR plane;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    CHECK(GfxBase != NULL);
    if (GfxBase)
    {
        CHECK(GfxBase->ChipRevBits0 == GFXF_BIG_BLITS);
        CHECK(SetChipRev(SETCHIPREV_BEST) == GFXF_BIG_BLITS);
        CHECK(GfxBase->ChipRevBits0 == GFXF_BIG_BLITS);
        rectangle(1120, 4, 3, 7, 1100);
        rectangle(16, 1100, 0, 0, 16);
        plane = AllocRaster(2048, 1100);
        CHECK(plane != NULL);
        if (plane) FreeRaster(plane, 2048, 1100);
        CloseLibrary((struct Library *)GfxBase);
    }
    printf("gfx_large_blit: %u checks, %u failed\n", checks, failed);
    return failed ? 10 : 0;
}

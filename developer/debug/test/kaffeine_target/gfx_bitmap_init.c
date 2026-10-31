/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original AROS InitBitMap metadata and plane-preservation oracle. */
#include <exec/types.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>
#include "m68ktest.h"

#ifndef BMF_STANDARD
#define BMF_STANDARD (1U << 3)
#endif
struct GfxBase *GfxBase;

struct CheckedBitmap {
    UBYTE before[16];
    struct BitMap bitmap;
    UBYTE after[16];
};

static void verify(struct CheckedBitmap *box, PLANEPTR *planes,
                   UWORD bytes, UWORD rows, UBYTE depth)
{
    unsigned i;
    BOOL guards = TRUE;
    check(box->bitmap.BytesPerRow == bytes, "InitBitMap rounds the narrowed pixel width");
    check(box->bitmap.Rows == rows, "InitBitMap publishes the narrowed row count");
    check(box->bitmap.Depth == depth, "InitBitMap publishes the depth byte");
    check(box->bitmap.Flags == BMF_STANDARD, "AROS InitBitMap sets the standard planar flag");
    check(box->bitmap.pad == 0, "InitBitMap clears the metadata pad");
    for (i = 0; i < 8; i++)
        check(box->bitmap.Planes[i] == planes[i], "InitBitMap preserves every caller plane pointer");
    for (i = 0; i < 16; i++)
        if (box->before[i] != 0xA5 || box->after[i] != 0xA5) guards = FALSE;
    check(guards, "InitBitMap leaves surrounding storage untouched");
}

int main(void)
{
    struct CheckedBitmap box;
    UBYTE storage[8];
    PLANEPTR planes[8];
    unsigned i;
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!check(GfxBase != NULL, "open graphics")) goto done;
    memset(&box, 0xA5, sizeof(box));
    for (i = 0; i < 8; i++)
        box.bitmap.Planes[i] = planes[i] = i == 7 ? NULL : storage + i;
    puts("STEP: initialize a caller-owned bitmap descriptor");
    InitBitMap(&box.bitmap, 3, 49, 25);
    verify(&box, planes, 8, 25, 3);
    InitBitMap(&box.bitmap, 0, 0, 0);
    verify(&box, planes, 0, 0, 0);
    InitBitMap(&box.bitmap, (BYTE)-1, (UWORD)65535, (UWORD)65535);
    verify(&box, planes, 8192, 65535, 255);
done:
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_bitmap_init");
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Native and classic ABI oracle for the original upfront layer constructor. */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <string.h>
#include <stdio.h>
static unsigned checks, failures;
static int check(int ok, const char *message)
{
    ++checks;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    fflush(stdout);
    return ok;
}
static void check_long(const char *message, LONG value, LONG expected)
{
    check(value == expected, message);
}
struct GfxBase *GfxBase;
struct Library *LayersBase;
static ULONG pixel(struct BitMap *bm, ULONG x, ULONG y)
{
    ULONG p, value = 0;
    for (p = 0; p < bm->Depth; ++p)
        if (((UBYTE *)bm->Planes[p])[y * bm->BytesPerRow + (x >> 3)] & (0x80 >> (x & 7)))
            value |= 1UL << p;
    return value;
}
int main(void)
{
    struct BitMap bm;
    struct Layer_Info *info = NULL;
    struct Layer *one = NULL, *two = NULL;
    ULONG p, bytes = 16 * 96;
    memset(&bm, 0, sizeof(bm));
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);
    if (!check(GfxBase && LayersBase, "open graphics and layers")) goto done;
#ifdef __AROS__
    InitBitMap(&bm, 3, 128, 96);
#else
    bm.BytesPerRow = 16; bm.Rows = 96; bm.Depth = 3;
#endif
    for (p = 0; p < 3; ++p) {
        bm.Planes[p] = AllocMem(bytes, MEMF_PUBLIC);
        if (!check(bm.Planes[p] != NULL, "allocate an accessible planar bitplane")) goto done;
        memset(bm.Planes[p], 0xFF, bytes);
    }
    info = NewLayerInfo();
    if (!check(info != NULL, "allocate an owned LayerInfo")) goto done;
    one = CreateUpfrontLayer(info, &bm, 0, 0, 127, 95, LAYERSIMPLE, NULL);
    if (!check(one != NULL, "classic upfront constructor returns a layer")) goto done;
    check(one->LayerInfo == info && info->top_layer == one, "LayerInfo and layer share typed identity");
    check(one->rp && one->rp->Layer == one, "layer RastPort has typed identity");
    check_long("default constructor backfill clears existing guest planes", pixel(&bm, 8, 8), 0);
    SetAPen(one->rp, 3); RectFill(one->rp, 4, 4, 12, 12);
    check_long("classic layer drawing reaches original planes", pixel(&bm, 8, 8), 3);
    two = CreateUpfrontLayer(info, &bm, 32, 24, 95, 71, LAYERSIMPLE, NULL);
    if (!check(two != NULL, "second classic layer on the same bitmap")) goto done;
    check(info->top_layer == two && two->back == one && one->front == two, "classic layers retain native ordering and guest links");
    check(DeleteLayer(0, two) != 0, "delete second classic layer"); two = NULL;
    check(info->top_layer == one && one->front == NULL, "deletion restores first layer identity");
    check(DeleteLayer(0, one) != 0, "delete first classic layer"); one = NULL;
    DisposeLayerInfo(info); info = NULL;
done:
    if (two) DeleteLayer(0, two);
    if (one) DeleteLayer(0, one);
    if (info) DisposeLayerInfo(info);
    for (p = 0; p < 3; ++p) if (bm.Planes[p]) FreeMem(bm.Planes[p], bytes);
    if (LayersBase) CloseLibrary(LayersBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    printf("gfx_classic_layer: %u checks, %u failed\n", checks, failures);
    return failures ? 10 : 0;
}

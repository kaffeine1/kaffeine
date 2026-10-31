/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Poison collaborators of the private layers of the layer bridge, for
   tests that never create one. Tests exercising them supply strong
   recording definitions instead. */
#include <stdlib.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <proto/graphics.h>
#include <proto/layers.h>

/* The native recipe entry is linked with the layer bridge. These older
   fixtures never replay a paint recipe; its focused suite supplies strong
   recording graphics functions instead. Unexpected replay must fail. */
__attribute__((weak)) void SetAPen(struct RastPort *rp, ULONG pen)
{
    (void)rp; (void)pen;
    abort();
}

__attribute__((weak)) void SetDrMd(struct RastPort *rp, ULONG mode)
{
    (void)rp; (void)mode;
    abort();
}

__attribute__((weak)) void RectFill(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1)
{
    (void)rp; (void)x0; (void)y0; (void)x1; (void)y1;
    abort();
}

__attribute__((weak)) struct Hook *InstallLayerInfoHook(struct Layer_Info *li, struct Hook *hook)
{
    (void)li; (void)hook;
    abort();
}

__attribute__((weak)) struct Layer *CreateUpfrontHookLayer(struct Layer_Info *li, struct BitMap *bm,
                                                           LONG x0, LONG y0, LONG x1, LONG y1,
                                                           LONG flags, struct Hook *hook,
                                                           struct BitMap *bm2)
{
    (void)li; (void)bm; (void)x0; (void)y0; (void)x1; (void)y1; (void)flags; (void)hook; (void)bm2;
    abort();
}

__attribute__((weak)) LONG DeleteLayer(LONG dummy, struct Layer *layer)
{
    (void)dummy; (void)layer;
    abort();
}

__attribute__((weak)) struct Region *InstallClipRegion(struct Layer *layer, struct Region *region)
{
    (void)layer; (void)region;
    abort();
}

__attribute__((weak)) void InitBitMap(struct BitMap *bm, BYTE depth, UWORD width, UWORD height)
{
    (void)bm; (void)depth; (void)width; (void)height;
    abort();
}

/* The native Regions of the Region bridge; test_regions defines them */
__attribute__((weak)) struct Region *NewRegion(void)
{
    abort();
}

__attribute__((weak)) void DisposeRegion(struct Region *region)
{
    (void)region;
    abort();
}

__attribute__((weak)) BOOL OrRectRegion(struct Region *region, struct Rectangle *rect)
{
    (void)region; (void)rect;
    abort();
}

__attribute__((weak)) BOOL OrRegionRegion(struct Region *r1, struct Region *r2)
{
    (void)r1; (void)r2;
    abort();
}

__attribute__((weak)) LONG BltBitMap(struct BitMap *srcBitMap, WORD xSrc, WORD ySrc,
                                     struct BitMap *destBitMap, WORD xDest, WORD yDest,
                                     WORD xSize, WORD ySize, UBYTE minterm, UBYTE mask,
                                     PLANEPTR tempA)
{
    (void)srcBitMap; (void)xSrc; (void)ySrc; (void)destBitMap; (void)xDest; (void)yDest;
    (void)xSize; (void)ySize; (void)minterm; (void)mask; (void)tempA;
    abort();
}

__attribute__((weak)) void WaitBlit(void)
{
    abort();
}

/* The size of the BitMap of a screen that the program opened */
__attribute__((weak)) IPTR GetBitMapAttr(struct BitMap *bitmap, ULONG attribute)
{
    (void)bitmap; (void)attribute;
    abort();
}

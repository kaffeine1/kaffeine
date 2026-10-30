#pragma once
#include <exec/types.h>
#include <exec/semaphores.h>
#include <graphics/gfx.h>
/* The native Layer of AROS (compiler/include/graphics/clip.h): field names,
   order and types, with host pointer width; the semaphore and the private
   tail stay opaque, since no bridge code reads them. */
struct ClipRect;
struct RastPort;
struct Layer_Info;
struct Hook;
struct Region;
struct Layer
{
    struct Layer *front;
    struct Layer *back;
    struct ClipRect *ClipRect;
    struct RastPort *rp;
    struct Rectangle bounds;
    struct Layer *parent;
    UWORD priority;
    UWORD Flags;
    struct BitMap *SuperBitMap;
    struct ClipRect *SuperClipRect;
    APTR Window;
    WORD Scroll_X;
    WORD Scroll_Y;
    struct ClipRect *cr, *cr2, *crnew, *SuperSaveClipRects, *_cliprects;
    struct Layer_Info *LayerInfo;
    struct SignalSemaphore Lock;
    struct Hook *BackFill;
    struct Region *VisibleRegion;
    struct Region *ClipRegion;
    struct Region *saveClipRects;
    WORD Width;
    WORD Height;
    struct Region *DamageList;
};

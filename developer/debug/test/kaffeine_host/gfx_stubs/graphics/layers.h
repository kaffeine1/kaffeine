#pragma once
#include <graphics/clip.h>
/* The values of compiler/include/graphics/layers.h that the bridge uses.
   struct Layer_Info comes with intuition/screens.h of these stubs. */
#define LAYERSIMPLE       (1<<0)
#define LAYERSMART        (1<<1)
#define LAYERSUPER        (1<<2)
#define LAYERUPDATING     (1<<4)
#define LAYERBACKDROP     (1<<6)
#define LAYERREFRESH      (1<<7)
#define NEWLAYERINFO_CALLED 1
#define LAYERS_BACKFILL   ((struct Hook *)0)
#define LAYERS_NOBACKFILL ((struct Hook *)1)

/* Backfill hook message */
struct BackFillMessage
{
    struct Layer     *Layer;
    struct Rectangle  Bounds;
    LONG              OffsetX;
    LONG              OffsetY;
};

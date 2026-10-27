#pragma once
#include <exec/types.h>
#include <graphics/gfx.h>
/* The native Region of AROS (compiler/include/graphics/regions.h), with
   host pointer width */
struct RegionRectangle;
struct Region
{
    struct Rectangle bounds;
    struct RegionRectangle *RegionRectangle;
};
struct RegionRectangle
{
    struct RegionRectangle *Next;
    struct RegionRectangle *Prev;
    struct Rectangle bounds;
};

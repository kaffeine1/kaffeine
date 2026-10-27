#pragma once
#include <exec/types.h>
typedef UBYTE *PLANEPTR;
struct BitMap
{
    UWORD BytesPerRow, Rows;
    UBYTE Flags, Depth;
    UWORD pad;
    PLANEPTR Planes[8];
};
struct Rectangle
{
    WORD MinX, MinY;
    WORD MaxX, MaxY;
};

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
#define BMF_DISPLAYABLE (1L<<1)
#define BMF_INTERLEAVED (1L<<2)
#define BMF_STANDARD    (1L<<3)
/* GetBitMapAttr() attributes */
#define BMA_HEIGHT 0
#define BMA_DEPTH  4
#define BMA_WIDTH  8
#define BMA_FLAGS  12
struct Rectangle
{
    WORD MinX, MinY;
    WORD MaxX, MaxY;
};

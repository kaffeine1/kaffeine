#pragma once
#include <exec/types.h>
/* The native RastPort of AROS, without its private space */
struct TextFont;
struct RastPort
{
    struct Layer *Layer;
    struct BitMap *BitMap;
    const UWORD *AreaPtrn;
    struct TmpRas *TmpRas;
    struct AreaInfo *AreaInfo;
    struct GelsInfo *GelsInfo;
    UBYTE Mask;
    BYTE FgPen;
    BYTE BgPen;
    BYTE AOlPen;
    BYTE DrawMode;
    BYTE AreaPtSz;
    BYTE linpatcnt;
    BYTE dummy;
    UWORD Flags;
    UWORD LinePtrn;
    WORD cp_x;
    WORD cp_y;
    UBYTE minterms[8];
    WORD PenWidth;
    WORD PenHeight;
    struct TextFont *Font;
    UBYTE AlgoStyle;
    UBYTE TxFlags;
    UWORD TxHeight;
    UWORD TxWidth;
    UWORD TxBaseline;
    WORD TxSpacing;
    APTR *RP_User;
};
#define JAM1 0
#define JAM2 1
#define COMPLEMENT 2
#define INVERSVID 4

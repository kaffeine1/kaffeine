#pragma once
#include <exec/nodes.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>

/* Opaque native layer storage; no bridge code reads its contents. */
struct Layer_Info { APTR unused; };

/* Field names, order and types from compiler/include/intuition/screens.h.
   Host pointers have host width; the tests check guest offsets separately. */
struct Screen
{
    struct Screen *NextScreen;
    struct Window *FirstWindow;
    WORD LeftEdge, TopEdge, Width, Height, MouseY, MouseX;
    UWORD Flags;
    UBYTE *Title, *DefaultTitle;
    BYTE BarHeight, BarVBorder, BarHBorder, MenuVBorder, MenuHBorder;
    BYTE WBorTop, WBorLeft, WBorRight, WBorBottom;
    struct TextAttr *Font;
    struct ViewPort ViewPort;
    struct RastPort RastPort;
    struct BitMap BitMap_OBSOLETE;
    struct Layer_Info LayerInfo;
    struct Gadget *FirstGadget;
    UBYTE DetailPen, BlockPen;
    UWORD SaveColor0;
    struct Layer *BarLayer;
    UBYTE *ExtData, *UserData;
};

struct PubScreenNode
{
    struct Node psn_Node;
    struct Screen *psn_Screen;
    UWORD psn_Flags;
    WORD psn_Size, psn_VisitorCount;
    struct Task *psn_SigTask;
    UBYTE psn_SigBit;
};

#define PSNF_PRIVATE (1 << 0)
#define MAXPUBSCREENNAME 139

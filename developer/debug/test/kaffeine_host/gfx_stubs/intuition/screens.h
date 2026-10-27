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

/* OpenScreen() and OpenScreenTagList(), as compiler/include/intuition/screens.h
   declares them; host pointers have host width. */
#include <utility/tagitem.h>
struct Gadget;
struct NewScreen
{
    WORD LeftEdge, TopEdge, Width, Height;
    WORD Depth;
    UBYTE DetailPen, BlockPen;
    UWORD ViewModes;
    UWORD Type;
    struct TextAttr *Font;
    UBYTE *DefaultTitle;
    struct Gadget *Gadgets;
    struct BitMap *CustomBitMap;
};
#ifndef WBENCHSCREEN
#define WBENCHSCREEN 0x0001
#define PUBLICSCREEN 0x0002
#define CUSTOMSCREEN 0x000F
#endif
#define SCREENTYPE   0x000F
#define SHOWTITLE    (1<<4)
#define CUSTOMBITMAP (1<<6)
#define SCREENBEHIND (1<<7)
#define SCREENQUIET  (1<<8)
#define PENSHARED    (1<<10)
#define NS_EXTENDED  (1<<12)
#define AUTOSCROLL   (1<<14)
#define SA_Dummy           (TAG_USER + 32)
#define SA_Left            (SA_Dummy + 1)
#define SA_Top             (SA_Dummy + 2)
#define SA_Width           (SA_Dummy + 3)
#define SA_Height          (SA_Dummy + 4)
#define SA_Depth           (SA_Dummy + 5)
#define SA_DetailPen       (SA_Dummy + 6)
#define SA_BlockPen        (SA_Dummy + 7)
#define SA_Title           (SA_Dummy + 8)
#define SA_Colors          (SA_Dummy + 9)
#define SA_ErrorCode       (SA_Dummy + 10)
#define SA_Font            (SA_Dummy + 11)
#define SA_SysFont         (SA_Dummy + 12)
#define SA_Type            (SA_Dummy + 13)
#define SA_BitMap          (SA_Dummy + 14)
#define SA_PubName         (SA_Dummy + 15)
#define SA_PubSig          (SA_Dummy + 16)
#define SA_PubTask         (SA_Dummy + 17)
#define SA_DisplayID       (SA_Dummy + 18)
#define SA_DClip           (SA_Dummy + 19)
#define SA_Overscan        (SA_Dummy + 20)
#define SA_ShowTitle       (SA_Dummy + 22)
#define SA_Behind          (SA_Dummy + 23)
#define SA_Quiet           (SA_Dummy + 24)
#define SA_AutoScroll      (SA_Dummy + 25)
#define SA_Pens            (SA_Dummy + 26)
#define SA_FullPalette     (SA_Dummy + 27)
#define SA_ColorMapEntries (SA_Dummy + 28)
#define SA_Parent          (SA_Dummy + 29)
#define SA_Draggable       (SA_Dummy + 30)
#define SA_Exclusive       (SA_Dummy + 31)
#define SA_SharePens       (SA_Dummy + 32)
#define SA_BackFill        (SA_Dummy + 33)
#define SA_Interleaved     (SA_Dummy + 34)
#define SA_Colors32        (SA_Dummy + 35)
#define SA_VideoControl    (SA_Dummy + 36)
#define SA_FrontChild      (SA_Dummy + 37)
#define SA_BackChild       (SA_Dummy + 38)
#define SA_LikeWorkbench   (SA_Dummy + 39)
#define SA_MinimizeISG     (SA_Dummy + 41)
#define OSERR_NOMONITOR    1
#define OSERR_NOCHIPS      2
#define OSERR_NOMEM        3
#define OSERR_NOCHIPMEM    4
#define OSERR_PUBNOTUNIQUE 5
#define OSERR_UNKNOWNMODE  6
#define OSERR_TOODEEP      7
#define OSERR_ATTACHFAIL   8
#define OSERR_NOTAVAILABLE 9

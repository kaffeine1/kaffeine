#pragma once
#include <exec/types.h>
#include <exec/ports.h>
/* The fields of the native Window and IntuiMessage that the bridge uses */
#include <graphics/text.h>
struct Screen;
struct RastPort;
struct Menu;
struct Gadget;
struct Requester
{
    struct Requester *OlderRequest;
    struct Gadget *ReqGadget;
};
struct Window
{
    WORD LeftEdge, TopEdge, Width, Height, MouseY, MouseX;
    WORD MinWidth, MinHeight;
    UWORD MaxWidth, MaxHeight;
    ULONG Flags;
    struct Menu *MenuStrip;
    UBYTE *Title;
    struct Screen *WScreen;
    struct RastPort *RPort;
    BYTE BorderLeft, BorderTop, BorderRight, BorderBottom;
    ULONG IDCMPFlags;
    struct MsgPort *UserPort;
    UBYTE DetailPen, BlockPen;
    WORD GZZMouseX, GZZMouseY, GZZWidth, GZZHeight;
    struct Gadget *FirstGadget;
    struct Requester *FirstRequest;
};
#ifndef __typedef_RAWARG
#define __typedef_RAWARG
typedef void *RAWARG;
#endif
struct EasyStruct
{
    ULONG es_StructSize;
    ULONG es_Flags;
    CONST_STRPTR es_Title;
    CONST_STRPTR es_TextFormat;
    CONST_STRPTR es_GadgetFormat;
};
struct IntuiMessage
{
    struct Message ExecMessage;
    ULONG Class;
    UWORD Code;
    UWORD Qualifier;
    APTR IAddress;
    WORD MouseX, MouseY;
    ULONG Seconds, Micros;
    struct Window *IDCMPWindow;
    struct IntuiMessage *SpecialLink;
};
struct TabletData;
struct ExtIntuiMessage
{
    struct IntuiMessage eim_IntuiMessage;
    struct TabletData *eim_TabletData;
};
#define IDCMP_NEWSIZE 0x00000002
#define IDCMP_MOUSEMOVE 0x00000010
#define IDCMP_GADGETDOWN 0x00000020
#define IDCMP_GADGETUP 0x00000040
#define IDCMP_MENUVERIFY 0x00002000
#define IDCMP_ACTIVEWINDOW 0x00040000
#define IDCMP_INTUITICKS 0x00400000
#define IDCMP_IDCMPUPDATE 0x00800000
#define IDCMP_MENUPICK 0x00000100
#define IDCMP_MENUHELP 0x01000000

/* Menus, as AROS lays them out */
struct IntuiText
{
    UBYTE FrontPen, BackPen, DrawMode;
    WORD LeftEdge, TopEdge;
    struct TextAttr *ITextFont;
    UBYTE *IText;
    struct IntuiText *NextText;
};
struct Image
{
    WORD LeftEdge, TopEdge, Width, Height, Depth;
    UWORD *ImageData;
    UBYTE PlanePick, PlaneOnOff;
    struct Image *NextImage;
};
struct Border
{
    WORD LeftEdge, TopEdge;
    UBYTE FrontPen, BackPen, DrawMode;
    BYTE Count;
    WORD *XY;
    struct Border *NextBorder;
};
struct MenuItem
{
    struct MenuItem *NextItem;
    WORD LeftEdge, TopEdge, Width, Height;
    UWORD Flags;
    LONG MutualExclude;
    APTR ItemFill, SelectFill;
    BYTE Command;
    struct MenuItem *SubItem;
    UWORD NextSelect;
};
struct Menu
{
    struct Menu *NextMenu;
    WORD LeftEdge, TopEdge, Width, Height;
    UWORD Flags;
    CONST_STRPTR MenuName;
    struct MenuItem *FirstItem;
    WORD JazzX, JazzY, BeatX, BeatY;
};
#define MENUENABLED 0x0001
#define MIDRAWN     0x0100
#define CHECKIT     0x0001
#define ITEMTEXT    0x0002
#define COMMSEQ     0x0004
#define MENUTOGGLE  0x0008
#define ITEMENABLED 0x0010
#define HIGHFLAGS   0x00C0
#define HIGHIMAGE   0x0000
#define HIGHCOMP    0x0040
#define HIGHBOX     0x0080
#define HIGHNONE    0x00C0
#define CHECKED     0x0100
#define ISDRAWN     0x1000
#define HIGHITEM    0x2000
#define MENUTOGGLED 0x4000
#define MENUNULL    0xFFFF
#define NOMENU      0x001F
#define NOITEM      0x003F
#define NOSUB       0x001F
#define MENUNUM(n)  ((n) & 0x1F)
#define ITEMNUM(n)  (((n) >> 5) & 0x003F)
#define SUBNUM(n)   (((n) >> 11) & 0x001F)
#define FULLMENUNUM(menu, item, sub) \
    ((((sub) & 0x1F) << 11) | (((item) & 0x3F) << 5) | ((menu) & 0x1F))

/* Gadgets, as AROS lays them out */
struct Gadget
{
    struct Gadget *NextGadget;
    WORD LeftEdge, TopEdge, Width, Height;
    UWORD Flags, Activation, GadgetType;
    APTR GadgetRender, SelectRender;
    struct IntuiText *GadgetText;
    IPTR MutualExclude;
    APTR SpecialInfo;
    UWORD GadgetID;
    APTR UserData;
};
struct ExtGadget
{
    struct ExtGadget *NextGadget;
    WORD LeftEdge, TopEdge, Width, Height;
    UWORD Flags, Activation, GadgetType;
    APTR GadgetRender, SelectRender;
    struct IntuiText *GadgetText;
    IPTR MutualExclude;
    APTR SpecialInfo;
    UWORD GadgetID;
    APTR UserData;
    ULONG MoreFlags;
    WORD BoundsLeftEdge, BoundsTopEdge, BoundsWidth, BoundsHeight;
};
struct StringExtend;
struct KeyMap;
struct StringInfo
{
    UBYTE *Buffer, *UndoBuffer;
    WORD BufferPos, MaxChars, DispPos, UndoPos, NumChars, DispCount, CLeft, CTop;
    struct StringExtend *Extension;
    LONG LongInt;
    struct KeyMap *AltKeyMap;
};
struct PropInfo
{
    UWORD Flags, HorizPot, VertPot, HorizBody, VertBody, CWidth, CHeight;
    UWORD HPotRes, VPotRes, LeftBorder, TopBorder;
};
#define GFLG_GADGHIGHBITS  0x0003
#define GFLG_GADGHCOMP     0x0000
#define GFLG_GADGHBOX      0x0001
#define GFLG_GADGHIMAGE    0x0002
#define GFLG_GADGHNONE     0x0003
#define GFLG_GADGIMAGE     0x0004
#define GFLG_SELECTED      0x0080
#define GFLG_DISABLED      0x0100
#define GFLG_STRINGEXTEND  0x0400
#define GFLG_LABELMASK     0x3000
#define GFLG_LABELITEXT    0x0000
#define GFLG_LABELSTRING   0x1000
#define GFLG_LABELIMAGE    0x2000
#define GFLG_EXTENDED      0x8000
#define GACT_RELVERIFY     0x0001
#define GACT_IMMEDIATE     0x0002
#define GACT_TOGGLESELECT  0x0100
#define GACT_LONGINT       0x0800
#define GACT_ALTKEYMAP     0x1000
#define GACT_BOOLEXTEND    0x2000
#define GACT_STRINGEXTEND  0x2000
#define GACT_ACTIVEGADGET  0x4000
#define GTYP_GTYPEMASK     0x0007
#define GTYP_SYSTYPEMASK   0x00F0
#define GTYP_BOOLGADGET    0x0001
#define GTYP_REQGADGET     0x1000
#define GTYP_GZZGADGET     0x2000
#define GTYP_SCRGADGET     0x4000
#define GTYP_SYSGADGET     0x8000
#define GTYP_PROPGADGET    0x0003
#define GTYP_STRGADGET     0x0004
#define GTYP_CUSTOMGADGET  0x0005
#define GMORE_BOUNDS       0x00000001
#define GMORE_GADGETHELP   0x00000002
#define GMORE_SCROLLRASTER 0x00000004
#define GMORE_BOOPSIGADGET 0x00000008
/* the pens of a DrawInfo, as AROS's screens.h counts them */
#define NUMDRIPENS         12
#define AUTOKNOB           0x0001
#define FREEHORIZ          0x0002
#define FREEVERT           0x0004
#define KNOBHIT            0x0100
#define MAXBODY            0xFFFF
#define MAXPOT             0xFFFF

/* The tags of OpenWindowTagList() that the bridge converts, and those that
   OpenWindow() gives for the fields of a NewWindow */
#include <utility/tagitem.h>
#define WA_Dummy             (TAG_USER + 99)
#define WA_Left              (WA_Dummy + 1)
#define WA_Top               (WA_Dummy + 2)
#define WA_Width             (WA_Dummy + 3)
#define WA_Height            (WA_Dummy + 4)
#define WA_DetailPen         (WA_Dummy + 5)
#define WA_BlockPen          (WA_Dummy + 6)
#define WA_IDCMP             (WA_Dummy + 7)
#define WA_Flags             (WA_Dummy + 8)
#define WA_Gadgets           (WA_Dummy + 9)
#define WA_Title             (WA_Dummy + 11)
#define WA_ScreenTitle       (WA_Dummy + 12)
#define WA_CustomScreen      (WA_Dummy + 13)
#define WA_MinWidth          (WA_Dummy + 15)
#define WA_MinHeight         (WA_Dummy + 16)
#define WA_MaxWidth          (WA_Dummy + 17)
#define WA_MaxHeight         (WA_Dummy + 18)
#define WA_PubScreenName     (WA_Dummy + 21)
#define WA_PubScreen         (WA_Dummy + 22)
#define WA_PubScreenFallBack (WA_Dummy + 23)

/* The screen types of a NewWindow, from intuition/screens.h */
#define WBENCHSCREEN 0x0001
#define PUBLICSCREEN 0x0002
#define CUSTOMSCREEN 0x000F

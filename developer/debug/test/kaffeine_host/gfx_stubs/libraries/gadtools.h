#pragma once
#include <exec/types.h>
#include <utility/tagitem.h>
#include <intuition/intuition.h>
/* The menu part of GadTools, as AROS declares it */
struct NewMenu
{
    UBYTE nm_Type;
    CONST_STRPTR nm_Label;
    CONST_STRPTR nm_CommKey;
    UWORD nm_Flags;
    LONG nm_MutualExclude;
    APTR nm_UserData;
};
#define NM_END    0x0000
#define NM_TITLE  0x0001
#define NM_ITEM   0x0002
#define NM_SUB    0x0003
#define NM_IGNORE 0x0040
#define IM_ITEM   0x0082
#define IM_SUB    0x0083
#define NM_BARLABEL ((STRPTR)-1)
#define NM_MENUDISABLED  MENUENABLED
#define NM_ITEMDISABLED  ITEMENABLED
#define NM_COMMANDSTRING COMMSEQ
#define GTMENU_USERDATA(menu)     (*((APTR *)(((struct Menu *)menu) + 1)))
#define GTMENUITEM_USERDATA(item) (*((APTR *)(((struct MenuItem *)item) + 1)))
#define GTMENU_TRIMMED 0x00000001
#define GTMENU_INVALID 0x00000002
#define GTMENU_NOMEM   0x00000003
#define GT_TagBase (TAG_USER + 0x80000)
#define GTMN_TextAttr       (GT_TagBase + 49)
#define GTMN_FrontPen       (GT_TagBase + 50)
#define GTMN_Menu           (GT_TagBase + 60)
#define GTMN_FullMenu       (GT_TagBase + 62)
#define GTMN_SecondaryError (GT_TagBase + 63)
#define GTMN_Checkmark      (GT_TagBase + 65)
#define GTMN_AmigaKey       (GT_TagBase + 66)
#define GTMN_NewLookMenus   (GT_TagBase + 67)

/* DrawBevelBoxA() */
#define GTBB_Recessed       (GT_TagBase + 51)
#define GT_VisualInfo       (GT_TagBase + 52)
#define GTBB_FrameType      (GT_TagBase + 77)
#define BBFT_BUTTON         1
#define BBFT_RIDGE          2
#define BBFT_ICONDROPBOX    3

/* The gadget part of GadTools, as AROS declares it */
struct NewGadget
{
    WORD ng_LeftEdge, ng_TopEdge, ng_Width, ng_Height;
    CONST_STRPTR ng_GadgetText;
    struct TextAttr *ng_TextAttr;
    UWORD ng_GadgetID;
    ULONG ng_Flags;
    APTR ng_VisualInfo;
    APTR ng_UserData;
};
#define GENERIC_KIND  0
#define BUTTON_KIND   1
#define CHECKBOX_KIND 2
#define INTEGER_KIND  3
#define LISTVIEW_KIND 4
#define MX_KIND       5
#define NUMBER_KIND   6
#define CYCLE_KIND    7
#define PALETTE_KIND  8
#define SCROLLER_KIND 9
#define SLIDER_KIND   11
#define STRING_KIND   12
#define TEXT_KIND     13
#define GTYP_GADTOOLS 0x0100
#define PLACETEXT_LEFT  0x0001
#define PLACETEXT_RIGHT 0x0002
#define PLACETEXT_ABOVE 0x0004
#define PLACETEXT_IN    0x0010
#define NG_HIGHLABEL    0x0020
#define GTCB_Checked         (GT_TagBase + 4)
#define GTLV_Top             (GT_TagBase + 5)
#define GTLV_Labels          (GT_TagBase + 6)
#define GTLV_ReadOnly        (GT_TagBase + 7)
#define GTLV_ScrollWidth     (GT_TagBase + 8)
#define GTLV_ShowSelected    (GT_TagBase + 53)
#define GTLV_Selected        (GT_TagBase + 54)
#define GTLV_MakeVisible     (GT_TagBase + 78)
#define GTLV_ItemHeight      (GT_TagBase + 79)
#define GTLV_MaxPen          (GT_TagBase + 84)
#define GTLV_Total           (GT_TagBase + 150)
#define GTLV_Visible         (GT_TagBase + 151)
#define GTMX_Labels          (GT_TagBase + 9)
#define GTMX_Active          (GT_TagBase + 10)
#define GTTX_Text            (GT_TagBase + 11)
#define GTTX_CopyText        (GT_TagBase + 12)
#define GTNM_Number          (GT_TagBase + 13)
#define GTCY_Labels          (GT_TagBase + 14)
#define GTCY_Active          (GT_TagBase + 15)
#define GTPA_Depth           (GT_TagBase + 16)
#define GTPA_Color           (GT_TagBase + 17)
#define GTPA_ColorOffset     (GT_TagBase + 18)
#define GTPA_IndicatorWidth  (GT_TagBase + 19)
#define GTPA_IndicatorHeight (GT_TagBase + 20)
#define GTSC_Top             (GT_TagBase + 21)
#define GTSC_Total           (GT_TagBase + 22)
#define GTSC_Visible         (GT_TagBase + 23)
#define GTSC_Overlap         (GT_TagBase + 24)
#define GTSL_Min             (GT_TagBase + 38)
#define GTSL_Max             (GT_TagBase + 39)
#define GTSL_Level           (GT_TagBase + 40)
#define GTSL_MaxLevelLen     (GT_TagBase + 41)
#define GTSL_LevelFormat     (GT_TagBase + 42)
#define GTSL_LevelPlace      (GT_TagBase + 43)
#define GTSL_DispFunc        (GT_TagBase + 44)
#define GTST_String          (GT_TagBase + 45)
#define GTST_MaxChars        (GT_TagBase + 46)
#define GTIN_Number          (GT_TagBase + 47)
#define GTIN_MaxChars        (GT_TagBase + 48)
#define GTST_EditHook        (GT_TagBase + 55)
#define GTIN_EditHook        GTST_EditHook
#define GTTX_Border          (GT_TagBase + 57)
#define GTNM_Border          (GT_TagBase + 58)
#define GTSC_Arrows          (GT_TagBase + 59)
#define GTMX_Spacing         (GT_TagBase + 61)
#define GT_Underscore        (GT_TagBase + 64)
#define GTCB_Scaled          (GT_TagBase + 68)
#define GTMX_Scaled          (GT_TagBase + 69)
#define GTPA_NumColors       (GT_TagBase + 70)
#define GTMX_TitlePlace      (GT_TagBase + 71)
#define GTTX_FrontPen        (GT_TagBase + 72)
#define GTNM_FrontPen        (GT_TagBase + 72)
#define GTTX_BackPen         (GT_TagBase + 73)
#define GTNM_BackPen         (GT_TagBase + 73)
#define GTTX_Justification   (GT_TagBase + 74)
#define GTNM_Justification   (GT_TagBase + 74)
#define GTJ_LEFT   0
#define GTJ_RIGHT  1
#define GTJ_CENTER 2
#define GTNM_Format          (GT_TagBase + 75)
#define GTNM_MaxNumberLen    (GT_TagBase + 76)
#define GTSL_MaxPixelLen     (GT_TagBase + 80)
#define GTSL_Justification   (GT_TagBase + 81)
#define GTPA_ColorTable      (GT_TagBase + 82)
#define GTLV_CallBack        (GT_TagBase + 83)
#define GTTX_Clipped         (GT_TagBase + 85)
#define GTNM_Clipped         (GT_TagBase + 85)

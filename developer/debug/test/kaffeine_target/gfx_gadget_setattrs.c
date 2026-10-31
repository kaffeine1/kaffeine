/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Changing the attributes of GadTools gadgets with GT_SetGadgetAttrsA(),
    with no input needed. For a gadget of every kind but LISTVIEW_KIND and
    GENERIC_KIND, the program submits each attribute that the NDK autodoc of
    GT_SetGadgetAttrsA() gives for its kind: GA_Disabled, GTCB_Checked,
    GTCY_Labels, GTCY_Active, GTMX_Active, GTPA_Color, GTPA_ColorOffset,
    GTPA_ColorTable, GTTX_Text, GTNM_Number, GTSL_Min, GTSL_Max, GTSL_Level,
    GTSC_Top, GTSC_Total, GTSC_Visible, GTST_String and GTIN_Number are read
    back with GT_GetGadgetAttrsA(), while GTTX_FrontPen, GTTX_BackPen,
    GTTX_Justification, GTNM_FrontPen, GTNM_BackPen, GTNM_Justification,
    GTNM_Format, GTSL_LevelFormat, GTSL_Justification and a NULL
    GTSL_DispFunc are accepted without an effect that the program can read
    back. Right after GT_SetGadgetAttrsA(), before any GT_GetGadgetAttrsA(),
    the program checks the 68k structures that the setter updates: Flags &
    GFLG_DISABLED of the button, GFLG_SELECTED of the checkbox, HorizPot and
    VertPot of the slider's and scroller's PropInfo, Buffer of the string's
    StringInfo and LongInt and Buffer of the integer's StringInfo. Where the
    result comes from AROS's classes or the bridge, the checks follow them:
    GTCY_Labels resets the active choice to 0 before a later GTCY_Active in
    the same list applies, an out-of-range GTMX_Active is ignored,
    GTSL_Level is clamped to GTSL_Min..GTSL_Max, GTSC_Top is kept within
    GTSC_Total less GTSC_Visible, GTST_String is cut to GTST_MaxChars and
    NULL empties the buffer, GTTX_Text NULL clears the text, GTTX_Text after
    GTTX_CopyText borrows the new string, and the button takes the first
    GA_Disabled of a list while the other kinds take the last of a repeated
    tag. Also TAG_MORE, TAG_SKIP and TAG_IGNORE, NULL for no gadget or no
    tag list, a window part for GT_SetGadgetAttrsA() on the palette,
    OnGadget() and OffGadget() with and without a window and on the scroller
    with its arrows, ModifyProp() on the slider and RefreshGadgets(), and
    the guest memory back after FreeGadgets().
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

/* What a storage holds until an attribute fills it */
#define UNTOUCHED 0x5A5A5A5AUL

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
static STRPTR cycle_labels[] = { (STRPTR)"Low", (STRPTR)"Middle", (STRPTR)"High", NULL };
static STRPTR other_labels[] = { (STRPTR)"Cold", (STRPTR)"Warm", (STRPTR)"Hot", NULL };
static STRPTR mx_labels[] = { (STRPTR)"One", (STRPTR)"Two", (STRPTR)"Three", NULL };
static UBYTE colors[8] = { 3, 2, 1, 0, 3, 2, 1, 0 };
static UBYTE other_colors[8] = { 1, 0, 3, 2, 1, 0, 3, 2 };
static char scratch[16];

static struct NewGadget ng;

static void place(WORD top, UWORD id)
{
    ng.ng_TopEdge = top;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = id;
}

static LONG get1(struct Gadget *g, Tag tag, ULONG *value)
{
    *value = UNTOUCHED;
    return GT_GetGadgetAttrs(g, NULL, NULL, tag, (ULONG)value, TAG_DONE);
}

int main(void)
{
    struct Gadget *glist = NULL, *context, *button, *checkbox, *cycle, *mx, *palette, *text;
    struct Gadget *copied, *number, *slider, *scroller, *string, *integer, *g;
    struct PropInfo *slider_pi, *scroller_pi;
    struct TagItem more[3], skip[5];
    struct StringInfo *si;
    struct Screen *scr;
    struct Window *win;
    ULONG a, b, c, d;
    UWORD pot, body;
    ULONG avail;
    APTR vi;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        return test_done("gfx_gadget_setattrs");

    scr = LockPubScreen(NULL);
    vi = scr ? GetVisualInfoA(scr, NULL) : NULL;
    if (!check(scr && vi, "a VisualInfo of the public screen"))
        return test_done("gfx_gadget_setattrs");

    /* Open and close a first window, as gfx_menus.c does, so that the
       screen font that the bridge keeps for a window is there before the
       guest memory is counted */
    win = OpenWindowTags(NULL, WA_Left, 10, WA_Top, 15, WA_InnerWidth, 240,
                         WA_InnerHeight, 80, TAG_DONE);
    if (win)
        CloseWindow(win);
    avail = AvailMem(MEMF_ANY);

    context = CreateContext(&glist);
    ng.ng_LeftEdge = 100;
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    ng.ng_TextAttr = &topaz8;
    ng.ng_VisualInfo = vi;

    place(10, 1);
    button = CreateGadget(BUTTON_KIND, context, &ng, GA_Disabled, TRUE, TAG_DONE);
    place(30, 2);
    checkbox = CreateGadget(CHECKBOX_KIND, button, &ng, GTCB_Checked, TRUE, TAG_DONE);
    place(50, 3);
    cycle = CreateGadget(CYCLE_KIND, checkbox, &ng, GTCY_Labels, (ULONG)cycle_labels,
                         GTCY_Active, 2, TAG_DONE);
    place(70, 4);
    mx = CreateGadget(MX_KIND, cycle, &ng, GTMX_Labels, (ULONG)mx_labels, GTMX_Active, 1, TAG_DONE);
    place(110, 5);
    palette = CreateGadget(PALETTE_KIND, mx, &ng, GTPA_NumColors, 4, GTPA_ColorTable, (ULONG)colors,
                           GTPA_Color, 2, TAG_DONE);
    place(130, 6);
    text = CreateGadget(TEXT_KIND, palette, &ng, GTTX_Text, (ULONG)"Hello", TAG_DONE);
    strcpy(scratch, "Copied");
    place(150, 7);
    copied = CreateGadget(TEXT_KIND, text, &ng, GTTX_Text, (ULONG)scratch, GTTX_CopyText, TRUE, TAG_DONE);
    strcpy(scratch, "Reused");
    place(170, 8);
    number = CreateGadget(NUMBER_KIND, copied, &ng, GTNM_Number, -1234, TAG_DONE);
    place(190, 9);
    slider = CreateGadget(SLIDER_KIND, number, &ng, GTSL_Min, -10, GTSL_Max, 10, GTSL_Level, -5,
                          GTSL_MaxLevelLen, 3, TAG_DONE);
    place(210, 10);
    ng.ng_Width = 16;
    ng.ng_Height = 80;
    scroller = CreateGadget(SCROLLER_KIND, slider, &ng, GTSC_Arrows, 12, PGA_Freedom, LORIENT_VERT,
                            GTSC_Top, 5, GTSC_Total, 100, GTSC_Visible, 10, TAG_DONE);
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    place(300, 11);
    string = CreateGadget(STRING_KIND, scroller, &ng, GTST_String, (ULONG)"Hello",
                          GTST_MaxChars, 5, TAG_DONE);
    place(320, 12);
    integer = CreateGadget(INTEGER_KIND, string, &ng, GTIN_Number, -42, TAG_DONE);
    if (!check(context && button && checkbox && cycle && mx && palette && text && copied &&
               number && slider && scroller && string && integer, "a gadget of each kind"))
    {
        FreeGadgets(glist);
        return test_done("gfx_gadget_setattrs");
    }

    /* BUTTON_KIND: GA_Disabled; AROS's buttonclass.c finds the first
       GA_Disabled with FindTagItem() and ignores the rest */
    GT_SetGadgetAttrs(button, NULL, NULL, GA_Disabled, FALSE, TAG_DONE);
    check(!(button->Flags & GFLG_DISABLED),
          "button: GFLG_DISABLED cleared in the 68k Gadget");
    check(get1(button, GA_Disabled, &a) == 1 && a == 0,
          "button: GA_Disabled FALSE read back");
    GT_SetGadgetAttrs(button, NULL, NULL, GA_Disabled, TRUE, TAG_DONE);
    check((button->Flags & GFLG_DISABLED) != 0,
          "button: GFLG_DISABLED set in the 68k Gadget");
    check(get1(button, GA_Disabled, &a) == 1 && a == 1,
          "button: GA_Disabled TRUE read back");
    GT_SetGadgetAttrs(button, NULL, NULL, GA_Disabled, FALSE, GA_Disabled, TRUE, TAG_DONE);
    check(!(button->Flags & GFLG_DISABLED),
          "button: the first GA_Disabled of a list clears GFLG_DISABLED");
    check(get1(button, GA_Disabled, &a) == 1 && a == 0,
          "button: the first GA_Disabled of a list counts");

    /* CHECKBOX_KIND: GTCB_Checked and GA_Disabled */
    GT_SetGadgetAttrs(checkbox, NULL, NULL, GTCB_Checked, FALSE, GA_Disabled, TRUE, TAG_DONE);
    check(!(checkbox->Flags & GFLG_SELECTED) && (checkbox->Flags & GFLG_DISABLED),
          "checkbox: GFLG_SELECTED cleared and GFLG_DISABLED set");
    check_long("checkbox: GTCB_Checked and GA_Disabled, counted",
               GT_GetGadgetAttrs(checkbox, NULL, NULL, GTCB_Checked, (ULONG)&a,
                                 GA_Disabled, (ULONG)&b, TAG_DONE), 2);
    check(a == 0 && b == 1, "checkbox: unchecked and disabled");
    GT_SetGadgetAttrs(checkbox, NULL, NULL, GTCB_Checked, TRUE, GA_Disabled, FALSE, TAG_DONE);
    check((checkbox->Flags & GFLG_SELECTED) && !(checkbox->Flags & GFLG_DISABLED),
          "checkbox: GFLG_SELECTED set and GFLG_DISABLED cleared");
    check(GT_GetGadgetAttrs(checkbox, NULL, NULL, GTCB_Checked, (ULONG)&a,
                            GA_Disabled, (ULONG)&b, TAG_DONE) == 2 &&
          a == 1 && b == 0,
          "checkbox: checked and enabled again");

    /* CYCLE_KIND: GTCY_Labels, GTCY_Active and GA_Disabled; in AROS's
       cycleclass.c, GTCY_Labels resets the active choice to 0 before a
       GTCY_Active later in the same list applies */
    GT_SetGadgetAttrs(cycle, NULL, NULL, GTCY_Labels, (ULONG)other_labels, GA_Disabled, TRUE, TAG_DONE);
    check_long("cycle: GTCY_Active, GTCY_Labels and GA_Disabled, counted",
               GT_GetGadgetAttrs(cycle, NULL, NULL, GTCY_Active, (ULONG)&a, GTCY_Labels, (ULONG)&b,
                                 GA_Disabled, (ULONG)&c, TAG_DONE), 3);
    check_long("cycle: GTCY_Labels resets GTCY_Active to 0", (LONG)a, 0);
    check(b == (ULONG)other_labels, "cycle: the new GTCY_Labels array");
    check(c == 1 && (cycle->Flags & GFLG_DISABLED), "cycle: GA_Disabled sets GFLG_DISABLED");
    GT_SetGadgetAttrs(cycle, NULL, NULL, GTCY_Labels, (ULONG)cycle_labels, GTCY_Active, 1,
                      GA_Disabled, FALSE, TAG_DONE);
    check(GT_GetGadgetAttrs(cycle, NULL, NULL, GTCY_Active, (ULONG)&a, GTCY_Labels, (ULONG)&b,
                            GA_Disabled, (ULONG)&c, TAG_DONE) == 3 &&
          a == 1 && b == (ULONG)cycle_labels && c == 0 && !(cycle->Flags & GFLG_DISABLED),
          "cycle: a GTCY_Active after GTCY_Labels in the same list applies");

    /* MX_KIND: GTMX_Active and GA_Disabled; in AROS's mxclass.c, an
       GTMX_Active at or above the number of labels is ignored */
    GT_SetGadgetAttrs(mx, NULL, NULL, GTMX_Active, 2, GA_Disabled, TRUE, TAG_DONE);
    check_long("mx: GTMX_Active and GA_Disabled, counted",
               GT_GetGadgetAttrs(mx, NULL, NULL, GTMX_Active, (ULONG)&a,
                                 GA_Disabled, (ULONG)&b, TAG_DONE), 2);
    check(a == 2 && b == 1 && (mx->Flags & GFLG_DISABLED), "mx: active choice 2 and disabled");
    GT_SetGadgetAttrs(mx, NULL, NULL, GTMX_Active, 3, GA_Disabled, FALSE, TAG_DONE);
    check(GT_GetGadgetAttrs(mx, NULL, NULL, GTMX_Active, (ULONG)&a,
                            GA_Disabled, (ULONG)&b, TAG_DONE) == 2 &&
          a == 2 && b == 0 && !(mx->Flags & GFLG_DISABLED),
          "mx: GTMX_Active at the number of labels is ignored, GA_Disabled cleared");

    /* PALETTE_KIND: GTPA_Color, GTPA_ColorOffset, GTPA_ColorTable and GA_Disabled */
    GT_SetGadgetAttrs(palette, NULL, NULL, GTPA_ColorTable, (ULONG)other_colors,
                      GTPA_ColorOffset, 1, GTPA_Color, 3, GA_Disabled, TRUE, TAG_DONE);
    check_long("palette: GTPA_Color, GTPA_ColorOffset, GTPA_ColorTable and GA_Disabled, counted",
               GT_GetGadgetAttrs(palette, NULL, NULL, GTPA_Color, (ULONG)&a,
                                 GTPA_ColorOffset, (ULONG)&b, GTPA_ColorTable, (ULONG)&c,
                                 GA_Disabled, (ULONG)&d, TAG_DONE), 4);
    check(a == 3 && b == 1, "palette: the new pen and colour offset");
    check(c == (ULONG)other_colors, "palette: the new GTPA_ColorTable");
    check(d == 1 && (palette->Flags & GFLG_DISABLED), "palette: GA_Disabled sets GFLG_DISABLED");
    GT_SetGadgetAttrs(palette, NULL, NULL, GA_Disabled, FALSE, TAG_DONE);
    check(get1(palette, GA_Disabled, &a) == 1 && a == 0 && !(palette->Flags & GFLG_DISABLED),
          "palette: enabled again");

    /* TEXT_KIND: GTTX_Text, GTTX_FrontPen, GTTX_BackPen and
       GTTX_Justification; in AROS's textclass.c, GTTX_Text NULL clears the
       text, and the first GTTX_Text after GTTX_CopyText frees the copy and
       borrows the new string */
    GT_SetGadgetAttrs(text, NULL, NULL, GTTX_Text, (ULONG)"World", GTTX_FrontPen, 2,
                      GTTX_BackPen, 1, GTTX_Justification, GTJ_CENTER, TAG_DONE);
    check_long("text: GTTX_Text, counted", get1(text, GTTX_Text, &a), 1);
    check_str("text: the new GTTX_Text", (const char *)a, "World");
    GT_SetGadgetAttrs(text, NULL, NULL, GTTX_Text, (ULONG)NULL, TAG_DONE);
    check(get1(text, GTTX_Text, &a) == 1 && a == 0, "text: GTTX_Text NULL clears the text");
    GT_SetGadgetAttrs(copied, NULL, NULL, GTTX_Text, (ULONG)scratch, TAG_DONE);
    check(get1(copied, GTTX_Text, &a) == 1 && a == (ULONG)scratch &&
          !strcmp((const char *)a, "Reused"),
          "text: GTTX_Text after GTTX_CopyText borrows the program's string");

    /* NUMBER_KIND: GTNM_Number, GTNM_FrontPen, GTNM_BackPen,
       GTNM_Justification and GTNM_Format */
    GT_SetGadgetAttrs(number, NULL, NULL, GTNM_Number, 5678, GTNM_FrontPen, 2, GTNM_BackPen, 1,
                      GTNM_Justification, GTJ_RIGHT, GTNM_Format, (ULONG)"%ld%%", TAG_DONE);
    check_long("number: GTNM_Number, counted", get1(number, GTNM_Number, &a), 1);
    check_long("number: the new GTNM_Number", (LONG)a, 5678);

    /* SLIDER_KIND: GTSL_Min, GTSL_Max, GTSL_Level, GTSL_LevelFormat,
       GTSL_Justification, GTSL_DispFunc and GA_Disabled; in AROS's
       sliderclass.c, GTSL_Level is clamped to GTSL_Min..GTSL_Max */
    slider_pi = (struct PropInfo *)slider->SpecialInfo;
    pot = slider_pi ? slider_pi->HorizPot : 0;
    GT_SetGadgetAttrs(slider, NULL, NULL, GTSL_Min, -20, GTSL_Max, 20, GTSL_Level, 15,
                      GTSL_LevelFormat, (ULONG)"%3ld", GTSL_Justification, GTJ_RIGHT,
                      GA_Disabled, TRUE, TAG_DONE);
    check(slider_pi && slider_pi->HorizPot != pot, "slider: PropInfo.HorizPot updated");
    check_long("slider: GTSL_Min, GTSL_Max, GTSL_Level and GA_Disabled, counted",
               GT_GetGadgetAttrs(slider, NULL, NULL, GTSL_Min, (ULONG)&a, GTSL_Max, (ULONG)&b,
                                 GTSL_Level, (ULONG)&c, GA_Disabled, (ULONG)&d, TAG_DONE), 4);
    check((LONG)a == -20 && (LONG)b == 20 && (LONG)c == 15, "slider: the new range and level");
    check(d == 1 && (slider->Flags & GFLG_DISABLED), "slider: GA_Disabled sets GFLG_DISABLED");
    pot = slider_pi ? slider_pi->HorizPot : 0;
    GT_SetGadgetAttrs(slider, NULL, NULL, GTSL_Level, 50, TAG_DONE);
    check(slider_pi && slider_pi->HorizPot != pot,
          "slider: PropInfo.HorizPot updated when clamped to GTSL_Max");
    check(get1(slider, GTSL_Level, &a) == 1 && (LONG)a == 20,
          "slider: GTSL_Level clamped to GTSL_Max");
    pot = slider_pi ? slider_pi->HorizPot : 0;
    GT_SetGadgetAttrs(slider, NULL, NULL, GTSL_Level, -50, GA_Disabled, FALSE, TAG_DONE);
    check(slider_pi && slider_pi->HorizPot != pot,
          "slider: PropInfo.HorizPot updated when clamped to GTSL_Min");
    check(get1(slider, GTSL_Level, &a) == 1 && (LONG)a == -20 && !(slider->Flags & GFLG_DISABLED),
          "slider: GTSL_Level clamped to GTSL_Min and enabled");
    GT_SetGadgetAttrs(slider, NULL, NULL, GTSL_DispFunc, (ULONG)NULL, TAG_DONE);
    check(get1(slider, GTSL_Level, &a) == 1 && (LONG)a == -20,
          "slider: GTSL_DispFunc NULL keeps GTSL_Level");

    /* SCROLLER_KIND: GTSC_Top, GTSC_Total, GTSC_Visible and GA_Disabled;
       in AROS's propgclass.c, GTSC_Top is kept within GTSC_Total less
       GTSC_Visible */
    scroller_pi = (struct PropInfo *)scroller->SpecialInfo;
    pot = scroller_pi ? scroller_pi->VertPot : 0;
    GT_SetGadgetAttrs(scroller, NULL, NULL, GTSC_Top, 25, GTSC_Total, 200, GTSC_Visible, 20,
                      GA_Disabled, TRUE, TAG_DONE);
    check(scroller_pi && scroller_pi->VertPot != pot, "scroller: PropInfo.VertPot updated");
    check_long("scroller: GTSC_Top, GTSC_Total, GTSC_Visible and GA_Disabled, counted",
               GT_GetGadgetAttrs(scroller, NULL, NULL, GTSC_Top, (ULONG)&a, GTSC_Total, (ULONG)&b,
                                 GTSC_Visible, (ULONG)&c, GA_Disabled, (ULONG)&d, TAG_DONE), 4);
    check(a == 25 && b == 200 && c == 20, "scroller: the new top, total and visible");
    check(d == 1 && (scroller->Flags & GFLG_DISABLED), "scroller: GA_Disabled sets GFLG_DISABLED");
    pot = scroller_pi ? scroller_pi->VertPot : 0;
    GT_SetGadgetAttrs(scroller, NULL, NULL, GTSC_Top, 195, GA_Disabled, FALSE, TAG_DONE);
    check(scroller_pi && scroller_pi->VertPot != pot,
          "scroller: PropInfo.VertPot updated when GTSC_Top is clamped");
    check(GT_GetGadgetAttrs(scroller, NULL, NULL, GTSC_Top, (ULONG)&a,
                            GA_Disabled, (ULONG)&b, TAG_DONE) == 2 &&
          a == 180 && b == 0 && !(scroller->Flags & GFLG_DISABLED),
          "scroller: GTSC_Top kept within GTSC_Total less GTSC_Visible");

    /* STRING_KIND: GTST_String and GA_Disabled; in AROS's strgclass.c,
       GTST_String NULL copies an empty string into the buffer, and
       NumChars is updated only when the gadget is drawn */
    si = (struct StringInfo *)string->SpecialInfo;
    GT_SetGadgetAttrs(string, NULL, NULL, GTST_String, (ULONG)"World", GA_Disabled, TRUE, TAG_DONE);
    check_str("string: the new text in StringInfo.Buffer",
              si ? (const char *)si->Buffer : NULL, "World");
    check_long("string: GTST_String and GA_Disabled, counted",
               GT_GetGadgetAttrs(string, NULL, NULL, GTST_String, (ULONG)&a,
                                 GA_Disabled, (ULONG)&b, TAG_DONE), 2);
    check(si && a == (ULONG)si->Buffer, "string: GTST_String is the buffer of the StringInfo");
    check(b == 1 && (string->Flags & GFLG_DISABLED), "string: GA_Disabled sets GFLG_DISABLED");
    GT_SetGadgetAttrs(string, NULL, NULL, GTST_String, (ULONG)"Hello, world", TAG_DONE);
    check_str("string: GTST_String cut to GTST_MaxChars in StringInfo.Buffer",
              si ? (const char *)si->Buffer : NULL, "Hello");
    check(get1(string, GTST_String, &a) == 1 && si && a == (ULONG)si->Buffer,
          "string: GTST_String after truncation is StringInfo.Buffer");
    GT_SetGadgetAttrs(string, NULL, NULL, GTST_String, (ULONG)NULL, GA_Disabled, FALSE, TAG_DONE);
    check_str("string: GTST_String NULL empties StringInfo.Buffer",
              si ? (const char *)si->Buffer : NULL, "");
    check(GT_GetGadgetAttrs(string, NULL, NULL, GTST_String, (ULONG)&a,
                            GA_Disabled, (ULONG)&b, TAG_DONE) == 2 &&
          si && a == (ULONG)si->Buffer && b == 0 && !(string->Flags & GFLG_DISABLED),
          "string: GTST_String NULL keeps the buffer and clears GFLG_DISABLED");

    /* INTEGER_KIND: GTIN_Number and GA_Disabled; AROS's strgclass.c sets
       StringInfo.LongInt and formats the number into StringInfo.Buffer */
    si = (struct StringInfo *)integer->SpecialInfo;
    GT_SetGadgetAttrs(integer, NULL, NULL, GTIN_Number, 1024, GA_Disabled, TRUE, TAG_DONE);
    check(si && si->LongInt == 1024, "integer: StringInfo.LongInt is 1024");
    check_str("integer: StringInfo.Buffer has 1024",
              si ? (const char *)si->Buffer : NULL, "1024");
    check_long("integer: GTIN_Number and GA_Disabled, counted",
               GT_GetGadgetAttrs(integer, NULL, NULL, GTIN_Number, (ULONG)&a,
                                 GA_Disabled, (ULONG)&b, TAG_DONE), 2);
    check((LONG)a == 1024 && b == 1 && (integer->Flags & GFLG_DISABLED),
          "integer: GTIN_Number 1024 and disabled");
    GT_SetGadgetAttrs(integer, NULL, NULL, GTIN_Number, -99, GA_Disabled, FALSE, TAG_DONE);
    check(si && si->LongInt == -99, "integer: StringInfo.LongInt is -99");
    check_str("integer: StringInfo.Buffer has -99",
              si ? (const char *)si->Buffer : NULL, "-99");
    check(get1(integer, GTIN_Number, &a) == 1 && (LONG)a == -99 &&
          !(integer->Flags & GFLG_DISABLED),
          "integer: negative GTIN_Number and enabled");

    /* a repeated tag in one call (the last counts, except for the button),
       TAG_IGNORE, TAG_SKIP and TAG_MORE, and NULL gadget or tag list */
    GT_SetGadgetAttrs(slider, NULL, NULL, GTSL_Level, -5, GTSL_Level, 7, TAG_DONE);
    check(get1(slider, GTSL_Level, &a) == 1 && (LONG)a == 7,
          "a repeated tag in one call: the last counts");
    more[0].ti_Tag = GTSL_Max;
    more[0].ti_Data = 30;
    more[1].ti_Tag = GTSL_Level;
    more[1].ti_Data = 25;
    more[2].ti_Tag = TAG_DONE;
    skip[0].ti_Tag = TAG_IGNORE;
    skip[0].ti_Data = 0;
    skip[1].ti_Tag = TAG_SKIP;
    skip[1].ti_Data = 1;
    skip[2].ti_Tag = GTSL_Min;              /* passed over */
    skip[2].ti_Data = 10;
    skip[3].ti_Tag = GTSL_Min;
    skip[3].ti_Data = -30;
    skip[4].ti_Tag = TAG_MORE;
    skip[4].ti_Data = (ULONG)more;
    GT_SetGadgetAttrsA(slider, NULL, NULL, skip);
    check(GT_GetGadgetAttrs(slider, NULL, NULL, GTSL_Min, (ULONG)&a, GTSL_Max, (ULONG)&b,
                            GTSL_Level, (ULONG)&c, TAG_DONE) == 3 &&
          (LONG)a == -30 && (LONG)b == 30 && (LONG)c == 25,
          "through TAG_IGNORE, TAG_SKIP and TAG_MORE");
    GT_SetGadgetAttrs(NULL, NULL, NULL, GTSL_Level, 0, TAG_DONE);
    GT_SetGadgetAttrsA(slider, NULL, NULL, NULL);
    check(get1(slider, GTSL_Level, &a) == 1 && (LONG)a == 25,
          "NULL gadget or NULL tag list: nothing changes");

    /* Window part: GT_SetGadgetAttrs() with a window, OffGadget() and
       OnGadget() with and without a window and on the scroller with its
       arrows, ModifyProp() on the slider and RefreshGadgets() */
    win = OpenWindowTags(NULL, WA_Left, 10, WA_Top, 15, WA_InnerWidth, 260,
                         WA_InnerHeight, 120, WA_Title, (ULONG)"gfx_gadget_setattrs", TAG_DONE);
    if (check(win != NULL, "OpenWindowTags()"))
    {
        GT_SetGadgetAttrs(palette, win, NULL, GTPA_Color, 2, TAG_DONE);
        check(get1(palette, GTPA_Color, &a) == 1 && a == 2,
              "palette: GT_SetGadgetAttrs() with a window sets GTPA_Color");

        OffGadget(palette, win, NULL);
        check((palette->Flags & GFLG_DISABLED) != 0,
              "palette: OffGadget() sets GFLG_DISABLED");
        check(get1(palette, GA_Disabled, &a) == 1 && a == 1,
              "palette: OffGadget() sets GA_Disabled");
        OnGadget(palette, win, NULL);
        check(!(palette->Flags & GFLG_DISABLED),
              "palette: OnGadget() clears GFLG_DISABLED");
        check(get1(palette, GA_Disabled, &a) == 1 && a == 0,
              "palette: OnGadget() clears GA_Disabled");

        OffGadget(button, NULL, NULL);
        check(!(button->Flags & GFLG_DISABLED) &&
              get1(button, GA_Disabled, &a) == 1 && a == 0,
              "button: OffGadget() with a NULL window changes nothing");

        OffGadget(scroller, win, NULL);
        a = 0;
        b = 1;
        for (g = slider->NextGadget; g && g != string; g = g->NextGadget)
        {
            a++;
            if (!(g->Flags & GFLG_DISABLED))
                b = 0;
        }
        check(a == 3 && b && (scroller->Flags & GFLG_DISABLED),
              "scroller: OffGadget() disables the scroller and its two arrows");
        OnGadget(scroller, win, NULL);
        b = 1;
        for (g = slider->NextGadget; g && g != string; g = g->NextGadget)
        {
            if (g->Flags & GFLG_DISABLED)
                b = 0;
        }
        check(b && !(scroller->Flags & GFLG_DISABLED),
              "scroller: OnGadget() enables the scroller and its two arrows");

        pot = slider_pi ? slider_pi->HorizPot : 0;
        body = slider_pi ? slider_pi->HorizBody : 0;
        ModifyProp(slider, win, NULL, AUTOKNOB | FREEHORIZ,
                   (UWORD)(pot ^ 0x4000U), 0, (UWORD)(body ^ 0x2000U), 0);
        check(slider_pi && slider_pi->HorizPot == pot && slider_pi->HorizBody == body &&
              get1(slider, GTSL_Level, &a) == 1 && (LONG)a == 25,
              "slider: ModifyProp() leaves PropInfo and GTSL_Level as they were");

        RefreshGadgets(context, win, NULL);
        check(!(palette->Flags & GFLG_DISABLED) && !(scroller->Flags & GFLG_DISABLED) &&
              slider_pi && slider_pi->HorizPot == pot &&
              get1(slider, GTSL_Level, &a) == 1 && (LONG)a == 25,
              "RefreshGadgets() returns and changes nothing");

        CloseWindow(win);
    }

    FreeGadgets(glist);
    check_long("FreeGadgets() gives the guest memory back", AvailMem(MEMF_ANY), avail);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);

    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_setattrs");
}

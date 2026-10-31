/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The attributes of GadTools gadgets, read back with GT_GetGadgetAttrsA(),
    with no window and no input needed. A gadget of every kind but
    LISTVIEW_KIND, made with values that a program reads again: a disabled
    button, a checked checkbox, a cycle on its third label, an mx, a palette
    with a colour table, a text, a text that copies its string, a number, a
    slider and a scroller with negative and ordinary levels, a string, an
    integer and a generic gadget. The program checks every value and the
    count that the autodoc of GT_GetGadgetAttrsA() gives: one for each
    attribute filled in, also for repeated tags and through TAG_MORE,
    TAG_SKIP and TAG_IGNORE, none for an attribute of another kind or of a
    gadget without attributes, whose storage stays as it was.
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
/* The bits of MoreFlags that the NDK and AROS give the same meaning */
#define SHARED_MOREFLAGS (GMORE_BOUNDS | GMORE_GADGETHELP | GMORE_SCROLLRASTER)

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
static STRPTR cycle_labels[] = { (STRPTR)"Low", (STRPTR)"Middle", (STRPTR)"High", NULL };
static STRPTR mx_labels[] = { (STRPTR)"One", (STRPTR)"Two", (STRPTR)"Three", NULL };
static UBYTE colors[4] = { 3, 2, 1, 0 };
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
    struct Gadget *copied, *number, *slider, *scroller, *string, *integer, *generic, *g;
    struct TagItem more[2], skip[5];
    struct Screen *scr;
    ULONG a, b, c, d;
    APTR vi;
    ULONG avail;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        return test_done("gfx_gadget_attrs");

    scr = LockPubScreen(NULL);
    vi = scr ? GetVisualInfoA(scr, NULL) : NULL;
    if (!check(scr && vi, "a VisualInfo of the public screen"))
        return test_done("gfx_gadget_attrs");
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
    /* the program reuses its buffer, as GTTX_CopyText allows */
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
    string = CreateGadget(STRING_KIND, scroller, &ng, GTST_String, (ULONG)"Hello", TAG_DONE);
    place(320, 12);
    integer = CreateGadget(INTEGER_KIND, string, &ng, GTIN_Number, -42, TAG_DONE);
    place(340, 13);
    generic = CreateGadget(GENERIC_KIND, integer, &ng, TAG_DONE);
    if (!check(context && button && checkbox && cycle && mx && palette && text && copied &&
               number && slider && scroller && string && integer && generic, "a gadget of each kind"))
    {
        FreeGadgets(glist);
        return test_done("gfx_gadget_attrs");
    }

    check_long("GA_Disabled of the button, counted", get1(button, GA_Disabled, &a), 1);
    check_long("the button is disabled", (LONG)a, 1);
    check_long("checkbox: GTCB_Checked and GA_Disabled, counted",
               GT_GetGadgetAttrs(checkbox, NULL, NULL, GTCB_Checked, (ULONG)&a,
                                 GA_Disabled, (ULONG)&b, TAG_DONE), 2);
    check(a == 1 && b == 0, "a checked checkbox, enabled");
    check_long("cycle: GTCY_Active, GTCY_Labels and GA_Disabled, all counted",
               GT_GetGadgetAttrs(cycle, NULL, NULL, GTCY_Active, (ULONG)&a, GTCY_Labels, (ULONG)&b,
                                 GA_Disabled, (ULONG)&c, TAG_DONE), 3);
    check_long("GTCY_Active", (LONG)a, 2);
    check(b == (ULONG)cycle_labels, "GTCY_Labels is the program's own array");
    check_long("GTMX_Active", get1(mx, GTMX_Active, &a), 1);
    check_long("the active mx choice", (LONG)a, 1);
    check_long("palette: GTPA_Color, GTPA_ColorOffset and GTPA_ColorTable, all counted",
               GT_GetGadgetAttrs(palette, NULL, NULL, GTPA_Color, (ULONG)&a, GTPA_ColorOffset,
                                 (ULONG)&b, GTPA_ColorTable, (ULONG)&c, TAG_DONE), 3);
    check(a == 2 && b == 0, "the pen of the palette and its first colour");
    check(c == (ULONG)colors, "GTPA_ColorTable is the program's own table");
    check_long("GTTX_Text, counted", get1(text, GTTX_Text, &a), 1);
    check(a && !strcmp((const char *)a, "Hello"), "GTTX_Text is the text of the gadget");
    check_long("GTTX_Text of a text that copies its string", get1(copied, GTTX_Text, &a), 1);
    check(a && a != (ULONG)scratch && !strcmp((const char *)a, "Copied"),
          "the copy, not the reused buffer of the program");
    check_long("GTNM_Number, counted", get1(number, GTNM_Number, &a), 1);
    check_long("a negative GTNM_Number", (LONG)a, -1234);
    check_long("slider: GTSL_Min, GTSL_Max and GTSL_Level, counted",
               GT_GetGadgetAttrs(slider, NULL, NULL, GTSL_Min, (ULONG)&a, GTSL_Max, (ULONG)&b,
                                 GTSL_Level, (ULONG)&c, TAG_DONE), 3);
    check((LONG)a == -10 && (LONG)b == 10 && (LONG)c == -5, "the range and level, signed");
    check_long("scroller: GTSC_Top, GTSC_Total and GTSC_Visible, counted",
               GT_GetGadgetAttrs(scroller, NULL, NULL, GTSC_Top, (ULONG)&a, GTSC_Total, (ULONG)&b,
                                 GTSC_Visible, (ULONG)&c, TAG_DONE), 3);
    check(a == 5 && b == 100 && c == 10, "the top, total and visible of the scroller");
    check_long("GTST_String, counted", get1(string, GTST_String, &a), 1);
    check(string->SpecialInfo && a == (ULONG)((struct StringInfo *)string->SpecialInfo)->Buffer,
          "GTST_String is the buffer of the gadget's StringInfo");
    check(a && !strcmp((const char *)a, "Hello"), "with the text of the gadget");
    check_long("GTIN_Number, counted", get1(integer, GTIN_Number, &a), 1);
    check_long("a negative GTIN_Number", (LONG)a, -42);

    /* attributes that a gadget does not have */
    check(get1(button, GTSL_Level, &a) == 0 && a == UNTOUCHED,
          "an attribute of another kind: not counted, storage untouched");
    check(get1(text, GA_Disabled, &a) == 0 && a == UNTOUCHED, "a text has no GA_Disabled");
    check(get1(generic, GA_Disabled, &a) == 0 && a == UNTOUCHED, "a generic gadget has none");
    check(get1(context, GA_Disabled, &a) == 0 && a == UNTOUCHED, "the context has none");
    g = slider->NextGadget;
    check(g && g != scroller && get1(g, GA_Disabled, &a) == 0 && a == UNTOUCHED,
          "an arrow that GadTools put before the scroller has none");
    check(GT_GetGadgetAttrs(NULL, NULL, NULL, GA_Disabled, (ULONG)&a, TAG_DONE) == 0,
          "no gadget: 0");
    check(GT_GetGadgetAttrsA(button, NULL, NULL, NULL) == 0, "no tag list: 0");

    /* repeated tags and the control tags */
    a = b = c = d = UNTOUCHED;
    check_long("a repeated tag is filled and counted each time",
               GT_GetGadgetAttrs(slider, NULL, NULL, GTSL_Level, (ULONG)&a, GTSL_Level, (ULONG)&b,
                                 TAG_DONE), 2);
    check((LONG)a == -5 && (LONG)b == -5, "both storages");
    more[0].ti_Tag = GTSL_Max;
    more[0].ti_Data = (ULONG)&c;
    more[1].ti_Tag = TAG_DONE;
    skip[0].ti_Tag = TAG_IGNORE;
    skip[0].ti_Data = 0;
    skip[1].ti_Tag = TAG_SKIP;
    skip[1].ti_Data = 1;
    skip[2].ti_Tag = GTSL_Min;              /* passed over */
    skip[2].ti_Data = (ULONG)&d;
    skip[3].ti_Tag = GTSL_Min;
    skip[3].ti_Data = (ULONG)&a;
    skip[4].ti_Tag = TAG_MORE;
    skip[4].ti_Data = (ULONG)more;
    a = c = d = UNTOUCHED;
    check_long("through TAG_IGNORE, TAG_SKIP and TAG_MORE",
               GT_GetGadgetAttrsA(slider, NULL, NULL, skip), 2);
    check((LONG)a == -10 && (LONG)c == 10 && d == UNTOUCHED, "the items that count, only them");

    /* the 68k mirrors keep only the bits of MoreFlags that the NDK knows */
    for (a = 0, g = glist; g; g = g->NextGadget)
        if ((g->Flags & GFLG_EXTENDED) && (((struct ExtGadget *)g)->MoreFlags & ~SHARED_MOREFLAGS))
            a++;
    check_long("gadgets with a MoreFlags bit that the NDK does not give", (LONG)a, 0);

    FreeGadgets(glist);
    check_long("FreeGadgets() gives the guest memory back", AvailMem(MEMF_ANY), avail);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);

    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_attrs");
}

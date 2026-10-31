/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    A GadTools LISTVIEW_KIND gadget, its attributes from GT_GetGadgetAttrsA()
    and changes to them with GT_SetGadgetAttrsA(), with no input needed. Two
    Exec Lists of Nodes built in the program's memory with NewList() and
    AddTail(); a context and a 160x40 listview with a label, GTLV_Labels on
    the first List of eight Nodes, items of 10 pixels, GTLV_Top 4,
    GTLV_Selected 2 and GTLV_ShowSelected NULL; the three gadgets that
    GadTools puts before the listview, two arrows and a vertical prop gadget
    with FREEVERT in its PropInfo. The GTLV_Labels, GTLV_Selected, GTLV_Top
    (4: AROS's builder loses it, and the bridge gives it to the listview
    again) and GA_Disabled of the new listview, and GTLV_Total and
    GTLV_Visible (2 rows, as creategadgeta.c, gadgets.c and listviewclass.c
    work them out) under their NDK numbers; GTLV_Top 3 updating VertPot of
    the prop's PropInfo right after GT_SetGadgetAttrsA(), GTLV_Top 100
    clamped to GTLV_Total less the shown entries, GTLV_Selected ~0 read back
    as -1 (0xFFFFFFFF), GTLV_MakeVisible 7 and 100 scrolling GTLV_Top to show
    the last entry and 0x8000, a negative WORD, back to the first, and
    GA_Disabled TRUE and FALSE updating GFLG_DISABLED of both the listview
    and its prop right after the call. A second listview created disabled,
    whose prop and arrows are disabled too, which AROS's builder misses. A
    third listview with a GTLV_ShowSelected string gadget of MaxChars 5:
    the string gadget moved below it, and the name of the selected entry in
    its buffer right after creation and after each change of the selection
    or the List, cut to 5 characters, empty for no selection or entries.
    Detaching the List with GTLV_Labels ~0 (read back as 0xFFFFFFFF with
    GTLV_Total 0 and GTLV_Top 0), changing the first List with Remove() and
    AddHead() while detached and reattaching it, attaching the second List of
    three Nodes, clearing with GTLV_Labels NULL, and the guest memory back
    after FreeGadgets().
*/

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <inline/alib.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

/* What a storage holds until an attribute fills it */
#define UNTOUCHED 0x5A5A5A5AUL

#ifndef GTLV_Total
#define GTLV_Total (GT_TagBase + 92)
#endif
#ifndef GTLV_Visible
#define GTLV_Visible (GT_TagBase + 93)
#endif

typedef char check_gtlv_total[GTLV_Total == GT_TagBase + 92 ? 1 : -1];
typedef char check_gtlv_visible[GTLV_Visible == GT_TagBase + 93 ? 1 : -1];

static CONST_STRPTR first_names[8] =
{
    (CONST_STRPTR)"Alpha", (CONST_STRPTR)"Beta", (CONST_STRPTR)"Gamma", (CONST_STRPTR)"Delta",
    (CONST_STRPTR)"Epsilon", (CONST_STRPTR)"Zeta", (CONST_STRPTR)"Eta", (CONST_STRPTR)"Theta"
};
static CONST_STRPTR second_names[3] =
{
    (CONST_STRPTR)"One", (CONST_STRPTR)"Two", (CONST_STRPTR)"Three"
};

static struct List first_list, second_list;
static struct Node first_nodes[8], second_nodes[3], zero_node;
static struct NewGadget ng;

static LONG get1(struct Gadget *g, Tag tag, ULONG *value)
{
    *value = UNTOUCHED;
    return GT_GetGadgetAttrs(g, NULL, NULL, tag, (ULONG)value, TAG_DONE);
}

int main(void)
{
    struct Gadget *glist = NULL, *context, *lv, *arrow1, *arrow2, *prop, *g, *str, *shown;
    ULONG a, b, c, d, total, visible, count, avail;
    struct PropInfo *pi;
    struct Screen *scr;
    struct Window *win;
    UWORD pot;
    LONG i;
    APTR vi;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        return test_done("gfx_gadget_listview");

    scr = LockPubScreen(NULL);
    vi = scr ? GetVisualInfoA(scr, NULL) : NULL;
    if (!check(scr && vi, "a VisualInfo of the public screen"))
        return test_done("gfx_gadget_listview");

    /* Open and close a first window, as gfx_gadget_setattrs.c does, so that
       the screen font that the bridge keeps for a window is there before the
       guest memory is counted */
    win = OpenWindowTags(NULL, WA_Left, 10, WA_Top, 15, WA_InnerWidth, 240,
                         WA_InnerHeight, 80, TAG_DONE);
    if (win)
        CloseWindow(win);
    avail = AvailMem(MEMF_ANY);

    NewList(&first_list);
    for (i = 0; i < 8; i++)
    {
        first_nodes[i].ln_Name = (char *)first_names[i];
        AddTail(&first_list, &first_nodes[i]);
    }
    NewList(&second_list);
    for (i = 0; i < 3; i++)
    {
        second_nodes[i].ln_Name = (char *)second_names[i];
        AddTail(&second_list, &second_nodes[i]);
    }

    context = CreateContext(&glist);
    ng.ng_LeftEdge = 20;
    ng.ng_TopEdge = 20;
    ng.ng_Width = 160;
    /* creategadgeta.c takes 7 off without a TextAttr: 33; gadgets.c keeps
       a whole number of 10-pixel items within its borders of 3: 26;
       listviewclass.c fits (26 + 2 - 6) / 10 = 2 rows */
    ng.ng_Height = 40;
    ng.ng_GadgetText = (CONST_STRPTR)"Items";
    ng.ng_GadgetID = 1;
    ng.ng_VisualInfo = vi;
    lv = CreateGadget(LISTVIEW_KIND, context, &ng,
                      GTLV_Labels, (ULONG)&first_list,
                      GTLV_ItemHeight, 10,
                      GTLV_Top, 4,
                      GTLV_Selected, 2,
                      GTLV_ShowSelected, (ULONG)NULL,
                      TAG_DONE);
    if (!check(context && lv, "CreateContext() and CreateGadget(LISTVIEW_KIND)"))
    {
        FreeGadgets(glist);
        return test_done("gfx_gadget_listview");
    }

    count = 0;
    for (g = context->NextGadget; g && g != lv; g = g->NextGadget)
        count++;
    check_long("gadgets that GadTools put before the listview", (LONG)count, 3);
    arrow1 = context->NextGadget;
    arrow2 = arrow1 ? arrow1->NextGadget : NULL;
    prop = arrow2 ? arrow2->NextGadget : NULL;
    pi = prop ? (struct PropInfo *)prop->SpecialInfo : NULL;
    check(arrow1 && arrow2 && prop && prop->NextGadget == lv &&
          !arrow1->SpecialInfo && !arrow2->SpecialInfo && !lv->SpecialInfo,
          "two arrows and a prop before the listview");
    check(pi && (pi->Flags & FREEVERT),
          "the prop before the listview has a PropInfo with FREEVERT");

    a = b = c = d = UNTOUCHED;
    check_long("GTLV_Labels, GTLV_Selected, GTLV_Top and GA_Disabled, counted",
               GT_GetGadgetAttrs(lv, NULL, NULL,
                                 GTLV_Labels, (ULONG)&a,
                                 GTLV_Selected, (ULONG)&b,
                                 GTLV_Top, (ULONG)&c,
                                 GA_Disabled, (ULONG)&d,
                                 TAG_DONE), 4);
    check(a == (ULONG)&first_list, "GTLV_Labels is the program's first List");
    check_long("GTLV_Selected", (LONG)b, 2);
    check_long("GTLV_Top of the creation", (LONG)c, 4);
    check_long("GA_Disabled", (LONG)d, 0);

    total = visible = UNTOUCHED;
    check_long("GTLV_Total and GTLV_Visible, counted",
               GT_GetGadgetAttrs(lv, NULL, NULL,
                                 GTLV_Total, (ULONG)&total,
                                 GTLV_Visible, (ULONG)&visible,
                                 TAG_DONE), 2);
    check_long("GTLV_Total", (LONG)total, 8);
    check_long("GTLV_Visible", (LONG)visible, 2);

    pot = pi ? pi->VertPot : 0;
    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Top, 3, TAG_DONE);
    check(pi && pi->VertPot != pot, "GTLV_Top 3 updates the prop's PropInfo.VertPot");
    check_long("GTLV_Top 3 read back", get1(lv, GTLV_Top, &a) == 1 ? (LONG)a : -1, 3);

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Top, 100, TAG_DONE);
    check_long("GTLV_Top 100 clamped to Total less shown entries",
               get1(lv, GTLV_Top, &a) == 1 ? (LONG)a : -1, (LONG)(8 - visible));

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Top, 0, GTLV_Selected, ~0UL, TAG_DONE);
    check(get1(lv, GTLV_Selected, &a) == 1 && a == 0xFFFFFFFFUL,
          "GTLV_Selected ~0 read back as -1 (0xFFFFFFFF)");

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_MakeVisible, 7, TAG_DONE);
    check_long("GTLV_MakeVisible 7 scrolls GTLV_Top to Total less Visible",
               get1(lv, GTLV_Top, &a) == 1 ? (LONG)a : -1, (LONG)(8 - visible));
    /* listviewclass.c takes MakeVisible as a WORD and clamps it to the
       entries */
    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Top, 0, GTLV_MakeVisible, 100, TAG_DONE);
    check_long("GTLV_MakeVisible 100 shows the last entry",
               get1(lv, GTLV_Top, &a) == 1 ? (LONG)a : -1, (LONG)(8 - visible));
    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_MakeVisible, 0x8000, TAG_DONE);
    check_long("GTLV_MakeVisible 0x8000, a negative WORD, shows the first entry",
               get1(lv, GTLV_Top, &a) == 1 ? (LONG)a : -1, 0);

    GT_SetGadgetAttrs(lv, NULL, NULL, GA_Disabled, TRUE, TAG_DONE);
    check((lv->Flags & GFLG_DISABLED) && (prop->Flags & GFLG_DISABLED),
          "GA_Disabled TRUE sets GFLG_DISABLED on the listview and its prop");
    check(get1(lv, GA_Disabled, &a) == 1 && a == 1, "GA_Disabled TRUE read back");

    GT_SetGadgetAttrs(lv, NULL, NULL, GA_Disabled, FALSE, TAG_DONE);
    check(!(lv->Flags & GFLG_DISABLED) && !(prop->Flags & GFLG_DISABLED),
          "GA_Disabled FALSE clears GFLG_DISABLED on the listview and its prop");
    check(get1(lv, GA_Disabled, &a) == 1 && a == 0, "GA_Disabled FALSE read back");

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Labels, ~0UL, TAG_DONE);
    a = b = c = UNTOUCHED;
    check_long("detached: GTLV_Labels, GTLV_Total and GTLV_Top, counted",
               GT_GetGadgetAttrs(lv, NULL, NULL,
                                 GTLV_Labels, (ULONG)&a,
                                 GTLV_Total, (ULONG)&b,
                                 GTLV_Top, (ULONG)&c,
                                 TAG_DONE), 3);
    check(a == 0xFFFFFFFFUL, "detached: GTLV_Labels read back as 0xFFFFFFFF");
    check(b == 0 && c == 0, "detached: GTLV_Total 0 and GTLV_Top 0");

    Remove(&first_nodes[0]);
    zero_node.ln_Name = (char *)"Zero";
    AddHead(&first_list, &zero_node);
    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Labels, (ULONG)&first_list, TAG_DONE);
    a = b = UNTOUCHED;
    check_long("reattached List: GTLV_Labels and GTLV_Total, counted",
               GT_GetGadgetAttrs(lv, NULL, NULL,
                                 GTLV_Labels, (ULONG)&a,
                                 GTLV_Total, (ULONG)&b,
                                 TAG_DONE), 2);
    check(a == (ULONG)&first_list, "reattached List: GTLV_Labels is the program's first List");
    check_long("reattached List: GTLV_Total", (LONG)b, 8);

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Labels, (ULONG)&second_list, TAG_DONE);
    a = b = UNTOUCHED;
    check(GT_GetGadgetAttrs(lv, NULL, NULL,
                            GTLV_Labels, (ULONG)&a,
                            GTLV_Total, (ULONG)&b,
                            TAG_DONE) == 2 &&
          a == (ULONG)&second_list,
          "second List: GTLV_Labels is the program's second List");
    check_long("second List: GTLV_Total", (LONG)b, 3);

    GT_SetGadgetAttrs(lv, NULL, NULL, GTLV_Labels, (ULONG)NULL, TAG_DONE);
    a = b = UNTOUCHED;
    check(GT_GetGadgetAttrs(lv, NULL, NULL,
                            GTLV_Labels, (ULONG)&a,
                            GTLV_Total, (ULONG)&b,
                            TAG_DONE) == 2 &&
          a == 0 && b == 0,
          "GTLV_Labels NULL: GTLV_Labels 0 and GTLV_Total 0");

    /* AROS's builder gives GA_Disabled to the listview before its scroller
       exists; the bridge gives it again, so the scroller and its arrows
       are disabled too */
    ng.ng_TopEdge = 80;
    ng.ng_GadgetID = 2;
    g = CreateGadget(LISTVIEW_KIND, lv, &ng, GTLV_Labels, (ULONG)&second_list,
                     GA_Disabled, TRUE, TAG_DONE);
    count = 0;
    if (g)
        for (prop = lv->NextGadget; prop && prop != g; prop = prop->NextGadget)
            if (prop->Flags & GFLG_DISABLED)
                count++;
    check(g && (g->Flags & GFLG_DISABLED) && count == 3,
          "a listview created disabled, with its prop and arrows disabled");

    /* GTLV_ShowSelected with a string gadget as wide as the listview:
       GadTools moves it below the listview, and the class writes the name
       of the selected entry into it, cut to its MaxChars of 5 characters,
       as on AmigaOS 3.2.3. AmigaOS moves only its edges and keeps its
       bounds, so the check takes the edges. */
    ng.ng_TopEdge = 140;
    ng.ng_Height = 14;
    ng.ng_GadgetID = 3;
    ng.ng_GadgetText = NULL;
    str = CreateGadget(STRING_KIND, g ? g : lv, &ng, GTST_MaxChars, 5, TAG_DONE);
    ng.ng_TopEdge = 160;
    ng.ng_Height = 60;
    ng.ng_GadgetID = 4;
    shown = str ? CreateGadget(LISTVIEW_KIND, str, &ng, GTLV_Labels, (ULONG)&first_list,
                               GTLV_ItemHeight, 10, GTLV_Selected, 1,
                               GTLV_ShowSelected, (ULONG)str, TAG_DONE) : NULL;
    check(str && shown, "a listview with a GTLV_ShowSelected string gadget");
    if (str && shown)
    {
        check(str->TopEdge >= shown->TopEdge + shown->Height,
              "the string gadget moved below the listview");
        check_str("the string gadget shows the selected entry",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "Beta");
        GT_SetGadgetAttrs(shown, NULL, NULL, GTLV_Selected, 3, TAG_DONE);
        check_str("a new selection right after the call",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "Delta");
        GT_SetGadgetAttrs(shown, NULL, NULL, GTLV_Selected, 4, TAG_DONE);
        check_str("a longer name cut to the MaxChars of the string gadget",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "Epsil");
        /* AROS's class empties the string for a detached List, where
           AmigaOS keeps the last name */
        GT_SetGadgetAttrs(shown, NULL, NULL, GTLV_Labels, ~0UL, TAG_DONE);
        check_str("no entries while the List is detached",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "");
        GT_SetGadgetAttrs(shown, NULL, NULL, GTLV_Labels, (ULONG)&second_list, GTLV_Selected, 2,
                          TAG_DONE);
        check_str("another List and selection",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "Three");
        GT_SetGadgetAttrs(shown, NULL, NULL, GTLV_Selected, ~0UL, TAG_DONE);
        check_str("no selection, an empty string",
                  (const char *)((struct StringInfo *)str->SpecialInfo)->Buffer, "");
    }

    FreeGadgets(glist);
    check_long("FreeGadgets() gives the guest memory back", AvailMem(MEMF_ANY), avail);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);

    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_listview");
}

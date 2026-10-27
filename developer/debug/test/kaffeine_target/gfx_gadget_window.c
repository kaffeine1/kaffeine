/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    A GadTools gadget list on a window, with no input needed. A VisualInfo of
    the default public screen; a context and four gadgets with a topaz 8
    TextAttr, a button, a string with "abc", a checked checkbox and a slider
    at level 4, each below the previous inside a 300 by 160 window. A first
    window opened with WA_Gadgets and refreshed with GT_RefreshWindow(): each
    gadget keeps its GadgetID and the place that GadTools gave it, which
    for a string gadget is inside its frame, GT_GetGadgetAttrs() reads back the
    string, the checkbox state and the slider level, and ReadPixel() on the
    top edge of the button's frame reads a pen other than 0. RemoveGList()
    takes the list off the window and leaves the same number of gadgets with
    NULL at the end; AddGList() at ~0 and at 0, RefreshGList() and
    RemoveGList() put the list back and take it off again, from the front
    with the number of its gadgets, since -1 would take the window's own
    gadgets after it too; then CloseWindow()
    closes the window with the list on it before FreeGadgets(). A second
    window opened without WA_Gadgets takes a new list built the same way
    through AddGList(), RefreshGList() and RemoveGList(). A third window,
    opened active with WA_Gadgets and waited for until IDCMP_ACTIVEWINDOW
    comes, gets the setters with its window: GT_SetGadgetAttrs() of a new
    text counts it in NumChars at once, as GadTools draws it, OffGadget()
    disables the button, ActivateGadget() of the string returns TRUE and
    leaves it active, and FALSE without the window; the guest memory is
    back at the end.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

/* What a storage holds until an attribute fills it */
#define UNTOUCHED 0x5A5A5A5AUL

#define WIN_FLAGS (WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET)

enum
{
    ID_BUTTON = 1,
    ID_STRING = 2,
    ID_CHECKBOX = 3,
    ID_SLIDER = 4
};

struct GadgetSet
{
    struct Gadget *glist;
    struct Gadget *context;
    struct Gadget *button;
    struct Gadget *string;
    struct Gadget *checkbox;
    struct Gadget *slider;
};

/* The place of a gadget, to compare after it went on a window */
struct Place
{
    WORD left, top, width, height;
    UWORD id;
};

static void place_of(struct Gadget *g, struct Place *p)
{
    p->left = g->LeftEdge;
    p->top = g->TopEdge;
    p->width = g->Width;
    p->height = g->Height;
    p->id = g->GadgetID;
}

static BOOL same_place(struct Gadget *g, const struct Place *p, UWORD id)
{
    return g->GadgetID == id && p->id == id && g->LeftEdge == p->left && g->TopEdge == p->top &&
           g->Width == p->width && g->Height == p->height;
}

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };

static BOOL make_glist(APTR vi, struct GadgetSet *gs, const char *what)
{
    struct NewGadget ng;

    memset(gs, 0, sizeof(*gs));
    gs->context = CreateContext(&gs->glist);

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = 80;
    ng.ng_TopEdge = 30;
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (CONST_STRPTR)"Button";
    ng.ng_TextAttr = &topaz8;
    ng.ng_GadgetID = ID_BUTTON;
    ng.ng_Flags = PLACETEXT_IN;
    ng.ng_VisualInfo = vi;
    gs->button = CreateGadget(BUTTON_KIND, gs->context, &ng, TAG_DONE);

    ng.ng_TopEdge = 50;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = ID_STRING;
    ng.ng_Flags = 0;
    gs->string = CreateGadget(STRING_KIND, gs->button, &ng,
                              GTST_String, (ULONG)"abc",
                              GTST_MaxChars, 20,
                              TAG_DONE);

    ng.ng_TopEdge = 70;
    ng.ng_Width = 26;
    ng.ng_Height = 11;
    ng.ng_GadgetID = ID_CHECKBOX;
    gs->checkbox = CreateGadget(CHECKBOX_KIND, gs->string, &ng,
                                GTCB_Checked, TRUE,
                                TAG_DONE);

    ng.ng_TopEdge = 90;
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    ng.ng_GadgetID = ID_SLIDER;
    gs->slider = CreateGadget(SLIDER_KIND, gs->checkbox, &ng,
                              GTSL_Min, 0,
                              GTSL_Max, 10,
                              GTSL_Level, 4,
                              TAG_DONE);

    if (!check(gs->context && gs->glist == gs->context && gs->button &&
               gs->string && gs->checkbox && gs->slider, what))
    {
        FreeGadgets(gs->glist);
        memset(gs, 0, sizeof(*gs));
        return FALSE;
    }
    return TRUE;
}

/* Window C: the setters and ActivateGadget() with the window that the
   gadgets are on. The checks stay in this function, apart from main(). */
static void window_c(APTR vi)
{
    struct GadgetSet gs;
    struct Window *win;
    struct IntuiMessage *msg;
    BOOL active = FALSE;
    int i;

    if (!make_glist(vi, &gs, "Window C: CreateContext() and four GadTools gadgets"))
        return;
    win = OpenWindowTags(NULL,
                         WA_Left, 20, WA_Top, 20,
                         WA_Width, 300, WA_Height, 160,
                         WA_Title, (ULONG)"gfx_gadget_window C",
                         WA_Gadgets, (ULONG)gs.glist,
                         WA_Flags, WIN_FLAGS,
                         WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP,
                         TAG_DONE);
    if (!check(win != NULL, "Window C: OpenWindowTags() with WA_Gadgets and WA_Activate"))
    {
        FreeGadgets(gs.glist);
        return;
    }
    /* the window is active once IDCMP_ACTIVEWINDOW comes, not when it opens */
    for (i = 0; i < 100 && !active; i++)
    {
        while ((msg = GT_GetIMsg(win->UserPort)))
        {
            if (msg->Class == IDCMP_ACTIVEWINDOW)
                active = TRUE;
            GT_ReplyIMsg(msg);
        }
        if (!active)
            Delay(5);
    }
    check(active, "Window C: IDCMP_ACTIVEWINDOW comes");

    GT_SetGadgetAttrs(gs.string, win, NULL, GTST_String, (ULONG)"refreshed", TAG_DONE);
    {
        struct StringInfo *si = (struct StringInfo *)gs.string->SpecialInfo;

        check(si && si->NumChars == 9 && !strcmp((const char *)si->Buffer, "refreshed"),
              "Window C: GT_SetGadgetAttrs(GTST_String) with the window counts the new text at once");
    }
    OffGadget(gs.button, win, NULL);
    check((gs.button->Flags & GFLG_DISABLED) != 0, "Window C: OffGadget() with the window disables the button");
    OnGadget(gs.button, win, NULL);
    check(!(gs.button->Flags & GFLG_DISABLED), "Window C: OnGadget() with the window enables it again");
    check(!ActivateGadget(gs.string, NULL, NULL),
          "Window C: ActivateGadget() of the string without a window gives FALSE");
    if (active)
    {
        check(ActivateGadget(gs.string, win, NULL) != FALSE,
              "Window C: ActivateGadget() of the string on its active window gives TRUE");
        check((gs.string->Activation & GACT_ACTIVEGADGET) != 0,
              "Window C: the string is the active gadget then");
    }
    CloseWindow(win);
    FreeGadgets(gs.glist);
}

static ULONG count_gadgets(struct Gadget *glist, struct Gadget **last)
{
    struct Gadget *g, *prev = NULL;
    ULONG count = 0;

    for (g = glist; g && count < 100; g = g->NextGadget)
    {
        prev = g;
        count++;
    }
    *last = prev;
    return count;
}

int main(void)
{
    struct Screen *screen = NULL;
    struct Window *win = NULL;
    struct GadgetSet gs;
    struct Gadget *last = NULL;
    struct Place places[4];
    ULONG avail, before, after, val;
    UWORD pos;
    LONG pen;
    APTR vi = NULL;

    memset(&gs, 0, sizeof(gs));

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GfxBase && GadToolsBase,
               "intuition.library, graphics.library and gadtools.library 39"))
        goto done;

    screen = LockPubScreen(NULL);
    vi = screen ? GetVisualInfo(screen, TAG_DONE) : NULL;
    if (!check(screen && vi, "LockPubScreen() and GetVisualInfo()"))
        goto done;

    /* Open and close a first window, as gfx_gadget_setattrs.c does, so that
       the screen font that the bridge keeps for a window is there before the
       guest memory is counted */
    win = OpenWindowTags(NULL, WA_Left, 20, WA_Top, 20, WA_Width, 100,
                         WA_Height, 40, TAG_DONE);
    if (win)
    {
        CloseWindow(win);
        win = NULL;
    }
    avail = AvailMem(MEMF_ANY);

    /* Window A: glist passed in WA_Gadgets */
    if (!make_glist(vi, &gs, "Window A: CreateContext() and four GadTools gadgets"))
        goto done;
    before = count_gadgets(gs.glist, &last);
    place_of(gs.button, &places[0]);
    place_of(gs.string, &places[1]);
    place_of(gs.checkbox, &places[2]);
    place_of(gs.slider, &places[3]);

    win = OpenWindowTags(NULL,
                         WA_Left, 20, WA_Top, 20,
                         WA_Width, 300, WA_Height, 160,
                         WA_Title, (ULONG)"gfx_gadget_window A",
                         WA_Gadgets, (ULONG)gs.glist,
                         WA_Flags, WIN_FLAGS,
                         TAG_DONE);
    if (!check(win != NULL, "Window A: OpenWindowTags() with WA_Gadgets"))
        goto done;
    GT_RefreshWindow(win, NULL);

    check(same_place(gs.button, &places[0], ID_BUTTON) && gs.button->LeftEdge == 80 &&
          gs.button->TopEdge == 30,
          "Window A: the button keeps its GadgetID and place");
    check(same_place(gs.string, &places[1], ID_STRING) && gs.string->LeftEdge >= 80 &&
          gs.string->TopEdge >= 50,
          "Window A: the string gadget keeps its GadgetID and place");
    check(same_place(gs.checkbox, &places[2], ID_CHECKBOX) && gs.checkbox->LeftEdge >= 80 &&
          gs.checkbox->TopEdge >= 70,
          "Window A: the checkbox keeps its GadgetID and place");
    check(same_place(gs.slider, &places[3], ID_SLIDER) && gs.slider->LeftEdge >= 80 &&
          gs.slider->TopEdge >= 90,
          "Window A: the slider keeps its GadgetID and place");

    val = UNTOUCHED;
    check(GT_GetGadgetAttrs(gs.string, win, NULL, GTST_String, (ULONG)&val, TAG_DONE) == 1 &&
          val && !strcmp((const char *)val, "abc"),
          "Window A: GT_GetGadgetAttrs(GTST_String) gives \"abc\"");
    val = UNTOUCHED;
    check(GT_GetGadgetAttrs(gs.checkbox, win, NULL, GTCB_Checked, (ULONG)&val, TAG_DONE) == 1 &&
          val == TRUE,
          "Window A: GT_GetGadgetAttrs(GTCB_Checked) gives TRUE");
    val = UNTOUCHED;
    check(GT_GetGadgetAttrs(gs.slider, win, NULL, GTSL_Level, (ULONG)&val, TAG_DONE) == 1 &&
          (LONG)val == 4,
          "Window A: GT_GetGadgetAttrs(GTSL_Level) gives 4");

    pen = (LONG)ReadPixel(win->RPort, gs.button->LeftEdge + gs.button->Width / 2,
                          gs.button->TopEdge);
    check(pen > 0,
          "Window A: ReadPixel() on the top edge of the button's frame reads a pen other than 0");

    pos = RemoveGList(win, gs.glist, -1);
    check(pos != 0xFFFF, "Window A: RemoveGList(win, glist, -1) returns a position other than 0xFFFF");
    after = count_gadgets(gs.glist, &last);
    check_long("Window A: glist holds the same number of gadgets after RemoveGList()",
               (LONG)after, (LONG)before);
    check(last != NULL && last->NextGadget == NULL,
          "Window A: NextGadget of the last gadget is NULL after RemoveGList()");

    check_long("Window A: AddGList(win, glist, ~0, -1, NULL) returns pos again",
               AddGList(win, gs.glist, (UWORD)~0, -1, NULL), pos);
    RefreshGList(gs.glist, win, NULL, -1);
    check_long("Window A: RemoveGList(win, glist, -1) gives pos again",
               RemoveGList(win, gs.glist, -1), pos);
    check_long("Window A: AddGList(win, glist, 0, -1, NULL) returns 0",
               AddGList(win, gs.glist, 0, -1, NULL), 0);
    check_long("Window A: RemoveGList() of its gadgets from the front gives 0",
               RemoveGList(win, gs.glist, (WORD)before), 0);

    check_long("Window A: AddGList(win, glist, ~0, -1, NULL) before CloseWindow()",
               AddGList(win, gs.glist, (UWORD)~0, -1, NULL), pos);
    CloseWindow(win);
    win = NULL;
    FreeGadgets(gs.glist);
    memset(&gs, 0, sizeof(gs));

    /* Window B: opened without WA_Gadgets, then a new glist added */
    win = OpenWindowTags(NULL,
                         WA_Left, 20, WA_Top, 20,
                         WA_Width, 300, WA_Height, 160,
                         WA_Title, (ULONG)"gfx_gadget_window B",
                         WA_Flags, WIN_FLAGS,
                         TAG_DONE);
    if (!check(win != NULL, "Window B: OpenWindowTags() without WA_Gadgets"))
        goto done;
    if (!make_glist(vi, &gs, "Window B: CreateContext() and four GadTools gadgets"))
        goto done;

    pos = AddGList(win, gs.glist, (UWORD)~0, -1, NULL);
    check(pos != 0xFFFF,
          "Window B: AddGList(win, glist, ~0, -1, NULL) returns a position other than 0xFFFF");
    RefreshGList(gs.glist, win, NULL, -1);
    check_long("Window B: RemoveGList(win, glist, -1) gives the same position",
               RemoveGList(win, gs.glist, -1), pos);
    CloseWindow(win);
    win = NULL;
    FreeGadgets(gs.glist);
    memset(&gs, 0, sizeof(gs));

    window_c(vi);

    check_long("the windows and FreeGadgets() give the guest memory back",
               AvailMem(MEMF_ANY), avail);

done:
    if (win)
        CloseWindow(win);
    if (gs.glist)
        FreeGadgets(gs.glist);
    if (vi)
        FreeVisualInfo(vi);
    if (screen)
        UnlockPubScreen(NULL, screen);
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_window");
}

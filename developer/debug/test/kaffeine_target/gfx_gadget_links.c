/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The links of a GadTools gadget list on a window and the memory that
    FreeGadgets() gives back, with no input needed. A VisualInfo of the
    default public screen and a list of a context and three topaz 8 buttons
    opened on a window with WA_Gadgets and refreshed with GT_RefreshWindow():
    RemoveGList() of the middle button alone gives the position of the
    context plus 2, leaves its NextGadget on the third button rather than
    NULL, and links the first button to the third, while AddGList() of that
    button with 1 puts it back at the same position and restores the links;
    RemoveGList() of the context and the first button with 2 gives the
    position of the context and leaves the first button linked to the second,
    and AddGList() with 2 puts them back at that position; removing the
    context and the three buttons one by one with 1 gives that same position
    each time and leaves all NextGadget links intact with NULL at the end,
    so AddGList() of the whole list with -1 at that position puts all four
    back and RemoveGList() with -1 takes them off from that position again.
    Comparing AvailMem(MEMF_ANY) before and after shows that FreeGadgets()
    gives all memory back for a whole list never put on a window, for a list
    whose four gadgets were removed from a window one by one with 1 or all
    at once with -1 before CloseWindow(), and for a list into which the
    program spliced a GTYP_BOOLGADGET of its own between the two buttons,
    whose NextGadget still names the second button afterwards; when the
    program cuts NextGadget of the first button to NULL, FreeGadgets() of
    the head leaves the second button allocated until FreeGadgets() of the
    second button frees the rest.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
static APTR vi;

static struct Gadget *button(struct Gadget *prev, WORD top, UWORD id)
{
    struct NewGadget ng;

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = 20;
    ng.ng_TopEdge = top;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (CONST_STRPTR)"B";
    ng.ng_TextAttr = &topaz8;
    ng.ng_GadgetID = id;
    ng.ng_VisualInfo = vi;
    ng.ng_Flags = PLACETEXT_IN;
    return CreateGadget(BUTTON_KIND, prev, &ng, TAG_DONE);
}

static LONG count(struct Gadget *g)
{
    LONG n = 0;

    for (; g && n < 200; g = g->NextGadget)
        n++;
    return n;
}

static struct Window *window(struct Gadget *glist)
{
    struct Window *win = OpenWindowTags(NULL,
                                        WA_Left, 10,
                                        WA_Top, 15,
                                        WA_Width, 200,
                                        WA_Height, 120,
                                        WA_Gadgets, (ULONG)glist,
                                        WA_Title, (ULONG)"gfx_gadget_links",
                                        WA_Flags, WFLG_DRAGBAR | WFLG_SMART_REFRESH,
                                        TAG_DONE);

    if (win)
        GT_RefreshWindow(win, NULL);
    return win;
}

/* A and B: the links after removals of a part of the list, and one by one
   off the window then the whole list back with -1 */
static void section_ab(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2, *b3;
    struct Window *win;
    UWORD pos_b2, pos_ctx, p0, p1, p2, p3;

    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    b3 = button(b2, 70, 3);
    if (!check(ctx && glist == ctx && b1 && b2 && b3, "A: CreateContext() and three buttons"))
    {
        FreeGadgets(glist);
        return;
    }
    check_long("A: the program's list has 4 gadgets", count(glist), 4);

    win = window(glist);
    if (!check(win != NULL, "A: OpenWindowTags() with WA_Gadgets"))
    {
        FreeGadgets(glist);
        return;
    }

    pos_b2 = RemoveGList(win, b2, 1);
    check(pos_b2 != 0xFFFF, "A: RemoveGList() of b2 returns a position other than 0xFFFF");
    check(b2->NextGadget == b3, "A: b2 NextGadget names b3");
    check(b2->NextGadget != NULL, "A: b2 NextGadget is not NULL");
    check(b1->NextGadget == b3, "A: b1 NextGadget names b3");
    check_long("A: AddGList() of b2 with 1 gives that position again",
               (LONG)AddGList(win, b2, pos_b2, 1, NULL), (LONG)pos_b2);
    check(b1->NextGadget == b2, "A: b1 NextGadget names b2 again");
    check(b2->NextGadget == b3, "A: b2 NextGadget names b3 again");

    pos_ctx = RemoveGList(win, ctx, 2);
    check(pos_ctx != 0xFFFF,
          "A: RemoveGList() of ctx and b1 returns a position other than 0xFFFF");
    check_long("A: RemoveGList() of b2 gave the position of ctx plus 2",
               (LONG)pos_b2, (LONG)pos_ctx + 2);
    check(b1->NextGadget == b2, "A: b1 NextGadget names b2");
    check_long("A: AddGList() of ctx and b1 with 2 gives the position of ctx again",
               (LONG)AddGList(win, ctx, pos_ctx, 2, NULL), (LONG)pos_ctx);

    p0 = RemoveGList(win, ctx, 1);
    p1 = RemoveGList(win, b1, 1);
    p2 = RemoveGList(win, b2, 1);
    p3 = RemoveGList(win, b3, 1);
    check_long("B: RemoveGList() of ctx gives the position of ctx", (LONG)p0, (LONG)pos_ctx);
    check_long("B: RemoveGList() of b1 gives the same position", (LONG)p1, (LONG)p0);
    check_long("B: RemoveGList() of b2 gives the same position", (LONG)p2, (LONG)p0);
    check_long("B: RemoveGList() of b3 gives the same position", (LONG)p3, (LONG)p0);
    check(ctx->NextGadget == b1, "B: ctx NextGadget names b1");
    check(b1->NextGadget == b2, "B: b1 NextGadget names b2");
    check(b2->NextGadget == b3, "B: b2 NextGadget names b3");
    check(b3->NextGadget == NULL, "B: b3 NextGadget is NULL");
    check_long("B: AddGList() of the whole list with -1 gives that position",
               (LONG)AddGList(win, glist, p0, -1, NULL), (LONG)p0);
    RefreshGList(glist, win, NULL, -1);
    check_long("B: RemoveGList() of the whole list with -1 gives that position again",
               (LONG)RemoveGList(win, glist, -1), (LONG)p0);

    CloseWindow(win);
    FreeGadgets(glist);
}

/* C: FreeGadgets() of a whole list off any window gives all back */
static void section_c(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2;
    ULONG before, after;

    before = AvailMem(MEMF_ANY);
    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    if (!check(ctx && b1 && b2, "C: CreateContext() and two buttons"))
    {
        FreeGadgets(glist);
        return;
    }
    FreeGadgets(glist);
    after = AvailMem(MEMF_ANY);
    check_long("C: memory not given back after FreeGadgets() of a whole list",
               (LONG)(before - after), 0);
}

/* D: removed one by one, then the window closes and FreeGadgets() of the
   head: the kept links lead on */
static void section_d(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2, *b3;
    struct Window *win;
    ULONG before, after;

    before = AvailMem(MEMF_ANY);
    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    b3 = button(b2, 70, 3);
    win = window(glist);
    if (!check(ctx && b1 && b2 && b3 && win,
               "D: CreateContext(), three buttons and OpenWindowTags()"))
    {
        if (win)
            CloseWindow(win);
        FreeGadgets(glist);
        return;
    }
    RemoveGList(win, ctx, 1);
    RemoveGList(win, b1, 1);
    RemoveGList(win, b2, 1);
    RemoveGList(win, b3, 1);
    CloseWindow(win);
    FreeGadgets(glist);
    after = AvailMem(MEMF_ANY);
    check_long("D: memory not given back after removals one by one and FreeGadgets() of the head",
               (LONG)(before - after), 0);
}

/* D0: the same with RemoveGList() of the whole list at once */
static void section_d0(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2, *b3;
    struct Window *win;
    ULONG before, after;

    before = AvailMem(MEMF_ANY);
    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    b3 = button(b2, 70, 3);
    win = window(glist);
    if (!check(ctx && b1 && b2 && b3 && win,
               "D0: CreateContext(), three buttons and OpenWindowTags()"))
    {
        if (win)
            CloseWindow(win);
        FreeGadgets(glist);
        return;
    }
    RemoveGList(win, glist, -1);
    CloseWindow(win);
    FreeGadgets(glist);
    after = AvailMem(MEMF_ANY);
    check_long("D0: memory not given back after RemoveGList() of all and FreeGadgets()",
               (LONG)(before - after), 0);
}

/* E: a gadget of the program's own in the middle of the list:
   FreeGadgets() of the head goes on past it */
static void section_e(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2;
    struct Gadget own;
    ULONG before, after;

    before = AvailMem(MEMF_ANY);
    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    if (!check(ctx && b1 && b2, "E: CreateContext() and two buttons"))
    {
        FreeGadgets(glist);
        return;
    }
    memset(&own, 0, sizeof(own));
    own.LeftEdge = 130;
    own.TopEdge = 30;
    own.Width = 20;
    own.Height = 10;
    own.Flags = GFLG_GADGHCOMP;
    own.Activation = GACT_RELVERIFY;
    own.GadgetType = GTYP_BOOLGADGET;
    own.NextGadget = b2;
    b1->NextGadget = &own;
    FreeGadgets(glist);
    after = AvailMem(MEMF_ANY);
    check_long("E: memory not given back after FreeGadgets() past a gadget of the program's own",
               (LONG)(before - after), 0);
    check(own.NextGadget == b2,
          "E: the program's own gadget NextGadget still names b2 after FreeGadgets()");
}

/* F: the program cuts its list; FreeGadgets() of the head frees the
   head part, and of the rest the rest */
static void section_f(void)
{
    struct Gadget *glist = NULL, *ctx, *b1, *b2;
    ULONG before, after;

    before = AvailMem(MEMF_ANY);
    ctx = CreateContext(&glist);
    b1 = button(ctx, 30, 1);
    b2 = button(b1, 50, 2);
    if (!check(ctx && b1 && b2, "F: CreateContext() and two buttons"))
    {
        FreeGadgets(glist);
        return;
    }
    b1->NextGadget = NULL;
    FreeGadgets(glist);
    after = AvailMem(MEMF_ANY);
    check((LONG)(before - after) > 0,
          "F: memory of b2 is not given back after FreeGadgets() of the cut head part");
    FreeGadgets(b2);
    after = AvailMem(MEMF_ANY);
    check_long("F: memory not given back after FreeGadgets() of the rest too",
               (LONG)(before - after), 0);
}

int main(void)
{
    struct Screen *scr = NULL;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        goto done;

    scr = LockPubScreen(NULL);
    vi = scr ? GetVisualInfo(scr, TAG_DONE) : NULL;
    if (!check(scr && vi, "LockPubScreen() and GetVisualInfo()"))
        goto done;

    section_ab();
    section_c();
    section_d();
    section_d0();
    section_e();
    section_f();

done:
    if (vi)
        FreeVisualInfo(vi);
    if (scr)
        UnlockPubScreen(NULL, scr);
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadget_links");
}

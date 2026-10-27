/*
 * Classic Intuition Boolean gadget test (application-allocated structs,
 * no GadTools).
 *
 * Builds a linked list of four GTYP_BOOLGADGET gadgets in application
 * memory (a plain border gadget with IntuiText, a toggle image gadget
 * with GadgetRender and SelectRender in chip RAM, a GFLG_RELRIGHT
 * gadget with GFLG_GADGHBOX and two chained IntuiTexts, and a
 * struct ExtGadget with GMORE_BOUNDS), attaches them through WA_Gadgets,
 * and checks rendering, programmatic GFLG_SELECTED refresh,
 * OffGadget/OnGadget, RemoveGList/AddGList,
 * RemoveGadget/AddGadget, and NewWindow.FirstGadget via OpenWindow().
 */

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

#define WIN_WIDTH   320
#define WIN_HEIGHT  160
#define WIN_FLAGS   (WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET)
#define WIN_IDCMP   (IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN)

#define ID_PLAIN    1
#define ID_TOGGLE   2
#define ID_RELRIGHT 3
#define ID_EXT      4
#define ID_NEWWIN   5

#define USER_PLAIN    ((APTR)0x1001UL)
#define USER_TOGGLE   ((APTR)0x1002UL)
#define USER_RELRIGHT ((APTR)0x1003UL)
#define USER_EXT      ((APTR)0x1004UL)
#define USER_NEWWIN   ((APTR)0x1005UL)

#define TOGGLE_WIDTH  16
#define TOGGLE_HEIGHT 8
#define TOGGLE_WORDS  8
#define TOGGLE_BYTES  (TOGGLE_WORDS * sizeof(UWORD))

static struct TextAttr topaz8 = {
    (STRPTR)"topaz.font", 8, FS_NORMAL, 0
};

struct ClassicBoolSet {
    /* 1. Plain Boolean gadget with Border and IntuiText */
    WORD             plain_xy[10];
    struct Border    plain_border;
    struct IntuiText plain_text;
    struct Gadget    plain_gad;

    /* 2. Toggle image gadget with 16x8 1-plane normal and select images */
    UWORD           *img_norm_data;
    UWORD           *img_sel_data;
    struct Image     img_norm;
    struct Image     img_sel;
    struct Gadget    toggle_gad;

    /* 3. Relative-right Boolean gadget with Border and two IntuiTexts */
    WORD             rel_xy[10];
    struct Border    rel_border;
    struct IntuiText rel_text1;
    struct IntuiText rel_text2;
    struct Gadget    rel_gad;

    /* 4. Extended Boolean gadget with GMORE_BOUNDS and Border */
    WORD             ext_xy[10];
    struct Border    ext_border;
    struct ExtGadget ext_gad;

    /* Separate one-gadget list for the second window (OpenWindow) */
    WORD             nw_xy[10];
    struct Border    nw_border;
    struct Gadget    nw_gad;
};

struct Place {
    WORD  left, top, width, height;
    UWORD id;
    APTR  user;
};

static struct ClassicBoolSet g_set;

/* Fill a 5-point closed rectangle polyline for a Border of width x height. */
static void fill_rect_xy(WORD *xy, WORD width, WORD height)
{
    xy[0] = 0;         xy[1] = 0;
    xy[2] = width - 1; xy[3] = 0;
    xy[4] = width - 1; xy[5] = height - 1;
    xy[6] = 0;         xy[7] = height - 1;
    xy[8] = 0;         xy[9] = 0;
}

static void place_of(const struct Gadget *g, struct Place *p)
{
    p->left   = g->LeftEdge;
    p->top    = g->TopEdge;
    p->width  = g->Width;
    p->height = g->Height;
    p->id     = g->GadgetID;
    p->user   = g->UserData;
}

static BOOL same_place(const struct Gadget *g, const struct Place *p)
{
    return g->LeftEdge == p->left &&
           g->TopEdge == p->top &&
           g->Width == p->width &&
           g->Height == p->height &&
           g->GadgetID == p->id &&
           g->UserData == p->user;
}

/*
 * Initialise all gadget structures and allocate chip RAM for the two
 * 16x8 1-plane images of the toggle gadget.
 */
static BOOL build_gadgets(struct ClassicBoolSet *s)
{
    WORD i;

    memset(s, 0, sizeof(*s));

    s->img_norm_data = (UWORD *)AllocMem(TOGGLE_BYTES, MEMF_CHIP | MEMF_CLEAR);
    s->img_sel_data  = (UWORD *)AllocMem(TOGGLE_BYTES, MEMF_CHIP | MEMF_CLEAR);
    if (!s->img_norm_data || !s->img_sel_data)
        return FALSE;

    for (i = 0; i < TOGGLE_WORDS; i++) {
        s->img_norm_data[i] = 0xFFFF;
        s->img_sel_data[i]  = 0x0000;
    }

    /* 1. Plain Boolean gadget: GFLG_GADGHCOMP, GACT_RELVERIFY, Border, IntuiText */
    fill_rect_xy(s->plain_xy, 80, 14);
    s->plain_border.FrontPen = 1;
    s->plain_border.DrawMode = JAM1;
    s->plain_border.Count    = 5;
    s->plain_border.XY       = s->plain_xy;

    s->plain_text.FrontPen  = 1;
    s->plain_text.DrawMode  = JAM1;
    s->plain_text.LeftEdge  = 8;
    s->plain_text.TopEdge   = 3;
    s->plain_text.ITextFont = &topaz8;
    s->plain_text.IText     = (STRPTR)"Plain";

    s->plain_gad.NextGadget   = &s->toggle_gad;
    s->plain_gad.LeftEdge     = 20;
    s->plain_gad.TopEdge      = 30;
    s->plain_gad.Width        = 80;
    s->plain_gad.Height       = 14;
    s->plain_gad.Flags        = GFLG_GADGHCOMP;
    s->plain_gad.Activation   = GACT_RELVERIFY;
    s->plain_gad.GadgetType   = GTYP_BOOLGADGET;
    s->plain_gad.GadgetRender = &s->plain_border;
    s->plain_gad.GadgetText   = &s->plain_text;
    s->plain_gad.GadgetID     = ID_PLAIN;
    s->plain_gad.UserData     = USER_PLAIN;

    /* 2. Toggle gadget: 16x8 1-plane Image (all set) and SelectRender (all clear) */
    s->img_norm.Width      = TOGGLE_WIDTH;
    s->img_norm.Height     = TOGGLE_HEIGHT;
    s->img_norm.Depth      = 1;
    s->img_norm.ImageData  = s->img_norm_data;
    s->img_norm.PlanePick  = 1;
    s->img_norm.PlaneOnOff = 0;

    s->img_sel.Width      = TOGGLE_WIDTH;
    s->img_sel.Height     = TOGGLE_HEIGHT;
    s->img_sel.Depth      = 1;
    s->img_sel.ImageData  = s->img_sel_data;
    s->img_sel.PlanePick  = 1;
    s->img_sel.PlaneOnOff = 0;

    s->toggle_gad.NextGadget   = &s->rel_gad;
    s->toggle_gad.LeftEdge     = 20;
    s->toggle_gad.TopEdge      = 55;
    s->toggle_gad.Width        = TOGGLE_WIDTH;
    s->toggle_gad.Height       = TOGGLE_HEIGHT;
    s->toggle_gad.Flags        = GFLG_GADGIMAGE | GFLG_GADGHIMAGE;
    s->toggle_gad.Activation   = GACT_TOGGLESELECT | GACT_IMMEDIATE;
    s->toggle_gad.GadgetType   = GTYP_BOOLGADGET;
    s->toggle_gad.GadgetRender = &s->img_norm;
    s->toggle_gad.SelectRender = &s->img_sel;
    s->toggle_gad.GadgetID     = ID_TOGGLE;
    s->toggle_gad.UserData     = USER_TOGGLE;

    /* 3. Relative-right gadget: GFLG_GADGHBOX | GFLG_RELRIGHT, two IntuiTexts */
    fill_rect_xy(s->rel_xy, 80, 14);
    s->rel_border.FrontPen = 1;
    s->rel_border.DrawMode = JAM1;
    s->rel_border.Count    = 5;
    s->rel_border.XY       = s->rel_xy;

    s->rel_text2.FrontPen  = 1;
    s->rel_text2.DrawMode  = JAM1;
    s->rel_text2.LeftEdge  = 36;
    s->rel_text2.TopEdge   = 3;
    s->rel_text2.ITextFont = &topaz8;
    s->rel_text2.IText     = (STRPTR)"Right";
    s->rel_text2.NextText  = NULL;

    s->rel_text1.FrontPen  = 1;
    s->rel_text1.DrawMode  = JAM1;
    s->rel_text1.LeftEdge  = 6;
    s->rel_text1.TopEdge   = 3;
    s->rel_text1.ITextFont = &topaz8;
    s->rel_text1.IText     = (STRPTR)"Rel";
    s->rel_text1.NextText  = &s->rel_text2;

    s->rel_gad.NextGadget   = (struct Gadget *)&s->ext_gad;
    s->rel_gad.LeftEdge     = -100;
    s->rel_gad.TopEdge      = 80;
    s->rel_gad.Width        = 80;
    s->rel_gad.Height       = 14;
    s->rel_gad.Flags        = GFLG_GADGHBOX | GFLG_RELRIGHT;
    s->rel_gad.Activation   = GACT_RELVERIFY;
    s->rel_gad.GadgetType   = GTYP_BOOLGADGET;
    s->rel_gad.GadgetRender = &s->rel_border;
    s->rel_gad.GadgetText   = &s->rel_text1;
    s->rel_gad.GadgetID     = ID_RELRIGHT;
    s->rel_gad.UserData     = USER_RELRIGHT;

    /* 4. Extended Boolean gadget: GFLG_EXTENDED, GMORE_BOUNDS, Border */
    fill_rect_xy(s->ext_xy, 80, 14);
    s->ext_border.FrontPen = 1;
    s->ext_border.DrawMode = JAM1;
    s->ext_border.Count    = 5;
    s->ext_border.XY       = s->ext_xy;

    s->ext_gad.NextGadget     = NULL;
    s->ext_gad.LeftEdge       = 20;
    s->ext_gad.TopEdge        = 105;
    s->ext_gad.Width          = 80;
    s->ext_gad.Height         = 14;
    s->ext_gad.Flags          = GFLG_GADGHCOMP | GFLG_EXTENDED;
    s->ext_gad.Activation     = GACT_RELVERIFY;
    s->ext_gad.GadgetType     = GTYP_BOOLGADGET;
    s->ext_gad.GadgetRender   = &s->ext_border;
    s->ext_gad.GadgetID       = ID_EXT;
    s->ext_gad.UserData       = USER_EXT;
    s->ext_gad.MoreFlags      = GMORE_BOUNDS;
    s->ext_gad.BoundsLeftEdge = 16;
    s->ext_gad.BoundsTopEdge  = 101;
    s->ext_gad.BoundsWidth    = 88;
    s->ext_gad.BoundsHeight   = 22;

    /* Separate one-gadget list for the second window (FrontPen 2) */
    fill_rect_xy(s->nw_xy, 80, 14);
    s->nw_border.FrontPen = 2;
    s->nw_border.DrawMode = JAM1;
    s->nw_border.Count    = 5;
    s->nw_border.XY       = s->nw_xy;

    s->nw_gad.NextGadget   = NULL;
    s->nw_gad.LeftEdge     = 20;
    s->nw_gad.TopEdge      = 30;
    s->nw_gad.Width        = 80;
    s->nw_gad.Height       = 14;
    s->nw_gad.Flags        = GFLG_GADGHCOMP;
    s->nw_gad.Activation   = GACT_RELVERIFY;
    s->nw_gad.GadgetType   = GTYP_BOOLGADGET;
    s->nw_gad.GadgetRender = &s->nw_border;
    s->nw_gad.GadgetID     = ID_NEWWIN;
    s->nw_gad.UserData     = USER_NEWWIN;

    return TRUE;
}

static void free_gadgets(struct ClassicBoolSet *s)
{
    if (s->img_norm_data)
        FreeMem(s->img_norm_data, TOGGLE_BYTES);
    if (s->img_sel_data)
        FreeMem(s->img_sel_data, TOGGLE_BYTES);
    s->img_norm_data = NULL;
    s->img_sel_data  = NULL;
}

/*
 * What ReadPixel() gives for a pen in this window: on a screen of more
 * than 256 colours ReadPixel() gives the pen found for a colour, which
 * need not be the pen that drew it, so the checks compare a pixel with
 * one that the program drew in that pen, low at the left of the window,
 * where no gadget lies.
 */
static LONG pen_value(struct Window *win, UBYTE pen)
{
    struct RastPort *rp = win->RPort;
    LONG x = win->BorderLeft + 2, y = win->Height - win->BorderBottom - 3;

    SetAPen(rp, pen);
    WritePixel(rp, x, y);
    return ReadPixel(rp, x, y);
}

/*
 * Open the test window with WA_Gadgets pointing to the four-gadget list
 * and verify that each gadget keeps its position, size, GadgetID and UserData.
 */
static struct Window *open_test_window(struct ClassicBoolSet *s)
{
    struct Place plain_before, toggle_before, rel_before, ext_before;
    struct Window *win;

    place_of(&s->plain_gad, &plain_before);
    place_of(&s->toggle_gad, &toggle_before);
    place_of(&s->rel_gad, &rel_before);
    place_of((struct Gadget *)&s->ext_gad, &ext_before);

    win = OpenWindowTags(NULL,
                         WA_Left, 20,
                         WA_Top, 20,
                         WA_Width, WIN_WIDTH,
                         WA_Height, WIN_HEIGHT,
                         WA_Flags, WIN_FLAGS,
                         WA_IDCMP, WIN_IDCMP,
                         WA_Activate, TRUE,
                         WA_Gadgets, (Tag)&s->plain_gad,
                         WA_Title, (Tag)"gfx_classic_bool",
                         TAG_DONE);
    check(win != NULL,
          "OpenWindowTags with WA_Gadgets succeeds");
    if (!win)
        return NULL;

    check(same_place(&s->plain_gad, &plain_before),
          "plain Boolean gadget keeps its place, ID and UserData");
    check(same_place(&s->toggle_gad, &toggle_before),
          "toggle image gadget keeps its place, ID and UserData");
    check(same_place(&s->rel_gad, &rel_before),
          "relative-right gadget keeps its place, ID and UserData");
    check(same_place((struct Gadget *)&s->ext_gad, &ext_before),
          "extended Boolean gadget keeps its place, ID and UserData");

    return win;
}

/*
 * Check that the first gadget's Border is drawn in FrontPen 1, and that
 * the toggle gadget's Image switches between its normal pen 1 and its
 * SelectRender when GFLG_SELECTED is toggled and RefreshGList is called.
 */
static void test_render_and_toggle(struct Window *win, struct ClassicBoolSet *s)
{
    LONG border_pen, unsel_pen, sel_pen, restored_pen;

    border_pen = ReadPixel(win->RPort,
                           s->plain_gad.LeftEdge + s->plain_gad.Width / 2,
                           s->plain_gad.TopEdge);
    check_long("plain Boolean gadget border pixel matches FrontPen",
               border_pen, pen_value(win, s->plain_border.FrontPen));

    unsel_pen = ReadPixel(win->RPort,
                          s->toggle_gad.LeftEdge + 4,
                          s->toggle_gad.TopEdge + 4);
    check_long("toggle gadget normal image pixel has pen 1",
               unsel_pen, pen_value(win, 1));

    s->toggle_gad.Flags |= GFLG_SELECTED;
    RefreshGList(&s->toggle_gad, win, NULL, 1);

    sel_pen = ReadPixel(win->RPort,
                        s->toggle_gad.LeftEdge + 4,
                        s->toggle_gad.TopEdge + 4);
    /* AmigaOS, and AROS since #71, draw the SelectRender as it is, pen 0,
       where AROS before drew an Image of a selected gadget inverted,
       through DrawImageState() with IDS_SELECTED: this checks only that
       the program's GFLG_SELECTED reached Intuition and drew the other
       render, which both do. */
    check(sel_pen != unsel_pen,
          "RefreshGList with GFLG_SELECTED draws the SelectRender");
    check((s->toggle_gad.Flags & GFLG_SELECTED) != 0,
          "GFLG_SELECTED remains set on toggle gadget after RefreshGList");

    s->toggle_gad.Flags &= ~GFLG_SELECTED;
    RefreshGList(&s->toggle_gad, win, NULL, 1);

    restored_pen = ReadPixel(win->RPort,
                             s->toggle_gad.LeftEdge + 4,
                             s->toggle_gad.TopEdge + 4);
    check_long("RefreshGList after clearing GFLG_SELECTED restores normal pen",
               restored_pen, unsel_pen);
}

/*
 * Check OffGadget/OnGadget flag updates. ActivateGadget() of a Boolean
 * gadget is left out: the autodoc has it for string and custom gadgets
 * only, AmigaOS 3.2.3 and AROS since #71 give TRUE, AROS before FALSE.
 */
static void test_enable_and_activate(struct Window *win, struct ClassicBoolSet *s)
{
    OffGadget(&s->plain_gad, win, NULL);
    check((s->plain_gad.Flags & GFLG_DISABLED) != 0,
          "OffGadget sets GFLG_DISABLED on plain Boolean gadget");

    OnGadget(&s->plain_gad, win, NULL);
    check((s->plain_gad.Flags & GFLG_DISABLED) == 0,
          "OnGadget clears GFLG_DISABLED on plain Boolean gadget");
}

/*
 * Check RemoveGList/AddGList on the whole four-gadget list, ReadPixel on the
 * GFLG_RELRIGHT gadget's Border, and RemoveGadget/AddGadget on the third gadget.
 */
static void test_list_and_relright(struct Window *win, struct ClassicBoolSet *s)
{
    UWORD pos, add_pos;
    LONG pen;

    pos = RemoveGList(win, &s->plain_gad, -1);
    check(pos != 0xFFFF,
          "RemoveGList returns a valid position");
    /* the last of the window's list: RemoveGList() leaves the link of the
       last gadget that it takes off, which is NULL here */
    check(s->ext_gad.NextGadget == NULL,
          "the last gadget taken off, at the end of the window's list, keeps a NULL NextGadget");

    add_pos = AddGList(win, &s->plain_gad, pos, -1, NULL);
    check_long("AddGList re-inserts list at the same position",
               (LONG)add_pos, (LONG)pos);

    RefreshGList(&s->plain_gad, win, NULL, -1);

    pen = ReadPixel(win->RPort,
                    s->plain_gad.LeftEdge + s->plain_gad.Width / 2,
                    s->plain_gad.TopEdge);
    check_long("plain Boolean gadget border pixel matches FrontPen after RefreshGList",
               pen, pen_value(win, s->plain_border.FrontPen));

    pen = ReadPixel(win->RPort,
                    win->Width + s->rel_gad.LeftEdge + s->rel_gad.Width / 2,
                    s->rel_gad.TopEdge);
    check_long("relative-right gadget border pixel matches FrontPen",
               pen, pen_value(win, s->rel_border.FrontPen));

    pos = RemoveGadget(win, &s->rel_gad);
    check(pos != 0xFFFF,
          "RemoveGadget of third gadget returns a valid position");

    add_pos = AddGadget(win, &s->rel_gad, pos);
    check_long("AddGadget re-inserts third gadget at the same position",
               (LONG)add_pos, (LONG)pos);
}

/*
 * Open a second window via OpenWindow() with a struct NewWindow whose
 * FirstGadget is a separate one-gadget list (Border FrontPen 2), verify
 * its rendered border pixel, and close it.
 */
static void test_newwindow_firstgadget(struct ClassicBoolSet *s)
{
    struct NewWindow nw;
    struct Window *win2;
    LONG pen;

    memset(&nw, 0, sizeof(nw));
    nw.LeftEdge    = 40;
    nw.TopEdge     = 30;
    nw.Width       = 200;
    nw.Height      = 100;
    nw.DetailPen   = 0xFF;
    nw.BlockPen    = 0xFF;
    nw.IDCMPFlags  = 0;
    nw.Flags       = WIN_FLAGS;
    nw.FirstGadget = &s->nw_gad;
    nw.Title       = (STRPTR)"gfx_classic_bool 2";
    nw.Type        = WBENCHSCREEN;

    win2 = OpenWindow(&nw);
    check(win2 != NULL,
          "OpenWindow with NewWindow.FirstGadget succeeds");
    if (!win2)
        return;

    pen = ReadPixel(win2->RPort,
                    s->nw_gad.LeftEdge + s->nw_gad.Width / 2,
                    s->nw_gad.TopEdge);
    check_long("NewWindow FirstGadget border pixel matches FrontPen 2",
               pen, pen_value(win2, s->nw_border.FrontPen));

    CloseWindow(win2);
}

int main(void)
{
    struct Window *win;
    ULONG avail, avail_after;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    check(IntuitionBase != NULL,
          "intuition.library v39 opens");
    check(GfxBase != NULL,
          "graphics.library v39 opens");
    if (!IntuitionBase || !GfxBase) {
        if (GfxBase)
            CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase)
            CloseLibrary((struct Library *)IntuitionBase);
        return test_done("gfx_classic_bool");
    }

    if (!build_gadgets(&g_set)) {
        check(FALSE,
          "chip memory for toggle images allocated");
        free_gadgets(&g_set);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return test_done("gfx_classic_bool");
    }

    /* Warm up Intuition window allocations before recording free memory. */
    win = OpenWindowTags(NULL, WA_Left, 20, WA_Top, 20, WA_Width, 100,
                         WA_Height, 40, TAG_DONE);
    if (win)
        CloseWindow(win);

    avail = AvailMem(MEMF_ANY);

    win = open_test_window(&g_set);
    if (win) {
        test_render_and_toggle(win, &g_set);
        test_enable_and_activate(win, &g_set);
        test_list_and_relright(win, &g_set);
        test_newwindow_firstgadget(&g_set);

        CloseWindow(win);

        avail_after = AvailMem(MEMF_ANY);
        check_long("CloseWindow frees all window memory",
                   (LONG)(avail - avail_after), 0);
    }

    free_gadgets(&g_set);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);

    return test_done("gfx_classic_bool");
}

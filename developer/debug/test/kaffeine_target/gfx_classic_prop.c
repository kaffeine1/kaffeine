/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Classic Intuition proportional gadgets built in the program's own
    memory, without GadTools, with no input needed (298 checks). Four
    GTYP_PROPGADGET gadgets with their own PropInfo and knob Image
    structures are linked in order and attached through WA_Gadgets to an
    active 320 by 160 window: a horizontal AUTOKNOB gadget (HorizBody
    MAXBODY / 4), a vertical AUTOKNOB | PROPNEWLOOK gadget (VertPot
    MAXPOT / 3, VertBody MAXBODY / 5), a horizontal gadget with a 16 by 8
    1-plane knob Image of its own in MEMF_CHIP memory (HorizBody
    MAXBODY / 8), and a horizontal and vertical AUTOKNOB | PROPBORDERLESS
    gadget (HorizBody and VertBody MAXBODY / 2); a fifth horizontal
    AUTOKNOB gadget with the same box as the first never goes on a window.
    The program checks the initial PropInfo and knob Image fields of the
    four window gadgets and IDCMP_ACTIVEWINDOW, NewModifyProp() and
    programmatic HorizPot updates with RefreshGList() on the first gadget,
    ModifyProp() on the third gadget, KNOBHIT set and cleared through
    NewModifyProp() flags, RemoveGList(), AddGList() and RefreshGList(),
    OffGadget(), OnGadget() and ActivateGadget(), NewModifyProp() of the
    first gadget with a second window B drawing the knob into B while
    leaving the first window's pixel at the new knob place as background,
    NewModifyProp() of the fifth gadget (on no window) with the first
    window storing the five PropInfo values, NewModifyProp() of the first
    gadget without AUTOKNOB keeping the 38 by 8 knob size and placing the
    knob for MAXPOT / 4 before restoring AUTOKNOB, and the preserved
    PropInfo and freed guest memory after CloseWindow() and FreeMem().
*/

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

#define WIN_LEFT   10
#define WIN_TOP    15
#define WIN_WIDTH  320
#define WIN_HEIGHT 160
#define WIN_FLAGS  (WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_ACTIVATE | WFLG_REPORTMOUSE)
#define WIN_IDCMP  (IDCMP_ACTIVEWINDOW | IDCMP_GADGETDOWN | IDCMP_GADGETUP)

#define WINB_LEFT   340
#define WINB_TOP    15
#define WINB_WIDTH  200
#define WINB_HEIGHT 80
#define WINB_FLAGS  WFLG_SMART_REFRESH

#define KNOB_WIDTH  16
#define KNOB_HEIGHT 8
#define KNOB_WORDS  8
#define KNOB_BYTES  (KNOB_WORDS * sizeof(UWORD))

struct ClassicPropSet
{
    UWORD          *knob_data;
    struct PropInfo pi1, pi2, pi3, pi4, pi5;
    struct Image    im1, im2, im3, im4, im5;
    struct Gadget   g1, g2, g3, g4, g5;
};

struct PropExpect
{
    UWORD flags;
    UWORD hpot, vpot;
    UWORD hbody, vbody;
    UWORD cwidth, cheight;
    UWORD hpotres, vpotres;
    UWORD leftborder, topborder;
};

static const struct PropExpect exp_a1 = {
    0x3, 0, 0, 16383, 65535, 160, 12, 21844, 65535, 4, 2
};

static const struct PropExpect exp_a2 = {
    0x15, 0, 21845, 65535, 13107, 16, 80, 65535, 16384, 1, 1
};

static const struct PropExpect exp_a3 = {
    0x2, 0, 0, 8191, 65535, 160, 12, 9361, 65535, 4, 2
};

static const struct PropExpect exp_a4 = {
    0xF, 0, 0, 32767, 32767, 40, 40, 65534, 65534, 0, 0
};

static const struct PropExpect exp_b1 = {
    0x3, 32767, 0, 16383, 65535, 160, 12, 21844, 65535, 4, 2
};

static const struct PropExpect exp_b1_hit = {
    0x103, 32767, 0, 16383, 65535, 160, 12, 21844, 65535, 4, 2
};

static const struct PropExpect exp_c1 = {
    0x3, 65535, 0, 16383, 65535, 160, 12, 21844, 65535, 4, 2
};

static const struct PropExpect exp_d3 = {
    0x2, 16383, 0, 8191, 65535, 160, 12, 9361, 65535, 4, 2
};

static const struct PropExpect exp_k1 = {
    0x2, 16383, 0, 16383, 65535, 160, 12, 21844, 65535, 4, 2
};

static struct ClassicPropSet g_set;

/* The messages of win up to the first one of class cls, at most ticks
   times 1/50 s; the messages after it stay in the port. */
static struct IntuiMessage *wait_for(struct Window *win, ULONG cls, int ticks,
                                     struct IntuiMessage *copy)
{
    struct IntuiMessage *msg;
    BOOL got;
    int i;

    for (i = 0; i < ticks; i += 5)
    {
        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if ((got = (msg->Class == cls)))
                *copy = *msg;
            ReplyMsg(&msg->ExecMessage);
            if (got)
                return copy;
        }
        Delay(5);
    }
    return NULL;
}

static void check_field(const char *tag, const char *field, LONG got, LONG expected)
{
    char label[64];

    strcpy(label, tag);
    strcat(label, " ");
    strcat(label, field);
    check_long(label, got, expected);
}

static void check_prop(const char *tag, const struct PropInfo *pi,
                       const struct PropExpect *e)
{
    check_field(tag, "Flags", pi->Flags, e->flags);
    check_field(tag, "HorizPot", pi->HorizPot, e->hpot);
    check_field(tag, "VertPot", pi->VertPot, e->vpot);
    check_field(tag, "HorizBody", pi->HorizBody, e->hbody);
    check_field(tag, "VertBody", pi->VertBody, e->vbody);
    check_field(tag, "CWidth", pi->CWidth, e->cwidth);
    check_field(tag, "CHeight", pi->CHeight, e->cheight);
    check_field(tag, "HPotRes", pi->HPotRes, e->hpotres);
    check_field(tag, "VPotRes", pi->VPotRes, e->vpotres);
    check_field(tag, "LeftBorder", pi->LeftBorder, e->leftborder);
    check_field(tag, "TopBorder", pi->TopBorder, e->topborder);
}

static void check_knob(const char *tag, const struct Image *im,
                       WORD left, WORD top, WORD width, WORD height)
{
    check_field(tag, "LeftEdge", im->LeftEdge, left);
    check_field(tag, "TopEdge", im->TopEdge, top);
    check_field(tag, "Width", im->Width, width);
    check_field(tag, "Height", im->Height, height);
}

static void check_custom_knob(const char *tag, const struct Image *im,
                              const UWORD *knob_data)
{
    char label[64];

    check_field(tag, "Depth", im->Depth, 1);
    strcpy(label, tag);
    strcat(label, " ImageData unchanged");
    check(im->ImageData == (UWORD *)knob_data, label);
    check_field(tag, "PlanePick", im->PlanePick, 1);
}

static void init_gadget(struct Gadget *g, WORD left, WORD top, WORD width, WORD height,
                        UWORD flags, UWORD act, struct Image *im, struct PropInfo *pi,
                        UWORD id, struct Gadget *next)
{
    memset(g, 0, sizeof(*g));
    g->NextGadget   = next;
    g->LeftEdge     = left;
    g->TopEdge      = top;
    g->Width        = width;
    g->Height       = height;
    g->Flags        = flags;
    g->Activation   = act;
    g->GadgetType   = GTYP_PROPGADGET;
    g->GadgetRender = im;
    g->SpecialInfo  = pi;
    g->GadgetID     = id;
}

static BOOL build_gadgets(struct ClassicPropSet *s)
{
    WORD i;

    memset(s, 0, sizeof(*s));

    s->knob_data = (UWORD *)AllocMem(KNOB_BYTES, MEMF_CHIP);
    if (!s->knob_data)
        return FALSE;

    for (i = 0; i < KNOB_WORDS; i++)
        s->knob_data[i] = 0xFFFF;

    /* 1: AUTOKNOB, horizontal, a quarter of the body */
    s->pi1.Flags     = AUTOKNOB | FREEHORIZ;
    s->pi1.HorizBody = MAXBODY / 4;
    s->pi1.VertBody  = MAXBODY;

    /* 2: AUTOKNOB, vertical, PROPNEWLOOK */
    s->pi2.Flags     = AUTOKNOB | FREEVERT | PROPNEWLOOK;
    s->pi2.VertPot   = MAXPOT / 3;
    s->pi2.HorizBody = MAXBODY;
    s->pi2.VertBody  = MAXBODY / 5;

    /* 3: a knob of its own, 16x8, one plane, horizontal */
    s->pi3.Flags     = FREEHORIZ;
    s->pi3.HorizBody = MAXBODY / 8;
    s->pi3.VertBody  = MAXBODY;
    s->im3.Width     = KNOB_WIDTH;
    s->im3.Height    = KNOB_HEIGHT;
    s->im3.Depth     = 1;
    s->im3.ImageData = s->knob_data;
    s->im3.PlanePick = 1;

    /* 4: AUTOKNOB, PROPBORDERLESS, both ways */
    s->pi4.Flags     = AUTOKNOB | FREEHORIZ | FREEVERT | PROPBORDERLESS;
    s->pi4.HorizBody = MAXBODY / 2;
    s->pi4.VertBody  = MAXBODY / 2;

    /* 5: AUTOKNOB, horizontal, same box as g1, never on a window */
    s->pi5.Flags     = AUTOKNOB | FREEHORIZ;
    s->pi5.HorizBody = MAXBODY / 4;
    s->pi5.VertBody  = MAXBODY;

    init_gadget(&s->g5, 20, 30, 160, 12, GFLG_GADGHCOMP, GACT_RELVERIFY,
                &s->im5, &s->pi5, 5, NULL);
    init_gadget(&s->g4, 250, 30, 40, 40, GFLG_GADGHNONE, GACT_RELVERIFY,
                &s->im4, &s->pi4, 4, NULL);
    init_gadget(&s->g3, 20, 90, 160, 12, GFLG_GADGHNONE | GFLG_GADGIMAGE, GACT_RELVERIFY,
                &s->im3, &s->pi3, 3, &s->g4);
    init_gadget(&s->g2, 200, 30, 16, 80, GFLG_GADGHCOMP, GACT_RELVERIFY,
                &s->im2, &s->pi2, 2, &s->g3);
    init_gadget(&s->g1, 20, 30, 160, 12, GFLG_GADGHCOMP,
                GACT_RELVERIFY | GACT_IMMEDIATE | GACT_FOLLOWMOUSE,
                &s->im1, &s->pi1, 1, &s->g2);

    return TRUE;
}

static void free_gadgets(struct ClassicPropSet *s)
{
    if (s->knob_data)
    {
        FreeMem(s->knob_data, KNOB_BYTES);
        s->knob_data = NULL;
    }
}

/* Step A: initial PropInfo and knob Image fields after OpenWindowTags,
   followed by IDCMP_ACTIVEWINDOW within 10 seconds. */
static void check_initial(struct Window *win, struct ClassicPropSet *s)
{
    struct IntuiMessage msg;

    check_prop("A1", &s->pi1, &exp_a1);
    check_knob("A1 knob", &s->im1, 0, 0, 38, 8);

    check_prop("A2", &s->pi2, &exp_a2);
    check_knob("A2 knob", &s->im2, 0, 20, 14, 16);

    check_prop("A3", &s->pi3, &exp_a3);
    check_knob("A3 knob", &s->im3, 0, 0, 16, 8);
    check_custom_knob("A3 knob", &s->im3, s->knob_data);

    check_prop("A4", &s->pi4, &exp_a4);
    check_knob("A4 knob", &s->im4, 0, 0, 20, 20);

    check(wait_for(win, IDCMP_ACTIVEWINDOW, 10 * 50, &msg) != NULL,
          "IDCMP_ACTIVEWINDOW comes within 10 s");
}

/* Steps B and C: NewModifyProp() of the first gadget to MAXPOT / 2, and
   programmatic HorizPot updates to MAXPOT and 0 with RefreshGList(). */
static void check_newmodify_and_refresh(struct Window *win, struct ClassicPropSet *s)
{
    NewModifyProp(&s->g1, win, NULL, AUTOKNOB | FREEHORIZ,
                  MAXPOT / 2, 0, MAXBODY / 4, MAXBODY, 1);
    check_prop("B1", &s->pi1, &exp_b1);
    check_knob("B1 knob", &s->im1, 57, 0, 38, 8);

    s->pi1.HorizPot = MAXPOT;
    RefreshGList(&s->g1, win, NULL, 1);
    check_prop("C1", &s->pi1, &exp_c1);
    check_knob("C1 knob", &s->im1, 114, 0, 38, 8);

    s->pi1.HorizPot = 0;
    RefreshGList(&s->g1, win, NULL, 1);
    check_prop("C1b", &s->pi1, &exp_a1);
    check_knob("C1b knob", &s->im1, 0, 0, 38, 8);
}

/* Step D and KNOBHIT flags: ModifyProp() of the third gadget to MAXPOT / 4,
   then NewModifyProp() of the first gadget with and without KNOBHIT. */
static void check_modify_and_flags(struct Window *win, struct ClassicPropSet *s)
{
    ModifyProp(&s->g3, win, NULL, FREEHORIZ, MAXPOT / 4, 0, MAXBODY / 8, MAXBODY);
    check_prop("D3", &s->pi3, &exp_d3);
    check_knob("D3 knob", &s->im3, 34, 0, 16, 8);
    check_custom_knob("D3 knob", &s->im3, s->knob_data);

    NewModifyProp(&s->g1, win, NULL, AUTOKNOB | FREEHORIZ | KNOBHIT,
                  MAXPOT / 2, 0, MAXBODY / 4, MAXBODY, 1);
    check_prop("B2 with KNOBHIT", &s->pi1, &exp_b1_hit);
    check_knob("B2 with KNOBHIT knob", &s->im1, 57, 0, 38, 8);

    NewModifyProp(&s->g1, win, NULL, AUTOKNOB | FREEHORIZ,
                  MAXPOT / 2, 0, MAXBODY / 4, MAXBODY, 1);
    check_prop("B3 without KNOBHIT", &s->pi1, &exp_b1);
    check_knob("B3 without KNOBHIT knob", &s->im1, 57, 0, 38, 8);
}

/* Step G: RemoveGList() and AddGList() of the first gadget, then RefreshGList(). */
static void check_relist(struct Window *win, struct ClassicPropSet *s)
{
    UWORD pos, add_pos;

    pos = RemoveGList(win, &s->g1, 1);
    check(pos != 0xFFFF,
          "RemoveGList of first gadget gives a position other than 0xFFFF");
    check_prop("G1 off", &s->pi1, &exp_b1);
    check_knob("G1 off knob", &s->im1, 57, 0, 38, 8);

    add_pos = AddGList(win, &s->g1, pos, 1, NULL);
    check_long("AddGList of first gadget back gives the same position",
               (LONG)add_pos, (LONG)pos);
    check_prop("G1 added", &s->pi1, &exp_b1);
    check_knob("G1 added knob", &s->im1, 57, 0, 38, 8);

    RefreshGList(&s->g1, win, NULL, 1);
    check_prop("G1 on", &s->pi1, &exp_b1);
    check_knob("G1 on knob", &s->im1, 57, 0, 38, 8);
}

/* Step H: OffGadget(), OnGadget() and ActivateGadget() on the first gadget. */
static void check_enable_and_activate(struct Window *win, struct ClassicPropSet *s)
{
    OffGadget(&s->g1, win, NULL);
    check((s->g1.Flags & GFLG_DISABLED) != 0,
          "OffGadget sets GFLG_DISABLED on first gadget");
    check_prop("H1 off", &s->pi1, &exp_b1);

    OnGadget(&s->g1, win, NULL);
    check((s->g1.Flags & GFLG_DISABLED) == 0,
          "OnGadget clears GFLG_DISABLED on first gadget");
    check_prop("H1 on", &s->pi1, &exp_b1);

    check(ActivateGadget(&s->g1, win, NULL) != FALSE,
          "ActivateGadget of the first gadget gives TRUE");
}

/* Step E: NewModifyProp() of the first gadget (on win) with a second
   window B draws the knob into B at the gadget's box and leaves win as
   it was. */
static void check_other_window(struct Window *win, struct ClassicPropSet *s)
{
    struct Window *win_b;
    LONG knob_x, knob_y;
    LONG b_bg, a_bg, b_knob, a_after;

    win_b = OpenWindowTags(NULL,
                           WA_Left, WINB_LEFT,
                           WA_Top, WINB_TOP,
                           WA_Width, WINB_WIDTH,
                           WA_Height, WINB_HEIGHT,
                           WA_Flags, WINB_FLAGS,
                           TAG_DONE);
    check(win_b != NULL, "OpenWindowTags for second window B succeeds");
    if (!win_b)
        return;

    NewModifyProp(&s->g1, win, NULL, AUTOKNOB | FREEHORIZ,
                  0, 0, s->pi1.HorizBody, s->pi1.VertBody, 1);

    /* Midpoint of the 38 by 8 knob at LeftEdge 57 (HorizPot MAXPOT / 2),
       which does not overlap the knob at LeftEdge 0 (x 24..61 vs 81..118). */
    knob_x = s->g1.LeftEdge + s->pi1.LeftBorder + 57 + 38 / 2;
    knob_y = s->g1.TopEdge + s->g1.Height / 2;

    b_bg = ReadPixel(win_b->RPort, knob_x, knob_y);
    a_bg = ReadPixel(win->RPort, knob_x, knob_y);

    NewModifyProp(&s->g1, win_b, NULL, AUTOKNOB | FREEHORIZ,
                  MAXPOT / 2, 0, s->pi1.HorizBody, s->pi1.VertBody, 1);
    check_prop("E1", &s->pi1, &exp_b1);
    check_knob("E1 knob", &s->im1, 57, 0, 38, 8);

    b_knob = ReadPixel(win_b->RPort, knob_x, knob_y);
    a_after = ReadPixel(win->RPort, knob_x, knob_y);
    check(b_knob != b_bg,
          "E1 knob pixel in second window B differs from B background");
    check_long("E1 pixel in first window at new knob place stays background",
               a_after, a_bg);

    Delay(10);
    CloseWindow(win_b);
}

/* Step F: NewModifyProp() of a gadget that is on no window (g5) with win.
   Only the five values stored into PropInfo are checked: AmigaOS 3.2.3
   also works out CWidth to TopBorder, writes the knob place and size into
   the gadget's Image and draws the gadget into win, whereas the bridge
   stores the five values alone. Never call NewModifyProp() or ModifyProp()
   with a NULL window, as that hangs AmigaOS 3.2.3. */
static void check_off_window(struct Window *win, struct ClassicPropSet *s)
{
    NewModifyProp(&s->g5, win, NULL, AUTOKNOB | FREEHORIZ,
                  MAXPOT / 4, 0, MAXBODY / 4, MAXBODY, 1);
    check_field("F5", "Flags", s->pi5.Flags, AUTOKNOB | FREEHORIZ);
    check_field("F5", "HorizPot", s->pi5.HorizPot, MAXPOT / 4);
    check_field("F5", "VertPot", s->pi5.VertPot, 0);
    check_field("F5", "HorizBody", s->pi5.HorizBody, MAXBODY / 4);
    check_field("F5", "VertBody", s->pi5.VertBody, MAXBODY);
}

/* Step K: NewModifyProp() of the first gadget without AUTOKNOB and with
   unchanged bodies, then back with AUTOKNOB | FREEHORIZ. On AmigaOS 3.2.3
   the knob without AUTOKNOB keeps the Width (38) and Height (8) that
   Intuition last wrote into im1 as an AUTOKNOB, while with the bridge the
   native copy keeps AUTOKNOB until RemoveGList() and AddGList() and the
   program's PropInfo gets the Flags given, so both systems produce the
   same PropInfo and knob Image fields. */
static void check_autoknob_flag(struct Window *win, struct ClassicPropSet *s)
{
    NewModifyProp(&s->g1, win, NULL, FREEHORIZ,
                  MAXPOT / 4, 0, s->pi1.HorizBody, s->pi1.VertBody, 1);
    check_prop("K1 without AUTOKNOB", &s->pi1, &exp_k1);
    check_knob("K1 without AUTOKNOB knob", &s->im1, 28, 0, 38, 8);

    NewModifyProp(&s->g1, win, NULL, AUTOKNOB | FREEHORIZ,
                  MAXPOT / 2, 0, s->pi1.HorizBody, s->pi1.VertBody, 1);
    check_prop("K2 with AUTOKNOB", &s->pi1, &exp_b1);
    check_knob("K2 with AUTOKNOB knob", &s->im1, 57, 0, 38, 8);
}

int main(void)
{
    struct Window *win;
    ULONG avail, avail_after;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    check(IntuitionBase != NULL, "intuition.library v39 opens");
    check(GfxBase != NULL, "graphics.library v39 opens");
    if (!IntuitionBase || !GfxBase)
    {
        if (GfxBase)
            CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase)
            CloseLibrary((struct Library *)IntuitionBase);
        return test_done("gfx_classic_prop");
    }

    /* Warm up Intuition window allocations before recording free memory. */
    win = OpenWindowTags(NULL, WA_Left, 20, WA_Top, 20, WA_Width, 100,
                         WA_Height, 40, TAG_DONE);
    if (win)
        CloseWindow(win);

    avail = AvailMem(MEMF_ANY);

    check(build_gadgets(&g_set), "chip memory for knob image allocated");
    if (g_set.knob_data)
    {
        win = OpenWindowTags(NULL,
                             WA_Left, WIN_LEFT,
                             WA_Top, WIN_TOP,
                             WA_Width, WIN_WIDTH,
                             WA_Height, WIN_HEIGHT,
                             WA_Flags, WIN_FLAGS,
                             WA_IDCMP, WIN_IDCMP,
                             WA_Gadgets, (ULONG)&g_set.g1,
                             WA_Title, (ULONG)"gfx_classic_prop",
                             TAG_DONE);
        check(win != NULL, "OpenWindowTags with WA_Gadgets succeeds");
        if (win)
        {
            check_initial(win, &g_set);
            check_newmodify_and_refresh(win, &g_set);
            check_modify_and_flags(win, &g_set);
            check_relist(win, &g_set);
            check_enable_and_activate(win, &g_set);
            check_other_window(win, &g_set);
            check_off_window(win, &g_set);
            check_autoknob_flag(win, &g_set);

            CloseWindow(win);
            check_prop("Z1 after CloseWindow", &g_set.pi1, &exp_b1);
        }

        free_gadgets(&g_set);

        avail_after = AvailMem(MEMF_ANY);
        check_long("CloseWindow and FreeMem free all window and image memory",
                   (LONG)(avail - avail_after), 0);
    }

    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);

    return test_done("gfx_classic_prop");
}

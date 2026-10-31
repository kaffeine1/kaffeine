/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    States, flags, geometry and messages of classic Intuition proportional
    gadgets that a program builds itself (GTYP_PROPGADGET with a PropInfo
    in SpecialInfo and an Image in GadgetRender), as on AmigaOS 3.2.3.
    OpenWindowTags() computes the PropInfo container and pot step fields
    and writes the knob position and dimensions into the Image in
    GadgetRender. A bordered gadget without PROPNEWLOOK gets LeftBorder 4
    and TopBorder 2, with CWidth and CHeight equal to the gadget's Width
    and Height; in a 160 by 12 FREEHORIZ AUTOKNOB gadget with HorizBody
    MAXBODY / 4 and VertBody MAXBODY, HPotRes is 21844, VPotRes is 65535,
    and the knob Image is 38 by 8 at 0, 0. A bordered FREEVERT AUTOKNOB
    gadget with PROPNEWLOOK gets LeftBorder 1 and TopBorder 1; in a 16 by
    80 gadget with VertPot MAXPOT / 3, HorizBody MAXBODY and VertBody
    MAXBODY / 5, HPotRes is 65535, VPotRes is 16384, and the knob Image is
    14 by 16 at 0, 20. A bordered FREEHORIZ gadget with a 16 by 8 knob
    Image of its own and HorizBody MAXBODY / 8 in a 160 by 12 gadget gets
    LeftBorder 4, TopBorder 2, HPotRes 9361 and VPotRes 65535, keeping the
    knob Image 16 by 8 at 0, 0. A PROPBORDERLESS AUTOKNOB gadget moving on
    both axes gets LeftBorder 0 and TopBorder 0; in a 40 by 40 gadget with
    HorizBody and VertBody MAXBODY / 2, HPotRes and VPotRes are 65534 and
    the knob Image is 20 by 20 at 0, 0. NewModifyProp() on the first
    gadget to MAXPOT / 2 sets HorizPot to 32767 and moves the knob Image to
    LeftEdge 57. Writing HorizPot directly to MAXPOT and 0 and calling
    RefreshGList() keeps HorizPot at 65535 and 0 and moves the knob Image
    to LeftEdge 114 and 0. ModifyProp() on the custom-knob gadget to
    MAXPOT / 4 sets HorizPot to 16383 and moves its knob Image to LeftEdge
    34. Pressing the left mouse button on the first gadget's knob through
    input.device sets KNOBHIT in PropInfo Flags (0x103) and GFLG_SELECTED
    in Gadget Flags; dragging the pointer 30 and 60 pixels right while the
    button is held updates HorizPot to 17246 (with knob LeftEdge 30) and
    34492; releasing the button clears GFLG_SELECTED, leaves KNOBHIT set in
    PropInfo Flags (0x103), leaves HorizPot at 34492, sets VertPot to
    65535, sets knob LeftEdge to 60, and sends one IDCMP_GADGETDOWN,
    IDCMP_MOUSEMOVE messages as the knob moves and one IDCMP_GADGETUP,
    which names the gadget; a pointer move after the release changes no
    pot. Clicking in the container to the right of the knob clears
    KNOBHIT in PropInfo Flags (0x3), steps HorizPot by HPotRes to 56336,
    moves the knob Image to LeftEdge 97, and sends one IDCMP_GADGETDOWN and
    one IDCMP_GADGETUP. RemoveGList() and AddGList() return the same gadget
    position and leave PropInfo and the knob Image unchanged. OffGadget()
    sets GFLG_DISABLED and OnGadget() clears it without changing PropInfo,
    ActivateGadget() on the proportional gadget returns TRUE, and
    CloseWindow() leaves PropInfo unchanged.
    The pointer events assume mouse buttons that the input preferences do
    not swap. Output in the form of the kaffeine target tests: a line per
    check and a summary.
*/

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <stdio.h>
#include <string.h>

#define WIN_WIDTH  320
#define WIN_HEIGHT 160

static int checks, failures;

static int check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    return ok;
}

static struct MsgPort *inport;
static struct IOStdReq *inreq;

/* An event through input.device */
static void event(UBYTE class, UBYTE subclass, UWORD code, UWORD qual, APTR address)
{
    struct InputEvent ie;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = class;
    ie.ie_SubClass = subclass;
    ie.ie_Code = code;
    ie.ie_Qualifier = qual;
    ie.ie_EventAddress = address;
    inreq->io_Command = IND_WRITEEVENT;
    inreq->io_Data = &ie;
    inreq->io_Length = sizeof(ie);
    DoIO((struct IORequest *)inreq);
}

/* The pointer to x, y of the window, with the left button held or not */
static void point(struct Window *win, WORD x, WORD y, BOOL held)
{
    struct IEPointerPixel pp;

    pp.iepp_Screen = win->WScreen;
    pp.iepp_Position.X = win->LeftEdge + x;
    pp.iepp_Position.Y = win->TopEdge + y;
    event(IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL, IECODE_NOBUTTON,
          held ? IEQUALIFIER_LEFTBUTTON : 0, &pp);
    Delay(2);
}

static void button(BOOL down)
{
    event(IECLASS_RAWMOUSE, 0, down ? IECODE_LBUTTON : IECODE_LBUTTON | IECODE_UP_PREFIX,
          IEQUALIFIER_RELATIVEMOUSE | (down ? IEQUALIFIER_LEFTBUTTON : 0), NULL);
    Delay(2);
}

static LONG downs, ups, moves;
static struct Gadget *up_address;

/* The messages so far, counted */
static void drain(struct Window *win)
{
    struct IntuiMessage *im;

    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)))
    {
        if (im->Class == IDCMP_GADGETDOWN)
            downs++;
        else if (im->Class == IDCMP_GADGETUP)
        {
            ups++;
            up_address = (struct Gadget *)im->IAddress;
        }
        else if (im->Class == IDCMP_MOUSEMOVE)
            moves++;
        ReplyMsg((struct Message *)im);
    }
}

static BOOL wait_active(struct Window *win)
{
    struct IntuiMessage *im;
    LONG i;
    BOOL active = FALSE;

    for (i = 0; i < 200 && !active; i++)
    {
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (im->Class == IDCMP_ACTIVEWINDOW)
                active = TRUE;
            ReplyMsg((struct Message *)im);
        }
        if (!active)
            Delay(5);
    }
    return active;
}

static BOOL prop_is(const struct PropInfo *pi, UWORD flags, UWORD hpot, UWORD vpot,
                    UWORD hbody, UWORD vbody, UWORD cwidth, UWORD cheight,
                    UWORD hpotres, UWORD vpotres, UWORD lborder, UWORD tborder)
{
    return pi->Flags == flags &&
           pi->HorizPot == hpot &&
           pi->VertPot == vpot &&
           pi->HorizBody == hbody &&
           pi->VertBody == vbody &&
           pi->CWidth == cwidth &&
           pi->CHeight == cheight &&
           pi->HPotRes == hpotres &&
           pi->VPotRes == vpotres &&
           pi->LeftBorder == lborder &&
           pi->TopBorder == tborder;
}

static BOOL knob_is(const struct Image *im, WORD left, WORD top, WORD width, WORD height)
{
    return im->LeftEdge == left &&
           im->TopEdge == top &&
           im->Width == width &&
           im->Height == height;
}

static UWORD knobdata[8] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static struct PropInfo pi1, pi2, pi3, pi4;
static struct Image im1, im2, im3, im4;
static struct Gadget g1, g2, g3, g4;

static void gadget(struct Gadget *g, WORD left, WORD top, WORD width, WORD height, UWORD flags,
                   UWORD act, struct Image *im, struct PropInfo *pi, UWORD id, struct Gadget *next)
{
    memset(g, 0, sizeof(*g));
    g->NextGadget = next;
    g->LeftEdge = left;
    g->TopEdge = top;
    g->Width = width;
    g->Height = height;
    g->Flags = flags;
    g->Activation = act;
    g->GadgetType = GTYP_PROPGADGET;
    g->GadgetRender = im;
    g->SpecialInfo = pi;
    g->GadgetID = id;
}

/* A. PropInfo and knob Image fields of all four gadgets after
   OpenWindowTags(), and IDCMP_ACTIVEWINDOW. */
static void check_open(struct Window *win)
{
    check(pi1.Flags == 0x3 && pi1.HorizPot == 0 && pi1.VertPot == 0 &&
          pi1.HorizBody == 16383 && pi1.VertBody == 65535,
          "A1 PropInfo Flags 0x3, Pots 0, 0, Bodies 16383, 65535");
    check(pi1.CWidth == 160 && pi1.CHeight == 12 &&
          pi1.HPotRes == 21844 && pi1.VPotRes == 65535 &&
          pi1.LeftBorder == 4 && pi1.TopBorder == 2,
          "A1 PropInfo CWidth 160, CHeight 12, PotRes 21844, 65535, Borders 4, 2");
    check(knob_is(&im1, 0, 0, 38, 8),
          "A1 knob Image LeftEdge 0, TopEdge 0, Width 38, Height 8");

    check(pi2.Flags == 0x15 && pi2.HorizPot == 0 && pi2.VertPot == 21845 &&
          pi2.HorizBody == 65535 && pi2.VertBody == 13107,
          "A2 PropInfo Flags 0x15, Pots 0, 21845, Bodies 65535, 13107");
    check(pi2.CWidth == 16 && pi2.CHeight == 80 &&
          pi2.HPotRes == 65535 && pi2.VPotRes == 16384 &&
          pi2.LeftBorder == 1 && pi2.TopBorder == 1,
          "A2 PropInfo CWidth 16, CHeight 80, PotRes 65535, 16384, Borders 1, 1");
    check(knob_is(&im2, 0, 20, 14, 16),
          "A2 knob Image LeftEdge 0, TopEdge 20, Width 14, Height 16");

    check(pi3.Flags == 0x2 && pi3.HorizPot == 0 && pi3.VertPot == 0 &&
          pi3.HorizBody == 8191 && pi3.VertBody == 65535,
          "A3 PropInfo Flags 0x2, Pots 0, 0, Bodies 8191, 65535");
    check(pi3.CWidth == 160 && pi3.CHeight == 12 &&
          pi3.HPotRes == 9361 && pi3.VPotRes == 65535 &&
          pi3.LeftBorder == 4 && pi3.TopBorder == 2,
          "A3 PropInfo CWidth 160, CHeight 12, PotRes 9361, 65535, Borders 4, 2");
    check(knob_is(&im3, 0, 0, 16, 8),
          "A3 knob Image LeftEdge 0, TopEdge 0, Width 16, Height 8");

    check(pi4.Flags == 0xF && pi4.HorizPot == 0 && pi4.VertPot == 0 &&
          pi4.HorizBody == 32767 && pi4.VertBody == 32767,
          "A4 PropInfo Flags 0xF, Pots 0, 0, Bodies 32767, 32767");
    check(pi4.CWidth == 40 && pi4.CHeight == 40 &&
          pi4.HPotRes == 65534 && pi4.VPotRes == 65534 &&
          pi4.LeftBorder == 0 && pi4.TopBorder == 0,
          "A4 PropInfo CWidth 40, CHeight 40, PotRes 65534, 65534, Borders 0, 0");
    check(knob_is(&im4, 0, 0, 20, 20),
          "A4 knob Image LeftEdge 0, TopEdge 0, Width 20, Height 20");

    check(wait_active(win), "IDCMP_ACTIVEWINDOW received");
}

/* B. NewModifyProp() of g1 to MAXPOT / 2 sets HorizPot to 32767 and knob
   LeftEdge to 57. */
static void check_newmodifyprop(struct Window *win)
{
    NewModifyProp(&g1, win, NULL, AUTOKNOB | FREEHORIZ, MAXPOT / 2, 0, MAXBODY / 4, MAXBODY, 1);
    check(pi1.HorizPot == 32767,
          "B1 HorizPot after NewModifyProp() to MAXPOT / 2 is 32767");
    check(im1.LeftEdge == 57,
          "B1 knob LeftEdge after NewModifyProp() to MAXPOT / 2 is 57");
}

/* C. Writing HorizPot of g1 before RefreshGList() sets knob LeftEdge to
   114 at MAXPOT and 0 at 0. */
static void check_refresh_horizpot(struct Window *win)
{
    pi1.HorizPot = MAXPOT;
    RefreshGList(&g1, win, NULL, 1);
    check(pi1.HorizPot == 65535,
          "C1 HorizPot set to MAXPOT stays 65535 after RefreshGList()");
    check(im1.LeftEdge == 114,
          "C1 knob LeftEdge after RefreshGList() with HorizPot MAXPOT is 114");

    pi1.HorizPot = 0;
    RefreshGList(&g1, win, NULL, 1);
    check(pi1.HorizPot == 0,
          "C1b HorizPot set to 0 stays 0 after RefreshGList()");
    check(im1.LeftEdge == 0,
          "C1b knob LeftEdge after RefreshGList() with HorizPot 0 is 0");
}

/* D. ModifyProp() of g3 to MAXPOT / 4 sets HorizPot to 16383 and knob
   LeftEdge to 34. */
static void check_modifyprop(struct Window *win)
{
    ModifyProp(&g3, win, NULL, FREEHORIZ, MAXPOT / 4, 0, MAXBODY / 8, MAXBODY);
    check(pi3.HorizPot == 16383,
          "D3 HorizPot after ModifyProp() to MAXPOT / 4 is 16383");
    check(im3.LeftEdge == 34,
          "D3 knob LeftEdge after ModifyProp() to MAXPOT / 4 is 34");
}

/* E. Dragging g1's knob 30 and 60 pixels right through input.device. */
static void check_drag(struct Window *win)
{
    LONG kx, ky;

    kx = g1.LeftEdge + im1.LeftEdge + im1.Width / 2;
    ky = g1.TopEdge + im1.TopEdge + im1.Height / 2;
    point(win, kx, ky, FALSE);
    /* the messages of the drag only, not of the pointer moving there */
    drain(win);
    downs = ups = moves = 0;
    up_address = NULL;
    button(TRUE);
    check(pi1.Flags == 0x103,
          "E PropInfo Flags while button held is 0x103 (KNOBHIT set)");
    check((g1.Flags & GFLG_SELECTED) != 0 && g1.Flags == GFLG_SELECTED,
          "E gadget Flags while button held has GFLG_SELECTED (128)");

    point(win, kx + 30, ky, TRUE);
    check(pi1.HorizPot == 17246,
          "E HorizPot while held after first move (30 pixels right) is 17246");
    check(im1.LeftEdge == 30,
          "E knob LeftEdge while held after first move is 30");

    point(win, kx + 60, ky, TRUE);
    check(pi1.HorizPot == 34492,
          "E HorizPot while held after second move (60 pixels right) is 34492");

    button(FALSE);
    Delay(5);
    drain(win);
    check(pi1.Flags == 0x103,
          "E1 PropInfo Flags after release is 0x103 (KNOBHIT still set)");
    check(pi1.HorizPot == 34492,
          "E1 HorizPot after release is 34492");
    check(pi1.VertPot == 65535,
          "E1 VertPot after release is 65535");
    check(im1.LeftEdge == 60,
          "E1 knob LeftEdge after release is 60");
    check((g1.Flags & GFLG_SELECTED) == 0 && g1.Flags == 0,
          "E gadget Flags after release has no GFLG_SELECTED (0)");
    check(downs == 1,
          "E drag sent one IDCMP_GADGETDOWN");
    check(ups == 1,
          "E drag sent one IDCMP_GADGETUP");
    check(moves >= 2,
          "E drag sent an IDCMP_MOUSEMOVE for each of its two moves at least");
    check(up_address == &g1,
          "E the IDCMP_GADGETUP names g1");

    point(win, kx + 90, ky, FALSE);
    drain(win);
    check(pi1.HorizPot == 34492 && im1.LeftEdge == 60,
          "E a pointer move after the release leaves HorizPot 34492 and knob LeftEdge 60");
}

/* F. Clicking in the container to the right of g1's knob. */
static void check_container_click(struct Window *win)
{
    LONG kx, ky;

    downs = ups = moves = 0;
    kx = g1.LeftEdge + im1.LeftEdge + im1.Width + 10;
    if (kx > g1.LeftEdge + g1.Width - 2)
        kx = g1.LeftEdge + 2;
    ky = g1.TopEdge + im1.TopEdge + im1.Height / 2;
    point(win, kx, ky, FALSE);
    button(TRUE);
    button(FALSE);
    Delay(5);
    drain(win);
    check(pi1.Flags == 0x3,
          "F1 PropInfo Flags after container click is 0x3 (KNOBHIT cleared)");
    check(pi1.HorizPot == 56336,
          "F1 HorizPot after container click is 56336");
    check(im1.LeftEdge == 97,
          "F1 knob LeftEdge after container click is 97");
    check(downs == 1,
          "F container click sent one IDCMP_GADGETDOWN");
    check(ups == 1,
          "F container click sent one IDCMP_GADGETUP");
}

/* G. RemoveGList() and AddGList() of g1 return the same position and leave
   PropInfo and knob Image unchanged. */
static void check_remove_add(struct Window *win)
{
    UWORD pos, add_pos;

    pos = RemoveGList(win, &g1, 1);
    check(prop_is(&pi1, 0x3, 56336, 65535, 16383, 65535, 160, 12, 21844, 65535, 4, 2),
          "G1 off PropInfo unchanged after RemoveGList()");
    add_pos = AddGList(win, &g1, pos, 1, NULL);
    check(pos != (UWORD)~0 && add_pos == pos,
          "G RemoveGList() and AddGList() give the same position for g1");
    RefreshGList(&g1, win, NULL, 1);
    check(prop_is(&pi1, 0x3, 56336, 65535, 16383, 65535, 160, 12, 21844, 65535, 4, 2) &&
          knob_is(&im1, 97, 0, 38, 8),
          "G1 on PropInfo and knob Image unchanged after AddGList() and RefreshGList()");
}

/* H. OffGadget(), OnGadget() and ActivateGadget() on g1. */
static void check_off_on_activate(struct Window *win)
{
    OffGadget(&g1, win, NULL);
    check((g1.Flags & GFLG_DISABLED) != 0 && g1.Flags == GFLG_DISABLED,
          "H OffGadget() sets GFLG_DISABLED (256) on g1");
    check(prop_is(&pi1, 0x3, 56336, 65535, 16383, 65535, 160, 12, 21844, 65535, 4, 2),
          "H1 PropInfo unchanged after OffGadget()");

    OnGadget(&g1, win, NULL);
    check((g1.Flags & GFLG_DISABLED) == 0 && g1.Flags == 0,
          "H OnGadget() clears GFLG_DISABLED (0) on g1");
    check(ActivateGadget(&g1, win, NULL) == TRUE,
          "H ActivateGadget() of g1 returns TRUE");
}

int main(void)
{
    struct Window *win;

    inport = CreateMsgPort();
    if (inport)
        inreq = (struct IOStdReq *)CreateIORequest(inport, sizeof(*inreq));
    if (!check(inreq && OpenDevice("input.device", 0, (struct IORequest *)inreq, 0) == 0,
               "input.device opened"))
    {
        if (inreq)
            DeleteIORequest((struct IORequest *)inreq);
        if (inport)
            DeleteMsgPort(inport);
        printf("propstates: %d checks, %d failed\n", checks, failures);
        return 20;
    }

    /* 1: AUTOKNOB, horizontal, a quarter of the body */
    memset(&pi1, 0, sizeof(pi1));
    pi1.Flags = AUTOKNOB | FREEHORIZ;
    pi1.HorizBody = MAXBODY / 4;
    pi1.VertBody = MAXBODY;
    memset(&im1, 0, sizeof(im1));
    /* 2: AUTOKNOB, vertical, PROPNEWLOOK */
    memset(&pi2, 0, sizeof(pi2));
    pi2.Flags = AUTOKNOB | FREEVERT | PROPNEWLOOK;
    pi2.VertPot = MAXPOT / 3;
    pi2.HorizBody = MAXBODY;
    pi2.VertBody = MAXBODY / 5;
    memset(&im2, 0, sizeof(im2));
    /* 3: a knob of its own, 16x8, one plane, horizontal */
    memset(&pi3, 0, sizeof(pi3));
    pi3.Flags = FREEHORIZ;
    pi3.HorizBody = MAXBODY / 8;
    pi3.VertBody = MAXBODY;
    memset(&im3, 0, sizeof(im3));
    im3.Width = 16;
    im3.Height = 8;
    im3.Depth = 1;
    im3.ImageData = knobdata;
    im3.PlanePick = 1;
    /* 4: AUTOKNOB, PROPBORDERLESS, both ways */
    memset(&pi4, 0, sizeof(pi4));
    pi4.Flags = AUTOKNOB | FREEHORIZ | FREEVERT | PROPBORDERLESS;
    pi4.HorizBody = MAXBODY / 2;
    pi4.VertBody = MAXBODY / 2;
    memset(&im4, 0, sizeof(im4));

    gadget(&g4, 250, 30, 40, 40, GFLG_GADGHNONE, GACT_RELVERIFY, &im4, &pi4, 4, NULL);
    gadget(&g3, 20, 90, 160, 12, GFLG_GADGHNONE | GFLG_GADGIMAGE, GACT_RELVERIFY, &im3, &pi3, 3, &g4);
    gadget(&g2, 200, 30, 16, 80, GFLG_GADGHCOMP, GACT_RELVERIFY, &im2, &pi2, 2, &g3);
    gadget(&g1, 20, 30, 160, 12, GFLG_GADGHCOMP,
           GACT_RELVERIFY | GACT_IMMEDIATE | GACT_FOLLOWMOUSE, &im1, &pi1, 1, &g2);

    win = OpenWindowTags(NULL,
                         WA_Left, 10,
                         WA_Top, 15,
                         WA_Width, WIN_WIDTH,
                         WA_Height, WIN_HEIGHT,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_ACTIVATE | WFLG_REPORTMOUSE,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETDOWN | IDCMP_GADGETUP | IDCMP_MOUSEMOVE,
                         WA_Gadgets, (IPTR)&g1,
                         WA_Title, (IPTR)"propstates",
                         TAG_DONE);
    if (check(win != NULL, "window with the four proportional gadgets opened"))
    {
        check_open(win);
        check_newmodifyprop(win);
        check_refresh_horizpot(win);
        check_modifyprop(win);
        check_drag(win);
        check_container_click(win);
        check_remove_add(win);
        check_off_on_activate(win);
        CloseWindow(win);
        check(prop_is(&pi1, 0x3, 56336, 65535, 16383, 65535, 160, 12, 21844, 65535, 4, 2),
              "Z1 PropInfo unchanged after CloseWindow()");
    }

    CloseDevice((struct IORequest *)inreq);
    DeleteIORequest((struct IORequest *)inreq);
    DeleteMsgPort(inport);

    printf("propstates: %d checks, %d failed\n", checks, failures);
    return failures ? 20 : 0;
}

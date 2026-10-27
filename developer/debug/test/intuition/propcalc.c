/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Arithmetic of classic Intuition proportional gadgets, as AmigaOS 3.2.3
    computes it. A bordered gadget gets LeftBorder 4 and TopBorder 2 without
    PROPNEWLOOK and 1 and 1 with PROPNEWLOOK, CWidth and CHeight equal the
    gadget's Width and Height, and the container interior is CWidth less
    twice LeftBorder by CHeight less twice TopBorder. For an AUTOKNOB
    gadget, the knob size on a moving axis is the interior times the body
    divided by 65535, rounded up, kept between its minimum (6 horizontally,
    4 vertically) and the interior, or 1 when the interior is less than 1,
    while on a fixed axis the knob fills the interior; a
    gadget with a knob Image of its own keeps that image's Width and Height.
    The knob place on a moving axis is (pot + 1) times the room (the
    interior less the knob size) shifted right by 16, which is negative when
    the container is narrower than its borders and the 1-pixel knob, and 0
    on an axis that does not move. The step of a page in HPotRes and VPotRes
    is 0 for body 0, body times 65536 divided by 65535 less the body below
    32768, and MAXPOT (65535) from 32768 on, also on the axis that does not
    move; clicking in the container left of the knob steps HorizPot down by
    HPotRes. Dragging the knob updates the pot on a moving axis from the
    knob's offset in the room, and on the axis that does not move sets the
    pot to MAXPOT when the knob fills the interior (an AUTOKNOB) and to 0
    when the knob is smaller than the interior (a smaller knob Image of its
    own), regardless of the initial pot or body on that axis or pointer
    motion along it. Pressing on the knob sets KNOBHIT (0x100) in PropInfo
    Flags and releasing leaves it set, also after a click on the knob
    without moving; NewModifyProp(), ModifyProp(), and clicking in the
    container outside the knob clear KNOBHIT. ActivateGadget() of a
    proportional gadget returns TRUE whether the gadget is disabled or
    enabled.
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void checkf(int ok, const char *format, ...)
{
    va_list args;

    checks++;
    if (!ok)
        failures++;
    printf("%s ", ok ? "ok  " : "FAIL");
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static int check(int ok, const char *what)
{
    checkf(ok, "%s", what);
    return ok;
}

static struct MsgPort *inport;
static struct IOStdReq *inreq;
static struct Window *win;

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
static void point(WORD x, WORD y, BOOL held)
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

static void drain(void)
{
    struct IntuiMessage *im;

    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)))
        ReplyMsg((struct Message *)im);
}

static struct PropInfo ph, pv, pn, pw;
static struct Image ih, iv, in, iw;
static struct Gadget gh, gv, gn, gw;

static UWORD knobdata[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static struct PropInfo pa, pb;
static struct Image ia, ib;
static struct Gadget ga, gb;

static void gadget(struct Gadget *g, WORD left, WORD top, WORD width, WORD height,
                   struct Image *im, struct PropInfo *pi, UWORD id, struct Gadget *next)
{
    memset(g, 0, sizeof(*g));
    g->NextGadget = next;
    g->LeftEdge = left;
    g->TopEdge = top;
    g->Width = width;
    g->Height = height;
    g->Flags = GFLG_GADGHCOMP;
    g->Activation = GACT_RELVERIFY | GACT_IMMEDIATE;
    g->GadgetType = GTYP_PROPGADGET;
    g->GadgetRender = im;
    g->SpecialInfo = pi;
    g->GadgetID = id;
}

static void set(struct Gadget *g, UWORD flags, UWORD hpot, UWORD vpot, UWORD hbody, UWORD vbody)
{
    NewModifyProp(g, win, NULL, flags, hpot, vpot, hbody, vbody, 1);
}

/* H: horizontal, old look, 160x12: HPotRes and knob dimensions for each body */
static void check_h(void)
{
    static const struct
    {
        UWORD body;
        UWORD hpotres;
        WORD width;
    } expected[] = {
        {     1,     1,   6 },
        {   100,   100,   6 },
        {   655,   661,   6 },
        {  1000,  1015,   6 },
        {  4096,  4369,  10 },
        {  6553,  7281,  16 },
        { 10000, 11800,  24 },
        { 13107, 16384,  31 },
        { 16383, 21844,  38 },
        { 16384, 21845,  39 },
        { 20000, 28784,  47 },
        { 21845, 32768,  51 },
        { 30000, 55327,  70 },
        { 32766, 65530,  76 },
        { 32767, 65534,  76 },
        { 32768, 65535,  77 },
        { 40000, 65535,  93 },
        { 49151, 65535, 114 },
        { 50000, 65535, 116 },
        { 60000, 65535, 140 },
        { 65534, 65535, 152 },
        { 65535, 65535, 152 }
    };
    ULONG i;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        set(&gh, AUTOKNOB | FREEHORIZ, 0, 0, expected[i].body, MAXBODY);
        checkf(ph.HPotRes == expected[i].hpotres && ih.Width == expected[i].width &&
               ih.LeftEdge == 0 && ih.TopEdge == 0 && ih.Height == 8,
               "H body %u: HPotRes %u, knob Width %d, LeftEdge 0, TopEdge 0, Height 8",
               (unsigned)expected[i].body, (unsigned)expected[i].hpotres, (int)expected[i].width);
    }
}

/* P: knob LeftEdge and Width for each pot, bodies 16383 and 1000 */
static void check_p(void)
{
    static const struct
    {
        UWORD body;
        UWORD pot;
        WORD left;
        WORD width;
    } expected[] = {
        { 16383,     1,   0, 38 },
        { 16383,   172,   0, 38 },
        { 16383,   173,   0, 38 },
        { 16383,  1000,   1, 38 },
        { 16383, 16383,  28, 38 },
        { 16383, 32767,  57, 38 },
        { 16383, 32768,  57, 38 },
        { 16383, 49151,  85, 38 },
        { 16383, 65534, 113, 38 },
        { 16383, 65535, 114, 38 },
        {  1000,     1,   0,  6 },
        {  1000,   172,   0,  6 },
        {  1000,   173,   0,  6 },
        {  1000,  1000,   2,  6 },
        {  1000, 16383,  36,  6 },
        {  1000, 32767,  73,  6 },
        {  1000, 32768,  73,  6 },
        {  1000, 49151, 109,  6 },
        {  1000, 65534, 145,  6 },
        {  1000, 65535, 146,  6 }
    };
    ULONG i;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        set(&gh, AUTOKNOB | FREEHORIZ, expected[i].pot, 0, expected[i].body, MAXBODY);
        checkf(ih.LeftEdge == expected[i].left && ih.Width == expected[i].width,
               "P body %u pot %u: knob LeftEdge %d, Width %d",
               (unsigned)expected[i].body, (unsigned)expected[i].pot,
               (int)expected[i].left, (int)expected[i].width);
    }
}

/* V: vertical, old look, 16x120: borders, container, VPotRes and knob dimensions */
static void check_v(void)
{
    static const struct
    {
        UWORD body;
        UWORD vpotres;
        WORD height;
    } expected[] = {
        {     1,     1,   4 },
        {   100,   100,   4 },
        {   655,   661,   4 },
        {  1000,  1015,   4 },
        {  4096,  4369,   8 },
        {  6553,  7281,  12 },
        { 10000, 11800,  18 },
        { 13107, 16384,  24 },
        { 16383, 21844,  29 },
        { 16384, 21845,  30 },
        { 20000, 28784,  36 },
        { 21845, 32768,  39 },
        { 30000, 55327,  54 },
        { 32766, 65530,  58 },
        { 32767, 65534,  58 },
        { 32768, 65535,  59 },
        { 40000, 65535,  71 },
        { 49151, 65535,  87 },
        { 50000, 65535,  89 },
        { 60000, 65535, 107 },
        { 65534, 65535, 116 },
        { 65535, 65535, 116 }
    };
    ULONG i;

    check(pv.LeftBorder == 4 && pv.TopBorder == 2 && pv.CWidth == 16 && pv.CHeight == 120,
          "V LeftBorder 4, TopBorder 2, CWidth 16, CHeight 120");
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        set(&gv, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, expected[i].body);
        checkf(pv.VPotRes == expected[i].vpotres && iv.Height == expected[i].height &&
               iv.TopEdge == 0 && iv.LeftEdge == 0 && iv.Width == 8,
               "V body %u: VPotRes %u, knob Height %d, TopEdge 0, LeftEdge 0, Width 8",
               (unsigned)expected[i].body, (unsigned)expected[i].vpotres, (int)expected[i].height);
    }
}

/* N: PROPNEWLOOK, horizontal, 160x12 */
static void check_n(void)
{
    check(pn.LeftBorder == 1 && pn.TopBorder == 1 && pn.CWidth == 160 && pn.CHeight == 12 &&
          pn.HPotRes == 21844 && pn.VPotRes == 65535 && in.Width == 40 && in.Height == 10,
          "N LeftBorder 1, TopBorder 1, CWidth 160, CHeight 12, PotRes 21844, 65535, knob Width 40, Height 10");
    set(&gn, AUTOKNOB | FREEHORIZ | PROPNEWLOOK, MAXPOT / 2, 0, 16383, MAXBODY);
    check(in.LeftEdge == 59 && in.Width == 40,
          "N pot MAXPOT / 2: knob LeftEdge 59, Width 40");
}

/* W: widths of the container from 5 to 300 pixels, body 16383, pot MAXPOT */
static void check_w(void)
{
    static const struct
    {
        WORD width;
        WORD kwidth;
        WORD kleft;
    } expected[] = {
        {   5,  1,  -4 },
        {   7,  1,  -2 },
        {  10,  2,   0 },
        {  20,  6,   6 },
        { 100, 23,  69 },
        { 161, 39, 114 },
        { 300, 73, 219 }
    };
    ULONG i;
    UWORD pos, add_pos;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        pos = RemoveGList(win, &gw, 1);
        gw.Width = expected[i].width;
        add_pos = AddGList(win, &gw, ~0, 1, NULL);
        set(&gw, AUTOKNOB | FREEHORIZ, MAXPOT, 0, 16383, MAXBODY);
        checkf(pos != (UWORD)~0 && add_pos == pos &&
               pw.CWidth == expected[i].width && pw.HPotRes == 21844 &&
               iw.Width == expected[i].kwidth && iw.Height == 8 &&
               iw.LeftEdge == expected[i].kleft && iw.TopEdge == 0 &&
               pw.LeftBorder == 4 && pw.TopBorder == 2,
               "W width %d: CWidth %d, HPotRes 21844, knob Width %d, Height 8, LeftEdge %d, TopEdge 0, Borders 4, 2",
               (int)expected[i].width, (int)expected[i].width,
               (int)expected[i].kwidth, (int)expected[i].kleft);
    }
}

/* D: three drags of the horizontal knob, with a vertical body and pot of their own */
static void check_d(void)
{
    static const UWORD vbody[] = { MAXBODY, MAXBODY / 2, MAXBODY / 2 };
    static const UWORD vpot[] = { 1000, 1000, 40000 };
    ULONG j;
    WORD x, y;

    for (j = 0; j < 3; j++)
    {
        set(&gh, AUTOKNOB | FREEHORIZ, 0, vpot[j], 16383, vbody[j]);
        drain();
        x = gh.LeftEdge + ph.LeftBorder + ih.LeftEdge + ih.Width / 2;
        y = gh.TopEdge + ph.TopBorder + ih.TopEdge + ih.Height / 2;
        point(x, y, FALSE);
        button(TRUE);
        point(x + 1, y, TRUE);
        checkf(ph.HorizPot == 574 && ph.VertPot == 65535 && ph.Flags == 0x103,
               "D %u (VertBody %u, VertPot %u) held +1: HorizPot 574, VertPot 65535, Flags 0x103",
               (unsigned)j, (unsigned)vbody[j], (unsigned)vpot[j]);
        point(x + 20, y, TRUE);
        checkf(ph.HorizPot == 11497 && ph.VertPot == 65535,
               "D %u (VertBody %u, VertPot %u) held +20: HorizPot 11497, VertPot 65535",
               (unsigned)j, (unsigned)vbody[j], (unsigned)vpot[j]);
        point(x + 20, y + 3, TRUE);
        checkf(ph.HorizPot == 11497 && ph.VertPot == 65535,
               "D %u (VertBody %u, VertPot %u) held +20 +3: HorizPot 11497, VertPot 65535",
               (unsigned)j, (unsigned)vbody[j], (unsigned)vpot[j]);
        button(FALSE);
        Delay(3);
        drain();
        checkf(ph.HorizPot == 11497 && ph.VertPot == 65535 && ph.Flags == 0x103 &&
               ih.LeftEdge == 20 && ih.TopEdge == 0,
               "D %u (VertBody %u, VertPot %u) after release: HorizPot 11497, VertPot 65535, Flags 0x103, knob LeftEdge 20, TopEdge 0",
               (unsigned)j, (unsigned)vbody[j], (unsigned)vpot[j]);
    }
}

/* K: KNOBHIT before and after NewModifyProp(), after a click on the knob, and after a click left of the knob */
static void check_k(void)
{
    WORD x, y;

    check(ph.Flags == 0x103,
          "K Flags before NewModifyProp() is 0x103 (KNOBHIT set)");
    set(&gh, AUTOKNOB | FREEHORIZ, 1000, 0, 16383, MAXBODY);
    check(ph.Flags == 0x3,
          "K Flags after NewModifyProp() is 0x3 (KNOBHIT cleared)");

    x = gh.LeftEdge + ph.LeftBorder + ih.LeftEdge + ih.Width / 2;
    y = gh.TopEdge + ph.TopBorder + ih.TopEdge + ih.Height / 2;
    point(x, y, FALSE);
    button(TRUE);
    button(FALSE);
    Delay(3);
    drain();
    check(ph.Flags == 0x103 && ph.HorizPot == 1000,
          "K after a click on the knob without a move: Flags 0x103, HorizPot 1000");

    /* KNOBHIT set first, so that the click shows that it clears it */
    set(&gh, AUTOKNOB | FREEHORIZ | KNOBHIT, 40000, 0, 16383, MAXBODY);
    x = gh.LeftEdge + ph.LeftBorder + 2;
    point(x, y, FALSE);
    button(TRUE);
    button(FALSE);
    Delay(3);
    drain();
    check(ph.HorizPot == 18156 && ph.Flags == 0x3,
          "K click left of the knob from HorizPot 40000: HorizPot 18156, Flags 0x3");
}

/* R: resolutions and knob dimensions of body 0 and 16691 */
static void check_r(void)
{
    NewModifyProp(&ga, win, NULL, AUTOKNOB | FREEHORIZ, 0, 0, 0, MAXBODY, 1);
    check(pa.HPotRes == 0 && ia.Width == 6,
          "R HorizBody 0: HPotRes 0, knob Width 6");
    NewModifyProp(&ga, win, NULL, AUTOKNOB | FREEHORIZ, 0, 0, 16691, MAXBODY, 1);
    check(pa.HPotRes == 22394,
          "R HorizBody 16691: HPotRes 22394");
    NewModifyProp(&ga, win, NULL, AUTOKNOB | FREEHORIZ, 0, 0, 16383, 0, 1);
    check(pa.VPotRes == 0 && ia.Height == 8,
          "R VertBody 0: VPotRes 0, knob Height 8");
    NewModifyProp(&ga, win, NULL, AUTOKNOB | FREEHORIZ, 0, 0, 16383, MAXBODY, 1);
}

/* A: ActivateGadget() of a disabled and of an enabled prop gadget */
static void check_a(void)
{
    OffGadget(&ga, win, NULL);
    check(ActivateGadget(&ga, win, NULL) == TRUE,
          "A ActivateGadget() of a disabled prop gadget returns TRUE (1)");
    OnGadget(&ga, win, NULL);
    check(ActivateGadget(&ga, win, NULL) == TRUE,
          "A ActivateGadget() of an enabled prop gadget returns TRUE (1)");
}

/* B: a knob Image of its own, 16x4 in a 160x16 gadget: opening state, drag, and ModifyProp() */
static void check_b(void)
{
    WORD x, y;

    check(ib.LeftEdge == 0 && ib.TopEdge == 0 && pb.LeftBorder == 4 && pb.TopBorder == 2 &&
          pb.CWidth == 160 && pb.CHeight == 16 && pb.VertPot == 1000 && pb.VPotRes == 65534,
          "B after open: knob LeftEdge 0, TopEdge 0, Borders 4, 2, CWidth 160, CHeight 16, VertPot 1000, VPotRes 65534");
    drain();
    x = gb.LeftEdge + pb.LeftBorder + ib.LeftEdge + ib.Width / 2;
    y = gb.TopEdge + pb.TopBorder + ib.TopEdge + ib.Height / 2;
    point(x, y, FALSE);
    button(TRUE);
    check(pb.Flags == 0x102,
          "B while held: Flags 0x102 (KNOBHIT set)");
    point(x + 10, y, TRUE);
    check(pb.HorizPot == 4818 && pb.VertPot == 0 && ib.LeftEdge == 10 && ib.TopEdge == 0,
          "B held +10: HorizPot 4818, VertPot 0, knob LeftEdge 10, TopEdge 0");
    point(x + 10, y + 4, TRUE);
    check(pb.HorizPot == 4818 && pb.VertPot == 0 && ib.LeftEdge == 10 && ib.TopEdge == 0,
          "B held +10 +4: HorizPot 4818, VertPot 0, knob LeftEdge 10, TopEdge 0");
    button(FALSE);
    Delay(3);
    drain();
    check(pb.HorizPot == 4818 && pb.VertPot == 0 && pb.Flags == 0x102 &&
          ib.LeftEdge == 10 && ib.TopEdge == 0,
          "B after release: HorizPot 4818, VertPot 0, Flags 0x102, knob LeftEdge 10, TopEdge 0");
    ModifyProp(&gb, win, NULL, FREEHORIZ, 1000, 1000, MAXBODY / 8, MAXBODY / 2);
    check(pb.Flags == 0x2 && ib.LeftEdge == 2 && ib.TopEdge == 0 && pb.VertPot == 1000,
          "B after ModifyProp(): Flags 0x2 (KNOBHIT cleared), knob LeftEdge 2, TopEdge 0, VertPot 1000");
}

int main(void)
{
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
        printf("propcalc: %d checks, %d failed\n", checks, failures);
        return 20;
    }

    memset(&ph, 0, sizeof(ph));
    ph.Flags = AUTOKNOB | FREEHORIZ;
    ph.HorizBody = MAXBODY / 4;
    ph.VertBody = MAXBODY;
    memset(&pv, 0, sizeof(pv));
    pv.Flags = AUTOKNOB | FREEVERT;
    pv.HorizBody = MAXBODY;
    pv.VertBody = MAXBODY / 4;
    memset(&pn, 0, sizeof(pn));
    pn.Flags = AUTOKNOB | FREEHORIZ | PROPNEWLOOK;
    pn.HorizBody = MAXBODY / 4;
    pn.VertBody = MAXBODY;
    memset(&pw, 0, sizeof(pw));
    pw.Flags = AUTOKNOB | FREEHORIZ;
    pw.HorizBody = MAXBODY / 4;
    pw.VertBody = MAXBODY;
    memset(&ih, 0, sizeof(ih));
    memset(&iv, 0, sizeof(iv));
    memset(&in, 0, sizeof(in));
    memset(&iw, 0, sizeof(iw));
    gadget(&gw, 20, 120, 100, 12, &iw, &pw, 4, NULL);
    gadget(&gn, 20, 100, 160, 12, &in, &pn, 3, &gw);
    gadget(&gv, 260, 20, 16, 120, &iv, &pv, 2, &gn);
    gadget(&gh, 20, 30, 160, 12, &ih, &ph, 1, &gv);

    win = OpenWindowTags(NULL,
                         WA_Left, 10,
                         WA_Top, 15,
                         WA_Width, 320,
                         WA_Height, 170,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_ACTIVATE,
                         WA_IDCMP, IDCMP_GADGETDOWN | IDCMP_GADGETUP,
                         WA_Gadgets, (IPTR)&gh,
                         WA_Title, (IPTR)"propcalc",
                         TAG_DONE);
    if (check(win != NULL, "first window with the four proportional gadgets opened"))
    {
        Delay(25);
        check_h();
        check_p();
        check_v();
        check_n();
        check_w();
        check_d();
        check_k();
        Delay(25);
        CloseWindow(win);
    }

    /* a: AUTOKNOB, horizontal, 160x12 */
    memset(&pa, 0, sizeof(pa));
    pa.Flags = AUTOKNOB | FREEHORIZ;
    pa.HorizBody = MAXBODY / 4;
    pa.VertBody = MAXBODY;
    memset(&ia, 0, sizeof(ia));
    memset(&ga, 0, sizeof(ga));
    ga.LeftEdge = 20;
    ga.TopEdge = 30;
    ga.Width = 160;
    ga.Height = 12;
    ga.Flags = GFLG_GADGHCOMP;
    ga.Activation = GACT_RELVERIFY | GACT_IMMEDIATE;
    ga.GadgetType = GTYP_PROPGADGET;
    ga.GadgetRender = &ia;
    ga.SpecialInfo = &pa;
    /* b: a knob of its own, 16x4, horizontal, in a 160x16 gadget, VertPot 1000 */
    memset(&pb, 0, sizeof(pb));
    pb.Flags = FREEHORIZ;
    pb.VertPot = 1000;
    pb.HorizBody = MAXBODY / 8;
    pb.VertBody = MAXBODY / 2;
    memset(&ib, 0, sizeof(ib));
    ib.Width = 16;
    ib.Height = 4;
    ib.Depth = 1;
    ib.ImageData = knobdata;
    ib.PlanePick = 1;
    memset(&gb, 0, sizeof(gb));
    gb.LeftEdge = 20;
    gb.TopEdge = 60;
    gb.Width = 160;
    gb.Height = 16;
    gb.Flags = GFLG_GADGHNONE | GFLG_GADGIMAGE;
    gb.Activation = GACT_RELVERIFY | GACT_IMMEDIATE;
    gb.GadgetType = GTYP_PROPGADGET;
    gb.GadgetRender = &ib;
    gb.SpecialInfo = &pb;
    ga.NextGadget = &gb;

    win = OpenWindowTags(NULL,
                         WA_Left, 10,
                         WA_Top, 15,
                         WA_Width, 320,
                         WA_Height, 120,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_ACTIVATE,
                         WA_IDCMP, IDCMP_GADGETDOWN | IDCMP_GADGETUP,
                         WA_Gadgets, (IPTR)&ga,
                         WA_Title, (IPTR)"propcalc2",
                         TAG_DONE);
    if (check(win != NULL, "second window with the two proportional gadgets opened"))
    {
        Delay(25);
        check_r();
        check_a();
        check_b();
        Delay(25);
        CloseWindow(win);
    }

    CloseDevice((struct IORequest *)inreq);
    DeleteIORequest((struct IORequest *)inreq);
    DeleteMsgPort(inport);

    printf("propcalc: %d checks, %d failed\n", checks, failures);
    return failures ? 20 : 0;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    States and redrawing of classic Intuition gadgets, as on AmigaOS 3.2.3.
    A Boolean gadget draws a traditional Image as it is in every state and
    passes its state to image objects only, where AROS passed it to the
    whole chain: DrawImageState() of AROS draws a traditional Image of
    IDS_SELECTED inverted, which AmigaOS 3.2.3 draws as of IDS_NORMAL, and
    which AROS's icon library relies on, so that stays. A Boolean gadget
    with GFLG_GADGIMAGE | GFLG_GADGHIMAGE draws its SelectRender (pen 0) as
    it is when GFLG_SELECTED is set and RefreshGList() is called, and its
    GadgetRender (pen 1) when GFLG_SELECTED is cleared and refreshed; with
    a chain of a traditional Image and a frame object, in either order, it
    draws the Image as it is and the frame recessed when selected. A
    Boolean gadget with GFLG_GADGIMAGE | GFLG_GADGHCOMP complements its
    image of pen 0 when refreshed with GFLG_SELECTED set, and restores pen
    0 when GFLG_SELECTED is cleared and refreshed. OffGadget() and
    OnGadget() on a gadget in a window redraw that gadget and every gadget
    after it on the window's list. ActivateGadget() of a Boolean gadget on
    the active window gives TRUE while no gadget is in use, disabled or
    not, and leaves its Flags and Activation unchanged (without
    GACT_ACTIVEGADGET), so that ActivateGadget() of a string gadget right
    after it succeeds and sets GACT_ACTIVEGADGET; with that string gadget
    in use, or another window active, it gives FALSE. First, the gadget
    help of AROS, which sends IDCMP_GADGETHELP a second after the pointer
    came onto a gadget: a gadget that RemoveGList() takes off meanwhile,
    which the program may free next, gets no message. Output in the form
    of the kaffeine target tests: a line per check and a summary.
*/

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <exec/memory.h>
#include <graphics/rastport.h>
#include <intuition/classusr.h>
#include <intuition/imageclass.h>
#include <intuition/intuition.h>
#include <stdio.h>
#include <string.h>

#define WIN_WIDTH  320
#define WIN_HEIGHT 160

#define IMG_WIDTH  16
#define IMG_HEIGHT 8
#define IMG_WORDS  8
#define IMG_BYTES  (IMG_WORDS * sizeof(UWORD))

/* the second image of a mixed chain lies this far right of the first */
#define MIX_OFFSET 20

#define STR_BYTES  32

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

/* What ReadPixel() gives for a pen in this window: on a screen of more
   than 256 colours ReadPixel() gives the pen found for a colour, which
   need not be the pen that drew it, so every pixel check compares with a
   pixel that the program draws in that pen in the same window, low at the
   left where no gadget lies. */
static LONG pen_value(struct Window *win, UBYTE pen)
{
    struct RastPort *rp = win->RPort;
    LONG x = win->BorderLeft + 2, y = win->Height - win->BorderBottom - 3;

    SetDrMd(rp, JAM1);
    SetAPen(rp, pen);
    WritePixel(rp, x, y);
    return ReadPixel(rp, x, y);
}

/* What ReadPixel() gives for the complement of a pen in this window: a
   pixel drawn in that pen, complemented with WritePixel() in COMPLEMENT
   mode, and read back with JAM1 restored after. */
static LONG comp_value(struct Window *win, UBYTE pen)
{
    struct RastPort *rp = win->RPort;
    LONG x = win->BorderLeft + 2, y = win->Height - win->BorderBottom - 3;
    LONG val;

    SetDrMd(rp, JAM1);
    SetAPen(rp, pen);
    WritePixel(rp, x, y);
    /* complemented as Intuition complements a gadget */
    SetDrMd(rp, COMPLEMENT);
    RectFill(rp, x, y, x, y);
    val = ReadPixel(rp, x, y);
    SetDrMd(rp, JAM1);
    return val;
}

/* A message of the class within at most 10 seconds, the others replied */
static int await(struct Window *win, ULONG class)
{
    int i;

    for (i = 0; i < 500; i++)
    {
        struct IntuiMessage *msg;
        int found = 0;

        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (msg->Class == class)
                found = 1;
            ReplyMsg(&msg->ExecMessage);
        }
        if (found)
            return 1;
        Delay(1);
    }
    return 0;
}

/* The pointer moved to a point of a window through input.device, which
   gives the event its time, whose handlers have taken it on return */
static void point(struct IOStdReq *inputreq, struct Window *win, WORD x, WORD y)
{
    struct IEPointerPixel pp;
    struct InputEvent ie;

    pp.iepp_Screen = win->WScreen;
    pp.iepp_Position.X = win->LeftEdge + x;
    pp.iepp_Position.Y = win->TopEdge + y;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_NEWPOINTERPOS;
    ie.ie_SubClass = IESUBCLASS_PIXEL;
    ie.ie_Code = IECODE_NOBUTTON;
    ie.ie_EventAddress = &pp;
    inputreq->io_Command = IND_WRITEEVENT;
    inputreq->io_Data = &ie;
    inputreq->io_Length = sizeof(ie);
    DoIO((struct IORequest *)inputreq);
}

/* The gadget that an IDCMP_GADGETHELP names within that many ticks, or
   NULL, the others replied: one that names the window or nothing is the
   help of no gadget */
static struct Gadget *help_for(struct Window *win, int ticks)
{
    int i;

    for (i = 0; i < ticks; i++)
    {
        struct IntuiMessage *msg;
        struct Gadget *found = NULL;

        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (msg->Class == IDCMP_GADGETHELP && msg->IAddress &&
                msg->IAddress != (APTR)win)
                found = (struct Gadget *)msg->IAddress;
            ReplyMsg(&msg->ExecMessage);
        }
        if (found)
            return found;
        Delay(1);
    }
    return NULL;
}

/* 1. On a window with HelpControl(), the pointer on a gadget with
   GMORE_GADGETHELP brings IDCMP_GADGETHELP for it a second later; when
   RemoveGList() takes the gadget off within that second, no message names
   it, where AROS kept it for its help past RemoveGList() and read it
   after. In a window of its own, before the others open. */
static void check_help(void)
{
    struct ExtGadget help1, help2;
    struct MsgPort *inputport = CreateMsgPort();
    struct IOStdReq *inputreq = NULL;
    struct Window *win;

    if (inputport)
        inputreq = (struct IOStdReq *)CreateIORequest(inputport, sizeof(struct IOStdReq));
    if (!check(inputreq && OpenDevice("input.device", 0, (struct IORequest *)inputreq, 0) == 0,
               "input.device opened"))
    {
        if (inputreq)
            DeleteIORequest((struct IORequest *)inputreq);
        if (inputport)
            DeleteMsgPort(inputport);
        return;
    }

    memset(&help1, 0, sizeof(help1));
    help1.NextGadget = &help2;
    help1.LeftEdge = 20;
    help1.TopEdge = 30;
    help1.Width = 40;
    help1.Height = 12;
    help1.Flags = GFLG_EXTENDED | GFLG_GADGHNONE;
    help1.GadgetType = GTYP_BOOLGADGET;
    help1.GadgetID = 1;
    help1.MoreFlags = GMORE_GADGETHELP;
    help2 = help1;
    help2.NextGadget = NULL;
    help2.LeftEdge = 80;
    help2.GadgetID = 2;

    win = OpenWindowTags(NULL,
                         WA_Left, 40,
                         WA_Top, 40,
                         WA_Width, 200,
                         WA_Height, 70,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETHELP,
                         WA_Activate, TRUE,
                         WA_Gadgets, (IPTR)&help1,
                         WA_Title, (IPTR)"gadstates help",
                         TAG_DONE);
    if (check(win != NULL, "help window opened"))
    {
        WORD x2 = help2.LeftEdge + 4, y2 = help2.TopEdge + 4;
        int tries, done = 0;

        if (check(await(win, IDCMP_ACTIVEWINDOW), "help window active"))
        {
            HelpControl(win, HC_GADGETHELP);
            point(inputreq, win, help1.LeftEdge + 4, help1.TopEdge + 4);
            check(help_for(win, 150) == (struct Gadget *)&help1,
                  "the pointer on a help gadget brings IDCMP_GADGETHELP for it");

            /* off the gadgets, then onto the second one, which goes within
               half a second, well before its help is due: a try where that
               took longer, or where the pointer moved meanwhile, proves
               nothing and is made again */
            for (tries = 0; tries < 3 && !done; tries++)
            {
                ULONG s1, m1, s2, m2;
                struct Gadget *found;

                point(inputreq, win, 150, 55);
                help_for(win, 10);
                CurrentTime(&s1, &m1);
                point(inputreq, win, x2, y2);
                RemoveGList(win, (struct Gadget *)&help2, 1);
                CurrentTime(&s2, &m2);
                found = help_for(win, 150);
                if ((s2 - s1) * 1000000 + m2 - m1 < 500000 &&
                    win->MouseX == x2 && win->MouseY == y2)
                {
                    check(found == NULL,
                          "no IDCMP_GADGETHELP names a help gadget that RemoveGList() took off");
                    done = 1;
                }
                else
                    AddGList(win, (struct Gadget *)&help2, ~0, 1, NULL);
            }
            if (!done)
                check(0, "the pointer onto a help gadget and its removal within half a second");
        }
        CloseWindow(win);
    }

    CloseDevice((struct IORequest *)inputreq);
    DeleteIORequest((struct IORequest *)inputreq);
    DeleteMsgPort(inputport);
}

/* 2. A Boolean gadget with GFLG_GADGIMAGE | GFLG_GADGHIMAGE draws its
   SelectRender (pen 0) when refreshed with GFLG_SELECTED, and its
   GadgetRender (pen 1) when GFLG_SELECTED is cleared and refreshed. */
static void check_gadghimage(struct Window *win, struct Gadget *gad)
{
    struct RastPort *rp = win->RPort;
    WORD px = gad->LeftEdge + 4, py = gad->TopEdge + 4;

    gad->Flags |= GFLG_SELECTED;
    RefreshGList(gad, win, NULL, 1);
    check(ReadPixel(rp, px, py) == pen_value(win, 0),
          "GADGHIMAGE gadget with GFLG_SELECTED and RefreshGList() draws pen 0");

    gad->Flags &= ~GFLG_SELECTED;
    RefreshGList(gad, win, NULL, 1);
    check(ReadPixel(rp, px, py) == pen_value(win, 1),
          "GADGHIMAGE gadget after clearing GFLG_SELECTED and RefreshGList() draws pen 1");
}

/* 3. A Boolean gadget with GFLG_GADGIMAGE | GFLG_GADGHCOMP and an image of
   pen 0 complements it when refreshed with GFLG_SELECTED, and restores pen
   0 when GFLG_SELECTED is cleared and refreshed. Pen 0, as on the deep
   screens of AROS the complement of pen 1 can have the colour of pen 1,
   which ReadPixel() cannot tell apart. */
static void check_gadghcomp(struct Window *win, struct Gadget *gad)
{
    struct RastPort *rp = win->RPort;
    WORD px = gad->LeftEdge + 4, py = gad->TopEdge + 4;

    check(comp_value(win, 0) != pen_value(win, 0),
          "the complement of pen 0 reads otherwise than pen 0");
    gad->Flags |= GFLG_SELECTED;
    RefreshGList(gad, win, NULL, 1);
    check(ReadPixel(rp, px, py) == comp_value(win, 0),
          "GADGHCOMP gadget with GFLG_SELECTED and RefreshGList() draws complement of pen 0");

    gad->Flags &= ~GFLG_SELECTED;
    RefreshGList(gad, win, NULL, 1);
    check(ReadPixel(rp, px, py) == pen_value(win, 0),
          "GADGHCOMP gadget after clearing GFLG_SELECTED and RefreshGList() draws pen 0");
}

/* 4. Two GFLG_GADGIMAGE | GFLG_GADGHIMAGE gadgets, one after the other on
   the list, with a chain of a traditional Image of pen 0 and a frame
   object, the Image first in one and the frame first in the other, both
   renders the same chain: selected, each draws the Image as it is and the
   frame recessed, its top edge in the shadow pen; cleared, the frames
   draw their top edges in the shine pen again. The Images are drawn over
   pen 1, so that an Image drawn as it is reads as pen 0, an inverted one
   as the complement of pen 0, which 3 found to read otherwise, and one
   not drawn as pen 1. */
static void check_mixed_chains(struct Window *win, struct Gadget *image_first,
                               struct Gadget *frame_first, struct DrawInfo *dri)
{
    struct RastPort *rp = win->RPort;
    WORD ix = image_first->LeftEdge, iy = image_first->TopEdge;
    WORD fx = frame_first->LeftEdge, fy = frame_first->TopEdge;
    LONG shine = pen_value(win, dri->dri_Pens[SHINEPEN]);
    LONG shadow = pen_value(win, dri->dri_Pens[SHADOWPEN]);

    check(shine != shadow, "the shine and shadow pens read apart");

    SetDrMd(rp, JAM1);
    SetAPen(rp, 1);
    RectFill(rp, ix, iy, ix + IMG_WIDTH - 1, iy + IMG_HEIGHT - 1);
    RectFill(rp, fx + MIX_OFFSET, fy, fx + MIX_OFFSET + IMG_WIDTH - 1, fy + IMG_HEIGHT - 1);
    image_first->Flags |= GFLG_SELECTED;
    frame_first->Flags |= GFLG_SELECTED;
    RefreshGList(image_first, win, NULL, 2);
    check(ReadPixel(rp, ix + 4, iy + 4) == pen_value(win, 0),
          "selected chain with the Image first draws the Image as it is, pen 0");
    check(ReadPixel(rp, ix + MIX_OFFSET + IMG_WIDTH / 2, iy) == shadow,
          "selected chain with the Image first draws the frame after it recessed");
    check(ReadPixel(rp, fx + IMG_WIDTH / 2, fy) == shadow,
          "selected chain with the frame first draws the frame recessed");
    check(ReadPixel(rp, fx + MIX_OFFSET + 4, fy + 4) == pen_value(win, 0),
          "selected chain with the frame first draws the Image after it as it is, pen 0");

    image_first->Flags &= ~GFLG_SELECTED;
    frame_first->Flags &= ~GFLG_SELECTED;
    RefreshGList(image_first, win, NULL, 2);
    check(ReadPixel(rp, ix + MIX_OFFSET + IMG_WIDTH / 2, iy) == shine &&
          ReadPixel(rp, fx + IMG_WIDTH / 2, fy) == shine,
          "cleared chains draw both frames with their top edges in the shine pen");
}

/* 5. OffGadget() and OnGadget() on gadget A redraw gadget A and every
   gadget after it on the window's list, including the GADGHIMAGE gadget. */
static void check_off_on_refresh(struct Window *win, struct Gadget *gad_a,
                                 struct Gadget *himage_gad)
{
    struct RastPort *rp = win->RPort;
    WORD px = himage_gad->LeftEdge + 4, py = himage_gad->TopEdge + 4;

    himage_gad->Flags |= GFLG_SELECTED;
    OffGadget(gad_a, win, NULL);
    check(ReadPixel(rp, px, py) == pen_value(win, 0),
          "OffGadget(A) redraws following GADGHIMAGE gadget with GFLG_SELECTED as pen 0");

    himage_gad->Flags &= ~GFLG_SELECTED;
    OnGadget(gad_a, win, NULL);
    check(ReadPixel(rp, px, py) == pen_value(win, 1),
          "OnGadget(A) redraws following GADGHIMAGE gadget with GFLG_SELECTED cleared as pen 1");
}

/* 6. ActivateGadget() of a Boolean gadget gives TRUE on the active window
   while no gadget is in use, disabled or not, and activates nothing, as
   AmigaOS 3.2.3 does; with a string gadget in use, or another window
   active, it gives FALSE. */
static void check_activate(struct Window *win, struct Gadget *bool_gad,
                           struct Gadget *str_gad)
{
    struct Window *win2;
    UWORD flags_before, act_before;
    BOOL ok;

    check(await(win, IDCMP_ACTIVEWINDOW),
          "IDCMP_ACTIVEWINDOW received on main window");

    flags_before = bool_gad->Flags;
    act_before = bool_gad->Activation;
    ok = ActivateGadget(bool_gad, win, NULL);
    check(ok && bool_gad->Flags == flags_before &&
          bool_gad->Activation == act_before &&
          !(bool_gad->Activation & GACT_ACTIVEGADGET),
          "ActivateGadget() of enabled Boolean gadget gives TRUE and leaves Flags and Activation unchanged");

    OffGadget(bool_gad, win, NULL);
    ok = ActivateGadget(bool_gad, win, NULL);
    OnGadget(bool_gad, win, NULL);
    check(ok, "ActivateGadget() of disabled Boolean gadget gives TRUE too");

    /* before any gadget is in use: AROS keeps a window inactive that
       opens while a gadget of another one is active */
    win2 = OpenWindowTags(NULL,
                          WA_Left, 200,
                          WA_Top, 40,
                          WA_Width, 120,
                          WA_Height, 60,
                          WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR,
                          WA_IDCMP, IDCMP_ACTIVEWINDOW,
                          WA_Activate, TRUE,
                          WA_Title, (IPTR)"gadstates 2",
                          TAG_DONE);
    check(win2 != NULL && await(win2, IDCMP_ACTIVEWINDOW),
          "second window opened and active");
    if (win2)
    {
        check(!ActivateGadget(bool_gad, win, NULL),
              "ActivateGadget() of Boolean gadget while another window is active gives FALSE");
        CloseWindow(win2);
    }
    ActivateWindow(win);
    check(await(win, IDCMP_ACTIVEWINDOW), "the first window is active again");

    ok = ActivateGadget(str_gad, win, NULL);
    check(ok && (str_gad->Activation & GACT_ACTIVEGADGET) != 0,
          "ActivateGadget() of string gadget gives TRUE and sets GACT_ACTIVEGADGET");

    ok = ActivateGadget(bool_gad, win, NULL);
    check(!ok && (str_gad->Activation & GACT_ACTIVEGADGET) != 0,
          "ActivateGadget() of Boolean gadget while the string gadget is in use gives FALSE");
}

int main(void)
{
    static WORD border_xy[10] = { 0, 0, 59, 0, 59, 11, 0, 11, 0, 0 };
    static UBYTE str_buf[STR_BYTES], str_undo[STR_BYTES];
    struct Border border_a;
    struct Image img_set, img_clear, img_mix1, img_mix2;
    struct StringInfo str_info;
    struct Gadget gad_a, himage_gad, hcomp_gad, mix_image, mix_frame, str_gad;
    struct Window *win = NULL;
    struct DrawInfo *dri;
    Object *frame1, *frame2;
    UWORD *data_set, *data_clear;
    int i;

    check_help();

    data_set = (UWORD *)AllocMem(IMG_BYTES, MEMF_CHIP | MEMF_CLEAR);
    data_clear = (UWORD *)AllocMem(IMG_BYTES, MEMF_CHIP | MEMF_CLEAR);
    /* frames of edges only, the first one right of the Image before it */
    frame1 = NewObject(NULL, FRAMEICLASS,
                       IA_Left, MIX_OFFSET,
                       IA_Width, IMG_WIDTH,
                       IA_Height, IMG_HEIGHT,
                       IA_EdgesOnly, TRUE,
                       TAG_DONE);
    frame2 = NewObject(NULL, FRAMEICLASS,
                       IA_Width, IMG_WIDTH,
                       IA_Height, IMG_HEIGHT,
                       IA_EdgesOnly, TRUE,
                       TAG_DONE);
    if (check(data_set != NULL && data_clear != NULL, "chip memory for images") &&
        check(frame1 != NULL && frame2 != NULL, "two frame objects"))
    {
        for (i = 0; i < IMG_WORDS; i++)
            data_set[i] = 0xFFFF;

        memset(&img_set, 0, sizeof(img_set));
        img_set.Width = IMG_WIDTH;
        img_set.Height = IMG_HEIGHT;
        img_set.Depth = 1;
        img_set.ImageData = data_set;
        img_set.PlanePick = 1;
        img_set.PlaneOnOff = 0;

        memset(&img_clear, 0, sizeof(img_clear));
        img_clear.Width = IMG_WIDTH;
        img_clear.Height = IMG_HEIGHT;
        img_clear.Depth = 1;
        img_clear.ImageData = data_clear;
        img_clear.PlanePick = 1;
        img_clear.PlaneOnOff = 0;

        /* the mixed chains: the Image first and the first frame after it,
           the second frame first and the Image right of it after it */
        img_mix1 = img_clear;
        img_mix1.NextImage = (struct Image *)frame1;
        img_mix2 = img_clear;
        img_mix2.LeftEdge = MIX_OFFSET;
        ((struct Image *)frame2)->NextImage = &img_mix2;

        /* Plain Boolean gadget A with GFLG_GADGHCOMP and a Border */
        memset(&border_a, 0, sizeof(border_a));
        border_a.FrontPen = 1;
        border_a.DrawMode = JAM1;
        border_a.Count = 5;
        border_a.XY = border_xy;

        memset(&gad_a, 0, sizeof(gad_a));
        gad_a.NextGadget = &himage_gad;
        gad_a.LeftEdge = 20;
        gad_a.TopEdge = 30;
        gad_a.Width = 60;
        gad_a.Height = 12;
        gad_a.Flags = GFLG_GADGHCOMP;
        gad_a.Activation = GACT_RELVERIFY;
        gad_a.GadgetType = GTYP_BOOLGADGET;
        gad_a.GadgetRender = &border_a;

        /* Boolean gadget with GFLG_GADGIMAGE | GFLG_GADGHIMAGE */
        memset(&himage_gad, 0, sizeof(himage_gad));
        himage_gad.NextGadget = &hcomp_gad;
        himage_gad.LeftEdge = 20;
        himage_gad.TopEdge = 50;
        himage_gad.Width = IMG_WIDTH;
        himage_gad.Height = IMG_HEIGHT;
        himage_gad.Flags = GFLG_GADGIMAGE | GFLG_GADGHIMAGE;
        himage_gad.Activation = GACT_RELVERIFY;
        himage_gad.GadgetType = GTYP_BOOLGADGET;
        himage_gad.GadgetRender = &img_set;
        himage_gad.SelectRender = &img_clear;

        /* Boolean gadget with GFLG_GADGIMAGE | GFLG_GADGHCOMP */
        memset(&hcomp_gad, 0, sizeof(hcomp_gad));
        hcomp_gad.NextGadget = &mix_image;
        hcomp_gad.LeftEdge = 20;
        hcomp_gad.TopEdge = 70;
        hcomp_gad.Width = IMG_WIDTH;
        hcomp_gad.Height = IMG_HEIGHT;
        hcomp_gad.Flags = GFLG_GADGIMAGE | GFLG_GADGHCOMP;
        hcomp_gad.Activation = GACT_RELVERIFY;
        hcomp_gad.GadgetType = GTYP_BOOLGADGET;
        hcomp_gad.GadgetRender = &img_clear;

        /* GFLG_GADGIMAGE | GFLG_GADGHIMAGE gadgets with a mixed chain in
           both renders */
        memset(&mix_image, 0, sizeof(mix_image));
        mix_image.NextGadget = &mix_frame;
        mix_image.LeftEdge = 160;
        mix_image.TopEdge = 30;
        mix_image.Width = MIX_OFFSET + IMG_WIDTH;
        mix_image.Height = IMG_HEIGHT;
        mix_image.Flags = GFLG_GADGIMAGE | GFLG_GADGHIMAGE;
        mix_image.Activation = GACT_RELVERIFY;
        mix_image.GadgetType = GTYP_BOOLGADGET;
        mix_image.GadgetRender = &img_mix1;
        mix_image.SelectRender = &img_mix1;

        mix_frame = mix_image;
        mix_frame.NextGadget = &str_gad;
        mix_frame.TopEdge = 50;
        mix_frame.GadgetRender = frame2;
        mix_frame.SelectRender = frame2;

        /* String gadget with StringInfo, Buffer and UndoBuffer of 32 bytes */
        memset(str_buf, 0, sizeof(str_buf));
        memset(str_undo, 0, sizeof(str_undo));
        memset(&str_info, 0, sizeof(str_info));
        str_info.Buffer = str_buf;
        str_info.UndoBuffer = str_undo;
        str_info.MaxChars = STR_BYTES;

        memset(&str_gad, 0, sizeof(str_gad));
        str_gad.NextGadget = NULL;
        str_gad.LeftEdge = 20;
        str_gad.TopEdge = 90;
        str_gad.Width = 120;
        str_gad.Height = 12;
        str_gad.Flags = GFLG_GADGHCOMP;
        str_gad.Activation = GACT_RELVERIFY;
        str_gad.GadgetType = GTYP_STRGADGET;
        str_gad.SpecialInfo = &str_info;

        win = OpenWindowTags(NULL,
                             WA_Left, 20,
                             WA_Top, 20,
                             WA_Width, WIN_WIDTH,
                             WA_Height, WIN_HEIGHT,
                             WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR,
                             WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP,
                             WA_Activate, TRUE,
                             WA_Gadgets, (IPTR)&gad_a,
                             WA_Title, (IPTR)"gadstates",
                             TAG_DONE);
        if (check(win != NULL, "window with the six gadgets opened"))
        {
            dri = GetScreenDrawInfo(win->WScreen);
            check(pen_value(win, 0) != pen_value(win, 1),
                  "pens 0 and 1 read apart in this window");
            check_gadghimage(win, &himage_gad);
            check_gadghcomp(win, &hcomp_gad);
            if (dri)
                check_mixed_chains(win, &mix_image, &mix_frame, dri);
            check_off_on_refresh(win, &gad_a, &himage_gad);
            check_activate(win, &gad_a, &str_gad);
            if (dri)
                FreeScreenDrawInfo(win->WScreen, dri);
            CloseWindow(win);
        }
    }

    if (frame2)
        DisposeObject(frame2);
    if (frame1)
        DisposeObject(frame1);
    if (data_clear)
        FreeMem(data_clear, IMG_BYTES);
    if (data_set)
        FreeMem(data_set, IMG_BYTES);

    printf("gadstates: %d checks, %d failed\n", checks, failures);
    return failures ? 20 : 0;
}

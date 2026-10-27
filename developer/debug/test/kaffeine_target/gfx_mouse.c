/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The mouse on a window of the program's own: a GadTools button, a
    Boolean gadget and two proportional gadgets that the program built
    itself. The window opens inactive at 40,40 of the default public
    screen, 300 by 150, without WFLG_REPORTMOUSE, and the program writes
    SYS:EMU68/NAME-mouse-N.txt, NAME being the name that it runs under and
    N 1 from the Shell or, once the runner's S:User-Startup wrote
    NAME-shell-rc.txt, 2 through Run, so that run-raspi-qemu.py --mouse
    moves the USB mouse; on AmigaOS a script writes the same events into
    input.device. The pointer positions below are on the screen, whose
    top left corner is the display's, and the program waits up to 60
    seconds for the first message and 20 for each later step:

      1. a click at 240,150 activates the window: IDCMP_ACTIVEWINDOW
         comes, and the SELECTUP of the click at 200,110 in the window;
      2. a click at 250,155: SELECTDOWN and SELECTUP at 210,115;
      3. a click on the GadTools button at 110,77: its IDCMP_GADGETUP;
      4. a click on the Boolean gadget at 230,77: IDCMP_GADGETDOWN and
         IDCMP_GADGETUP of the program's gadget;
      5. the knob of the first proportional gadget, pressed at 80,106 and
         dragged 57 pixels to the right, then released: while the button
         holds the knob, which is selected with KNOBHIT as it is pressed,
         the program reads HorizPot without reading messages for 6
         seconds or until the gadget loses GFLG_SELECTED, and prints what
         it saw; after the release HorizPot is the middle of the 114
         pixels the knob can move, 57 * MAXPOT / 114 as AROS's Intuition
         counts, or one more as AmigaOS 3.2.3 does, and KNOBHIT stays;
      6. the knob of the second one, GACT_FOLLOWMOUSE, pressed at 80,136
         and dragged 76 pixels to the right in steps: the program reads
         the messages during the drag, IDCMP_MOUSEMOVE among them, and
         HorizPot at each, which grows; the last IDCMP_MOUSEMOVE is at
         116,96 in the window and the final HorizPot 76 * MAXPOT / 114;
      7. the pointer moves 30,20 more with no button: there is no message
         without WFLG_REPORTMOUSE, and the program prints for 3 seconds
         whether the window's MouseX and MouseY follow.

    After the last step the program reads what else comes for a second:
    no IDCMP_MOUSEBUTTONS may have come since the second click. The rules
    checked hold on AmigaOS 3.2.3 and through kaffeine; what the program
    reads without messages (5 and 7), the pots it saw and the IAddress of
    IDCMP_MOUSEMOVE are printed as "info" lines, not checked. The script
    pauses between its steps, half a second and more, so that the program,
    which looks at its port every 40 ms, reads each message before the
    next step: a program held up longer would see a later state.
*/

#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

#define WIN_LEFT   40
#define WIN_TOP    40
#define WIN_WIDTH  300
#define WIN_HEIGHT 150

#define ID_BUTTON  1
#define ID_BOOL    2
#define ID_PROP_A  3
#define ID_PROP_B  4

/* where the clicks and the drags happen, in the window */
#define ACT_X      200
#define ACT_Y      110
#define CLICK_X    210
#define CLICK_Y    115
#define DRAG_A     57
#define DRAG_B     76
#define B_PRESS_X  40
#define B_PRESS_Y  96

/* the knob of a 160 pixel AUTOKNOB gadget with a quarter of the body:
   152 pixels inside the borders, 38 for the knob */
#define KNOB_ROOM  114

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };

static WORD bool_xy[] = { 0, 0, 99, 0, 99, 13, 0, 13, 0, 0 };
static struct Border bool_border = { 0, 0, 1, 0, JAM1, 5, bool_xy, NULL };
static struct IntuiText bool_text = { 1, 0, JAM1, 30, 3, &topaz8, (UBYTE *)"Bool", NULL };

static struct PropInfo prop_a_info, prop_b_info;
static struct Image prop_a_knob, prop_b_knob;
static struct Gadget bool_gadget, prop_a, prop_b;

/* one message as the program read it, with what it saw at that moment */
struct Seen
{
    ULONG cls;
    UWORD code, qual;
    WORD x, y;
    APTR address;
    UWORD pot_b;
    WORD win_x, win_y;
};

#define MAX_SEEN 256
static struct Seen seen[MAX_SEEN];
static LONG seen_count;

/* SYS:EMU68/NAME followed by tail, NAME the program's name without its path */
static BOOL emu68_file(char *path, LONG size, const char *tail)
{
    char program[64];

    if (!GetProgramName((STRPTR)program, sizeof(program)))
        return FALSE;
    if (strlen("SYS:EMU68/") + strlen((const char *)FilePart((CONST_STRPTR)program)) +
        strlen(tail) >= (ULONG)size)
        return FALSE;
    strcpy(path, "SYS:EMU68/");
    strcat(path, (const char *)FilePart((CONST_STRPTR)program));
    strcat(path, tail);
    return TRUE;
}

/* The marker of this execution: 2 once the Shell execution has ended */
static BOOL write_marker(void)
{
    char path[96];
    BPTR file;
    BOOL second = FALSE;

    if (emu68_file(path, sizeof(path), "-shell-rc.txt") &&
        (file = Open((CONST_STRPTR)path, MODE_OLDFILE)))
    {
        Close(file);
        second = TRUE;
    }
    if (!emu68_file(path, sizeof(path), second ? "-mouse-2.txt" : "-mouse-1.txt") ||
        !(file = Open((CONST_STRPTR)path, MODE_NEWFILE)))
        return FALSE;
    Write(file, (APTR)"ready\n", 6);
    Close(file);
    return TRUE;
}

/* The messages of win, kept in seen[] from first on, until one of class
   cls, from gadget when that is not NULL, at most ticks times 1/50 s.
   The index of that message in seen[], or -1. */
static LONG collect(struct Window *win, ULONG cls, APTR gadget, int ticks)
{
    struct IntuiMessage *msg;
    LONG found = -1;
    int i;

    for (i = 0; i < ticks && found < 0; i += 2)
    {
        while (found < 0 && (msg = GT_GetIMsg(win->UserPort)))
        {
            if (seen_count < MAX_SEEN)
            {
                struct Seen *s = &seen[seen_count];

                s->cls = msg->Class;
                s->code = msg->Code;
                s->qual = msg->Qualifier;
                s->x = msg->MouseX;
                s->y = msg->MouseY;
                s->address = msg->IAddress;
                s->pot_b = prop_b_info.HorizPot;
                s->win_x = win->MouseX;
                s->win_y = win->MouseY;
                if (msg->Class == cls && (!gadget || msg->IAddress == gadget))
                    found = seen_count;
                seen_count++;
            }
            GT_ReplyIMsg(msg);
        }
        if (found < 0)
            Delay(2);
    }
    return found;
}

/* The first message of class cls with that Code (any Code for -1) in
   seen[from..to), or -1 */
static LONG find(LONG from, LONG to, ULONG cls, LONG code)
{
    LONG i;

    for (i = from; i < to; i++)
        if (seen[i].cls == cls && (code < 0 || seen[i].code == code))
            return i;
    return -1;
}

static void check_at(const char *what, LONG i, WORD x, WORD y)
{
    char label[96];

    strcpy(label, what);
    strcat(label, ": MouseX");
    check_long(label, i >= 0 ? seen[i].x : -1, x);
    strcpy(label, what);
    strcat(label, ": MouseY");
    check_long(label, i >= 0 ? seen[i].y : -1, y);
}

static const char *address_kind(struct Window *win, APTR address)
{
    return address == (APTR)win ? "the window" : address == (APTR)&prop_b ? "the gadget" :
           address ? "something else" : "NULL";
}

static void init_prop(struct Gadget *g, struct PropInfo *pi, struct Image *knob,
                      WORD top, UWORD activation, UWORD id, struct Gadget *next)
{
    pi->Flags = AUTOKNOB | FREEHORIZ;
    pi->HorizBody = MAXBODY / 4;
    pi->VertBody = MAXBODY;
    g->NextGadget = next;
    g->LeftEdge = 20;
    g->TopEdge = top;
    g->Width = 160;
    g->Height = 12;
    g->Flags = GFLG_GADGHCOMP;
    g->Activation = activation;
    g->GadgetType = GTYP_PROPGADGET;
    g->GadgetRender = knob;
    g->SpecialInfo = pi;
    g->GadgetID = id;
}

/* 1 and 2: clicks on the window itself */
static void step_clicks(struct Window *win)
{
    LONG from = seen_count, up, down;

    up = collect(win, IDCMP_MOUSEBUTTONS, NULL, 60 * 50);
    while (up >= 0 && seen[up].code != SELECTUP)
        up = collect(win, IDCMP_MOUSEBUTTONS, NULL, 20 * 50);
    if (!check(find(from, seen_count, IDCMP_ACTIVEWINDOW, -1) >= 0,
               "1: the click activates the window, IDCMP_ACTIVEWINDOW"))
        return;
    check(up >= 0, "1: the SELECTUP of the click comes");
    check_at("1: SELECTUP", up, ACT_X, ACT_Y);
    down = find(from, seen_count, IDCMP_MOUSEBUTTONS, SELECTDOWN);
    if (down >= 0)
        Printf("info 1: the SELECTDOWN that activated the window came, at %ld,%ld, "
               "Qualifier 0x%04lx\n", (LONG)seen[down].x, (LONG)seen[down].y,
               (LONG)seen[down].qual);
    else
        Printf("info 1: the SELECTDOWN that activated the window did not come\n");

    from = seen_count;
    up = collect(win, IDCMP_MOUSEBUTTONS, NULL, 20 * 50);
    while (up >= 0 && seen[up].code != SELECTUP)
        up = collect(win, IDCMP_MOUSEBUTTONS, NULL, 20 * 50);
    down = find(from, seen_count, IDCMP_MOUSEBUTTONS, SELECTDOWN);
    check(down >= 0 && up > down, "2: SELECTDOWN, then SELECTUP of the second click");
    check_at("2: SELECTDOWN", down, CLICK_X, CLICK_Y);
    check_at("2: SELECTUP", up, CLICK_X, CLICK_Y);
    if (down >= 0)
        check((seen[down].qual & IEQUALIFIER_LEFTBUTTON) != 0,
              "2: the Qualifier of SELECTDOWN has IEQUALIFIER_LEFTBUTTON");
}

/* 3 and 4: a GadTools button and a Boolean gadget of the program's own */
static void step_gadgets(struct Window *win, struct Gadget *button)
{
    LONG from = seen_count, up, down;

    up = collect(win, IDCMP_GADGETUP, NULL, 20 * 50);
    if (check(up >= 0, "3: IDCMP_GADGETUP of the GadTools button comes"))
    {
        check(seen[up].address == (APTR)button, "3: its IAddress is the button");
        check_long("3: GadgetID of the button", button->GadgetID, ID_BUTTON);
    }

    from = seen_count;
    up = collect(win, IDCMP_GADGETUP, &bool_gadget, 20 * 50);
    down = find(from, seen_count, IDCMP_GADGETDOWN, -1);
    check(down >= 0 && seen[down].address == (APTR)&bool_gadget,
          "4: IDCMP_GADGETDOWN of the Boolean gadget, IAddress the program's gadget");
    check(up >= 0 && up > down,
          "4: then its IDCMP_GADGETUP, IAddress the program's gadget");
    check(!(bool_gadget.Flags & GFLG_SELECTED), "4: the Boolean gadget is not selected after");
}

#define MAX_POTS 8

/* "info" and the distinct pots in their order, at most MAX_POTS */
static void print_pots(const char *what, const UWORD *pots, LONG count)
{
    LONG i;

    Printf("%s", (LONG)what);
    for (i = 0; i < count && i < MAX_POTS; i++)
        Printf(" %ld", (LONG)pots[i]);
    Printf("%s\n", (LONG)(count > MAX_POTS ? " ..." : ""));
}

/* 5: a drag of the first knob, read without messages while it goes on */
static void step_drag_unread(struct Window *win)
{
    LONG down, up, values = 0, ticks;
    UWORD last = 0xFFFF, pots[MAX_POTS];
    BOOL released = FALSE;

    down = collect(win, IDCMP_GADGETDOWN, &prop_a, 20 * 50);
    if (!check(down >= 0, "5: IDCMP_GADGETDOWN of the first proportional gadget"))
        return;
    check(prop_a_info.HorizPot == 0, "5: HorizPot 0 as the knob is pressed");
    check((prop_a.Flags & GFLG_SELECTED) != 0, "5: the gadget is selected as the knob is pressed");
    check((prop_a_info.Flags & KNOBHIT) != 0, "5: and KNOBHIT is set");
    for (ticks = 0; ticks < 6 * 50 && !released; ticks += 2)
    {
        if (prop_a_info.HorizPot != last)
        {
            last = prop_a_info.HorizPot;
            if (values < MAX_POTS)
                pots[values] = last;
            values++;
        }
        released = !(prop_a.Flags & GFLG_SELECTED);
        if (!released)
            Delay(2);
    }
    if (released)
        Printf("info 5: read without messages while the button held the knob, HorizPot "
               "took %ld value(s); GFLG_SELECTED went off after %ld ms\n", values, ticks * 20);
    else
        Printf("info 5: read without messages while the button held the knob, HorizPot "
               "took %ld value(s); GFLG_SELECTED stayed on for 6 s\n", values);
    print_pots("info 5: they were", pots, values);

    up = collect(win, IDCMP_GADGETUP, &prop_a, 20 * 50);
    if (!check(up >= 0, "5: IDCMP_GADGETUP of the first proportional gadget, after the release"))
        return;
    /* the middle of the knob's travel: AROS's Intuition gives 57 * MAXPOT / 114,
       rounded down, AmigaOS 3.2.3 one more */
    check(prop_a_info.HorizPot == DRAG_A * MAXPOT / KNOB_ROOM ||
          prop_a_info.HorizPot == DRAG_A * MAXPOT / KNOB_ROOM + 1,
          "5: HorizPot after the drag of 57 of the knob's 114 pixels is 32767 or 32768");
    Printf("info 5: HorizPot %ld after the drag\n", (LONG)prop_a_info.HorizPot);
    check((prop_a_info.Flags & KNOBHIT) != 0, "5: KNOBHIT stays after the release");
    check(!(prop_a.Flags & GFLG_SELECTED), "5: the gadget is not selected after");
}

/* 6: a drag of the second knob, GACT_FOLLOWMOUSE, read at each message */
static void step_drag_read(struct Window *win)
{
    LONG from = seen_count, up, down, i, moves = 0, values = 0, last_move = -1;
    UWORD pot = 0, pots[MAX_POTS];
    BOOL grows = TRUE;

    up = collect(win, IDCMP_GADGETUP, &prop_b, 30 * 50);
    down = find(from, seen_count, IDCMP_GADGETDOWN, -1);
    check(down >= 0 && seen[down].address == (APTR)&prop_b,
          "6: IDCMP_GADGETDOWN of the second proportional gadget");
    if (!check(up >= 0, "6: then its IDCMP_GADGETUP"))
        return;
    for (i = down + 1; down >= 0 && i < up; i++)
    {
        if (seen[i].cls != IDCMP_MOUSEMOVE)
            continue;
        if (!moves || seen[i].pot_b != pot)
        {
            if (values < MAX_POTS)
                pots[values] = seen[i].pot_b;
            values++;
        }
        if (moves && seen[i].pot_b < pot)
            grows = FALSE;
        pot = seen[i].pot_b;
        last_move = i;
        moves++;
    }
    check(moves > 0, "6: IDCMP_MOUSEMOVE comes during the drag");
    check(values >= 2, "6: HorizPot read at those messages changes during the drag");
    check(grows, "6: and never goes back");
    check_at("6: the last IDCMP_MOUSEMOVE", last_move, B_PRESS_X + DRAG_B, B_PRESS_Y);
    check_long("6: HorizPot after the drag of 76 pixels", prop_b_info.HorizPot,
               DRAG_B * MAXPOT / KNOB_ROOM);
    check((prop_b_info.Flags & KNOBHIT) != 0, "6: KNOBHIT stays after the release");
    if (last_move >= 0)
    {
        Printf("info 6: %ld IDCMP_MOUSEMOVE, HorizPot took %ld value(s) at them; IAddress "
               "%s; the window's MouseX,MouseY %ld,%ld at the last one\n", moves, values,
               (LONG)address_kind(win, seen[last_move].address), (LONG)seen[last_move].win_x,
               (LONG)seen[last_move].win_y);
        print_pots("info 6: they were", pots, values);
    }
}

/* 7: the pointer moves with no message to say so */
static void step_unreported(struct Window *win)
{
    WORD x = win->MouseX, y = win->MouseY;
    LONG ticks;

    for (ticks = 0; ticks < 3 * 50 && win->MouseX == x && win->MouseY == y; ticks += 2)
        Delay(2);
    Printf("info 7: without messages the window's MouseX,MouseY went from %ld,%ld to %ld,%ld\n",
           (LONG)x, (LONG)y, (LONG)win->MouseX, (LONG)win->MouseY);
}

int main(void)
{
    struct Gadget *glist = NULL, *context, *button = NULL;
    LONG from;
    struct Screen *screen = NULL;
    struct Window *win = NULL;
    struct NewGadget ng;
    APTR vi = NULL;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 39"))
        goto done;
    screen = LockPubScreen(NULL);
    vi = screen ? GetVisualInfo(screen, TAG_DONE) : NULL;
    if (!check(screen && vi, "LockPubScreen() and GetVisualInfo()"))
        goto done;

    context = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = 20;
    ng.ng_TopEdge = 30;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Button";
    ng.ng_TextAttr = &topaz8;
    ng.ng_GadgetID = ID_BUTTON;
    ng.ng_VisualInfo = vi;
    button = CreateGadget(BUTTON_KIND, context, &ng, TAG_DONE);
    if (!check(context && button, "CreateContext() and a BUTTON_KIND gadget"))
        goto done;

    bool_gadget.NextGadget = &prop_a;
    bool_gadget.LeftEdge = 140;
    bool_gadget.TopEdge = 30;
    bool_gadget.Width = 100;
    bool_gadget.Height = 14;
    bool_gadget.Flags = GFLG_GADGHCOMP;
    bool_gadget.Activation = GACT_IMMEDIATE | GACT_RELVERIFY;
    bool_gadget.GadgetType = GTYP_BOOLGADGET;
    bool_gadget.GadgetRender = &bool_border;
    bool_gadget.GadgetText = &bool_text;
    bool_gadget.GadgetID = ID_BOOL;
    init_prop(&prop_a, &prop_a_info, &prop_a_knob, 60, GACT_IMMEDIATE | GACT_RELVERIFY,
              ID_PROP_A, &prop_b);
    init_prop(&prop_b, &prop_b_info, &prop_b_knob, 90,
              GACT_IMMEDIATE | GACT_RELVERIFY | GACT_FOLLOWMOUSE, ID_PROP_B, NULL);

    win = OpenWindowTags(NULL,
                         WA_Left, WIN_LEFT, WA_Top, WIN_TOP,
                         WA_Width, WIN_WIDTH, WA_Height, WIN_HEIGHT,
                         WA_Title, (ULONG)"gfx_mouse",
                         WA_PubScreen, (ULONG)screen,
                         WA_Gadgets, (ULONG)glist,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE |
                                   IDCMP_GADGETDOWN | IDCMP_GADGETUP,
                         TAG_DONE);
    if (!check(win != NULL, "OpenWindowTags() of an inactive window with the button"))
        goto done;
    GT_RefreshWindow(win, NULL);
    Printf("info: AddGList() put the program's three gadgets at position %ld\n",
           (LONG)AddGList(win, &bool_gadget, -1, 3, NULL));
    RefreshGList(&bool_gadget, win, NULL, 3);
    check(!(win->Flags & WFLG_WINDOWACTIVE), "the window opens inactive");
    if (!check(write_marker(), "SYS:EMU68 marker written for the mouse"))
        goto done;

    step_clicks(win);
    from = seen_count;
    step_gadgets(win, button);
    step_drag_unread(win);
    step_drag_read(win);
    step_unreported(win);
    /* what came after the last step, for a second: no IDCMP_NEWSIZE comes */
    collect(win, IDCMP_NEWSIZE, NULL, 50);
    check(find(from, seen_count, IDCMP_MOUSEBUTTONS, -1) < 0,
          "3 to 7: no IDCMP_MOUSEBUTTONS for the clicks and drags on gadgets");

done:
    if (win)
    {
        RemoveGList(win, &bool_gadget, 3);
        CloseWindow(win);
    }
    if (GadToolsBase)
        FreeGadgets(glist);
    if (vi)
        FreeVisualInfo(vi);
    if (screen)
        UnlockPubScreen(NULL, screen);
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_mouse");
}

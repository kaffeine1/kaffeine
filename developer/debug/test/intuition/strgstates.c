/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    States, flags and buffers of classic Intuition string gadgets, as on
    AmigaOS 3.2.3. OpenWindowTags() sets NumChars of each string gadget to
    the length of the text in its Buffer, also beyond MaxChars when a
    longer text has no NUL within MaxChars bytes. A gadget with
    GACT_STRINGEXTEND alone in Activation, without GFLG_STRINGEXTEND in
    Flags, uses its StringExtend and draws its background in Pens[1].
    ActivateGadget() of a string gadget on the active window returns TRUE,
    sets GACT_ACTIVEGADGET and GFLG_SELECTED, and copies its Buffer into
    UndoBuffer. Typing into a GFLG_TABCYCLE gadget and pressing Tab sends
    IDCMP_GADGETUP for it with Code 9, clears GACT_ACTIVEGADGET and
    GFLG_SELECTED on it, activates the next GFLG_TABCYCLE gadget, and
    copies that gadget's Buffer into the shared UndoBuffer. Typing into a
    GACT_LONGINT gadget and pressing Return sends IDCMP_GADGETUP for it
    with Code 0, updates its Buffer, NumChars and LongInt, and clears
    GACT_ACTIVEGADGET and GFLG_SELECTED. RefreshGList() keeps a BufferPos
    set within the text by the program. An AltKeyMap in StringInfo is used
    only when GACT_ALTKEYMAP is set in Activation. RemoveGList() of an
    active string gadget clears both GACT_ACTIVEGADGET and GFLG_SELECTED,
    and RemoveGList() and RemoveGadget() leave NextGadget of the last
    removed gadget pointing to the gadget that followed it. A click outside
    the active gadget ends it without GACT_ACTIVEGADGET and GFLG_SELECTED.
    Del in a GFLG_STRINGEXTEND gadget with a WorkBuffer and a text longer
    than MaxChars edits the whole text.
    The keys assume a keymap of the US layout, which maps raw key 0x20 to
    a and the top row to digits, and the click a mouse whose buttons the
    input preferences do not swap. Output in the form of the kaffeine target
    tests: a line per check and a summary.
*/

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/keymap.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/keymap.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <stdio.h>
#include <string.h>

#define WIN_WIDTH  320
#define WIN_HEIGHT 160

#define RAW_1         0x01
#define RAW_2         0x02
#define RAW_9         0x09
#define RAW_A         0x20
#define RAW_BACKSPACE 0x41
#define RAW_TAB       0x42
#define RAW_RETURN    0x44
#define RAW_DEL       0x46

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

static struct MsgPort *inputport;
static struct IOStdReq *inputreq;

/* An event through input.device */
static void writeevent(UBYTE class, UBYTE subclass, UWORD code, UWORD qual, APTR address)
{
    struct InputEvent ie;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = class;
    ie.ie_SubClass = subclass;
    ie.ie_Code = code;
    ie.ie_Qualifier = qual;
    ie.ie_EventAddress = address;
    inputreq->io_Command = IND_WRITEEVENT;
    inputreq->io_Data = &ie;
    inputreq->io_Length = sizeof(ie);
    DoIO((struct IORequest *)inputreq);
}

/* A key pressed and released through input.device */
static void type(UWORD raw, UWORD qual)
{
    writeevent(IECLASS_RAWKEY, 0, raw, qual, NULL);
    writeevent(IECLASS_RAWKEY, 0, raw | IECODE_UP_PREFIX, qual, NULL);
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

/* The next IDCMP_GADGETUP within at most about 5 seconds: its gadget and
   its Code, the other messages replied */
static struct Gadget *gadgetup(struct Window *win, UWORD *code)
{
    struct IntuiMessage *im;
    struct Gadget *g = NULL;
    int i;

    *code = 0xFFFF;
    for (i = 0; i < 250 && !g; i++)
    {
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (im->Class == IDCMP_GADGETUP && !g)
            {
                g = (struct Gadget *)im->IAddress;
                *code = im->Code;
            }
            ReplyMsg(&im->ExecMessage);
        }
        if (!g)
            Delay(1);
    }
    return g;
}

static UBYTE buf1[32], buf2[12], buf3[32], buf4[32], buf5[32], buf6[16], buf7[16], undo[32], work3[32],
             work7[32];
static struct StringInfo si1, si2, si3, si4, si5, si6, si7;
static struct StringExtend se3, se7;
static struct Gadget g1, g2, g3, g4, g5, g6, g7;
static struct KeyMap altmap;
static IPTR lomap[64];
static UBYTE lotypes[64];

static void gadget(struct Gadget *g, WORD top, WORD width, UWORD flags, UWORD act,
                   struct StringInfo *si, UWORD id, struct Gadget *next)
{
    memset(g, 0, sizeof(*g));
    g->NextGadget = next;
    g->LeftEdge = 20;
    g->TopEdge = top;
    g->Width = width;
    g->Height = 10;
    g->Flags = flags;
    g->Activation = act;
    g->GadgetType = GTYP_STRGADGET;
    g->SpecialInfo = si;
    g->GadgetID = id;
}

static void info(struct StringInfo *si, UBYTE *buffer, const char *text, WORD maxchars,
                 WORD pos, UBYTE *undobuf)
{
    memset(si, 0, sizeof(*si));
    strcpy((char *)buffer, text);
    si->Buffer = buffer;
    si->UndoBuffer = undobuf;
    si->MaxChars = maxchars;
    si->BufferPos = pos;
}

/* 1. NumChars after OpenWindowTags() of the first three gadgets (5, 2, 4)
   and of the two gadgets with ten characters in a Buffer of MaxChars 8
   (10 each). */
static void check_open_numchars(void)
{
    check(si1.NumChars == 5 && si2.NumChars == 2 && si3.NumChars == 4,
          "NumChars after OpenWindowTags() of the first three gadgets is 5, 2 and 4");
    check(si6.NumChars == 10 && si7.NumChars == 10,
          "NumChars after OpenWindowTags() of the 10-character gadgets with MaxChars 8 is 10");
}

/* 2. ReadPixel() inside the GACT_STRINGEXTEND gadget near its right end,
   where no text is drawn, matches a pixel drawn in pen 3, and inside a
   plain gadget it does not, so that the check sees the StringExtend. */
static void check_stringextend(struct Window *win)
{
    LONG bg = ReadPixel(win->RPort, g3.LeftEdge + g3.Width - 3, g3.TopEdge + 5);
    LONG plain = ReadPixel(win->RPort, g4.LeftEdge + g4.Width - 3, g4.TopEdge + 5);
    LONG pen3 = pen_value(win, 3);

    check(bg != -1 && plain != -1 && plain != pen3,
          "a plain string gadget reads otherwise than pen 3");
    check(bg == pen3,
          "GACT_STRINGEXTEND gadget draws its StringExtend background in pen 3");
}

/* 3. IDCMP_ACTIVEWINDOW arrives within 10 seconds; ActivateGadget() of the
   first gadget gives TRUE, sets GACT_ACTIVEGADGET and GFLG_SELECTED, and
   copies "Hello" into the shared UndoBuffer. */
static void check_activate(struct Window *win)
{
    BOOL active, ok;

    active = await(win, IDCMP_ACTIVEWINDOW);
    check(active, "IDCMP_ACTIVEWINDOW received within 10 seconds");

    ok = ActivateGadget(&g1, win, NULL);
    check(ok && (g1.Activation & GACT_ACTIVEGADGET) != 0 &&
          (g1.Flags & GFLG_SELECTED) != 0,
          "ActivateGadget() of the first gadget gives TRUE and sets GACT_ACTIVEGADGET and GFLG_SELECTED");
    check(strcmp((char *)undo, "Hello") == 0,
          "the shared UndoBuffer holds \"Hello\" after activating the first gadget");
}

/* 4. Keys 1, 2 and Tab through input.device end the first gadget with
   IDCMP_GADGETUP and Code 9, leaving "Hello12", NumChars 7, BufferPos 7,
   and neither GACT_ACTIVEGADGET nor GFLG_SELECTED, and activate the
   integer gadget with BufferPos 2 and "42" in the shared UndoBuffer. */
static void check_tab_cycle(struct Window *win)
{
    struct Gadget *g;
    UWORD code;

    type(RAW_1, 0);
    type(RAW_2, 0);
    type(RAW_TAB, 0);
    g = gadgetup(win, &code);
    check(g == &g1 && code == 9,
          "keys 1, 2 and Tab send IDCMP_GADGETUP for the first gadget with Code 9");
    check(strcmp((char *)buf1, "Hello12") == 0 &&
          si1.NumChars == 7 && si1.BufferPos == 7,
          "the first gadget holds \"Hello12\" with NumChars 7 and BufferPos 7");
    check(!(g1.Activation & GACT_ACTIVEGADGET) && !(g1.Flags & GFLG_SELECTED),
          "the first gadget has neither GACT_ACTIVEGADGET nor GFLG_SELECTED after Tab");
    check((g2.Activation & GACT_ACTIVEGADGET) != 0 && si2.BufferPos == 2,
          "the integer gadget is active with GACT_ACTIVEGADGET and BufferPos 2");
    check(strcmp((char *)undo, "42") == 0,
          "the shared UndoBuffer holds \"42\" when the integer gadget becomes active");
}

/* 5. Keys Backspace, Backspace, 9, 9 and Return end the integer gadget
   with IDCMP_GADGETUP and Code 0, leaving "99", NumChars 2, LongInt 99,
   and neither GACT_ACTIVEGADGET nor GFLG_SELECTED. */
static void check_longint_return(struct Window *win)
{
    struct Gadget *g;
    UWORD code;

    type(RAW_BACKSPACE, 0);
    type(RAW_BACKSPACE, 0);
    type(RAW_9, 0);
    type(RAW_9, 0);
    type(RAW_RETURN, 0);
    g = gadgetup(win, &code);
    check(g == &g2 && code == 0,
          "Backspaces, 9, 9 and Return send IDCMP_GADGETUP for the integer gadget with Code 0");
    check(strcmp((char *)buf2, "99") == 0 &&
          si2.NumChars == 2 && si2.LongInt == 99,
          "the integer gadget holds \"99\" with NumChars 2 and LongInt 99");
    check(!(g2.Activation & GACT_ACTIVEGADGET) && !(g2.Flags & GFLG_SELECTED),
          "the integer gadget has neither GACT_ACTIVEGADGET nor GFLG_SELECTED after Return");
}

/* 6. BufferPos of the first gadget set to 2 by the program stays 2 through
   RefreshGList() of it, with NumChars 7. */
static void check_refresh_bufferpos(struct Window *win)
{
    si1.BufferPos = 2;
    RefreshGList(&g1, win, NULL, 1);
    check(si1.BufferPos == 2 && si1.NumChars == 7,
          "BufferPos of the first gadget set to 2 stays 2 through RefreshGList(), NumChars 7");
}

/* 7. The A key in the gadget with an AltKeyMap but without GACT_ALTKEYMAP
   gives "a", and in the gadget with GACT_ALTKEYMAP gives "z". */
static void check_altkeymap(struct Window *win)
{
    struct Gadget *g;
    UWORD code;
    BOOL ok;

    ok = ActivateGadget(&g4, win, NULL);
    if (ok)
    {
        type(RAW_A, 0);
        type(RAW_RETURN, 0);
    }
    g = ok ? gadgetup(win, &code) : NULL;
    check(ok && g == &g4 && strcmp((char *)buf4, "a") == 0,
          "the A key in the gadget without GACT_ALTKEYMAP gives \"a\"");

    ok = ActivateGadget(&g5, win, NULL);
    if (ok)
    {
        type(RAW_A, 0);
        type(RAW_RETURN, 0);
    }
    g = ok ? gadgetup(win, &code) : NULL;
    check(ok && g == &g5 && strcmp((char *)buf5, "z") == 0,
          "the A key in the gadget with GACT_ALTKEYMAP gives \"z\"");
}

/* 8. RemoveGList() of the first gadget while it is active leaves it
   without GACT_ACTIVEGADGET and without GFLG_SELECTED; AddGList() puts it
   back. */
static void check_remove_active(struct Window *win)
{
    BOOL ok;
    UWORD pos;

    ok = ActivateGadget(&g1, win, NULL);
    pos = RemoveGList(win, &g1, 1);
    check(ok && pos != (UWORD)~0 &&
          !(g1.Activation & GACT_ACTIVEGADGET) && !(g1.Flags & GFLG_SELECTED),
          "RemoveGList() of the active first gadget clears GACT_ACTIVEGADGET and GFLG_SELECTED");
    AddGList(win, &g1, 0, 1, NULL);
    RefreshGList(&g1, win, NULL, 1);
}

/* Whether g is on the gadget list of win */
static BOOL on_list(struct Window *win, struct Gadget *g)
{
    struct Gadget *each;

    for (each = win->FirstGadget; each; each = each->NextGadget)
        if (each == g)
            return TRUE;
    return FALSE;
}

/* 9. RemoveGList() of two gadgets takes them off the window's list and
   leaves NextGadget of the last one on the gadget that followed it, and
   RemoveGadget() does too; put them back after. */
static void check_remove_links(struct Window *win)
{
    UWORD pos;

    pos = RemoveGList(win, &g2, 2);
    check(pos != (UWORD)~0 && !on_list(win, &g2) && !on_list(win, &g3) &&
          on_list(win, &g4) && g3.NextGadget == &g4,
          "RemoveGList() of two gadgets takes them off and leaves NextGadget of the last one on the gadget after it");
    AddGList(win, &g2, ~0, 2, NULL);

    pos = RemoveGadget(win, &g4);
    check(pos != (UWORD)~0 && !on_list(win, &g4) && on_list(win, &g5) && g4.NextGadget == &g5,
          "RemoveGadget() takes it off and leaves NextGadget on the gadget that followed it");
    AddGadget(win, &g4, ~0);
}

/* 10. A click outside the active first gadget, through input.device, ends
   it: neither GACT_ACTIVEGADGET nor GFLG_SELECTED any more. */
static void check_click_away(struct Window *win)
{
    struct IEPointerPixel pp;
    BOOL ok;

    ok = ActivateGadget(&g1, win, NULL);
    pp.iepp_Screen = win->WScreen;
    pp.iepp_Position.X = win->LeftEdge + 250;
    pp.iepp_Position.Y = win->TopEdge + 140;
    writeevent(IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL, IECODE_NOBUTTON, 0, &pp);
    writeevent(IECLASS_RAWMOUSE, 0, IECODE_LBUTTON, IEQUALIFIER_RELATIVEMOUSE | IEQUALIFIER_LEFTBUTTON,
               NULL);
    writeevent(IECLASS_RAWMOUSE, 0, IECODE_LBUTTON | IECODE_UP_PREFIX, IEQUALIFIER_RELATIVEMOUSE, NULL);
    Delay(10);
    check(ok && !(g1.Activation & GACT_ACTIVEGADGET) && !(g1.Flags & GFLG_SELECTED),
          "a click outside the active first gadget ends it without GACT_ACTIVEGADGET and GFLG_SELECTED");
}

/* 11. Del and Return in the GFLG_STRINGEXTEND gadget with a WorkBuffer and
   ten characters in MaxChars 8, the cursor at the start, leave "bcdefghij"
   with NumChars 9: Intuition edits a copy of the whole text. */
static void check_long_extended(struct Window *win)
{
    struct Gadget *g;
    UWORD code;
    BOOL ok;

    ok = ActivateGadget(&g7, win, NULL);
    if (ok)
    {
        type(RAW_DEL, 0);
        type(RAW_RETURN, 0);
    }
    g = ok ? gadgetup(win, &code) : NULL;
    check(ok && g == &g7 && strcmp((char *)buf7, "bcdefghij") == 0 && si7.NumChars == 9,
          "Del in the GFLG_STRINGEXTEND gadget with ten characters in MaxChars 8 leaves \"bcdefghij\", NumChars 9");
}

int main(void)
{
    struct TextAttr topaz = { (STRPTR)"topaz.font", 8, 0, 0 };
    struct TextFont *font;
    struct KeyMap *def;
    struct Window *win;
    int i;

    inputport = CreateMsgPort();
    if (inputport)
        inputreq = (struct IOStdReq *)CreateIORequest(inputport, sizeof(struct IOStdReq));
    if (!check(inputreq && OpenDevice("input.device", 0, (struct IORequest *)inputreq, 0) == 0,
               "input.device opened"))
    {
        if (inputreq)
            DeleteIORequest((struct IORequest *)inputreq);
        if (inputport)
            DeleteMsgPort(inputport);
        printf("strgstates: %d checks, %d failed\n", checks, failures);
        return 20;
    }

    font = OpenFont(&topaz);

    /* A keymap like the default one where the A key gives z */
    def = AskKeyMapDefault();
    altmap = *def;
    for (i = 0; i < 64; i++)
    {
        lomap[i] = def->km_LoKeyMap[i];
        lotypes[i] = def->km_LoKeyMapTypes[i];
    }
    lotypes[RAW_A] = KC_NOQUAL;
    lomap[RAW_A] = 'z';
    altmap.km_LoKeyMap = (APTR)lomap;
    altmap.km_LoKeyMapTypes = lotypes;

    info(&si1, buf1, "Hello", 32, 5, undo);
    info(&si2, buf2, "42", 12, 2, undo);
    si2.LongInt = 42;
    info(&si3, buf3, "Pens", 32, 0, NULL);
    memset(&se3, 0, sizeof(se3));
    se3.Font = font;
    se3.Pens[0] = 1;
    se3.Pens[1] = 3;
    se3.ActivePens[0] = 2;
    se3.ActivePens[1] = 3;
    se3.WorkBuffer = work3;
    si3.Extension = &se3;
    info(&si4, buf4, "", 32, 0, NULL);
    si4.AltKeyMap = &altmap;
    info(&si5, buf5, "", 32, 0, NULL);
    si5.AltKeyMap = &altmap;
    /* Eight characters without a NUL in MaxChars 8, then two more */
    memset(buf6, 0, sizeof(buf6));
    memcpy(buf6, "abcdefghij", 10);
    memset(&si6, 0, sizeof(si6));
    si6.Buffer = buf6;
    si6.MaxChars = 8;
    /* The same with a StringExtend and a WorkBuffer, larger than MaxChars
       as AmigaOS copies the whole text into it */
    memset(buf7, 0, sizeof(buf7));
    memcpy(buf7, "abcdefghij", 10);
    memset(&si7, 0, sizeof(si7));
    si7.Buffer = buf7;
    si7.MaxChars = 8;
    memset(&se7, 0, sizeof(se7));
    se7.Font = font;
    se7.Pens[0] = 1;
    se7.ActivePens[0] = 1;
    se7.WorkBuffer = work7;
    si7.Extension = &se7;

    gadget(&g7, 135, 120, GFLG_GADGHCOMP | GFLG_STRINGEXTEND, GACT_RELVERIFY, &si7, 7, NULL);
    gadget(&g6, 120, 120, GFLG_GADGHCOMP, GACT_RELVERIFY, &si6, 6, &g7);
    gadget(&g5, 105, 120, GFLG_GADGHCOMP, GACT_RELVERIFY | GACT_ALTKEYMAP, &si5, 5, &g6);
    gadget(&g4, 90, 120, GFLG_GADGHCOMP, GACT_RELVERIFY, &si4, 4, &g5);
    /* GACT_STRINGEXTEND alone, without GFLG_STRINGEXTEND */
    gadget(&g3, 70, 160, GFLG_GADGHCOMP, GACT_RELVERIFY | GACT_STRINGEXTEND, &si3, 3, &g4);
    gadget(&g2, 50, 80, GFLG_GADGHCOMP | GFLG_TABCYCLE, GACT_RELVERIFY | GACT_LONGINT, &si2, 2, &g3);
    gadget(&g1, 30, 160, GFLG_GADGHCOMP | GFLG_TABCYCLE, GACT_RELVERIFY, &si1, 1, &g2);

    win = OpenWindowTags(NULL,
                         WA_Left, 10,
                         WA_Top, 15,
                         WA_Width, WIN_WIDTH,
                         WA_Height, WIN_HEIGHT,
                         WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_ACTIVATE,
                         WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP,
                         WA_Gadgets, (IPTR)&g1,
                         WA_Title, (IPTR)"strgstates",
                         TAG_DONE);
    if (check(win != NULL, "window with the seven string gadgets opened"))
    {
        check_open_numchars();
        check_stringextend(win);
        check_activate(win);
        check_tab_cycle(win);
        check_longint_return(win);
        check_refresh_bufferpos(win);
        check_altkeymap(win);
        check_remove_active(win);
        check_remove_links(win);
        check_click_away(win);
        check_long_extended(win);
        CloseWindow(win);
    }

    if (font)
        CloseFont(font);
    CloseDevice((struct IORequest *)inputreq);
    DeleteIORequest((struct IORequest *)inputreq);
    DeleteMsgPort(inputport);

    printf("strgstates: %d checks, %d failed\n", checks, failures);
    return failures ? 20 : 0;
}

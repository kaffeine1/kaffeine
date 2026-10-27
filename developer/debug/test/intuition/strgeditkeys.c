/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The editing keys of string gadgets keep the cursor and the text within
    the buffer, and Ctrl-W deletes the word before the cursor, back to the
    start of the buffer when no space comes before it. First through the
    editing hook that SetEditHook() gives back, on buffers with guard bytes
    on both sides, then through input.device, typed into a string gadget of
    a window. Then the copies of the text into the undo and work buffers,
    which have MaxChars bytes, when the program has put a longer text into
    a larger buffer: activation, keys and a click, for a plain gadget, one
    with a work buffer and a strgclass gadget, and a text of 32768
    characters. Then clicks into a scrolled gadget whose text the program
    has shortened without a refresh, which measure the text and not what
    the buffer holds past it, and a DispPos set before the buffer. Then
    gadgets on a screen in a font of the test's own: a plain one is drawn
    in the screen's font, as on AmigaOS, and a click into any of them
    measures the text with the font it is drawn in. Last, centred and right
    aligned gadgets, also with a text wider than their box: the text is
    drawn where AmigaOS draws it, never left of the box, and a click lands
    on the character drawn under the pointer. Output in the form of the
    kaffeine target tests: a line per check and a summary.
*/

#include <aros/asmcall.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/keymap.h>
#include <proto/utility.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <intuition/classes.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <utility/hooks.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GUARD 32
#define GUARDBYTE 0xA5
#define ROOM 8

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

static void check(int ok, const char *what)
{
    checkf(ok, "%s", what);
}

/* A buffer of size bytes with GUARD bytes before and after it */
static UBYTE *guarded(UBYTE *area, ULONG size)
{
    memset(area, GUARDBYTE, size + 2 * GUARD);
    return area + GUARD;
}

static int intact(const UBYTE *buffer, ULONG size)
{
    ULONG i;

    for (i = 1; i <= GUARD; i++)
        if (buffer[-(LONG)i] != GUARDBYTE || buffer[size - 1 + i] != GUARDBYTE)
            return 0;
    return 1;
}

struct key
{
    char name[16];
    UWORD code; /* what MapRawKey() gives, 0 for a key without one */
    UWORD raw;
    UWORD qual;
};

static struct key keys[48];
static int nkeys;

static const struct key *addkey(const char *name, UWORD code, UWORD raw, UWORD qual)
{
    struct key *k = &keys[nkeys++];

    snprintf(k->name, sizeof(k->name), "%s", name);
    k->code = code;
    k->raw = raw;
    k->qual = qual;
    return k;
}

static const struct key *ctrl(char letter)
{
    return &keys[letter - 'a'];
}

static void makekeys(void)
{
    char name[16];
    int c;

    for (c = 1; c <= 26; c++)
    {
        snprintf(name, sizeof(name), "Ctrl-%c", 'A' + c - 1);
        addkey(name, c, 0x20, IEQUALIFIER_CONTROL);
    }
    addkey("Backspace", 8, 0x41, 0);
    addkey("Shift-Backspace", 8, 0x41, IEQUALIFIER_LSHIFT);
    addkey("Del", 127, 0x46, 0);
    addkey("Shift-Del", 127, 0x46, IEQUALIFIER_RSHIFT);
    addkey("Return", 13, 0x44, 0);
    addkey("Tab", 9, 0x42, 0);
    addkey("a", 'a', 0x20, 0);
    addkey("space", ' ', 0x40, 0);
    addkey("-", '-', 0x0B, 0);
    addkey("5", '5', 0x05, 0);
    addkey("left", 0, CURSORLEFT, 0);
    addkey("Shift-left", 0, CURSORLEFT, IEQUALIFIER_LSHIFT);
    addkey("right", 0, CURSORRIGHT, 0);
    addkey("Shift-right", 0, CURSORRIGHT, IEQUALIFIER_RSHIFT);
    addkey("Home", 0, 0x70, 0);
    addkey("End", 0, 0x71, 0);
}

AROS_UFH3(ULONG, noedit,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(struct SGWork *, sgw, A2),
          AROS_UFHA(ULONG *, command, A1))
{
    AROS_USERFUNC_INIT

    return 0;

    AROS_USERFUNC_EXIT
}

static struct Hook *edithook;

/* The editing hook of Intuition, or of a program that passes keys on to it */
static struct Hook *current_edithook(void)
{
    static struct Hook none;
    struct Hook *hook;

    none.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(noedit);
    Forbid();
    hook = SetEditHook(&none);
    SetEditHook(hook);
    Permit();
    return hook;
}

/* One key through the editing hook, as HandleStrInput() gives it */
static UWORD press(const struct key *k, UBYTE *buffer, WORD maxchars, WORD *pos,
                   WORD *numchars, ULONG modes, UWORD activation)
{
    struct StringInfo si;
    struct Gadget gad;
    struct InputEvent ie;
    struct SGWork sgw;
    ULONG command = SGH_KEY;

    memset(&si, 0, sizeof(si));
    si.Buffer = buffer;
    si.MaxChars = maxchars;
    si.BufferPos = *pos;
    si.NumChars = *numchars;
    memset(&gad, 0, sizeof(gad));
    gad.GadgetType = GTYP_STRGADGET;
    gad.Activation = activation;
    gad.SpecialInfo = &si;
    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = k->raw;
    ie.ie_Qualifier = k->qual;
    memset(&sgw, 0, sizeof(sgw));
    sgw.Gadget = &gad;
    sgw.StringInfo = &si;
    sgw.WorkBuffer = buffer;
    sgw.PrevBuffer = buffer;
    sgw.Modes = modes;
    sgw.IEvent = &ie;
    sgw.Code = k->code;
    sgw.BufferPos = *pos;
    sgw.NumChars = *numchars;
    sgw.Actions = SGA_USE;
    sgw.EditOp = EO_NOOP;
    CallHookPkt(edithook, &sgw, &command);
    *pos = sgw.BufferPos;
    *numchars = sgw.NumChars;
    return sgw.EditOp;
}

/* A key on text at pos must leave want at wantpos, and the guard bytes */
static void edit(const char *what, const struct key *k, const char *text, WORD pos, WORD maxchars,
                 ULONG modes, const char *want, WORD wantpos)
{
    static UBYTE area[ROOM + 2 * GUARD];
    UBYTE *buffer = guarded(area, maxchars);
    WORD numchars = strlen(text);
    int ok;

    strcpy((char *)buffer, text);
    press(k, buffer, maxchars, &pos, &numchars, modes, 0);
    ok = pos == wantpos && numchars == (WORD)strlen(want) && intact(buffer, maxchars) &&
         buffer[numchars < 0 ? 0 : numchars] == 0 && strcmp((char *)buffer, want) == 0;
    check(ok, what);
    if (!ok)
        printf("     got BufferPos %d, NumChars %d, text \"%.*s\", guard bytes %s\n", pos, numchars,
               (int)maxchars, (char *)buffer, intact(buffer, maxchars) ? "intact" : "CHANGED");
}

/* What Ctrl-W must leave with filtering on and no SGM_FIXEDFIELD */
static int ctrlw_ok(const char *text, WORD pos, const UBYTE *buffer, WORD newpos, WORD numchars)
{
    char want[ROOM + 1];
    WORD p;

    if (pos == 0)
        return newpos == 0 && strcmp((const char *)buffer, text) == 0;
    for (p = pos - 1; p > 0 && text[p] != ' '; p--)
        ;
    memcpy(want, text, p);
    strcpy(want + p, text + pos);
    return newpos == p && numchars == (WORD)strlen(want) && strcmp((const char *)buffer, want) == 0;
}

static const ULONG modeset[] = { 0, SGM_REPLACE, SGM_FIXEDFIELD, SGM_NOFILTER,
                                 SGM_REPLACE | SGM_FIXEDFIELD };

/* Every text of up to ROOM - 1 characters of "a " at every position, in every mode */
static void every(const struct key *k)
{
    static UBYTE area[ROOM + 2 * GUARD];
    UBYTE *buffer;
    char text[ROOM];
    int len, m, act, cases = 0, bad = 0, wbad = 0;
    WORD pos;
    LONG n;

    for (len = 0; len < ROOM; len++)
        for (n = 0; n < (1L << len); n++)
        {
            int i;

            for (i = 0; i < len; i++)
                text[i] = (n >> i) & 1 ? ' ' : 'a';
            text[len] = 0;
            for (pos = 0; pos <= len; pos++)
                for (m = 0; m < (int)(sizeof(modeset) / sizeof(modeset[0])); m++)
                    for (act = 0; act < 2; act++)
                    {
                        WORD newpos = pos, numchars = len;

                        buffer = guarded(area, ROOM);
                        strcpy((char *)buffer, text);
                        press(k, buffer, ROOM, &newpos, &numchars, modeset[m],
                              act ? GACT_LONGINT : 0);
                        cases++;
                        if (!intact(buffer, ROOM) || numchars < 0 || numchars > ROOM - 1 ||
                            newpos < 0 || newpos > numchars || buffer[numchars] != 0 ||
                            strlen((char *)buffer) != (size_t)numchars)
                        {
                            if (bad++ == 0)
                                printf("     \"%s\" at %d, modes %lu: BufferPos %d, NumChars %d, guard bytes %s\n",
                                       text, pos, (unsigned long)modeset[m], newpos, numchars,
                                       intact(buffer, ROOM) ? "intact" : "CHANGED");
                        }
                        else if (k == ctrl('w') && !(modeset[m] & (SGM_FIXEDFIELD | SGM_NOFILTER)) &&
                                 !ctrlw_ok(text, pos, buffer, newpos, numchars) && wbad++ == 0)
                            printf("     Ctrl-W on \"%s\" at %d leaves \"%s\" at %d\n", text, pos,
                                   (char *)buffer, newpos);
                    }
        }
    checkf(bad == 0, "%s keeps the cursor and the text within the buffer (%d cases)", k->name, cases);
    if (k == ctrl('w'))
        check(bad == 0 && wbad == 0,
              "Ctrl-W deletes back to the space before the word, or to the start of the buffer");
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

/* The pointer x pixels into a gadget of a window, and a click there */
static void click(struct Window *win, struct Gadget *gad, WORD x)
{
    struct IEPointerPixel pp;

    pp.iepp_Screen = win->WScreen;
    pp.iepp_Position.X = win->LeftEdge + gad->LeftEdge + x;
    pp.iepp_Position.Y = win->TopEdge + gad->TopEdge + gad->Height / 2;
    writeevent(IECLASS_NEWPOINTERPOS, IESUBCLASS_PIXEL, IECODE_NOBUTTON, 0, &pp);
    writeevent(IECLASS_RAWMOUSE, 0, IECODE_LBUTTON, IEQUALIFIER_RELATIVEMOUSE | IEQUALIFIER_LEFTBUTTON,
               NULL);
    writeevent(IECLASS_RAWMOUSE, 0, IECODE_LBUTTON | IECODE_UP_PREFIX, IEQUALIFIER_RELATIVEMOUSE, NULL);
}

static int openinput(void)
{
    inputport = CreateMsgPort();
    inputreq = inputport ? (struct IOStdReq *)CreateIORequest(inputport, sizeof(struct IOStdReq)) : NULL;
    if (!inputreq || OpenDevice("input.device", 0, (struct IORequest *)inputreq, 0) != 0)
    {
        check(0, "input.device opened");
        if (inputreq)
            DeleteIORequest((struct IORequest *)inputreq);
        if (inputport)
            DeleteMsgPort(inputport);
        return 0;
    }
    return 1;
}

static void closeinput(void)
{
    CloseDevice((struct IORequest *)inputreq);
    DeleteIORequest((struct IORequest *)inputreq);
    DeleteMsgPort(inputport);
}

/* What the default keymap gives for a raw key */
static int maps(UWORD raw, UWORD qual, UBYTE want)
{
    struct InputEvent ie;
    UBYTE buf[4];

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = raw;
    ie.ie_Qualifier = qual;
    return MapRawKey(&ie, (STRPTR)buf, sizeof(buf), NULL) == 1 && buf[0] == want;
}

/* A message of the class within about five seconds, the others replied */
static int await(struct Window *win, ULONG class)
{
    int i;

    for (i = 0; i < 250; i++)
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

#define RAW_W 0x11
#define RAW_A 0x20
#define RAW_B 0x35
#define RAW_C 0x33
#define RAW_RETURN 0x44

/* Ctrl-W and more typed into a string gadget of MaxChars 6 holding "Hello" */
static void typed(void)
{
    static UBYTE area[6 + 2 * GUARD], undoarea[6 + 2 * GUARD];
    struct StringInfo si;
    struct Gadget gad;
    struct Window *win;
    UBYTE *buffer = guarded(area, 6), *undo = guarded(undoarea, 6);
    int active;

    check(maps(RAW_W, IEQUALIFIER_CONTROL, 23) && maps(RAW_A, 0, 'a') && maps(RAW_B, 0, 'b') &&
          maps(RAW_C, 0, 'c') && maps(RAW_RETURN, 0, 13),
          "the keymap gives Ctrl-W, a, b, c and Return for the raw keys of the test");

    if (!openinput())
        return;

    strcpy((char *)buffer, "Hello");
    memset(&si, 0, sizeof(si));
    si.Buffer = buffer;
    si.UndoBuffer = undo;
    si.MaxChars = 6;
    si.BufferPos = 5;
    memset(&gad, 0, sizeof(gad));
    gad.LeftEdge = 20;
    gad.TopEdge = 40;
    gad.Width = 120;
    gad.Height = 12;
    gad.Flags = GFLG_GADGHCOMP;
    gad.Activation = GACT_RELVERIFY;
    gad.GadgetType = GTYP_STRGADGET;
    gad.SpecialInfo = &si;

    win = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 260, WA_Height, 80,
                         WA_Title, (IPTR)"strgeditkeys", WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                         WA_Activate, TRUE, WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP,
                         WA_Gadgets, (IPTR)&gad, TAG_DONE);
    check(win != NULL, "a window with a string gadget of MaxChars 6 holding \"Hello\"");
    if (win)
    {
        active = (await(win, IDCMP_ACTIVEWINDOW) || (win->Flags & WFLG_WINDOWACTIVE)) &&
                 ActivateGadget(&gad, win, NULL);
        check(active, "the window active, and its string gadget with the cursor after \"Hello\"");
        if (active)
        {
            type(RAW_W, IEQUALIFIER_CONTROL);
            check(buffer[0] == 0 && si.BufferPos == 0 && si.NumChars == 0 && intact(buffer, 6),
                  "Ctrl-W typed there empties the buffer");
            if (si.BufferPos >= 0)
            {
                type(RAW_A, 0);
                type(RAW_B, 0);
            }
            check(strcmp((char *)buffer, "ab") == 0 && si.BufferPos == 2 && si.NumChars == 2,
                  "a and b typed after it");
            type(RAW_RETURN, 0);
            check(await(win, IDCMP_GADGETUP) && strcmp((char *)buffer, "ab") == 0,
                  "Return ends the gadget with \"ab\" (IDCMP_GADGETUP)");
        }

        /* As a program can set it */
        si.BufferPos = -3;
        RefreshGList(&gad, win, NULL, 1);
        check(si.BufferPos == 0, "a cursor that the program puts before the buffer is at its start after a refresh");
        if (si.BufferPos >= 0 && ActivateGadget(&gad, win, NULL))
        {
            type(RAW_C, 0);
            check(strcmp((char *)buffer, "cab") == 0 && si.BufferPos == 1,
                  "c typed then goes in at the start");
            type(RAW_RETURN, 0);
            await(win, IDCMP_GADGETUP);
        }
        else
            check(0, "c typed then goes in at the start (not typed: no cursor in the buffer, or no active gadget)");
        CloseWindow(win);
    }
    check(intact(buffer, 6) && intact(undo, 6), "no guard byte of the buffer or the undo buffer touched");

    closeinput();
}

#define LONGTEXT "ABCDEFGHIJKLMNOPQRST" /* 20 characters */
#define SMALL 8                         /* MaxChars, and the size of undo and work buffers */
#define LARGE 64                        /* the program's buffer */
#define HUGELEN 32768                   /* a text longer than a WORD counts */
#define RAW_X 0x32
#define RAW_BACKSPACE 0x41

/* The first MaxChars - 1 characters of LONGTEXT and a NUL */
static int cut(const UBYTE *buffer)
{
    return memcmp(buffer, LONGTEXT, SMALL - 1) == 0 && buffer[SMALL - 1] == 0;
}

/* Nothing written into a buffer that guarded() filled */
static int untouched(const UBYTE *buffer, ULONG size)
{
    ULONG i;

    for (i = 0; i < size; i++)
        if (buffer[i] != GUARDBYTE)
            return 0;
    return intact(buffer, size);
}

/* What an edit hook of the program saw, and whether it rejects the key */
static struct
{
    STRPTR workbuffer, prevbuffer;
    WORD numchars, bufferpos;
    ULONG command;
    int calls, reject;
} seen;

AROS_UFH3(ULONG, recordedit,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(struct SGWork *, sgw, A2),
          AROS_UFHA(ULONG *, command, A1))
{
    AROS_USERFUNC_INIT

    seen.calls++;
    seen.workbuffer = sgw->WorkBuffer;
    seen.prevbuffer = sgw->PrevBuffer;
    seen.numchars = sgw->NumChars;
    seen.bufferpos = sgw->BufferPos;
    seen.command = *command;
    if (seen.reject)
        sgw->Actions &= ~SGA_USE;
    return 1;

    AROS_USERFUNC_EXIT
}

/* A window on a screen, the default public screen for NULL */
static struct Window *openon(struct Screen *scr, struct Gadget *gad)
{
    struct Window *win;

    win = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 260, WA_Height, 80,
                         WA_Title, (IPTR)"strgeditkeys", WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                         WA_Activate, TRUE, WA_IDCMP, IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP,
                         scr ? WA_CustomScreen : TAG_IGNORE, (IPTR)scr,
                         gad ? WA_Gadgets : TAG_IGNORE, (IPTR)gad, TAG_DONE);
    if (win && !(win->Flags & WFLG_WINDOWACTIVE))
        await(win, IDCMP_ACTIVEWINDOW);
    return win;
}

static struct Window *openwith(struct Gadget *gad)
{
    return openon(NULL, gad);
}

static void plaingadget(struct Gadget *gad, struct StringInfo *si, UBYTE *buffer, UBYTE *undo)
{
    memset(si, 0, sizeof(*si));
    si->Buffer = buffer;
    si->UndoBuffer = undo;
    si->MaxChars = SMALL;
    memset(gad, 0, sizeof(*gad));
    gad->LeftEdge = 20;
    gad->TopEdge = 40;
    gad->Width = 120;
    gad->Height = 12;
    gad->Flags = GFLG_GADGHCOMP;
    gad->Activation = GACT_RELVERIFY;
    gad->GadgetType = GTYP_STRGADGET;
    gad->SpecialInfo = si;
}

/* Cursor right, Shift-cursor right, x, Backspace and Return on LONGTEXT at
   cursor pos: all of it is edited, as on AmigaOS */
static void longkeys(struct Window *win, struct StringInfo *si, UBYTE *buffer, WORD pos, const char *what)
{
    type(CURSORRIGHT, 0);
    checkf(si->BufferPos == pos + 1 && si->NumChars == 20 && strcmp((char *)buffer, LONGTEXT) == 0,
           "%s, cursor right: cursor %d of 20", what, pos + 1);
    type(CURSORRIGHT, IEQUALIFIER_LSHIFT);
    checkf(si->BufferPos == 20, "%s, Shift-cursor right: cursor at the end, 20", what);
    type(RAW_X, 0);
    checkf(si->NumChars == 20 && strcmp((char *)buffer, LONGTEXT) == 0, "%s, x typed: no room, the text unchanged",
           what);
    type(RAW_BACKSPACE, 0);
    checkf(si->NumChars == 19 && si->BufferPos == 19 && strcmp((char *)buffer, "ABCDEFGHIJKLMNOPQRS") == 0,
           "%s, Backspace: \"ABCDEFGHIJKLMNOPQRS\"", what);
    type(RAW_RETURN, 0);
    checkf(await(win, IDCMP_GADGETUP), "%s, Return ends the gadget (IDCMP_GADGETUP)", what);
}

/* A text longer than MaxChars - 1, which the program put into a larger
   buffer: the undo buffer of MaxChars bytes takes no more than MaxChars - 1
   characters and a NUL, and the text is edited whole, in a copy of its own
   length instead of the work buffer of MaxChars bytes */
static void copies(void)
{
    static UBYTE area[LARGE + 2 * GUARD], undoarea[SMALL + 2 * GUARD], workarea[SMALL + 2 * GUARD];
    static UBYTE hugearea[HUGELEN + 256 + HUGELEN + 1 + GUARD];
    struct StringInfo si, *gsi;
    struct StringExtend sx;
    struct Hook hook;
    struct Gadget gad, *obj;
    struct Window *win;
    UBYTE *buffer, *undo, *work, *huge;
    ULONG i;
    int before;

    check(maps(RAW_X, 0, 'x') && maps(RAW_BACKSPACE, 0, 8),
          "the keymap gives x and Backspace for the raw keys of the copies");
    if (!openinput())
        return;

    /* A plain gadget */
    buffer = guarded(area, LARGE);
    undo = guarded(undoarea, SMALL);
    strcpy((char *)buffer, LONGTEXT);
    plaingadget(&gad, &si, buffer, undo);
    win = openwith(&gad);
    check(win && si.NumChars == 20 && strcmp((char *)buffer, LONGTEXT) == 0,
          "a gadget of MaxChars 8 with a text of 20 in a buffer of 64: NumChars 20, all of the text");
    if (win)
    {
        check(ActivateGadget(&gad, win, NULL) && cut(undo) && intact(undo, SMALL),
              "ActivateGadget(): the undo buffer of 8 bytes gets \"ABCDEFG\" and a NUL");
        longkeys(win, &si, buffer, 0, "plain");

        /* Activated by a click this time */
        strcpy((char *)buffer, LONGTEXT);
        undo = guarded(undoarea, SMALL);
        si.BufferPos = 0;
        RefreshGList(&gad, win, NULL, 1);
        click(win, &gad, 4);
        check((gad.Flags & GFLG_SELECTED) && cut(undo) && intact(undo, SMALL),
              "a click into the gadget: the undo buffer gets \"ABCDEFG\" and a NUL");
        type(RAW_RETURN, 0);
        await(win, IDCMP_GADGETUP);
        CloseWindow(win);
    }
    check(intact(buffer, LARGE) && intact(undo, SMALL), "plain: no guard byte of the buffer or the undo buffer touched");

    /* With a work buffer of MaxChars bytes */
    buffer = guarded(area, LARGE);
    undo = guarded(undoarea, SMALL);
    work = guarded(workarea, SMALL);
    strcpy((char *)buffer, LONGTEXT);
    plaingadget(&gad, &si, buffer, undo);
    memset(&sx, 0, sizeof(sx));
    sx.Pens[0] = sx.ActivePens[0] = 1;
    sx.WorkBuffer = work;
    si.Extension = &sx;
    si.BufferPos = 15;
    gad.Flags |= GFLG_STRINGEXTEND;
    win = openwith(&gad);
    check(win != NULL, "a gadget with a work buffer of 8 bytes and the same text, cursor 15");
    if (win)
    {
        check(ActivateGadget(&gad, win, NULL) && cut(undo) && intact(undo, SMALL),
              "work buffer, ActivateGadget(): the undo buffer gets \"ABCDEFG\" and a NUL");
        type(CURSORRIGHT, 0);
        check(si.BufferPos == 16 && si.NumChars == 20 && strcmp((char *)buffer, LONGTEXT) == 0 &&
              untouched(work, SMALL),
              "work buffer, cursor right: cursor 16 of 20, the work buffer untouched");
        type(CURSORLEFT, 0);
        longkeys(win, &si, buffer, 15, "work buffer");
        check(untouched(work, SMALL), "work buffer: the editing of that text leaves the work buffer untouched");

        /* A text that fits still goes through the work buffer */
        strcpy((char *)buffer, "Hello");
        si.BufferPos = 5;
        RefreshGList(&gad, win, NULL, 1);
        if (ActivateGadget(&gad, win, NULL))
            type(RAW_X, 0);
        check(strcmp((char *)buffer, "Hellox") == 0 && strcmp((char *)work, "Hellox") == 0 && si.NumChars == 6,
              "work buffer, \"Hello\" and x typed: \"Hellox\", through the work buffer");
        type(RAW_RETURN, 0);
        await(win, IDCMP_GADGETUP);

        /* An edit hook of the program, as in the RKRM, which rejects a key
           by clearing SGA_USE */
        memset(&hook, 0, sizeof(hook));
        hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(recordedit);
        sx.EditHook = &hook;
        strcpy((char *)buffer, LONGTEXT);
        work = guarded(workarea, SMALL);
        si.BufferPos = 20;
        RefreshGList(&gad, win, NULL, 1);
        memset(&seen, 0, sizeof(seen));
        seen.reject = 1;
        if (ActivateGadget(&gad, win, NULL))
            type(RAW_BACKSPACE, 0);
        check(seen.calls == 1 && seen.numchars == 19 && seen.workbuffer != buffer && seen.workbuffer != work &&
              seen.prevbuffer == buffer && strcmp((char *)buffer, LONGTEXT) == 0 && si.NumChars == 20,
              "an edit hook rejecting Backspace on that text: it saw the 19 characters in a copy of the "
              "text's length, the buffer as PrevBuffer, and the text stays whole");
        seen.reject = 0;
        type(RAW_BACKSPACE, 0);
        check(seen.calls == 2 && strcmp((char *)buffer, "ABCDEFGHIJKLMNOPQRS") == 0 && si.NumChars == 19 &&
              untouched(work, SMALL),
              "the same hook accepting Backspace: \"ABCDEFGHIJKLMNOPQRS\", the work buffer untouched");
        type(RAW_RETURN, 0);
        await(win, IDCMP_GADGETUP);
        sx.EditHook = NULL;
        CloseWindow(win);
    }
    check(intact(buffer, LARGE) && intact(undo, SMALL) && intact(work, SMALL),
          "work buffer: no guard byte of the buffer, the undo buffer or the work buffer touched");

    /* strgclass, with all the buffers of the program */
    buffer = guarded(area, LARGE);
    undo = guarded(undoarea, SMALL);
    work = guarded(workarea, SMALL);
    buffer[0] = 0;
    obj = (struct Gadget *)NewObject(NULL, STRGCLASS, GA_Left, 20, GA_Top, 40, GA_Width, 120, GA_Height, 12,
                                     GA_RelVerify, TRUE, STRINGA_MaxChars, SMALL, STRINGA_Buffer, (IPTR)buffer,
                                     STRINGA_UndoBuffer, (IPTR)undo, STRINGA_WorkBuffer, (IPTR)work, TAG_DONE);
    win = obj ? openwith(NULL) : NULL;
    check(win != NULL, "a strgclass gadget of MaxChars 8 with the buffer, undo and work buffer of the program");
    if (win)
    {
        gsi = (struct StringInfo *)obj->SpecialInfo;
        strcpy((char *)buffer, LONGTEXT);
        AddGList(win, obj, -1, 1, NULL);
        RefreshGList(obj, win, NULL, 1);
        check(ActivateGadget(obj, win, NULL) && cut(undo) && intact(undo, SMALL),
              "strgclass, ActivateGadget(): the undo buffer gets \"ABCDEFG\" and a NUL");
        type(CURSORRIGHT, 0);
        check(gsi->BufferPos == 1 && gsi->NumChars == 20 && strcmp((char *)buffer, LONGTEXT) == 0 &&
              untouched(work, SMALL),
              "strgclass, cursor right: cursor 1 of 20, the work buffer untouched");
        type(RAW_RETURN, 0);
        check(await(win, IDCMP_GADGETUP), "strgclass, Return ends the gadget (IDCMP_GADGETUP)");
        RemoveGList(win, obj, 1);
        CloseWindow(win);
    }
    if (obj)
        DisposeObject((Object *)obj);
    check(intact(buffer, LARGE) && intact(undo, SMALL) && intact(work, SMALL),
          "strgclass: no guard byte of the buffer, the undo buffer or the work buffer touched");

    /* More characters than a WORD counts, with room before the buffer for
       a NUL at a negative NumChars */
    memset(hugearea, GUARDBYTE, sizeof(hugearea));
    huge = hugearea + HUGELEN + 256;
    memset(huge, 'a', HUGELEN);
    huge[HUGELEN] = 0;
    plaingadget(&gad, &si, huge, NULL);
    win = openwith(&gad);
    for (before = 1, i = 0; i < HUGELEN + 256; i++)
        if (hugearea[i] != GUARDBYTE)
            before = 0;
    check(win && si.NumChars == HUGELEN - 2 && huge[HUGELEN - 2] == 0 && huge[HUGELEN - 3] == 'a' && before &&
          intact(huge, HUGELEN + 1),
          "a text of 32768 characters: NumChars 32766, where the render puts its NUL, and nothing before the buffer");
    if (win)
        CloseWindow(win);

    closeinput();
}

#define SCROLLTEXT "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd" /* 40 characters */

/* Activated with the cursor at the end of SCROLLTEXT, which the gadget then
   shows from its character DispPos, and ended with Return */
static WORD scrolled(struct Window *win, struct Gadget *gad, struct StringInfo *si, UBYTE *buffer)
{
    WORD disppos = -1;

    strcpy((char *)buffer, SCROLLTEXT);
    si->BufferPos = 40;
    if (ActivateGadget(gad, win, NULL))
    {
        disppos = si->DispPos;
        type(RAW_RETURN, 0);
        if (!await(win, IDCMP_GADGETUP))
            disppos = -1;
    }
    return disppos;
}

/* A click measures the text from DispPos, the first character that the
   last render showed. Here the program shortens the text of a scrolled
   gadget without a refresh, so DispPos lies past the new text, among the
   rest of the old one that the buffer still holds. In topaz 8 a click x
   pixels into the gadget is over character x / 8 of what it shows, and
   graphics.library measures a text of that font without reading it, so a
   measure from past the text reads no further than the click. An edit hook
   of the program sees the cursor that Intuition's click gives. */
static void stale(void)
{
    static UBYTE area[LARGE + 2 * GUARD], undoarea[LARGE + 2 * GUARD];
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, 0, 0 };
    struct TextFont *font;
    struct StringInfo si;
    struct StringExtend sx;
    struct Hook hook;
    struct Gadget gad;
    struct Window *win;
    UBYTE *buffer = guarded(area, LARGE), *undo = guarded(undoarea, LARGE);
    WORD disppos;

    font = OpenFont(&ta);
    check(font && font->tf_XSize == 8 && !(font->tf_Flags & FPF_PROPORTIONAL) && !font->tf_CharKern &&
          !font->tf_CharSpace,
          "topaz 8, of fixed width, for a scrolled gadget");
    if (!font || font->tf_XSize != 8 || (font->tf_Flags & FPF_PROPORTIONAL) || font->tf_CharKern ||
        font->tf_CharSpace)
    {
        if (font)
            CloseFont(font);
        return;
    }
    if (!openinput())
    {
        CloseFont(font);
        return;
    }

    buffer[0] = 0;
    plaingadget(&gad, &si, buffer, undo);
    si.MaxChars = LARGE;
    memset(&sx, 0, sizeof(sx));
    sx.Font = font;
    sx.Pens[0] = sx.ActivePens[0] = 1;
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(recordedit);
    sx.EditHook = &hook;
    si.Extension = &sx;
    gad.Flags |= GFLG_STRINGEXTEND;
    win = openwith(&gad);
    check(win != NULL, "a gadget of 120 pixels in topaz 8, MaxChars 64, with an edit hook of the program");
    if (win)
    {
        /* A text shorter than DispPos */
        disppos = scrolled(win, &gad, &si, buffer);
        checkf(disppos == 26, "the cursor at the end of a text of 40: the gadget shows it from character 26 (%d)",
               disppos);
        strcpy((char *)buffer, "abc");
        memset(&seen, 0, sizeof(seen));
        click(win, &gad, 100);
        checkf(seen.calls == 1 && seen.command == SGH_CLICK && seen.bufferpos == 3 && seen.numchars == 3,
               "the program puts \"abc\" there without a refresh, then a click 100 pixels in: the edit hook "
               "gets the cursor at the end of \"abc\", 3 (got %d of %d)", seen.bufferpos, seen.numchars);
        check((gad.Flags & GFLG_SELECTED) && si.BufferPos == 3 && si.NumChars == 3 && si.DispPos == 0 &&
              strcmp((char *)buffer, "abc") == 0 && memcmp(buffer + 4, SCROLLTEXT + 4, 37) == 0,
              "the gadget active then, cursor 3, shown from its start, and the old text after \"abc\" untouched");
        type(RAW_RETURN, 0);
        await(win, IDCMP_GADGETUP);

        /* A text still longer than DispPos, shorter than what the last
           render showed */
        disppos = scrolled(win, &gad, &si, buffer);
        buffer[30] = 0;
        memset(&seen, 0, sizeof(seen));
        click(win, &gad, 100);
        checkf(disppos == 26 && seen.calls == 1 && seen.bufferpos == 30 && si.BufferPos == 30,
               "a text cut to 30 characters the same way, a click right of it: the edit hook gets the cursor "
               "at its end, 30 (got %d)", seen.bufferpos);
        type(RAW_RETURN, 0);
        await(win, IDCMP_GADGETUP);

        /* As a program can set it */
        strcpy((char *)buffer, "abc");
        si.BufferPos = 0;
        si.DispPos = -5;
        RefreshGList(&gad, win, NULL, 1);
        checkf(si.DispPos == 0 && si.DispCount == 3,
               "a DispPos that the program puts before the buffer is at its start after a refresh (DispPos %d, "
               "DispCount %d)", si.DispPos, si.DispCount);
        CloseWindow(win);
    }
    check(intact(buffer, LARGE) && intact(undo, LARGE), "scrolled: no guard byte of the buffer or the undo buffer touched");

    closeinput();
    CloseFont(font);
}

#define CLICKTEXT "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123" /* 30 characters */
#define CLICKX 98

/* topaz 8's glyphs in wider cells, a font that the test adds to the
   system's list for its screen */
static struct TextFont wide;

/* The characters of CLICKTEXT that a font fits into width pixels */
static WORD fit(struct TextFont *font, WORD width)
{
    struct RastPort rp;
    struct TextExtent te;

    memset(&rp, 0, sizeof(rp));
    SetFont(&rp, font);
    return TextFit(&rp, (STRPTR)CLICKTEXT, strlen(CLICKTEXT), &te, NULL, 1, width, font->tf_YSize);
}

/* A gadget of 120 pixels on scr holding CLICKTEXT, a StringExtend for sx:
   how many characters it shows, and where a click CLICKX pixels in puts
   the cursor */
static void clickon(struct Screen *scr, UBYTE *buffer, struct StringExtend *sx, const char *what, const char *by,
                    WORD shown, WORD cursor)
{
    struct StringInfo si;
    struct Gadget gad;
    struct Window *win;

    strcpy((char *)buffer, CLICKTEXT);
    plaingadget(&gad, &si, buffer, NULL);
    si.MaxChars = LARGE;
    if (sx)
    {
        si.Extension = sx;
        gad.Flags |= GFLG_STRINGEXTEND;
    }
    win = openon(scr, &gad);
    checkf(win != NULL, "%s there, 120 pixels wide, with a text of 30 characters", what);
    if (!win)
        return;
    checkf(si.DispPos == 0 && si.DispCount == shown, "it shows %d characters, by %s (DispCount %d)", shown, by,
           si.DispCount);
    click(win, &gad, CLICKX);
    checkf((gad.Flags & GFLG_SELECTED) && si.BufferPos == cursor,
           "a click %d pixels into it puts the cursor on character %d, by %s too (got %d)", CLICKX, cursor, by,
           si.BufferPos);
    type(RAW_RETURN, 0);
    await(win, IDCMP_GADGETUP);
    CloseWindow(win);
}

/* A plain string gadget is drawn in the screen's font, as on AmigaOS, and a
   click into it measures the text with that font: neither the default font
   of graphics.library nor the font of the window's RastPort plays a part.
   Here the screen's font is made from topaz 8's glyphs in cells of 12
   pixels, so in 120 pixels the gadget shows 10 characters of a longer text
   and a click CLICKX pixels in lands on character 8, where the default font
   shows and gives others; in cells of 14 where it would show and give the
   same. A gadget with a StringExtend is drawn in its own font, else in the
   screen's too. */
static void clickfont(void)
{
    static UBYTE area[LARGE + 2 * GUARD];
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, 0, 0 };
    struct TextAttr wideta = { (STRPTR)"strgeditkeys.font", 8, 0, 0 };
    struct TextFont *topaz, *deffont = GfxBase->DefaultFont;
    struct StringExtend sx;
    struct Screen *scr;
    UBYTE *buffer = guarded(area, LARGE);
    WORD defshown, defcursor, shown, cursor;

    topaz = OpenFont(&ta);
    check(topaz && topaz->tf_XSize == 8 && !(topaz->tf_Flags & FPF_PROPORTIONAL),
          "topaz 8, of fixed width, for the glyphs of a wider font");
    if (!topaz || topaz->tf_XSize != 8 || (topaz->tf_Flags & FPF_PROPORTIONAL))
    {
        if (topaz)
            CloseFont(topaz);
        return;
    }
    memset(&wide, 0, sizeof(wide));
    wide.tf_Message.mn_Node.ln_Name = (char *)wideta.ta_Name;
    wide.tf_YSize = topaz->tf_YSize;
    wide.tf_Flags = FPF_DESIGNED;
    wide.tf_XSize = 12;
    wide.tf_Baseline = topaz->tf_Baseline;
    wide.tf_BoldSmear = topaz->tf_BoldSmear;
    wide.tf_LoChar = topaz->tf_LoChar;
    wide.tf_HiChar = topaz->tf_HiChar;
    wide.tf_CharData = topaz->tf_CharData;
    wide.tf_Modulo = topaz->tf_Modulo;
    wide.tf_CharLoc = topaz->tf_CharLoc;
    defshown = fit(deffont, 120);
    defcursor = fit(deffont, CLICKX);
    if (fit(&wide, 120) == defshown && fit(&wide, CLICKX) == defcursor)
        wide.tf_XSize = 14;
    shown = fit(&wide, 120);
    cursor = fit(&wide, CLICKX);
    AddFont(&wide);
    checkf((shown != defshown || cursor != defcursor) && fit(topaz, 120) == 15 && fit(topaz, CLICKX) == 12,
           "a font of %d pixels a character fits %d characters into 120 pixels, a click %d pixels in over "
           "character %d; topaz 8 fits 15, the click over character 12; the default font, %s %d, fits %d, "
           "the click over character %d", wide.tf_XSize, shown, CLICKX, cursor, deffont->tf_Message.mn_Node.ln_Name,
           deffont->tf_YSize, defshown, defcursor);

    scr = OpenScreenTags(NULL, SA_LikeWorkbench, TRUE, SA_Font, (IPTR)&wideta, SA_Title, (IPTR)"strgeditkeys",
                         TAG_DONE);
    checkf(scr && scr->RastPort.Font == &wide, "a screen in the font of %d pixels a character", wide.tf_XSize);
    if (scr && openinput())
    {
        clickon(scr, buffer, NULL, "a plain gadget", "the screen's font", shown, cursor);
        memset(&sx, 0, sizeof(sx));
        sx.Pens[0] = sx.ActivePens[0] = 1;
        clickon(scr, buffer, &sx, "a gadget with a StringExtend and no font of its own", "the screen's font", shown,
                cursor);
        sx.Font = topaz;
        clickon(scr, buffer, &sx, "a gadget with a StringExtend in topaz 8", "its own font", 15, 12);
        closeinput();
    }
    if (scr)
        CloseScreen(scr);
    if (!(wide.tf_Flags & FPF_REMOVED))
        RemFont(&wide);
    check(intact(buffer, LARGE), "clicks: no guard byte of the buffer touched");
    CloseFont(topaz);
}

#define ALIGNTEXT "ABCDEFGHIJKLMNOPQRST" /* 20 characters, 160 pixels in topaz 8 */
#define ALIGNWIDTH 68                     /* 8 characters of topaz 8 and 4 pixels */

static struct TextFont *aligntopaz;

/* The first column from x0 to x1 of the rows top..top + height - 1 of a
   window that holds another pen than back, -1 for none */
static WORD inkfrom(struct Window *win, WORD x0, WORD x1, WORD top, WORD height, LONG back)
{
    WORD x, y;

    for (x = x0; x <= x1; x++)
        for (y = top; y < top + height; y++)
            if ((LONG)ReadPixel(win->RPort, x, y) != back)
                return x;
    return -1;
}

/* A centred or right aligned gadget of ALIGNWIDTH pixels on scr, in topaz 8,
   holding text with the cursor at pos, with a StringExtend for sx: its
   text must start start pixels into the box, where AmigaOS 3.1 and 3.2.3
   draw it, with nothing drawn left of the box, and a click x pixels into
   the box must put the cursor on character cursor, the one drawn under the
   pointer. Where the text starts is told by the first column with ink in
   the gadget's rows, against that of the first character shown drawn
   alone right of the gadget. When again is not negative, a second click
   again pixels into the active gadget must put the cursor on character
   cursor2. */
static void alignon(struct Screen *scr, UBYTE *buffer, struct StringExtend *sx, UWORD align, const char *text,
                    WORD pos, const char *what, WORD start, WORD x, WORD cursor, WORD again, WORD cursor2)
{
    struct StringInfo si;
    struct Gadget gad;
    struct Window *win;
    struct RastPort *rp;
    WORD ink, glyph, reference, top;
    LONG back;

    strcpy((char *)buffer, text);
    plaingadget(&gad, &si, buffer, NULL);
    si.MaxChars = LARGE;
    si.BufferPos = pos;
    gad.Width = ALIGNWIDTH;
    gad.Activation |= align;
    if (sx)
    {
        si.Extension = sx;
        gad.Flags |= GFLG_STRINGEXTEND;
    }
    win = openon(scr, &gad);
    checkf(win != NULL, "%s, %d pixels wide, holding %d characters, cursor %d", what, ALIGNWIDTH,
           (int)strlen(text), pos);
    if (!win)
        return;
    rp = win->RPort;
    back = ReadPixel(rp, gad.LeftEdge, gad.TopEdge);
    reference = gad.LeftEdge + gad.Width + 20;
    top = gad.TopEdge + (gad.Height - aligntopaz->tf_YSize) / 2;
    SetFont(rp, aligntopaz);
    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, reference, top + aligntopaz->tf_Baseline);
    Text(rp, (STRPTR)buffer + si.DispPos, 1);
    glyph = inkfrom(win, reference, reference + aligntopaz->tf_XSize - 1, gad.TopEdge, gad.Height, back);
    ink = inkfrom(win, win->BorderLeft, gad.LeftEdge + gad.Width - 1, gad.TopEdge, gad.Height, back);
    checkf(glyph >= 0 && ink == gad.LeftEdge + start + glyph - reference,
           "it shows its text from character %d, %d pixels into the box, nothing left of it (first ink %d pixels "
           "into the box, %d expected)", si.DispPos, start, ink >= 0 ? ink - gad.LeftEdge : ink,
           start + glyph - reference);
    click(win, &gad, x);
    checkf((gad.Flags & GFLG_SELECTED) && si.BufferPos == cursor,
           "a click %d pixels into it puts the cursor on character %d, drawn there (got %d)", x, cursor,
           si.BufferPos);
    if (again >= 0)
    {
        click(win, &gad, again);
        checkf((gad.Flags & GFLG_SELECTED) && si.BufferPos == cursor2,
               "then a click %d pixels into the active gadget puts it on character %d (got %d)", again, cursor2,
               si.BufferPos);
    }
    type(RAW_RETURN, 0);
    await(win, IDCMP_GADGETUP);
    CloseWindow(win);
}

/* Centred and right aligned gadgets on a screen in topaz 8. AmigaOS aligns
   what such a gadget draws, the characters from DispPos that fit into the
   box and the cursor after them at the end of the text: a gadget with a
   StringExtend in its box, a plain one in the whole characters that fit
   into it (64 of the 68 pixels here), right aligned text up to the right
   edge. A text wider than the box thus starts at the left of the box in a
   plain gadget, 2 pixels into it when centred with a StringExtend, 4 when
   right aligned; and a click lands on the character drawn under the
   pointer, a character of topaz 8 being 8 pixels wide. */
static void aligned(void)
{
    static UBYTE area[LARGE + 2 * GUARD];
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, 0, 0 };
    struct StringExtend sx;
    struct Screen *scr;
    UBYTE *buffer = guarded(area, LARGE);

    aligntopaz = OpenFont(&ta);
    check(aligntopaz && aligntopaz->tf_XSize == 8 && !(aligntopaz->tf_Flags & FPF_PROPORTIONAL),
          "topaz 8, of fixed width, for centred and right aligned gadgets");
    if (!aligntopaz || aligntopaz->tf_XSize != 8 || (aligntopaz->tf_Flags & FPF_PROPORTIONAL))
    {
        if (aligntopaz)
            CloseFont(aligntopaz);
        return;
    }
    scr = OpenScreenTags(NULL, SA_LikeWorkbench, TRUE, SA_Font, (IPTR)&ta, SA_Title, (IPTR)"strgeditkeys",
                         TAG_DONE);
    checkf(scr && scr->RastPort.Font && scr->RastPort.Font->tf_XSize == 8 &&
           !(scr->RastPort.Font->tf_Flags & FPF_PROPORTIONAL), "a screen in topaz 8");
    if (scr && openinput())
    {
        memset(&sx, 0, sizeof(sx));
        sx.Font = aligntopaz;
        sx.Pens[0] = sx.ActivePens[0] = 1;

        /* A text of 160 pixels, cursor at its start: 8 characters shown */
        alignon(scr, buffer, NULL, GACT_STRINGCENTER, ALIGNTEXT, 0, "a plain centred gadget", 0, 17, 2, 41, 5);
        alignon(scr, buffer, NULL, GACT_STRINGRIGHT, ALIGNTEXT, 0, "a plain right aligned gadget", 0, 17, 2, -1, 0);
        alignon(scr, buffer, &sx, GACT_STRINGCENTER, ALIGNTEXT, 0, "a centred gadget with a StringExtend", 2, 17,
                1, 41, 4);
        alignon(scr, buffer, &sx, GACT_STRINGRIGHT, ALIGNTEXT, 0, "a right aligned gadget with a StringExtend", 4,
                17, 1, -1, 0);

        /* Cursor in the middle, shown from character 3; at the end, from
           character 13, 7 characters and the cursor */
        alignon(scr, buffer, NULL, GACT_STRINGCENTER, ALIGNTEXT, 10, "a plain centred gadget", 0, 17, 5, -1, 0);
        alignon(scr, buffer, &sx, GACT_STRINGRIGHT, ALIGNTEXT, 10, "a right aligned gadget with a StringExtend", 4,
                17, 4, -1, 0);
        alignon(scr, buffer, NULL, GACT_STRINGCENTER, ALIGNTEXT, 20, "a plain centred gadget", 0, 17, 15, -1, 0);
        alignon(scr, buffer, &sx, GACT_STRINGCENTER, ALIGNTEXT, 20, "a centred gadget with a StringExtend", 2, 17,
                14, -1, 0);

        /* A text of 24 pixels */
        alignon(scr, buffer, NULL, GACT_STRINGCENTER, "ABC", 0, "a plain centred gadget", 20, 28, 1, -1, 0);
        alignon(scr, buffer, NULL, GACT_STRINGRIGHT, "ABC", 0, "a plain right aligned gadget", 40, 48, 1, -1, 0);
        alignon(scr, buffer, &sx, GACT_STRINGRIGHT, "ABC", 0, "a right aligned gadget with a StringExtend", 44, 51,
                0, -1, 0);
        alignon(scr, buffer, NULL, GACT_STRINGCENTER, "ABC", 3, "a plain centred gadget", 16, 24, 1, -1, 0);
        closeinput();
    }
    if (scr)
        CloseScreen(scr);
    check(intact(buffer, LARGE), "aligned: no guard byte of the buffer touched");
    CloseFont(aligntopaz);
}

int main(void)
{
    WORD pos, numchars;
    static UBYTE area[ROOM + 2 * GUARD];
    UBYTE *buffer;
    int k;

    makekeys();
    edithook = current_edithook();
    check(edithook != NULL, "the editing hook from SetEditHook()");
    if (!edithook)
        return 20;

    /* Ctrl-A moves the cursor only when filtering is on */
    buffer = guarded(area, ROOM);
    strcpy((char *)buffer, "abc");
    pos = 3;
    numchars = 3;
    press(ctrl('a'), buffer, ROOM, &pos, &numchars, 0, 0);
    check(pos == 0, "control keys filtered (IControl \"Text Gadget Filter\")");

    edit("Ctrl-W at the end of \"Hello\", MaxChars 6, empties the buffer", ctrl('w'), "Hello", 5, 6, 0,
         "", 0);
    edit("Ctrl-W after \"He\" in \"Hello\" leaves \"llo\"", ctrl('w'), "Hello", 2, 6, 0, "llo", 0);
    edit("Ctrl-W after \"H\" in \"Hello\" leaves \"ello\"", ctrl('w'), "Hello", 1, 6, 0, "ello", 0);
    edit("Ctrl-W at the end of \"abc def\" leaves \"abc\"", ctrl('w'), "abc def", 7, 8, 0, "abc", 3);
    edit("Ctrl-W after \"abc d\" in \"abc def\" leaves \"abcef\"", ctrl('w'), "abc def", 5, 8, 0, "abcef", 3);
    edit("Ctrl-W at the end of \" abc\" empties the buffer", ctrl('w'), " abc", 4, 8, 0, "", 0);
    edit("Ctrl-W at the end of \"abc \" leaves \"abc\"", ctrl('w'), "abc ", 4, 8, 0, "abc", 3);
    edit("Ctrl-W at the start changes nothing", ctrl('w'), "Hello", 0, 6, 0, "Hello", 0);
    edit("Ctrl-W in fixed field mode changes nothing", ctrl('w'), "Hello", 5, 6, SGM_FIXEDFIELD, "Hello",
         5);
    edit("Ctrl-H after \"H\" in \"Hello w\" leaves \"ello w\"", ctrl('h'), "Hello w", 1, 8, 0, "ello w", 0);
    edit("Ctrl-U after \"He\" in \"Hello w\" leaves \"llo w\"", ctrl('u'), "Hello w", 2, 8, 0, "llo w", 0);
    edit("Ctrl-K after \"He\" in \"Hello\" leaves \"He\"", ctrl('k'), "Hello", 2, 6, 0, "He", 2);
    edit("Ctrl-X empties the buffer", ctrl('x'), "Hello", 2, 6, 0, "", 0);

    for (k = 0; k < nkeys; k++)
        every(&keys[k]);

    typed();
    copies();
    stale();
    clickfont();
    aligned();

    printf("strgeditkeys: %d checks, %d failed\n", checks, failures);
    return failures ? 5 : 0;
}

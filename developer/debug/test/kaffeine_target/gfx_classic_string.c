/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Classic Intuition string and integer gadgets built in the program's own
    memory, without GadTools. Three GTYP_STRGADGET gadgets are linked in
    order and attached through WA_Gadgets to an active 320 by 160 window:
    a string gadget with "Hello" (MaxChars 32, BufferPos 5, GFLG_TABCYCLE),
    a GACT_LONGINT integer gadget with "42" (MaxChars 12, BufferPos 2,
    LongInt 42, GFLG_TABCYCLE, sharing the first gadget's UndoBuffer), and
    a GFLG_STRINGEXTEND string gadget with "Pens" (MaxChars 32) whose
    StringExtend uses topaz 8 from OpenFont(), Pens {1, 3}, ActivePens
    {2, 3}, InitialModes 0, EditHook NULL, and a 32-byte WorkBuffer.
*/

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

#define WIN_WIDTH  320
#define WIN_HEIGHT 160
#define WIN_FLAGS  (WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET)
#define WIN_IDCMP  (IDCMP_ACTIVEWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN)

#define STR_MAXCHARS 32
#define INT_MAXCHARS 12

static struct TextAttr topaz8 = {
    (STRPTR)"topaz.font", 8, FS_NORMAL, 0
};

struct ClassicStringSet
{
    struct Gadget       str_gad;
    struct StringInfo   str_info;
    char                str_buf[STR_MAXCHARS];
    char                undo_buf[STR_MAXCHARS];

    struct Gadget       int_gad;
    struct StringInfo   int_info;
    char                int_buf[INT_MAXCHARS];

    struct Gadget       ext_gad;
    struct StringInfo   ext_info;
    struct StringExtend str_ext;
    char                ext_buf[STR_MAXCHARS];
    char                work_buf[STR_MAXCHARS];
};

static struct ClassicStringSet g_set;

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
    if (!emu68_file(path, sizeof(path), second ? "-keys-2.txt" : "-keys-1.txt") ||
        !(file = Open((CONST_STRPTR)path, MODE_NEWFILE)))
        return FALSE;
    Write(file, (APTR)"ready\n", 6);
    Close(file);
    return TRUE;
}

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

/* What ReadPixel() gives for a pen in this window: on a screen of more
   than 256 colours ReadPixel() gives the pen found for a colour, which
   need not be the pen that drew it, so the checks compare a pixel with
   one that the program drew in that pen, low at the left of the window,
   where no gadget lies. */
static LONG pen_value(struct Window *win, UBYTE pen)
{
    struct RastPort *rp = win->RPort;
    LONG x = win->BorderLeft + 2, y = win->Height - win->BorderBottom - 3;

    SetAPen(rp, pen);
    WritePixel(rp, x, y);
    return ReadPixel(rp, x, y);
}

static void build_gadgets(struct ClassicStringSet *s, struct TextFont *font)
{
    memset(s, 0, sizeof(*s));

    /* 1. String gadget with "Hello", UndoBuffer, MaxChars 32, BufferPos 5 */
    strcpy(s->str_buf, "Hello");
    s->str_info.Buffer     = (STRPTR)s->str_buf;
    s->str_info.UndoBuffer = (STRPTR)s->undo_buf;
    s->str_info.MaxChars   = STR_MAXCHARS;
    s->str_info.BufferPos  = 5;

    s->str_gad.NextGadget  = &s->int_gad;
    s->str_gad.LeftEdge    = 20;
    s->str_gad.TopEdge     = 30;
    s->str_gad.Width       = 160;
    s->str_gad.Height      = 10;
    s->str_gad.Flags       = GFLG_GADGHCOMP | GFLG_TABCYCLE;
    s->str_gad.Activation  = GACT_RELVERIFY;
    s->str_gad.GadgetType  = GTYP_STRGADGET;
    s->str_gad.SpecialInfo = &s->str_info;
    s->str_gad.GadgetID    = 1;

    /* 2. Integer gadget with "42", shared UndoBuffer, MaxChars 12, BufferPos 2, LongInt 42 */
    strcpy(s->int_buf, "42");
    s->int_info.Buffer     = (STRPTR)s->int_buf;
    s->int_info.UndoBuffer = (STRPTR)s->undo_buf;
    s->int_info.MaxChars   = INT_MAXCHARS;
    s->int_info.BufferPos  = 2;
    s->int_info.LongInt    = 42;

    s->int_gad.NextGadget  = &s->ext_gad;
    s->int_gad.LeftEdge    = 20;
    s->int_gad.TopEdge     = 50;
    s->int_gad.Width       = 80;
    s->int_gad.Height      = 10;
    s->int_gad.Flags       = GFLG_GADGHCOMP | GFLG_TABCYCLE;
    s->int_gad.Activation  = GACT_RELVERIFY | GACT_LONGINT;
    s->int_gad.GadgetType  = GTYP_STRGADGET;
    s->int_gad.SpecialInfo = &s->int_info;
    s->int_gad.GadgetID    = 2;

    /* 3. Extended string gadget with "Pens", MaxChars 32, and StringExtend */
    strcpy(s->ext_buf, "Pens");
    s->str_ext.Font          = font;
    s->str_ext.Pens[0]       = 1;
    s->str_ext.Pens[1]       = 3;
    s->str_ext.ActivePens[0] = 2;
    s->str_ext.ActivePens[1] = 3;
    s->str_ext.InitialModes  = 0;
    s->str_ext.EditHook      = NULL;
    s->str_ext.WorkBuffer    = (STRPTR)s->work_buf;

    s->ext_info.Buffer    = (STRPTR)s->ext_buf;
    s->ext_info.MaxChars  = STR_MAXCHARS;
    s->ext_info.Extension = &s->str_ext;

    s->ext_gad.NextGadget  = NULL;
    s->ext_gad.LeftEdge    = 20;
    s->ext_gad.TopEdge     = 70;
    s->ext_gad.Width       = 160;
    s->ext_gad.Height      = 10;
    s->ext_gad.Flags       = GFLG_GADGHCOMP | GFLG_STRINGEXTEND;
    s->ext_gad.Activation  = GACT_RELVERIFY;
    s->ext_gad.GadgetType  = GTYP_STRGADGET;
    s->ext_gad.SpecialInfo = &s->ext_info;
    s->ext_gad.GadgetID    = 3;
}

static void check_initial(struct Window *win, struct ClassicStringSet *s)
{
    LONG bg_pen;

    check_long("first StringInfo NumChars after OpenWindowTags",
               s->str_info.NumChars, 5);
    check_long("integer StringInfo NumChars after OpenWindowTags",
               s->int_info.NumChars, 2);
    check_long("extended StringInfo NumChars after OpenWindowTags",
               s->ext_info.NumChars, 4);

    bg_pen = ReadPixel(win->RPort,
                       s->ext_gad.LeftEdge + s->ext_gad.Width - 8,
                       s->ext_gad.TopEdge + s->ext_gad.Height / 2);
    check_long("third gadget background pixel matches StringExtend pen 3",
               bg_pen, pen_value(win, s->str_ext.Pens[1]));
}

static BOOL check_activate_and_keys(struct Window *win, struct ClassicStringSet *s)
{
    struct IntuiMessage msg;

    if (!check(wait_for(win, IDCMP_ACTIVEWINDOW, 10 * 50, &msg) != NULL,
               "IDCMP_ACTIVEWINDOW comes within 10 s"))
        return FALSE;

    if (!check(ActivateGadget(&s->str_gad, win, NULL) != FALSE,
               "ActivateGadget of the first gadget gives TRUE"))
        return FALSE;

    check((s->str_gad.Activation & GACT_ACTIVEGADGET) != 0,
          "ActivateGadget sets GACT_ACTIVEGADGET in the first gadget");

    write_marker();

    if (!check(wait_for(win, IDCMP_GADGETUP, 60 * 50, &msg) != NULL,
               "first IDCMP_GADGETUP comes within 60 s"))
        return FALSE;

    check(msg.IAddress == (APTR)&s->str_gad,
          "first IDCMP_GADGETUP IAddress is the first gadget");
    check_str("first gadget Buffer after typing",
              (const char *)s->str_info.Buffer, "Hello12");
    check_long("first gadget NumChars after typing",
               s->str_info.NumChars, 7);
    check_long("first gadget BufferPos after typing",
               s->str_info.BufferPos, 7);

    if (!check(wait_for(win, IDCMP_GADGETUP, 60 * 50, &msg) != NULL,
               "second IDCMP_GADGETUP comes within 60 s"))
        return FALSE;

    check(msg.IAddress == (APTR)&s->int_gad,
          "second IDCMP_GADGETUP IAddress is the integer gadget");
    check_str("integer gadget Buffer after typing",
              (const char *)s->int_info.Buffer, "99");
    check_long("integer gadget NumChars after typing",
               s->int_info.NumChars, 2);
    check_long("integer gadget LongInt after typing",
               s->int_info.LongInt, 99);
    check_str("shared UndoBuffer holds integer gadget initial text",
              (const char *)s->undo_buf, "42");

    return TRUE;
}

static void check_refresh_and_relist(struct Window *win, struct ClassicStringSet *s)
{
    UWORD pos, add_pos;

    s->str_info.BufferPos = 2;
    RefreshGList(&s->str_gad, win, NULL, 1);
    check_long("first gadget BufferPos stays 2 after RefreshGList",
               s->str_info.BufferPos, 2);
    check_long("first gadget NumChars stays 7 after RefreshGList",
               s->str_info.NumChars, 7);

    pos = RemoveGList(win, &s->int_gad, 1);
    check(pos != 0xFFFF,
          "RemoveGList of integer gadget alone gives a position other than 0xFFFF");
    /* AmigaOS 3.2.3 and AROS leave the link of the last gadget taken
       off, where the NDK says that V36 cuts it */
    check(s->int_gad.NextGadget == &s->ext_gad,
          "RemoveGList of integer gadget leaves its NextGadget on the gadget after it");

    add_pos = AddGList(win, &s->int_gad, pos, 1, NULL);
    check_long("AddGList of integer gadget back gives the same position",
               (LONG)add_pos, (LONG)pos);

    RefreshGList(&s->int_gad, win, NULL, 1);
}

int main(void)
{
    struct TextFont *font = NULL;
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
        return test_done("gfx_classic_string");
    }

    /* Warm up Intuition window allocations before recording free memory. */
    win = OpenWindowTags(NULL, WA_Left, 20, WA_Top, 20, WA_Width, 100,
                         WA_Height, 40, TAG_DONE);
    if (win)
        CloseWindow(win);

    avail = AvailMem(MEMF_ANY);

    font = OpenFont(&topaz8);
    check(font != NULL, "OpenFont of topaz 8 succeeds");
    if (font)
    {
        build_gadgets(&g_set, font);

        win = OpenWindowTags(NULL,
                             WA_Left, 20,
                             WA_Top, 20,
                             WA_Width, WIN_WIDTH,
                             WA_Height, WIN_HEIGHT,
                             WA_Flags, WIN_FLAGS,
                             WA_IDCMP, WIN_IDCMP,
                             WA_Activate, TRUE,
                             WA_Gadgets, (ULONG)&g_set.str_gad,
                             WA_Title, (ULONG)"gfx_classic_string",
                             TAG_DONE);
        check(win != NULL, "OpenWindowTags with WA_Gadgets and WA_Activate succeeds");
        if (win)
        {
            check_initial(win, &g_set);
            if (check_activate_and_keys(win, &g_set))
                check_refresh_and_relist(win, &g_set);
            CloseWindow(win);
        }

        CloseFont(font);

        avail_after = AvailMem(MEMF_ANY);
        check_long("CloseWindow and CloseFont free all window memory",
                   (LONG)(avail - avail_after), 0);
    }

    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);

    return test_done("gfx_classic_string");
}

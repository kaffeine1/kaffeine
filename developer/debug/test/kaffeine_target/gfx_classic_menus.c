/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    A classic Intuition menu strip that the program builds in its own
    memory, without GadTools. The strip has two Menus, "Project" and "Edit":
    Project has "Open" (ITEMTEXT | COMMSEQ | ITEMENABLED | HIGHCOMP, Command
    'O'), "Check" (ITEMTEXT | CHECKIT | MENUTOGGLE | ITEMENABLED | HIGHCOMP)
    and "More" with a SubItem chain of two items; Edit has "A" and "B",
    CHECKIT items that exclude each other through MutualExclude, B with
    Command 'B'. The positions and sizes of the Menu and MenuItem boxes are
    set as a classic program sets them for topaz 8; the window's screen font
    may differ, so the geometry is kept simple. Each IntuiText uses FrontPen
    1 and DrawMode JAM1. On a window opened with WA_Activate and WA_IDCMP
    IDCMP_MENUPICK | IDCMP_ACTIVEWINDOW, with no input: SetMenuStrip() gives
    TRUE; ItemAddress() finds "Check" at FULLMENUNUM(0, 1, NOSUB) and the
    second subitem at FULLMENUNUM(0, 2, 1); OffMenu() and OnMenu() clear and
    set ITEMENABLED on "Open" and MENUENABLED on the whole Edit menu; after
    ClearMenuStrip() and setting CHECKED on "A" in the program's own item,
    ResetMenuStrip() gives TRUE and keeps "A" CHECKED. Then, after
    IDCMP_ACTIVEWINDOW and writing the marker file as gfx_gadget_keys.c does,
    the runner types Right Amiga + O and Right Amiga + B: within 60 seconds
    IDCMP_MENUPICK comes with Code FULLMENUNUM(0, 0, NOSUB), whose
    ItemAddress() is "Open" with NextSelect MENUNULL, and then one with
    FULLMENUNUM(1, 1, NOSUB), "B", which Intuition has CHECKED, clearing
    CHECKED of "A", which B excludes, all in the program's own items.
    ClearMenuStrip() detaches the strip before CloseWindow(). Run it with
    --keys gfx_classic_menus "meta_r+o meta_r+b".
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;

#define WIN_FLAGS (WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET)

struct ClassicMenus
{
    struct Menu project_menu;
    struct Menu edit_menu;
    struct MenuItem open_item;
    struct MenuItem check_item;
    struct MenuItem more_item;
    struct MenuItem sub1_item;
    struct MenuItem sub2_item;
    struct MenuItem a_item;
    struct MenuItem b_item;
    struct IntuiText open_text;
    struct IntuiText check_text;
    struct IntuiText more_text;
    struct IntuiText sub1_text;
    struct IntuiText sub2_text;
    struct IntuiText a_text;
    struct IntuiText b_text;
};

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
static struct ClassicMenus ms;

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
            if ((got = msg->Class == cls))
                *copy = *msg;
            ReplyMsg(&msg->ExecMessage);
            if (got)
                return copy;
        }
        Delay(5);
    }
    return NULL;
}

static void init_itext(struct IntuiText *it, WORD left, const char *label)
{
    it->FrontPen = 1;
    it->BackPen = 0;
    it->DrawMode = JAM1;
    it->LeftEdge = left;
    it->TopEdge = 1;
    it->ITextFont = &topaz8;
    it->IText = (STRPTR)label;
    it->NextText = NULL;
}

static void init_item(struct MenuItem *mi, struct MenuItem *next,
                      WORD left, WORD top, WORD width, UWORD flags,
                      struct IntuiText *it)
{
    mi->NextItem = next;
    mi->LeftEdge = left;
    mi->TopEdge = top;
    mi->Width = width;
    mi->Height = 10;
    mi->Flags = flags;
    mi->ItemFill = it;
}

/* Positions and sizes as a classic program sets them for topaz 8; the
   window's screen font may differ, so the layout is kept simple. */
static void build_menus(struct ClassicMenus *m)
{
    memset(m, 0, sizeof(*m));

    init_itext(&m->open_text, 0, "Open");
    init_itext(&m->check_text, CHECKWIDTH, "Check");
    init_itext(&m->more_text, 0, "More");
    init_itext(&m->sub1_text, 0, "Sub1");
    init_itext(&m->sub2_text, 0, "Sub2");
    init_itext(&m->a_text, CHECKWIDTH, "A");
    init_itext(&m->b_text, CHECKWIDTH, "B");

    init_item(&m->open_item, &m->check_item, 0, 0, 96,
              ITEMTEXT | COMMSEQ | ITEMENABLED | HIGHCOMP, &m->open_text);
    m->open_item.Command = 'O';

    init_item(&m->check_item, &m->more_item, 0, 10, 96,
              ITEMTEXT | CHECKIT | MENUTOGGLE | ITEMENABLED | HIGHCOMP, &m->check_text);

    init_item(&m->sub1_item, &m->sub2_item, 80, 0, 64,
              ITEMTEXT | ITEMENABLED | HIGHCOMP, &m->sub1_text);
    init_item(&m->sub2_item, NULL, 80, 10, 64,
              ITEMTEXT | ITEMENABLED | HIGHCOMP, &m->sub2_text);

    init_item(&m->more_item, NULL, 0, 20, 96,
              ITEMTEXT | ITEMENABLED | HIGHCOMP, &m->more_text);
    m->more_item.SubItem = &m->sub1_item;

    init_item(&m->a_item, &m->b_item, 0, 0, 64,
              ITEMTEXT | CHECKIT | ITEMENABLED | HIGHCOMP, &m->a_text);
    m->a_item.MutualExclude = 2;

    init_item(&m->b_item, NULL, 0, 10, 64,
              ITEMTEXT | CHECKIT | COMMSEQ | ITEMENABLED | HIGHCOMP, &m->b_text);
    m->b_item.MutualExclude = 1;
    m->b_item.Command = 'B';

    m->project_menu.NextMenu = &m->edit_menu;
    m->project_menu.LeftEdge = 0;
    m->project_menu.TopEdge = 0;
    m->project_menu.Width = 64;
    m->project_menu.Height = 10;
    m->project_menu.Flags = MENUENABLED;
    m->project_menu.MenuName = (CONST_STRPTR)"Project";
    m->project_menu.FirstItem = &m->open_item;

    m->edit_menu.NextMenu = NULL;
    m->edit_menu.LeftEdge = 72;
    m->edit_menu.TopEdge = 0;
    m->edit_menu.Width = 48;
    m->edit_menu.Height = 10;
    m->edit_menu.Flags = MENUENABLED;
    m->edit_menu.MenuName = (CONST_STRPTR)"Edit";
    m->edit_menu.FirstItem = &m->a_item;
}

static BOOL check_no_input(struct Window *win, struct ClassicMenus *m)
{
    if (!check(SetMenuStrip(win, &m->project_menu) != FALSE, "SetMenuStrip() gives TRUE"))
        return FALSE;
    check(win->MenuStrip == &m->project_menu, "the Window shows its MenuStrip");
    check(ItemAddress(&m->project_menu, FULLMENUNUM(0, 1, NOSUB)) == &m->check_item,
          "ItemAddress() of FULLMENUNUM(0, 1, NOSUB) is the Check item");
    check(ItemAddress(&m->project_menu, FULLMENUNUM(0, 2, 1)) == &m->sub2_item,
          "ItemAddress() of FULLMENUNUM(0, 2, 1) is the second sub item");

    OffMenu(win, FULLMENUNUM(0, 0, NOSUB));
    check((m->open_item.Flags & ITEMENABLED) == 0,
          "OffMenu() of FULLMENUNUM(0, 0, NOSUB) clears ITEMENABLED of Open");
    OnMenu(win, FULLMENUNUM(0, 0, NOSUB));
    check((m->open_item.Flags & ITEMENABLED) != 0,
          "OnMenu() of FULLMENUNUM(0, 0, NOSUB) sets ITEMENABLED of Open again");

    OffMenu(win, FULLMENUNUM(1, NOITEM, NOSUB));
    check((m->edit_menu.Flags & MENUENABLED) == 0,
          "OffMenu() of the whole Edit menu (item NOITEM) clears its MENUENABLED");
    OnMenu(win, FULLMENUNUM(1, NOITEM, NOSUB));
    check((m->edit_menu.Flags & MENUENABLED) != 0,
          "OnMenu() of the whole Edit menu sets its MENUENABLED again");

    ClearMenuStrip(win);
    check(win->MenuStrip == NULL, "ClearMenuStrip() detaches the strip");
    m->a_item.Flags |= CHECKED;
    check(ResetMenuStrip(win, &m->project_menu) != FALSE, "ResetMenuStrip() gives TRUE");
    check((m->a_item.Flags & CHECKED) != 0, "item A stays CHECKED after ResetMenuStrip()");
    return TRUE;
}

static void check_menupick(struct Window *win, struct ClassicMenus *m)
{
    struct IntuiMessage msg;
    struct MenuItem *picked;

    if (!check(wait_for(win, IDCMP_ACTIVEWINDOW, 10 * 50, &msg) != NULL,
               "IDCMP_ACTIVEWINDOW comes within 10 s"))
        return;
    /* without SYS:EMU68, as on AmigaOS, the keys come from elsewhere */
    write_marker();
    if (!check(wait_for(win, IDCMP_MENUPICK, 60 * 50, &msg) != NULL,
               "IDCMP_MENUPICK comes within 60 s"))
        return;
    check_long("the message Code is FULLMENUNUM(0, 0, NOSUB)",
               msg.Code, FULLMENUNUM(0, 0, NOSUB));
    picked = ItemAddress(&m->project_menu, msg.Code);
    check(picked == &m->open_item,
          "ItemAddress() of the Code is the Open item");
    if (picked)
        check_long("NextSelect of the Open item is MENUNULL",
                   picked->NextSelect, MENUNULL);

    if (!check(wait_for(win, IDCMP_MENUPICK, 30 * 50, &msg) != NULL,
               "a second IDCMP_MENUPICK comes within 30 s"))
        return;
    check_long("its Code is FULLMENUNUM(1, 1, NOSUB)", msg.Code, FULLMENUNUM(1, 1, NOSUB));
    picked = ItemAddress(&m->project_menu, msg.Code);
    check(picked == &m->b_item, "ItemAddress() of the Code is the B item");
    check((m->b_item.Flags & CHECKED) != 0, "Intuition has CHECKED B");
    check((m->a_item.Flags & CHECKED) == 0, "and cleared CHECKED of A, which B excludes");
    check_long("NextSelect of the B item is MENUNULL", m->b_item.NextSelect, MENUNULL);
}

static void run_menus(void)
{
    struct Window *win;

    build_menus(&ms);
    win = OpenWindowTags(NULL,
                         WA_Left, 40, WA_Top, 40,
                         WA_Width, 260, WA_Height, 100,
                         WA_Title, (ULONG)"gfx_classic_menus",
                         WA_Flags, WIN_FLAGS,
                         WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_MENUPICK | IDCMP_ACTIVEWINDOW,
                         TAG_DONE);
    if (!check(win != NULL, "OpenWindowTags() with WA_Activate and IDCMP_MENUPICK"))
        return;
    if (check_no_input(win, &ms))
    {
        check_menupick(win, &ms);
        ClearMenuStrip(win);
    }
    CloseWindow(win);
}

int main(void)
{
    struct Window *win;
    ULONG avail;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!check(IntuitionBase != NULL, "intuition.library 39"))
        return test_done("gfx_classic_menus");

    /* Open and close a first window so that the screen font that the bridge
       keeps for a window is there before the guest memory is counted. */
    win = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 100,
                         WA_Height, 40, TAG_DONE);
    if (win)
        CloseWindow(win);
    avail = AvailMem(MEMF_ANY);

    run_menus();

    check_long("ClearMenuStrip() and CloseWindow() give the guest memory back",
               AvailMem(MEMF_ANY), avail);

    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_classic_menus");
}

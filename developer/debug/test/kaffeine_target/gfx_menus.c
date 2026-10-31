/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    GadTools menus, the way C programs build them, with no input needed.
    A VisualInfo of the default public screen; a strip from a NewMenu array
    with titles, items, a separator, a submenu, command keys, a command
    string, a disabled menu and item, CHECKIT items that exclude each other,
    a toggle, an ignored entry and UserData everywhere; its layout with the
    program's own TextAttr; SetMenuStrip(), ItemAddress(), OffMenu(),
    OnMenu(), flags changed by the program and ResetMenuStrip(),
    ClearMenuStrip() and FreeMenus(). Also a strip of items only and the
    secondary error of an invalid array. The guest memory is back at the
    end.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

/* The 68k layouts that programs read */
typedef char check_menu[sizeof(struct Menu) == 30 ? 1 : -1];
typedef char check_item[sizeof(struct MenuItem) == 34 ? 1 : -1];

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };

static struct NewMenu newmenu[] =
{
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, (APTR)0x100 },
    { NM_ITEM, (STRPTR)"Open...", (STRPTR)"O", 0, 0, (APTR)0x101 },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Recent", NULL, 0, 0, (APTR)0x102 },
    { NM_SUB, (STRPTR)"one.txt", NULL, 0, 0, (APTR)0x103 },
    { NM_SUB, (STRPTR)"two.txt", NULL, 0, 0, (APTR)0x104 },
    { NM_IGNORE | NM_ITEM, (STRPTR)"Ignored", NULL, 0, 0, (APTR)0x1FF },
    { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Ctrl Q", NM_COMMANDSTRING, 0, (APTR)0x105 },
    { NM_TITLE, (STRPTR)"Edit", NULL, NM_MENUDISABLED, 0, (APTR)0x200 },
    { NM_ITEM, (STRPTR)"Undo", NULL, NM_ITEMDISABLED, 0, (APTR)0x201 },
    { NM_TITLE, (STRPTR)"Options", NULL, 0, 0, (APTR)0x300 },
    { NM_ITEM, (STRPTR)"Small", NULL, CHECKIT | CHECKED, 2, (APTR)0x301 },
    { NM_ITEM, (STRPTR)"Large", NULL, CHECKIT, 1, (APTR)0x302 },
    { NM_ITEM, (STRPTR)"Sound", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)0x303 },
    { NM_END, NULL, NULL, 0, 0, NULL }
};

static struct NewMenu items_only[] =
{
    { NM_ITEM, (STRPTR)"Alone", (STRPTR)"A", 0, 0, (APTR)0x400 },
    { NM_ITEM, (STRPTR)"Too", NULL, 0, 0, (APTR)0x401 },
    { NM_END, NULL, NULL, 0, 0, NULL }
};

static struct NewMenu invalid[] =
{
    { NM_ITEM, (STRPTR)"Orphan", NULL, 0, 0, NULL },
    { NM_END, NULL, NULL, 0, 0, NULL }
};

static const char *text_of(struct MenuItem *item)
{
    struct IntuiText *it = (struct IntuiText *)item->ItemFill;

    return (item->Flags & ITEMTEXT) && it && it->IText ? (const char *)it->IText : "";
}

static void structure(struct Menu *menu)
{
    struct Menu *edit = menu->NextMenu, *options = edit ? edit->NextMenu : NULL;
    struct MenuItem *open = menu->FirstItem, *bar = open ? open->NextItem : NULL;
    struct MenuItem *recent = bar ? bar->NextItem : NULL, *quit = recent ? recent->NextItem : NULL;
    struct MenuItem *small, *large, *sound;
    struct IntuiText *it;

    if (!check(edit && options && !options->NextMenu, "three menus, the ignored entry left out"))
        return;
    check(!strcmp((const char *)menu->MenuName, "Project") && GTMENU_USERDATA(menu) == (APTR)0x100,
          "a title with its name and UserData");
    check(!(edit->Flags & MENUENABLED) && (options->Flags & MENUENABLED), "NM_MENUDISABLED");
    if (!check(open && bar && recent && quit && !quit->NextItem, "four items in the first menu"))
        return;
    check(!strcmp(text_of(open), "Open...") && (open->Flags & COMMSEQ) && open->Command == 'O' &&
          GTMENUITEM_USERDATA(open) == (APTR)0x101, "a text item with its command key and UserData");
    check(!(bar->Flags & ITEMTEXT) && bar->ItemFill != NULL && !(bar->Flags & ITEMENABLED),
          "a separator is an image, disabled");
    it = (struct IntuiText *)recent->ItemFill;
    check(recent->SubItem && recent->SubItem->NextItem && !recent->SubItem->NextItem->NextItem &&
          GTMENUITEM_USERDATA(recent->SubItem->NextItem) == (APTR)0x104,
          "a submenu of two items");
    check(it && it->NextText && it->NextText->IText && it->NextText->IText[0],
          "the item of a submenu gets its mark");
    it = (struct IntuiText *)quit->ItemFill;
    check(it && it->NextText && !strcmp((const char *)it->NextText->IText, "Ctrl Q") &&
          !(quit->Flags & COMMSEQ), "a command string as a second text");
    check(edit->FirstItem && !(edit->FirstItem->Flags & ITEMENABLED), "NM_ITEMDISABLED");
    small = options->FirstItem;
    large = small ? small->NextItem : NULL;
    sound = large ? large->NextItem : NULL;
    if (!check(small && large && sound, "three options"))
        return;
    check((small->Flags & (CHECKIT | CHECKED)) == (CHECKIT | CHECKED) &&
          (large->Flags & (CHECKIT | CHECKED)) == CHECKIT && (sound->Flags & MENUTOGGLE),
          "CHECKIT, CHECKED and MENUTOGGLE");
    check(small->MutualExclude == 2 && large->MutualExclude == 1 &&
          GTMENUITEM_USERDATA(sound) == (APTR)0x303, "MutualExclude and UserData of the options");
}

static void layout(struct Menu *menu)
{
    struct MenuItem *small = menu->NextMenu->NextMenu->FirstItem;
    struct IntuiText *it = (struct IntuiText *)small->ItemFill;
    struct MenuItem *one = menu->FirstItem->NextItem->NextItem->SubItem;

    check(small->Width > 0 && small->Height > 0 && small->NextItem->TopEdge > small->TopEdge,
          "the layout places the items");
    check(it && it->ITextFont == &topaz8, "the texts use the program's own TextAttr");
    check(one->Width > 0 && ((struct IntuiText *)one->ItemFill)->ITextFont == &topaz8,
          "the submenu has its layout too");
}

static void on_window(struct Window *win, struct Menu *menu)
{
    struct Menu *options = menu->NextMenu->NextMenu;
    struct MenuItem *small = options->FirstItem, *large = small->NextItem;
    struct MenuItem *two = menu->FirstItem->NextItem->NextItem->SubItem->NextItem;

    if (!check(SetMenuStrip(win, menu), "SetMenuStrip() returns TRUE"))
        return;
    check(win->MenuStrip == menu, "the Window shows its strip");
    check(ItemAddress(menu, FULLMENUNUM(2, 1, NOSUB)) == large, "ItemAddress() of an item");
    check(ItemAddress(menu, FULLMENUNUM(0, 2, 1)) == two, "ItemAddress() of a subitem");
    check(ItemAddress(menu, MENUNULL) == NULL, "ItemAddress() of MENUNULL");

    OffMenu(win, FULLMENUNUM(2, 1, NOSUB));
    check(!(large->Flags & ITEMENABLED), "OffMenu() of an item");
    OnMenu(win, FULLMENUNUM(2, 1, NOSUB));
    check(large->Flags & ITEMENABLED, "OnMenu() of an item");
    OffMenu(win, FULLMENUNUM(2, NOITEM, NOSUB));
    check(!(options->Flags & MENUENABLED), "OffMenu() of a menu");
    OnMenu(win, FULLMENUNUM(2, NOITEM, NOSUB));
    check(options->Flags & MENUENABLED, "OnMenu() of a menu");

    /* The program checks Large itself and lets ResetMenuStrip() know. */
    ClearMenuStrip(win);
    small->Flags &= ~CHECKED;
    large->Flags |= CHECKED;
    check(ResetMenuStrip(win, menu), "ResetMenuStrip() returns TRUE");
    check(!(small->Flags & CHECKED) && (large->Flags & CHECKED),
          "the flags that the program set stay");
    ClearMenuStrip(win);
    check(win->MenuStrip == NULL, "ClearMenuStrip()");
}

int main(void)
{
    struct Screen *scr;
    struct Window *win;
    struct Menu *menu;
    APTR vi;
    ULONG err, avail;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 37"))
        return test_done("gfx_menus");

    /* The 68k Screen of LockPubScreen() stays for the program, and a first
       window shows the bridge the screen font, which it keeps. */
    scr = LockPubScreen(NULL);
    win = OpenWindowTags(NULL, WA_Left, 60, WA_Top, 60, WA_InnerWidth, 100, WA_InnerHeight, 40,
                         WA_Title, (ULONG)"gfx_menus", TAG_DONE);
    if (win)
        CloseWindow(win);
    avail = AvailMem(MEMF_ANY);

    vi = scr ? GetVisualInfoA(scr, NULL) : NULL;
    win = OpenWindowTags(NULL, WA_Left, 60, WA_Top, 60, WA_InnerWidth, 240, WA_InnerHeight, 80,
                         WA_Title, (ULONG)"gfx_menus", WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_MENUPICK, TAG_DONE);
    if (check(scr && vi && win, "a VisualInfo of the public screen and a window"))
    {
        err = 0x55;
        menu = CreateMenus(newmenu, GTMN_SecondaryError, (ULONG)&err, TAG_DONE);
        if (check(menu != NULL, "CreateMenusA() gives a strip"))
        {
            check_long("and no secondary error", err, 0);
            structure(menu);
            if (check(LayoutMenus(menu, vi, GTMN_TextAttr, (ULONG)&topaz8,
                                  GTMN_NewLookMenus, TRUE, TAG_DONE),
                      "LayoutMenusA() returns TRUE"))
                layout(menu);
            on_window(win, menu);
            FreeMenus(menu);
        }

        menu = CreateMenus(items_only, TAG_DONE);
        if (check(menu != NULL, "a strip of items only"))
        {
            struct MenuItem *alone = (struct MenuItem *)menu;

            check(!strcmp(text_of(alone), "Alone") && alone->Command == 'A' &&
                  GTMENUITEM_USERDATA(alone) == (APTR)0x400 && alone->NextItem &&
                  !alone->NextItem->NextItem, "its items, as 68k MenuItems");
            FreeMenus(menu);
        }

        err = 0;
        menu = CreateMenus(invalid, GTMN_FullMenu, TRUE, GTMN_SecondaryError, (ULONG)&err, TAG_DONE);
        check(menu == NULL, "an item without a title is no full menu");
        check_long("GTMENU_INVALID as the secondary error", err, GTMENU_INVALID);
    }
    if (win)
        CloseWindow(win);
    if (vi)
        FreeVisualInfo(vi);
    check_long("the menus, the VisualInfo and the window return all of their memory",
               AvailMem(MEMF_ANY), avail);
    if (scr)
        UnlockPubScreen(NULL, scr);

    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_menus");
}

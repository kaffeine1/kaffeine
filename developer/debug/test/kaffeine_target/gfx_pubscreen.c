/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public screens and the calls around LockPubScreen(): the fields of the
    Screen, its Font TextAttr, also a copy of it kept after the unlock, its
    embedded RastPort and ViewPort, its BitMap depth, repeated locks by NULL
    and by name returning the same 68k Screen, guest memory returned after
    unlocking, WScreen of a visitor window staying valid after
    UnlockPubScreen(), LockPubScreenList() and psn_VisitorCount across locks
    and an extra unlock, GetDefaultPubScreen() and NextPubScreen(), and
    GetVPModeID() on the embedded ViewPort.
*/

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

/* The 68k layouts that programs read. */
typedef char check_screen[sizeof(struct Screen) == 346 ? 1 : -1];
typedef char check_pubscreennode[sizeof(struct PubScreenNode) == 30 ? 1 : -1];
typedef char check_textattr[sizeof(struct TextAttr) == 8 ? 1 : -1];

/* Fields as LONGs: GCC 6.5 can pass a BYTE copied with move.b as a LONG
   without extending it. */
static LONG byte_field(BYTE value)
{
    return (LONG)value & 0xFF;
}

static BOOL ends_with_font(const char *name)
{
    size_t len;

    if (!name)
        return FALSE;
    len = strlen(name);
    return len >= 5 && !strcmp(name + len - 5, ".font");
}

/* Part 1: The screen and its fields, font, RastPort and BitMap. */
static void part_screen(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    LONG width, height, bar_height, bor_top, bor_left, bor_right, bor_bottom;
    LONG depth, ta_ysize = 0, title_height, rp_tf_ysize, rp_txheight, text_len;
    const char *ta_name = NULL;
    struct TextFont *opened_font;

    if (!check(scr != NULL, "LockPubScreen(NULL) is not NULL"))
        return;

    width = (LONG)scr->Width;
    height = (LONG)scr->Height;
    bar_height = byte_field(scr->BarHeight);
    bor_top = byte_field(scr->WBorTop);
    bor_left = byte_field(scr->WBorLeft);
    bor_right = byte_field(scr->WBorRight);
    bor_bottom = byte_field(scr->WBorBottom);
    depth = byte_field((BYTE)scr->BitMap.Depth);

    Printf("screen size: %ld x %ld, depth: %ld\n",
           (LONG)width, (LONG)height, (LONG)depth);
    Printf("screen bar: %ld, borders: top %ld, left %ld, right %ld, bottom %ld\n",
           (LONG)bar_height, (LONG)bor_top, (LONG)bor_left,
           (LONG)bor_right, (LONG)bor_bottom);

    check(width > 0 && width <= 16384 && height > 0 && height <= 16384,
          "Width and Height are between 1 and 16384");

    if (check(scr->Font != NULL, "scr->Font is set"))
    {
        ta_name = (const char *)scr->Font->ta_Name;
        ta_ysize = (LONG)scr->Font->ta_YSize;
        Printf("screen font: %s, size: %ld\n",
               (LONG)(ta_name ? ta_name : "NULL"), (LONG)ta_ysize);
        check(ends_with_font(ta_name), "Font->ta_Name ends with \".font\"");
        check(ta_ysize > 0, "Font->ta_YSize is greater than 0");
    }

    if (check(scr->RastPort.Font != NULL, "the screen RastPort has a font"))
    {
        rp_tf_ysize = (LONG)scr->RastPort.Font->tf_YSize;
        rp_txheight = (LONG)scr->RastPort.TxHeight;
        Printf("RastPort font tf_YSize: %ld, TxHeight: %ld\n",
               (LONG)rp_tf_ysize, (LONG)rp_txheight);
        check_long("RastPort.Font->tf_YSize equals ta_YSize", rp_tf_ysize, ta_ysize);
        check_long("RastPort.TxHeight equals ta_YSize", rp_txheight, ta_ysize);
    }

    check(bar_height > 0, "BarHeight is greater than 0");
    check(bor_top >= 0 && bor_top <= 31 &&
          bor_left >= 0 && bor_left <= 31 &&
          bor_right >= 0 && bor_right <= 31 &&
          bor_bottom >= 0 && bor_bottom <= 31,
          "WBorTop, WBorLeft, WBorRight and WBorBottom are between 0 and 31");

    title_height = bor_top + ta_ysize + 1;
    Printf("GadTools title bar height: %ld\n", (LONG)title_height);
    check(title_height >= 1 && title_height <= height - 1,
          "WBorTop + ta_YSize + 1 lies between 1 and Height - 1");

    check(depth >= 1 && depth <= 32, "BitMap.Depth is between 1 and 32");

    if (scr->Font != NULL)
    {
        opened_font = OpenFont(scr->Font);
        if (check(opened_font != NULL, "OpenFont(scr->Font)"))
        {
            Printf("OpenFont(scr->Font) tf_YSize: %ld\n",
                   (LONG)opened_font->tf_YSize);
            check_long("OpenFont(scr->Font) tf_YSize equals ta_YSize",
                       (LONG)opened_font->tf_YSize, ta_ysize);
            CloseFont(opened_font);
        }
    }

    if (scr->RastPort.Font != NULL)
    {
        text_len = (LONG)TextLength(&scr->RastPort, (CONST_STRPTR)"kaffeine", 7);
        Printf("TextLength(\"kaffeine\"): %ld\n", (LONG)text_len);
        check(text_len > 0,
              "TextLength() on &scr->RastPort of \"kaffeine\" is greater than 0");
    }

    UnlockPubScreen(NULL, scr);
}

/* Part 1b: A copy of the screen's TextAttr that the program keeps still
   names the font after the last unlock, also once the memory that the 68k
   Screen had has been taken and written again. */
static void part_font_copy(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    struct TextAttr copy;
    struct TextFont *font;
    APTR blocks[64];
    char name[64];
    LONG i;

    if (!check(scr != NULL && scr->Font != NULL && scr->Font->ta_Name != NULL,
               "LockPubScreen(NULL) with a font name to copy"))
    {
        if (scr)
            UnlockPubScreen(NULL, scr);
        return;
    }
    copy = *scr->Font;
    strncpy(name, (const char *)copy.ta_Name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    UnlockPubScreen(NULL, scr);
    for (i = 0; i < 64; i++)
        if ((blocks[i] = AllocMem(16, MEMF_ANY)))
            memset(blocks[i], 0x55, 16);
    check_str("the name of a kept copy of the TextAttr after the unlock",
              (const char *)copy.ta_Name, name);
    font = OpenFont(&copy);
    if (check(font != NULL, "OpenFont() of the kept copy"))
    {
        check_long("its tf_YSize", (LONG)font->tf_YSize, (LONG)copy.ta_YSize);
        CloseFont(font);
    }
    for (i = 0; i < 64; i++)
        if (blocks[i])
            FreeMem(blocks[i], 16);
}

/* Part 2: Repeated locks return the same 68k Screen and keep it until the
   last unlock: two locks of NULL, the default public screen, and one of
   the Workbench by name, the same screen when it is the default one, as
   on both test systems; the lock taken by name goes back by name. */
static void part_same_screen(void)
{
    char name[MAXPUBSCREENNAME + 1];
    struct Screen *scr1 = LockPubScreen(NULL);
    struct Screen *scr2 = LockPubScreen(NULL);
    struct Screen *scr3 = LockPubScreen((CONST_STRPTR)"Workbench");
    LONG width, ta_ysize = 0, width_after, ta_ysize_after = 0;
    BOOL workbench;

    name[0] = 0;
    GetDefaultPubScreen((STRPTR)name);
    workbench = !strcmp(name, "Workbench");
    if (!check(scr1 != NULL && scr2 != NULL && scr3 != NULL,
               "three LockPubScreen() calls succeed"))
    {
        if (scr3)
            UnlockPubScreen(NULL, scr3);
        if (scr2)
            UnlockPubScreen(NULL, scr2);
        if (scr1)
            UnlockPubScreen(NULL, scr1);
        return;
    }

    check(scr1 == scr2, "LockPubScreen(NULL) twice gives the same pointer");
    if (workbench)
        check(scr3 == scr1, "LockPubScreen(\"Workbench\") gives the pointer of the default one");
    else
        Printf("the default public screen is %s, not the Workbench\n", (LONG)name);

    width = (LONG)scr1->Width;
    if (scr1->Font != NULL)
        ta_ysize = (LONG)scr1->Font->ta_YSize;
    Printf("before unlocking two: Width %ld, ta_YSize %ld\n",
           (LONG)width, (LONG)ta_ysize);

    UnlockPubScreen(NULL, scr2);
    UnlockPubScreen((CONST_STRPTR)"Workbench", NULL);

    width_after = (LONG)scr1->Width;
    if (scr1->Font != NULL)
        ta_ysize_after = (LONG)scr1->Font->ta_YSize;
    Printf("with one lock left: Width %ld, ta_YSize %ld\n",
           (LONG)width_after, (LONG)ta_ysize_after);

    check_long("Width with one lock left", width_after, width);
    if (check(scr1->Font != NULL, "scr->Font is still set with one lock left"))
        check_long("Font->ta_YSize with one lock left", ta_ysize_after, ta_ysize);

    UnlockPubScreen(NULL, scr1);
}

/* Part 3: Guest memory across one lock and unlock, then 1100 locks and
   unlocks. */
static void part_memory(void)
{
    LONG avail_before = (LONG)AvailMem(MEMF_ANY), avail_one, avail_many;
    struct Screen *scr;
    LONG i, nulls = 0;

    scr = LockPubScreen(NULL);
    if (check(scr != NULL, "LockPubScreen(NULL) for single memory check"))
        UnlockPubScreen(NULL, scr);
    avail_one = (LONG)AvailMem(MEMF_ANY);
    Printf("AvailMem before: %ld, after one lock and unlock: %ld\n",
           (LONG)avail_before, (LONG)avail_one);
    check_long("AvailMem(MEMF_ANY) after one lock and unlock",
               avail_one, avail_before);

    for (i = 0; i < 1100; i++)
    {
        scr = LockPubScreen(NULL);
        if (scr != NULL)
            UnlockPubScreen(NULL, scr);
        else
            nulls++;
    }
    avail_many = (LONG)AvailMem(MEMF_ANY);
    Printf("1100 locks: %ld NULLs, AvailMem after: %ld\n",
           (LONG)nulls, (LONG)avail_many);
    check_long("NULL count across 1100 LockPubScreen() calls", nulls, 0);
    check_long("AvailMem(MEMF_ANY) after 1100 locks and unlocks",
               avail_many, avail_before);
}

/* Part 4: A visitor window keeps WScreen valid after UnlockPubScreen() until
   CloseWindow(). A first window shows the bridge the font of windows, which
   it keeps for the program, before the memory is measured. */
static void part_window(void)
{
    LONG avail_before, avail_after;
    struct Screen *scr;
    struct Window *win;
    LONG width, ta_ysize = 0, wscreen_width, wscreen_ta_ysize = 0;

    win = OpenWindowTags(NULL, WA_Left, 20, WA_Top, 20, WA_InnerWidth, 200, WA_InnerHeight, 60,
                         WA_Title, (ULONG)"gfx_pubscreen", TAG_DONE);
    if (win)
        CloseWindow(win);
    avail_before = (LONG)AvailMem(MEMF_ANY);
    scr = LockPubScreen(NULL);
    if (!check(scr != NULL, "LockPubScreen(NULL) for window check"))
        return;

    width = (LONG)scr->Width;
    if (scr->Font != NULL)
        ta_ysize = (LONG)scr->Font->ta_YSize;
    Printf("screen before window unlock: Width %ld, ta_YSize %ld\n",
           (LONG)width, (LONG)ta_ysize);

    win = OpenWindowTags(NULL,
                         WA_PubScreen, (ULONG)scr,
                         WA_Left, 20,
                         WA_Top, 20,
                         WA_InnerWidth, 200,
                         WA_InnerHeight, 60,
                         WA_Title, (ULONG)"gfx_pubscreen",
                         WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                         TAG_DONE);
    UnlockPubScreen(NULL, scr);

    if (check(win != NULL, "OpenWindowTags() on locked public screen"))
    {
        if (check(win->WScreen == scr, "win->WScreen is the locked screen after unlock"))
        {
            wscreen_width = (LONG)win->WScreen->Width;
            if (win->WScreen->Font != NULL)
                wscreen_ta_ysize = (LONG)win->WScreen->Font->ta_YSize;
            Printf("win->WScreen after unlock: Width %ld, ta_YSize %ld\n",
                   (LONG)wscreen_width, (LONG)wscreen_ta_ysize);
            check_long("win->WScreen->Width after unlock", wscreen_width, width);
            if (check(win->WScreen->Font != NULL, "win->WScreen->Font is set after unlock"))
                check_long("win->WScreen->Font->ta_YSize after unlock",
                           wscreen_ta_ysize, ta_ysize);
        }
        CloseWindow(win);
    }

    avail_after = (LONG)AvailMem(MEMF_ANY);
    Printf("AvailMem before lock: %ld, after CloseWindow: %ld\n",
           (LONG)avail_before, (LONG)avail_after);
    check_long("AvailMem(MEMF_ANY) after CloseWindow()", avail_after, avail_before);
}

static BOOL workbench_visitors(LONG *visitors_out)
{
    struct List *list = LockPubScreenList();
    struct Node *node;
    BOOL found = FALSE;

    *visitors_out = 0;
    if (list == NULL)
        return FALSE;
    for (node = list->lh_Head; node != NULL && node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if (node->ln_Name != NULL && !strcmp((const char *)node->ln_Name, "Workbench"))
        {
            *visitors_out = (LONG)((struct PubScreenNode *)node)->psn_VisitorCount;
            found = TRUE;
            break;
        }
    }
    UnlockPubScreenList();
    return found;
}

/* Part 5: LockPubScreenList() and Intuition's psn_VisitorCount across locks
   and an extra unlock. */
static void part_visitor_count(void)
{
    struct Screen *scr = LockPubScreen((CONST_STRPTR)"Workbench"), *scr2;
    struct List *list;
    struct Node *node;
    struct PubScreenNode *wb = NULL;
    struct Screen *wb_scr = NULL;
    LONG count = 0, v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    BOOL found;

    if (!check(scr != NULL, "LockPubScreen(\"Workbench\") for LockPubScreenList()"))
        return;

    list = LockPubScreenList();
    if (!check(list != NULL, "LockPubScreenList() is not NULL"))
    {
        UnlockPubScreen(NULL, scr);
        return;
    }

    for (node = list->lh_Head; node != NULL && node->ln_Succ != NULL; node = node->ln_Succ)
    {
        count++;
        if (node->ln_Name != NULL && !strcmp((const char *)node->ln_Name, "Workbench"))
        {
            wb = (struct PubScreenNode *)node;
            wb_scr = wb->psn_Screen;
            v1 = (LONG)wb->psn_VisitorCount;
        }
    }
    UnlockPubScreenList();

    Printf("public screen list nodes: %ld, v1: %ld\n", (LONG)count, (LONG)v1);
    check(count >= 1, "the public screen list has at least 1 node");
    if (!check(wb != NULL, "the public screen list has a \"Workbench\" node"))
    {
        UnlockPubScreen(NULL, scr);
        return;
    }
    check(wb_scr == scr, "psn_Screen of \"Workbench\" is the locked screen");

    scr2 = LockPubScreen((CONST_STRPTR)"Workbench");
    check(scr2 == scr, "a second LockPubScreen(\"Workbench\") gives the same pointer");
    found = workbench_visitors(&v2);
    Printf("visitor count v2 (two locks): %ld\n", (LONG)v2);
    if (check(found, "\"Workbench\" node found with two locks"))
        check_long("psn_VisitorCount with two locks is v1 + 1", v2, v1 + 1);

    if (scr2 != NULL)
        UnlockPubScreen(NULL, scr2);
    UnlockPubScreen(NULL, scr);
    found = workbench_visitors(&v3);
    Printf("visitor count v3 (both unlocked): %ld\n", (LONG)v3);
    if (check(found, "\"Workbench\" node found after unlocking both"))
        check_long("psn_VisitorCount after unlocking both is v1 - 1", v3, v1 - 1);

    UnlockPubScreen(NULL, scr);
    found = workbench_visitors(&v4);
    Printf("visitor count v4 (extra unlock): %ld\n", (LONG)v4);
    if (check(found, "\"Workbench\" node found after extra unlock"))
        check_long("psn_VisitorCount after extra unlock is v3", v4, v3);
}

/* Part 6: GetDefaultPubScreen() and NextPubScreen(). */
static void part_names(void)
{
    char buffer[MAXPUBSCREENNAME + 1];
    char *next;
    struct Screen *scr;

    memset(buffer, 0, sizeof(buffer));
    GetDefaultPubScreen((STRPTR)buffer);
    buffer[MAXPUBSCREENNAME] = '\0';
    Printf("GetDefaultPubScreen: %s\n", (LONG)buffer);
    if (check(buffer[0] != '\0', "GetDefaultPubScreen(buffer) writes a name"))
    {
        struct Screen *named = LockPubScreen((CONST_STRPTR)buffer), *scr_null = LockPubScreen(NULL);

        check(named != NULL && named == scr_null,
              "LockPubScreen() of that name gives the screen of LockPubScreen(NULL)");
        if (scr_null)
            UnlockPubScreen(NULL, scr_null);
        if (named)
            UnlockPubScreen(NULL, named);
    }

    memset(buffer, 'x', sizeof(buffer));
    buffer[0] = '\0';
    next = (char *)NextPubScreen(NULL, (STRPTR)buffer);
    buffer[MAXPUBSCREENNAME] = '\0';
    Printf("NextPubScreen(NULL): %s\n", (LONG)(next ? buffer : "NULL"));
    check(next == buffer && buffer[0] != '\0',
          "NextPubScreen(NULL, buffer) returns buffer with a non-empty name");

    scr = LockPubScreen(NULL);
    if (check(scr != NULL, "LockPubScreen(NULL) for NextPubScreen(scr)"))
    {
        memset(buffer, 'x', sizeof(buffer));
        buffer[0] = '\0';
        next = (char *)NextPubScreen(scr, (STRPTR)buffer);
        buffer[MAXPUBSCREENNAME] = '\0';
        if (next == buffer)
            Printf("NextPubScreen(scr): buffer (%s)\n", (LONG)buffer);
        else if (next == NULL)
            Printf("NextPubScreen(scr): NULL\n");
        else
            Printf("NextPubScreen(scr): unexpected pointer\n");
        check(next == NULL || next == buffer,
              "NextPubScreen(scr, buffer) returns NULL or buffer");
        UnlockPubScreen(NULL, scr);
    }
}

/* Part 7: GetVPModeID() on the embedded ViewPort of the screen. */
static void part_viewport(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    LONG mode_id;

    if (!check(scr != NULL, "LockPubScreen(NULL) for GetVPModeID()"))
        return;

    mode_id = GetVPModeID(&scr->ViewPort);
    Printf("GetVPModeID(&scr->ViewPort): %ld\n", (LONG)mode_id);
    check(mode_id != (LONG)INVALID_ID,
          "GetVPModeID(&scr->ViewPort) is not INVALID_ID");

    UnlockPubScreen(NULL, scr);
}

int main(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    if (check(IntuitionBase && GfxBase, "intuition.library and graphics.library 37"))
    {
        part_screen();
        part_font_copy();
        part_same_screen();
        part_memory();
        part_window();
        part_visitor_count();
        part_names();
        part_viewport();
    }
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_pubscreen");
}

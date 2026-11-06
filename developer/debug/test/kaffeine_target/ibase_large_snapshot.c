/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Real classic menus exercise a read-only IBase snapshot larger than the
   former 1024-view bound. All assertions and DOS output run after unlock.
   There is no input, image operation, or application-specific dependency. */
#include <exec/types.h>
#include <intuition/intuitionbase.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>
#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
#define MENUS 20
#define ITEMS 20
#define VIEWS (MENUS * (2 + ITEMS * 3))
struct Strip {
    struct Menu menu[MENUS];
    struct MenuItem item[MENUS][ITEMS];
    struct IntuiText text[MENUS][ITEMS];
    char name[MENUS][3], label[MENUS][ITEMS][5];
};
static struct Strip strip[2];
static const char title[] = "IBase large snapshot";
static APTR seen[VIEWS];
static ULONG seen_count;

static void init_strip(struct Strip *s, unsigned variant)
{
    unsigned i, j;
    for (i = 0; i < MENUS; ++i) {
        struct Menu *m = &s->menu[i];
        s->name[i][0] = 'A' + i; s->name[i][1] = '0' + variant;
        m->NextMenu = i + 1 < MENUS ? &s->menu[i + 1] : NULL;
        m->LeftEdge = i * 24; m->Width = 20; m->Height = 10;
        m->Flags = MENUENABLED; m->MenuName = (CONST_STRPTR)s->name[i];
        m->FirstItem = s->item[i];
        for (j = 0; j < ITEMS; ++j) {
            struct MenuItem *item = &s->item[i][j];
            struct IntuiText *text = &s->text[i][j];
            s->label[i][j][0] = 'A' + i;
            s->label[i][j][1] = 'A' + j;
            s->label[i][j][2] = '0' + variant;
            item->NextItem = j + 1 < ITEMS ? &s->item[i][j + 1] : NULL;
            item->TopEdge = j * 9; item->Width = 64; item->Height = 9;
            item->Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
            item->ItemFill = text;
            text->FrontPen = 1; text->DrawMode = JAM1;
            text->IText = (STRPTR)s->label[i][j];
        }
    }
}

/* Called only while IBase is held. Bounded loops, guest memory only. */
static BOOL unique(APTR p)
{
    ULONG i;
    if (!p || seen_count == VIEWS) return FALSE;
    for (i = 0; i < seen_count; ++i) if (seen[i] == p) return FALSE;
    seen[seen_count++] = p;
    return TRUE;
}
static struct Window *find_window(void)
{
    struct Screen *s;
    unsigned si, wi;
    for (s = IntuitionBase->FirstScreen, si = 0; s && si < 8;
         s = s->NextScreen, ++si) {
        struct Window *w;
        for (w = s->FirstWindow, wi = 0; w && wi < 64;
             w = w->NextWindow, ++wi)
            if (w->Title && !strcmp((const char *)w->Title, title)) return w;
    }
    return NULL;
}
static BOOL walk(struct Menu *m, const struct Strip *expected)
{
    unsigned i, j;
    seen_count = 0;
    for (i = 0; i < MENUS; ++i) {
        struct MenuItem *item;
        if (!m || !unique(m) || !unique((APTR)m->MenuName) ||
            strcmp((const char *)m->MenuName, expected->name[i])) return FALSE;
        item = m->FirstItem;
        for (j = 0; j < ITEMS; ++j) {
            struct IntuiText *text;
            if (!item || !(item->Flags & ITEMTEXT) || !unique(item)) return FALSE;
            text = (struct IntuiText *)item->ItemFill;
            if (!unique(text) || !unique(text->IText) ||
                strcmp((const char *)text->IText, expected->label[i][j]) ||
                item->TopEdge != j * 9 || text->NextText) return FALSE;
            item = item->NextItem;
        }
        if (item) return FALSE;
        m = m->NextMenu;
    }
    return m == NULL && seen_count == VIEWS;
}
int main(void)
{
    struct Window *window;
    unsigned epoch;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!check(IntuitionBase != NULL, "intuition.library opens"))
        return test_done("ibase_large_snapshot");
    init_strip(&strip[0], 0); init_strip(&strip[1], 1);
    window = OpenWindowTags(NULL, WA_Title, (ULONG)title,
        WA_Width, 520, WA_Height, 240,
        WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET,
        TAG_DONE);
    if (check(window != NULL, "a private test window opens")) {
        for (epoch = 0; epoch < 8; ++epoch) {
            struct Strip *s = &strip[epoch & 1];
            ULONG key, nested, count;
            struct Window *mirror, *again;
            struct Menu *menu_token;
            BOOL valid, stable, detached;
            if (!check(SetMenuStrip(window, s->menu), "400 real menu items attach")) break;
            key = LockIBase(0);
            mirror = find_window();
            menu_token = mirror ? mirror->MenuStrip : NULL;
            valid = mirror && walk(menu_token, s);
            count = seen_count;
            nested = LockIBase(0);
            again = find_window();
            stable = nested == key && again == mirror && again && again->MenuStrip == menu_token;
            UnlockIBase(nested);
            UnlockIBase(key);
            check(valid, "all menu items, text and distinct strings survive the snapshot");
            check_long("the visited strip alone exceeds 1024 distinct views", count, VIEWS);
            check(stable, "a nested lock preserves live mirror identities");
            key = LockIBase(0);
            again = find_window();
            stable = again == mirror && again && again->MenuStrip == menu_token && walk(menu_token, s);
            UnlockIBase(key);
            check(stable, "the next successful epoch preserves live mirror identities");
            ClearMenuStrip(window);
            key = LockIBase(0);
            again = find_window(); detached = again && !again->MenuStrip;
            UnlockIBase(key);
            check(detached, "the next epoch excludes the detached strip");
        }
        ClearMenuStrip(window);
        CloseWindow(window);
        {
            ULONG key = LockIBase(0);
            BOOL removed = find_window() == NULL;
            UnlockIBase(key);
            check(removed, "closing the test window removes it from the snapshot");
        }
    }
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("ibase_large_snapshot");
}

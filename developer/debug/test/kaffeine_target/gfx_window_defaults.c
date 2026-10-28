/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Original OpenWindowTagList NewWindow/default and override regression. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;

static void inspect(struct Window *w, WORD left, WORD top, WORD width, WORD height,
                    ULONG idcmp, const char *title, int defaults)
{
    if (!check(w != NULL, "open a window")) return;
    check(w->LeftEdge == left && w->TopEdge == top, "effective window position");
    check(w->Width == width && w->Height == height, "effective window dimensions");
    check(w->IDCMPFlags == idcmp, "effective IDCMP flags");
    check((w->UserPort != NULL) == (idcmp != 0), "IDCMP creates or omits the UserPort");
    if (w->UserPort) check(GetMsg(w->UserPort) == NULL, "new event port is empty");
    check(w->Title != NULL && strcmp((const char *)w->Title, title) == 0, "effective title");
    check((w->Flags & WFLG_BORDERLESS) != 0, "NewWindow or tags supply the window flags");
    if (defaults) {
        check(w->MinWidth == 80 && w->MinHeight == 50, "NewWindow minimum limits");
        check(w->MaxWidth == 640 && w->MaxHeight == 480, "NewWindow maximum limits");
    }
    CloseWindow(w);
}

int main(void)
{
    struct NewWindow nw, before;
    struct Image legacy_check = {0};
    UWORD check_data[1] = {0xA000};
    struct TagItem done[] = {{ TAG_DONE, 0 }};
    struct TagItem overrides[] = {
        { WA_Left, 72 }, { WA_Top, 48 }, { WA_Width, 260 }, { WA_Height, 120 },
        { WA_IDCMP, 0 }, { WA_Title, (uintptr_t)"overridden title" }, { TAG_DONE, 0 }
    };
    struct TagItem only[] = {
        { WA_Left, 80 }, { WA_Top, 64 }, { WA_Width, 200 }, { WA_Height, 88 },
        { WA_Flags, WFLG_BORDERLESS }, { WA_IDCMP, IDCMP_CLOSEWINDOW },
        { WA_Title, (uintptr_t)"tag-only title" }, { TAG_DONE, 0 }
    };
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!check(IntuitionBase != NULL, "open intuition.library")) return test_done("gfx_window_defaults");
    memset(&nw, 0, sizeof(nw));
    nw.LeftEdge = 48; nw.TopEdge = 32; nw.Width = 240; nw.Height = 96;
    nw.DetailPen = 1; nw.BlockPen = 0;
    nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_RAWKEY;
    nw.Flags = WFLG_BORDERLESS;
    nw.Title = (UBYTE *)"NewWindow defaults";
    nw.MinWidth = 80; nw.MinHeight = 50; nw.MaxWidth = 640; nw.MaxHeight = 480;
    nw.Type = WBENCHSCREEN;
    before = nw;

    puts("STEP: non-NULL NewWindow with an empty tag list");
    inspect(OpenWindowTagList(&nw, done), 48, 32, 240, 96, nw.IDCMPFlags,
            "NewWindow defaults", 1);
    check(memcmp(&nw, &before, sizeof(nw)) == 0, "empty tags preserve caller NewWindow");
    puts("STEP: non-NULL NewWindow with NULL tags");
    inspect(OpenWindowTagList(&nw, NULL), 48, 32, 240, 96, nw.IDCMPFlags,
            "NewWindow defaults", 1);
    check(memcmp(&nw, &before, sizeof(nw)) == 0, "NULL tags preserve caller NewWindow");
    puts("STEP: tags override geometry, title and IDCMP, including zero");
    inspect(OpenWindowTagList(&nw, overrides), 72, 48, 260, 120, 0,
            "overridden title", 1);
    check(memcmp(&nw, &before, sizeof(nw)) == 0, "overrides preserve caller NewWindow");
    puts("STEP: tag-only input retains existing behavior");
    inspect(OpenWindowTagList(NULL, only), 80, 64, 200, 88, IDCMP_CLOSEWINDOW,
            "tag-only title", 0);
    puts("STEP: the legacy NewWindow CheckMark does not change window creation");
    legacy_check.Width = 16; legacy_check.Height = 1; legacy_check.Depth = 1;
    legacy_check.ImageData = check_data; legacy_check.PlanePick = 1;
    nw.CheckMark = &legacy_check;
    before = nw;
    inspect(OpenWindowTagList(&nw, done), 48, 32, 240, 96, nw.IDCMPFlags,
            "NewWindow defaults", 1);
    check(memcmp(&nw, &before, sizeof(nw)) == 0, "legacy CheckMark preserves caller input");
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_window_defaults");
}

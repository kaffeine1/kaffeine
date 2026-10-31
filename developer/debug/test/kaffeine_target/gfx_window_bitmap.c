/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original owned-window RastPort bitmap identity and metadata oracle. */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdint.h>
#include <string.h>
#include "m68ktest.h"

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

int main(void)
{
    struct Screen *screen = NULL;
    struct Window *window = NULL;
    ULONG mode = INVALID_ID, pass;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    if (!check(GfxBase && IntuitionBase, "open graphics and intuition"))
        goto done;
    while ((mode = NextDisplayInfo(mode)) != INVALID_ID) {
        struct DimensionInfo dim;
        memset(&dim, 0, sizeof(dim));
        if (GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, mode) && dim.MaxDepth == 8)
            break;
    }
    if (!check(mode != INVALID_ID, "find an indexed display mode"))
        goto done;
    for (pass = 0; pass < 2; pass++) {
        struct BitMap *bitmap;
        screen = OpenScreenTags(NULL, SA_DisplayID, mode, SA_Width, 320,
                                SA_Height, 200, SA_Depth, 3, SA_Type, CUSTOMSCREEN,
                                SA_Quiet, TRUE, SA_Behind, TRUE, TAG_DONE);
        if (!check(screen != NULL, "open an owned indexed screen"))
            goto done;
        window = OpenWindowTags(NULL, WA_CustomScreen, (uintptr_t)screen,
                                WA_Left, 8, WA_Top, 8, WA_Width, 160, WA_Height, 80,
                                WA_Borderless, TRUE, WA_Activate, FALSE,
                                WA_IDCMP, 0, TAG_DONE);
        if (!check(window != NULL, "open a window on the indexed screen"))
            goto done;
        check(window->WScreen == screen, "window preserves its screen identity");
        if (!check(window->RPort != NULL, "window exposes its RastPort"))
            goto done;
        bitmap = window->RPort->BitMap;
        check(bitmap != NULL, "window RastPort exposes its screen bitmap");
        if (bitmap) {
            check(bitmap == screen->RastPort.BitMap, "window and screen share bitmap identity");
            check(bitmap->Depth >= 3 && bitmap->Depth <= 8, "window bitmap has readable indexed depth");
            check(bitmap->Depth == screen->RastPort.BitMap->Depth, "window bitmap depth matches the screen");
            check(GetBitMapAttr(bitmap, BMA_DEPTH) == GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH),
                  "typed bitmap depth query agrees for window and screen");
            check(GetBitMapAttr(bitmap, BMA_WIDTH) >= 320 && GetBitMapAttr(bitmap, BMA_HEIGHT) >= 200,
                  "window bitmap describes the full backing screen");
        }
        SetAPen(window->RPort, 3);
        Move(window->RPort, 2, 2);
        Draw(window->RPort, 12, 2);
        check(window->RPort->BitMap == bitmap && bitmap != NULL,
              "RastPort drawing preserves the typed bitmap handle");
        CloseWindow(window);
        window = NULL;
        check(screen->RastPort.BitMap != NULL && screen->RastPort.BitMap->Depth >= 3,
              "window retirement leaves the screen bitmap readable");
        check(CloseScreen(screen) != 0, "close the screen after its window");
        screen = NULL;
    }
done:
    if (window) CloseWindow(window);
    if (screen) CloseScreen(screen);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    return test_done("gfx_window_bitmap");
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    DrawBevelBox() of gadtools.library into a window's RastPort on the
    default public screen, with no input needed: a raised bevel box with
    GT_VisualInfo, a recessed one with GTBB_Recessed, a call without
    GT_VisualInfo that draws nothing, and a ridge frame with
    GTBB_FrameType, BBFT_RIDGE, all read back with ReadPixel() against the
    background that the cleared box reads: on a screen of more than 256
    colours ReadPixel() gives the pen found for a colour, which need not be
    the pen that drew it.
*/

#include <exec/types.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

#define BOX_W 40
#define BOX_H 20

static BOOL check_ne(const char *what, LONG got, LONG unexpected)
{
    test_checks++;
    if (got != unexpected)
    {
        Printf("ok   %s: %ld != %ld\n", (LONG)what, got, unexpected);
        test_flush();
        return TRUE;
    }
    test_failures++;
    Printf("FAIL %s: got %ld (0x%08lx), expected != %ld (0x%08lx)\n",
           (LONG)what, got, got, unexpected, unexpected);
    test_flush();
    return FALSE;
}

/* Clear a box to pen 0, and read back what that gives */
static LONG clear_box(struct RastPort *rp, LONG x, LONG y)
{
    SetAPen(rp, 0);
    RectFill(rp, x - 2, y - 2, x + BOX_W + 1, y + BOX_H + 1);
    return ReadPixel(rp, x + BOX_W / 2, y + BOX_H / 2);
}

static void check_bevel_boxes(struct Window *win, struct RastPort *rp, APTR vi)
{
    LONG ax = win->BorderLeft + 10, ay = win->BorderTop + 6;
    LONG bx = win->BorderLeft + 70, by = win->BorderTop + 6;
    LONG cx = win->BorderLeft + 10, cy = win->BorderTop + 36;
    LONG dx = win->BorderLeft + 70, dy = win->BorderTop + 36;
    LONG top, left, bottom, right, centre;
    LONG shine, shadow, bg;

    /* Box A: raised bevel box with GT_VisualInfo */
    bg = clear_box(rp, ax, ay);
    DrawBevelBox(rp, ax, ay, BOX_W, BOX_H,
                 GT_VisualInfo, (ULONG)vi,
                 TAG_DONE);
    top = ReadPixel(rp, ax + 20, ay);
    left = ReadPixel(rp, ax, ay + 10);
    bottom = ReadPixel(rp, ax + 20, ay + 19);
    right = ReadPixel(rp, ax + 39, ay + 10);
    centre = ReadPixel(rp, ax + 20, ay + 10);
    shine = top;
    shadow = bottom;
    check_long("box A top and left read the same shine pen", left, shine);
    check_long("box A bottom and right read the same shadow pen", right, shadow);
    check_ne("box A shine and shadow pens differ", shine, shadow);
    check_ne("box A shine pen differs from the background", shine, bg);
    check_ne("box A shadow pen differs from the background", shadow, bg);
    check_long("box A centre is still the background", centre, bg);

    /* Box B: recessed bevel box with GT_VisualInfo and GTBB_Recessed, TRUE */
    bg = clear_box(rp, bx, by);
    DrawBevelBox(rp, bx, by, BOX_W, BOX_H,
                 GT_VisualInfo, (ULONG)vi,
                 GTBB_Recessed, TRUE,
                 TAG_DONE);
    top = ReadPixel(rp, bx + 20, by);
    left = ReadPixel(rp, bx, by + 10);
    bottom = ReadPixel(rp, bx + 20, by + 19);
    right = ReadPixel(rp, bx + 39, by + 10);
    centre = ReadPixel(rp, bx + 20, by + 10);
    check_long("box B recessed top reads shadow pen", top, shadow);
    check_long("box B recessed left reads shadow pen", left, shadow);
    check_long("box B recessed bottom reads shine pen", bottom, shine);
    check_long("box B recessed right reads shine pen", right, shine);
    check_long("box B recessed centre is the background", centre, bg);

    /* Box C: no GT_VisualInfo tag (only GTBB_Recessed, FALSE) */
    bg = clear_box(rp, cx, cy);
    DrawBevelBox(rp, cx, cy, BOX_W, BOX_H,
                 GTBB_Recessed, FALSE,
                 TAG_DONE);
    top = ReadPixel(rp, cx + 20, cy);
    left = ReadPixel(rp, cx, cy + 10);
    bottom = ReadPixel(rp, cx + 20, cy + 19);
    right = ReadPixel(rp, cx + 39, cy + 10);
    centre = ReadPixel(rp, cx + 20, cy + 10);
    check_long("box C without GT_VisualInfo top is the background", top, bg);
    check_long("box C without GT_VisualInfo left is the background", left, bg);
    check_long("box C without GT_VisualInfo bottom is the background", bottom, bg);
    check_long("box C without GT_VisualInfo right is the background", right, bg);
    check_long("box C without GT_VisualInfo centre is the background", centre, bg);

    /* Box D: ridge frame with GT_VisualInfo and GTBB_FrameType, BBFT_RIDGE */
    bg = clear_box(rp, dx, dy);
    DrawBevelBox(rp, dx, dy, BOX_W, BOX_H,
                 GT_VisualInfo, (ULONG)vi,
                 GTBB_FrameType, BBFT_RIDGE,
                 TAG_DONE);
    top = ReadPixel(rp, dx + 20, dy);
    check_ne("box D BBFT_RIDGE top edge differs from the background", top, bg);
}

int main(void)
{
    struct Screen *screen;
    struct Window *win;
    APTR vi;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (check(IntuitionBase && GfxBase && GadToolsBase,
              "intuition.library, graphics.library and gadtools.library 39"))
    {
        screen = LockPubScreen(NULL);
        if (check(screen != NULL, "LockPubScreen(NULL)"))
        {
            vi = GetVisualInfo(screen, TAG_DONE);
            if (check(vi != NULL, "GetVisualInfo(screen, TAG_DONE)"))
            {
                win = OpenWindowTags(NULL,
                                     WA_Left, 40,
                                     WA_Top, 40,
                                     WA_Width, 240,
                                     WA_Height, 120,
                                     WA_Title, (ULONG)"gfx_bevel",
                                     WA_Flags, WFLG_SMART_REFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                                     TAG_DONE);
                if (check(win != NULL && win->RPort != NULL, "OpenWindowTags()"))
                {
                    check_bevel_boxes(win, win->RPort, vi);
                    CloseWindow(win);
                }
                FreeVisualInfo(vi);
            }
            UnlockPubScreen(NULL, screen);
        }
    }
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_bevel");
}

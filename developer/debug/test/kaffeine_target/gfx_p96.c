/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Picasso96API.library as AmiPixelShop uses it: OpenLibrary() of version 2
    and of none, NULL for version 3, p96BestModeIDTagList() with its tags,
    which gives INVALID_ID or a 4-byte mode of an allowed format, and with
    requests that no mode can meet (depth 33, 64 and 256, a width of
    0x10320, a format allowed and forbidden), which give INVALID_ID while
    the program goes on, and
    p96WritePixelArray() of an 8 by 4 pattern into a window on the default
    public screen, which it locks so that the window's WScreen is that
    screen, in each of the six formats that the bridge copies, from
    SrcX 3 and SrcY 2 of a larger buffer, read back pixel by pixel with
    ReadRGBPixel() of cybergraphics.library on a screen of 24 bits or more.
    An empty rectangle writes nothing, and a rectangle past the right edge
    of the window is clipped by its layer.

    Without an argument it is a smoke test: on a screen of fewer than 24
    bits the patterns are written but not compared, and AmiPixelShop's mode
    request may give INVALID_ID; both are reported as SKIP info lines and
    counted as no check. "gfx_p96 rgb" is the strict test: the screen must
    have 24 bits or more and the request a mode, and every pattern is read
    back, with the columns and rows around it untouched; it reports the
    depth and the number of formats compared.

    "gfx_p96 unsupported" calls p96GetModeIDAttr(), which the bridge does
    not have, and "gfx_p96 badri" passes a NULL RenderInfo: both stop the
    program with rc 20 (--expect-rc NAME 20).

    "gfx_p96 refresh" writes in a refresh, which needs 24 bits or more: a
    simple refresh window paints the pattern's rectangle and a frame of one
    pixel around it in one colour, a borderless window of the program
    covers the right half of the pattern and closes, and once the
    REFRESHWINDOW message has come the whole pattern is written between
    BeginRefresh() and EndRefresh(). Only the damaged right half may take
    it; the left half and the frame keep their colour. Nothing else runs in
    the refresh, as the bridge stops a program there for anything but
    drawing.

    The calls go through the LP macros of the NDK at the offsets of
    Picasso96API.fd and cybergraphics.fd, since the NDK has no Picasso96
    header.
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <inline/macros.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
static struct Library *P96Base, *CyberGfxBase;

struct RenderInfo {
    APTR Memory;
    WORD BytesPerRow;
    WORD pad;
    ULONG RGBFormat;
};

typedef char check_renderinfo[sizeof(struct RenderInfo) == 12 ? 1 : -1];

#define p96BestModeIDTagList(tags) \
    LP1(0x3c, ULONG, p96BestModeIDTagList, struct TagItem *, tags, a0, , P96Base)
#define p96GetModeIDAttr(id, attr) \
    LP2(0x54, ULONG, p96GetModeIDAttr, ULONG, id, d0, ULONG, attr, d1, , P96Base)
#define p96WritePixelArray(ri, sx, sy, rp, dx, dy, w, h) \
    LP8NR(0x66, p96WritePixelArray, struct RenderInfo *, ri, a0, UWORD, sx, d0, UWORD, sy, d1, \
          struct RastPort *, rp, a1, UWORD, dx, d2, UWORD, dy, d3, UWORD, w, d4, UWORD, h, d5, \
          , P96Base)
#define GetCyberIDAttr(attr, id) \
    LP2(0x66, ULONG, GetCyberIDAttr, ULONG, attr, d0, ULONG, id, d1, , CyberGfxBase)
#define ReadRGBPixel(rp, x, y) \
    LP3(0x6c, ULONG, ReadRGBPixel, struct RastPort *, rp, a1, UWORD, x, d0, UWORD, y, d1, \
        , CyberGfxBase)

/* Picasso96.h */
#define RGBFB_R8G8B8   2
#define RGBFB_B8G8R8   3
#define RGBFB_A8R8G8B8 6
#define RGBFB_A8B8G8R8 7
#define RGBFB_R8G8B8A8 8
#define RGBFB_B8G8R8A8 9
#define P96BIDTAG_FormatsAllowed   (TAG_USER + 97)
#define P96BIDTAG_FormatsForbidden (TAG_USER + 98)
#define P96BIDTAG_NominalWidth     (TAG_USER + 99)
#define P96BIDTAG_NominalHeight    (TAG_USER + 100)
#define P96BIDTAG_Depth            (TAG_USER + 101)
/* cybergraphics.h */
#define CYBRIDATTR_PIXFMT 0x80000001UL
#define CYBRIDATTR_BPPIX  0x80000005UL

#define PATTERN_W 8
#define PATTERN_H 4
#define SRC_X 3
#define SRC_Y 2
#define BUF_W 24
#define BUF_H 12
#define SENTINEL 0x00FF00FFUL

static UBYTE buffer[BUF_W * BUF_H * 4];
static BOOL strict;                 /* "gfx_p96 rgb" */

static ULONG pattern(ULONG i, ULONG j)
{
    return ((0x18 + 0x1C * i) << 16) | ((0x30 + 0x31 * j) << 8) | (0x40 + 0x0B * (i + j));
}

/* The buffer in rgbformat: the pattern at SrcX and SrcY, SENTINEL around. */
static ULONG fill(ULONG rgbformat)
{
    ULONG bpp = rgbformat == RGBFB_R8G8B8 || rgbformat == RGBFB_B8G8R8 ? 3 : 4;
    ULONG x, y;

    for (y = 0; y < BUF_H; y++)
        for (x = 0; x < BUF_W; x++)
        {
            ULONG c = (x >= SRC_X && x < SRC_X + PATTERN_W && y >= SRC_Y && y < SRC_Y + PATTERN_H)
                      ? pattern(x - SRC_X, y - SRC_Y) : SENTINEL;
            UBYTE r = c >> 16, g = c >> 8, b = c, *p = buffer + y * BUF_W * bpp + x * bpp;

            switch (rgbformat)
            {
                case RGBFB_A8R8G8B8: p[0] = 0xFF; p[1] = r; p[2] = g; p[3] = b; break;
                case RGBFB_B8G8R8A8: p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; break;
                case RGBFB_R8G8B8A8: p[0] = r; p[1] = g; p[2] = b; p[3] = 0xFF; break;
                case RGBFB_A8B8G8R8: p[0] = 0xFF; p[1] = b; p[2] = g; p[3] = r; break;
                case RGBFB_R8G8B8:   p[0] = r; p[1] = g; p[2] = b; break;
                case RGBFB_B8G8R8:   p[0] = b; p[1] = g; p[2] = r; break;
            }
        }
    return BUF_W * bpp;
}

static ULONG screen_depth(struct Window *win)
{
    return GetBitMapAttr(win->WScreen->RastPort.BitMap, BMA_DEPTH);
}

static void check_open(void)
{
    struct Library *again, *plain, *later, *old;

    check_long("the version of Picasso96API.library", P96Base->lib_Version, 2);
    again = OpenLibrary((CONST_STRPTR)"LIBS:Picasso96API.library", 0);
    check(again == P96Base, "OpenLibrary() by a path gives the same base");
    plain = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 1);
    check(plain == P96Base, "version 1 gives the same base");
    later = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 3);
    check(later == NULL, "version 3 is not there");
    old = OldOpenLibrary((CONST_STRPTR)"Picasso96API.library");
    check(old == P96Base, "OldOpenLibrary() gives the same base");
    if (again) CloseLibrary(again);
    if (plain) CloseLibrary(plain);
    if (later) CloseLibrary(later);
    if (old) CloseLibrary(old);
}

static void check_best_mode(void)
{
    struct TagItem tags[] = {
        { P96BIDTAG_NominalWidth, 640 },
        { P96BIDTAG_NominalHeight, 480 },
        { P96BIDTAG_Depth, 32 },
        { P96BIDTAG_FormatsAllowed,
          (1UL << RGBFB_A8R8G8B8) | (1UL << RGBFB_B8G8R8A8) | (1UL << RGBFB_R8G8B8A8) },
        { TAG_DONE, 0 }
    };
    struct TagItem none[] = {
        { P96BIDTAG_FormatsAllowed, 1UL << RGBFB_A8R8G8B8 },
        { P96BIDTAG_FormatsForbidden, 1UL << RGBFB_A8R8G8B8 },
        { TAG_DONE, 0 }
    };
    ULONG id = p96BestModeIDTagList(tags);

    if (id == (ULONG)INVALID_ID)
    {
        Printf("info p96BestModeIDTagList() of AmiPixelShop: INVALID_ID\n");
        if (strict)
            check(FALSE, "AmiPixelShop's mode request gives a mode");
        else
            Printf("info SKIP AmiPixelShop's mode: INVALID_ID is allowed without rgb\n");
    }
    else
    {
        ULONG pixfmt = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);

        Printf("info p96BestModeIDTagList() of AmiPixelShop: 0x%08lx, PIXFMT %lu\n", id, pixfmt);
        /* ARGB32, BGRA32, RGBA32 and the 0RGB32, BGR032 and RGB032 of AROS */
        check(pixfmt == 11 || pixfmt == 12 || pixfmt == 13 || pixfmt == 101 || pixfmt == 102 ||
              pixfmt == 103, "AmiPixelShop's mode request gives a mode of an allowed format");
        check_long("the bytes a pixel of that mode", GetCyberIDAttr(CYBRIDATTR_BPPIX, id), 4);
    }
    check(p96BestModeIDTagList(none) == (ULONG)INVALID_ID,
          "a format allowed and forbidden gives INVALID_ID");
    {
        static const ULONG depths[] = { 33, 64, 256 };
        struct TagItem deep[] = { { P96BIDTAG_Depth, 0 }, { TAG_DONE, 0 } };
        struct TagItem wide[] = { { P96BIDTAG_NominalWidth, 0x10320 }, { TAG_DONE, 0 } };
        int i;

        for (i = 0; i < 3; i++)
        {
            deep[0].ti_Data = depths[i];
            check(p96BestModeIDTagList(deep) == (ULONG)INVALID_ID,
                  "a depth above 32 gives INVALID_ID");
        }
        check(p96BestModeIDTagList(wide) == (ULONG)INVALID_ID,
              "a width above 0xffff gives INVALID_ID, not a cut one");
    }
}

static void check_pixels(struct Window *win)
{
    static const ULONG formats[] = {
        RGBFB_A8R8G8B8, RGBFB_B8G8R8A8, RGBFB_R8G8B8A8, RGBFB_A8B8G8R8, RGBFB_R8G8B8, RGBFB_B8G8R8
    };
    struct RastPort *rp = win->RPort;
    struct RenderInfo ri;
    ULONG f, i, j, x0, y0, before, depth = screen_depth(win), compared = 0;
    BOOL exact = depth >= 24;

    Printf("info screen depth %lu\n", depth);
    if (!exact)
    {
        if (strict)
        {
            check(FALSE, "the screen has 24 bits or more");
            return;
        }
        Printf("info SKIP readback: fewer than 24 bits, the patterns are written, not compared\n");
    }
    for (f = 0; f < 6; f++)
    {
        BOOL same = TRUE, around = TRUE;
        ULONG above[PATTERN_W + 2], below[PATTERN_W + 2];
        ULONG left[PATTERN_H], right[PATTERN_H];

        ri.Memory = buffer;
        ri.BytesPerRow = fill(formats[f]);
        ri.pad = 0;
        ri.RGBFormat = formats[f];
        x0 = win->BorderLeft + 4 + f * (PATTERN_W + 4);
        y0 = win->BorderTop + 4;
        /* Every background colour is valid. Keep all four borders, with
           their corners, so the comparison detects changes, not a colour. */
        if (exact)
        {
            for (i = 0; i < PATTERN_W + 2; i++)
            {
                above[i] = ReadRGBPixel(rp, x0 + i - 1, y0 - 1) & 0xFFFFFF;
                below[i] = ReadRGBPixel(rp, x0 + i - 1, y0 + PATTERN_H) & 0xFFFFFF;
            }
            for (j = 0; j < PATTERN_H; j++)
            {
                left[j] = ReadRGBPixel(rp, x0 - 1, y0 + j) & 0xFFFFFF;
                right[j] = ReadRGBPixel(rp, x0 + PATTERN_W, y0 + j) & 0xFFFFFF;
            }
        }
        p96WritePixelArray(&ri, SRC_X, SRC_Y, rp, x0, y0, PATTERN_W, PATTERN_H);
        Printf("info format %lu written at %lu,%lu\n", formats[f], x0, y0);
        if (!exact)
            continue;
        for (j = 0; j < PATTERN_H; j++)
            for (i = 0; i < PATTERN_W; i++)
                if ((ReadRGBPixel(rp, x0 + i, y0 + j) & 0xFFFFFF) != pattern(i, j))
                    same = FALSE;
        /* The buffer outside the copied rectangle must change no border. */
        for (j = 0; j < PATTERN_H; j++)
            if ((ReadRGBPixel(rp, x0 + PATTERN_W, y0 + j) & 0xFFFFFF) != right[j] ||
                (ReadRGBPixel(rp, x0 - 1, y0 + j) & 0xFFFFFF) != left[j])
                around = FALSE;
        for (i = 0; i < PATTERN_W + 2; i++)
            if ((ReadRGBPixel(rp, x0 + i - 1, y0 - 1) & 0xFFFFFF) != above[i] ||
                (ReadRGBPixel(rp, x0 + i - 1, y0 + PATTERN_H) & 0xFFFFFF) != below[i])
                around = FALSE;
        check(same, "the pattern reads back in its format, from SrcX and SrcY");
        check(around, "nothing around the pattern is written");
        compared++;
    }
    Printf("info formats compared: %lu of 6\n", compared);

    /* an empty rectangle writes nothing */
    ri.RGBFormat = RGBFB_A8R8G8B8;
    ri.BytesPerRow = fill(RGBFB_A8R8G8B8);
    x0 = win->BorderLeft + 4;
    y0 = win->BorderTop + 4 + PATTERN_H + 4;
    before = ReadRGBPixel(rp, x0, y0);
    p96WritePixelArray(&ri, SRC_X, SRC_Y, rp, x0, y0, 0, PATTERN_H);
    p96WritePixelArray(&ri, SRC_X, SRC_Y, rp, x0, y0, PATTERN_W, 0);
    check(ReadRGBPixel(rp, x0, y0) == before, "an empty rectangle writes nothing");

    /* past the right edge of the window: the columns inside are written */
    x0 = win->Width - PATTERN_W / 2;
    p96WritePixelArray(&ri, SRC_X, SRC_Y, rp, x0, y0, PATTERN_W, PATTERN_H);
    if (exact)
        check((ReadRGBPixel(rp, win->Width - 1, y0) & 0xFFFFFF) == pattern(PATTERN_W / 2 - 1, 0),
              "a rectangle past the window is written up to its edge");
    else
        Printf("info SKIP clipping readback: fewer than 24 bits\n");
}

#define FIELD 0x00123456UL          /* the colour around the write in the refresh */

/* Replies to the messages of win; TRUE if one of them was REFRESHWINDOW. */
static BOOL refresh_message(struct Window *win)
{
    struct IntuiMessage *msg;
    BOOL refresh = FALSE;

    while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
    {
        if (msg->Class == IDCMP_REFRESHWINDOW)
            refresh = TRUE;
        ReplyMsg((struct Message *)msg);
    }
    return refresh;
}

static void check_refresh(struct Screen *pub)
{
    struct Window *win, *cover;
    struct RastPort *rp;
    struct RenderInfo ri;
    ULONG i, j, c, x0, y0, wrong = 0, wrong_in = 0, wrong_out = 0, backfilled = 0;
    BOOL refresh = FALSE;
    int k;

    win = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 120, WA_InnerWidth, 2 * PATTERN_W + 8,
                         WA_InnerHeight, PATTERN_H + 8, WA_Title, (ULONG)"gfx_p96 refresh",
                         WA_PubScreen, (ULONG)pub, WA_SimpleRefresh, TRUE,
                         WA_IDCMP, IDCMP_REFRESHWINDOW, WA_Activate, FALSE, TAG_DONE);
    if (!check(win != NULL, "a simple refresh window that reports REFRESHWINDOW"))
        return;
    rp = win->RPort;
    x0 = win->BorderLeft + 4;
    y0 = win->BorderTop + 4;
    Printf("info screen depth %lu\n", screen_depth(win));
    if (!check(screen_depth(win) >= 24, "the screen has 24 bits or more"))
    {
        CloseWindow(win);
        return;
    }
    /* an empty refresh first, so that no damage of the opening is left,
       whether or not its message has come yet */
    Delay(25);
    refresh_message(win);
    BeginRefresh(win);
    EndRefresh(win, TRUE);

    /* the pattern's rectangle and its frame in FIELD, outside a refresh */
    for (i = 0; i < BUF_W * BUF_H; i++)
    {
        buffer[4 * i] = 0xFF;
        buffer[4 * i + 1] = (UBYTE)(FIELD >> 16);
        buffer[4 * i + 2] = (UBYTE)(FIELD >> 8);
        buffer[4 * i + 3] = (UBYTE)FIELD;
    }
    ri.Memory = buffer;
    ri.BytesPerRow = BUF_W * 4;
    ri.pad = 0;
    ri.RGBFormat = RGBFB_A8R8G8B8;
    p96WritePixelArray(&ri, 0, 0, rp, x0 - 1, y0 - 1, PATTERN_W + 2, PATTERN_H + 2);
    for (j = 0; j < PATTERN_H + 2; j++)
        for (i = 0; i < PATTERN_W + 2; i++)
            if ((ReadRGBPixel(rp, x0 - 1 + i, y0 - 1 + j) & 0xFFFFFF) != FIELD)
                wrong++;
    check(wrong == 0, "the rectangle and its frame have one colour before the refresh");
    refresh_message(win);

    /* real damage: a window of the program over the right half, closed */
    cover = OpenWindowTags(NULL, WA_Left, win->LeftEdge + x0 + PATTERN_W / 2,
                           WA_Top, win->TopEdge + y0, WA_Width, PATTERN_W / 2,
                           WA_Height, PATTERN_H, WA_Borderless, TRUE, WA_PubScreen, (ULONG)pub,
                           WA_IDCMP, 0, WA_Activate, FALSE, TAG_DONE);
    if (!check(cover != NULL, "a window over the right half of the pattern"))
    {
        CloseWindow(win);
        return;
    }
    if (!check(cover->LeftEdge == win->LeftEdge + (WORD)(x0 + PATTERN_W / 2) &&
               cover->TopEdge == win->TopEdge + (WORD)y0 && cover->Width == PATTERN_W / 2 &&
               cover->Height == PATTERN_H, "it covers exactly that half"))
    {
        CloseWindow(cover);
        CloseWindow(win);
        return;
    }
    CloseWindow(cover);
    for (k = 0; k < 100 && !refresh; k++)
        if (!(refresh = refresh_message(win)))
            Delay(2);
    if (!check(refresh, "closing it gives REFRESHWINDOW"))
    {
        CloseWindow(win);
        return;
    }
    for (j = 0; j < PATTERN_H; j++)
        for (i = PATTERN_W / 2; i < PATTERN_W; i++)
            if ((ReadRGBPixel(rp, x0 + i, y0 + j) & 0xFFFFFF) != FIELD)
                backfilled++;
    Printf("info damaged pixels that the backfill changed: %lu of %lu\n", backfilled,
           (ULONG)(PATTERN_H * PATTERN_W / 2));

    /* the whole pattern in the refresh, and nothing else there */
    ri.BytesPerRow = fill(RGBFB_A8R8G8B8);
    test_flush();
    BeginRefresh(win);
    p96WritePixelArray(&ri, SRC_X, SRC_Y, rp, x0, y0, PATTERN_W, PATTERN_H);
    EndRefresh(win, TRUE);

    for (j = 0; j < PATTERN_H + 2; j++)
        for (i = 0; i < PATTERN_W + 2; i++)
        {
            c = ReadRGBPixel(rp, x0 - 1 + i, y0 - 1 + j) & 0xFFFFFF;
            if (i > PATTERN_W / 2 && i <= PATTERN_W && j >= 1 && j <= PATTERN_H)
                wrong_in += c != pattern(i - 1, j - 1);
            else
                wrong_out += c != FIELD;
        }
    check(wrong_in == 0, "the write in the refresh fills the damaged half with the pattern");
    check(wrong_out == 0, "the undamaged half and the frame keep their colour");
    Printf("info refresh write: %lu damaged pixels compared, %lu around them\n",
           (ULONG)(PATTERN_H * PATTERN_W / 2),
           (ULONG)((PATTERN_W + 2) * (PATTERN_H + 2) - PATTERN_H * PATTERN_W / 2));
    CloseWindow(win);
}

int main(int argc, char **argv)
{
    struct Window *win;
    struct Screen *pub;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 41);
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    if (!check(IntuitionBase && GfxBase && CyberGfxBase, "intuition, graphics and cybergraphics") ||
        !check(P96Base != NULL, "OpenLibrary() of Picasso96API.library version 2"))
        return test_done("gfx_p96");

    strict = argc > 1 && !strcmp(argv[1], "rgb");
    if (strict)
        Printf("info strict rgb test\n");
    if (argc > 1 && !strcmp(argv[1], "unsupported"))
    {
        Printf("STEP p96GetModeIDAttr() is not bridged and must stop with 20\n");
        test_flush();
        p96GetModeIDAttr(0, 0);
        Printf("FAIL p96GetModeIDAttr() returned\n");
        return 10;
    }

    /* the screen locked, so that the window's WScreen is a 68k Screen of
       the program, as GetBitMapAttr() of its BitMap needs */
    pub = LockPubScreen(NULL);
    if (!check(pub != NULL, "LockPubScreen() of the default public screen"))
        return test_done("gfx_p96");
    win = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_InnerWidth, 6 * (PATTERN_W + 4) + 8,
                         WA_InnerHeight, 3 * (PATTERN_H + 4), WA_Title, (ULONG)"gfx_p96",
                         WA_PubScreen, (ULONG)pub, WA_Activate, FALSE, TAG_DONE);
    if (!check(win != NULL, "a window on the default public screen") ||
        !check(win->WScreen == pub, "the window lies on the locked screen"))
    {
        if (win)
            CloseWindow(win);
        UnlockPubScreen(NULL, pub);
        return test_done("gfx_p96");
    }

    if (argc > 1 && !strcmp(argv[1], "badri"))
    {
        Printf("STEP a NULL RenderInfo must stop with 20\n");
        test_flush();
        p96WritePixelArray(NULL, 0, 0, win->RPort, 0, 0, 1, 1);
        Printf("FAIL p96WritePixelArray() of a NULL RenderInfo returned\n");
        return 10;
    }

    if (argc > 1 && !strcmp(argv[1], "refresh"))
    {
        Printf("info refresh test\n");
        check_refresh(pub);
    }
    else
    {
        check_open();
        check_best_mode();
        check_pixels(win);
    }

    CloseWindow(win);
    UnlockPubScreen(NULL, pub);
    CloseLibrary(P96Base);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_p96");
}

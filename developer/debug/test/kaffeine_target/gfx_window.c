/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    A window on the default public screen and its RastPort, the way C
    programs use them: the fields of the Window, the RastPort and its font
    that programs read, pens, drawing modes and the pen position set through
    graphics.library and read back through it and through the fields,
    pixels drawn and read back, the size of text from TextLength(),
    TextExtent() and TextFit(), and topaz.font through OpenFont(), SetFont()
    and CloseFont(). The program does all that twice and checks that the
    second window returns all of its memory. Then windows on the public
    screen that LockPubScreen() gave: through WA_PubScreen, NULL for the
    default one, WA_PubScreenName with and without WA_PubScreenFallBack, and
    OpenWindow() with a NewWindow of each screen type.

    "gfx_window badscreen" opens a window with WA_PubScreen of something
    that is not a screen, which stops the program with rc 20.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stddef.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

/* The 68k layouts that programs read. */
typedef char check_window[sizeof(struct Window) == 136 ? 1 : -1];
typedef char check_rastport[sizeof(struct RastPort) == 100 ? 1 : -1];
typedef char check_textfont[sizeof(struct TextFont) == 52 ? 1 : -1];
typedef char check_textextent[sizeof(struct TextExtent) == 12 ? 1 : -1];

#define INNER_WIDTH 320
#define INNER_HEIGHT 160

static const char title[] = "gfx_window";

/* Fields as LONGs: GCC 6.5 can pass a BYTE copied with move.b as a LONG
   without extending it. */
static LONG byte_field(BYTE value)
{
    return (LONG)value & 0xFF;
}

static void window_fields(struct Window *win)
{
    check(win->LeftEdge == 40 && win->TopEdge == 40, "the window lies where it was asked to");
    check(win->BorderTop > 0 && win->BorderLeft >= 0 && win->BorderRight >= 0 &&
          win->BorderBottom >= 0, "the window has its borders");
    check_long("Width less the left and right borders",
               (LONG)win->Width - win->BorderLeft - win->BorderRight, INNER_WIDTH);
    check_long("Height less the top and bottom borders",
               (LONG)win->Height - win->BorderTop - win->BorderBottom, INNER_HEIGHT);
    check(win->Title == (UBYTE *)title, "the window keeps its title");
    check(win->RPort != NULL, "the window has a RastPort");
    check(win->UserPort == NULL && win->IDCMPFlags == 0,
          "a window without IDCMP flags has no IDCMP port");
}

static void rastport_fields(struct RastPort *rp)
{
    struct TextFont *font = rp->Font;

    if (!check(font != NULL, "the RastPort has a font"))
        return;
    check(rp->TxHeight > 0 && rp->TxBaseline < rp->TxHeight,
          "the RastPort has a text height and a baseline within it");
    check_long("TxHeight is tf_YSize of the font", rp->TxHeight, font->tf_YSize);
    check_long("TxWidth is tf_XSize of the font", rp->TxWidth, font->tf_XSize);
    check_long("TxBaseline is tf_Baseline of the font", rp->TxBaseline, font->tf_Baseline);
    check(font->tf_LoChar <= 'A' && font->tf_HiChar >= 'z', "the font has the letters");
}

static void pens(struct RastPort *rp)
{
    SetAPen(rp, 2);
    check_long("GetAPen() after SetAPen(2)", GetAPen(rp), 2);
    check_long("FgPen after SetAPen(2)", byte_field(rp->FgPen), 2);
    SetBPen(rp, 3);
    check_long("GetBPen() after SetBPen(3)", GetBPen(rp), 3);
    check_long("BgPen after SetBPen(3)", byte_field(rp->BgPen), 3);
    SetDrMd(rp, JAM2);
    check_long("GetDrMd() after SetDrMd(JAM2)", GetDrMd(rp), JAM2);
    check_long("DrawMode after SetDrMd(JAM2)", byte_field(rp->DrawMode), JAM2);
    SetABPenDrMd(rp, 1, 2, JAM1);
    check(GetAPen(rp) == 1 && GetBPen(rp) == 2 && GetDrMd(rp) == JAM1,
          "SetABPenDrMd(1, 2, JAM1) sets the pens and the mode");
    check(byte_field(rp->FgPen) == 1 && byte_field(rp->BgPen) == 2 &&
          byte_field(rp->DrawMode) == JAM1,
          "the fields follow SetABPenDrMd()");
    Move(rp, 10, 20);
    check(rp->cp_x == 10 && rp->cp_y == 20, "Move(10, 20) sets the pen position");
}

/* On a hi- or truecolor screen, AROS's ReadPixel() gives the low byte of
   the pixel rather than its pen, so the checks compare pixels drawn with the
   same pen and with different ones. */
static void pixels(struct Window *win, struct RastPort *rp)
{
    LONG x = win->BorderLeft, y = win->BorderTop, clear, drawn;

    SetAPen(rp, 0);
    RectFill(rp, x, y, x + 99, y + 49);
    clear = ReadPixel(rp, x + 5, y + 5);
    SetAPen(rp, 2);
    RectFill(rp, x + 10, y + 10, x + 29, y + 29);
    drawn = ReadPixel(rp, x + 20, y + 20);
    check(drawn != clear, "RectFill() with pen 2 changes the pixels");
    check_long("a pixel beside the rectangle stays cleared", ReadPixel(rp, x + 30, y + 20), clear);
    check_long("WritePixel() with pen 2", WritePixel(rp, x + 40, y + 40), 0);
    check_long("WritePixel() gives the pixel of RectFill() with the same pen",
               ReadPixel(rp, x + 40, y + 40), drawn);
    Move(rp, x + 50, y + 12);
    Draw(rp, x + 90, y + 12);
    check_long("Draw() with pen 2 gives that pixel too", ReadPixel(rp, x + 70, y + 12), drawn);
    check(rp->cp_x == x + 90 && rp->cp_y == y + 12, "Draw() leaves the pen at the end of the line");
    check_long("ReadPixel() outside the window", (LONG)ReadPixel(rp, 3000, 3000), -1);
    check_long("WritePixel() outside the window", WritePixel(rp, 3000, 3000), -1);
}

static void text(struct RastPort *rp)
{
    struct TextExtent te, fit;
    LONG len, width, x;

    len = TextLength(rp, (STRPTR)"Hello", 5);
    check(len > 0, "TextLength() of \"Hello\"");
    TextExtent(rp, (STRPTR)"Hello", 5, &te);
    check_long("te_Width of TextExtent() is TextLength()", te.te_Width, len);
    check_long("te_Height is TxHeight", te.te_Height, rp->TxHeight);
    check_long("te_Extent.MinY is minus TxBaseline", te.te_Extent.MinY, -(LONG)rp->TxBaseline);
    check_long("te_Extent.MaxY is below the baseline",
               te.te_Extent.MaxY, (LONG)rp->TxHeight - 1 - rp->TxBaseline);
    check(te.te_Extent.MinX <= 0 && te.te_Extent.MaxX >= te.te_Extent.MinX,
          "te_Extent spans the text from the pen position");
    width = te.te_Extent.MaxX - te.te_Extent.MinX + 1;
    check_long("TextFit() into the extent of \"Hello\"",
               TextFit(rp, (STRPTR)"Hello world", 11, &fit, NULL, 1, width, rp->TxHeight), 5);
    check_long("te_Width of TextFit() is that of \"Hello\"", fit.te_Width, len);
    check_long("TextFit() with too little height",
               TextFit(rp, (STRPTR)"Hello", 5, &fit, NULL, 1, width, rp->TxHeight - 1), 0);
    Move(rp, 20, 30 + rp->TxBaseline);
    x = rp->cp_x;
    Text(rp, (STRPTR)"Hello", 5);
    check_long("Text() moves the pen by TextLength()", rp->cp_x - x, len);
}

static void topaz(struct RastPort *rp)
{
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
    struct TextFont *old = rp->Font, *font = OpenFont(&ta);
    struct TextExtent te;

    if (!check(font != NULL, "OpenFont(topaz.font 8)"))
        return;
    check(font->tf_YSize == 8 && font->tf_XSize == 8 && !(font->tf_Flags & FPF_PROPORTIONAL),
          "topaz.font 8 is 8 by 8 and fixed");
    SetFont(rp, font);
    check(rp->Font == font && rp->TxHeight == 8 && rp->TxWidth == 8 &&
          rp->TxBaseline == font->tf_Baseline, "SetFont() gives the RastPort that font");
    check_long("TextLength() of \"Hello\" in topaz 8", TextLength(rp, (STRPTR)"Hello", 5), 40);
    TextExtent(rp, (STRPTR)"Hello", 5, &te);
    check(te.te_Width == 40 && te.te_Height == 8 && te.te_Extent.MinX == 0 &&
          te.te_Extent.MaxX == 39, "TextExtent() of \"Hello\" in topaz 8");
    SetFont(rp, old);
    check(rp->Font == old, "SetFont() gives the RastPort its old font back");
    CloseFont(font);
}

static void window_round(LONG n)
{
    struct Window *win;

    Printf("round %ld\n", n);
    win = OpenWindowTags(NULL,
                         WA_Left, 40, WA_Top, 40,
                         WA_InnerWidth, INNER_WIDTH, WA_InnerHeight, INNER_HEIGHT,
                         WA_Title, (ULONG)title,
                         WA_DragBar, TRUE, WA_DepthGadget, TRUE,
                         WA_SmartRefresh, TRUE, WA_NoCareRefresh, TRUE,
                         TAG_DONE);
    if (!check(win != NULL, "OpenWindowTags()"))
        return;
    window_fields(win);
    if (win->RPort)
    {
        rastport_fields(win->RPort);
        pens(win->RPort);
        pixels(win, win->RPort);
        text(win->RPort);
        topaz(win->RPort);
    }
    CloseWindow(win);
}

/* A window that should lie on scr; a visitor window or not. */
static void on_screen(struct Window *win, struct Screen *scr, BOOL visitor, const char *what)
{
    if (!check(win != NULL, what))
        return;
    check(win->WScreen == scr, "the window lies on the public screen");
    check(!(win->Flags & WFLG_VISITOR) == !visitor,
          visitor ? "as a visitor window" : "not as a visitor window");
    CloseWindow(win);
}

/* Windows on the public screen that LockPubScreen(NULL) gave, the default
   one, which is the Workbench here. */
static void public_screen(void)
{
    struct NewWindow nw = { 10, 15, 260, 100, 0xFF, 0xFF, 0, WFLG_DRAGBAR | WFLG_DEPTHGADGET,
                            NULL, NULL, (UBYTE *)title, NULL, NULL, 0, 0, 0, 0, PUBLICSCREEN };
    struct Screen *scr = LockPubScreen(NULL);
    ULONG avail;

    if (!check(scr != NULL, "LockPubScreen(NULL)"))
        return;
    avail = AvailMem(MEMF_ANY);
    on_screen(OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Left, 10, WA_Top, 15,
                             WA_InnerWidth, 240, WA_InnerHeight, 80, TAG_DONE),
              scr, TRUE, "OpenWindowTags() with WA_PubScreen of LockPubScreen(NULL)");
    on_screen(OpenWindowTags(NULL, WA_PubScreen, 0, WA_Left, 10, WA_Top, 15,
                             WA_InnerWidth, 240, WA_InnerHeight, 80, TAG_DONE),
              scr, TRUE, "WA_PubScreen NULL, the default public screen");
    on_screen(OpenWindowTags(NULL, WA_PubScreenName, (ULONG)"kaffeine no such screen",
                             WA_PubScreenFallBack, TRUE, WA_Left, 10, WA_Top, 15,
                             WA_InnerWidth, 240, WA_InnerHeight, 80, TAG_DONE),
              scr, TRUE, "a WA_PubScreenName that is not there, with WA_PubScreenFallBack");
    check(OpenWindowTags(NULL, WA_PubScreenName, (ULONG)"kaffeine no such screen",
                         WA_Left, 10, WA_Top, 15, WA_InnerWidth, 240, WA_InnerHeight, 80,
                         TAG_DONE) == NULL,
          "without WA_PubScreenFallBack no window opens");

    nw.Screen = scr;
    on_screen(OpenWindow(&nw), scr, TRUE, "OpenWindow() of Type PUBLICSCREEN");
    nw.Screen = NULL;
    on_screen(OpenWindow(&nw), scr, TRUE, "OpenWindow() of Type PUBLICSCREEN with NULL");
    nw.Type = WBENCHSCREEN;
    on_screen(OpenWindow(&nw), scr, TRUE, "OpenWindow() of Type WBENCHSCREEN");
    nw.Type = CUSTOMSCREEN;
    nw.Screen = scr;
    on_screen(OpenWindow(&nw), scr, FALSE, "OpenWindow() of Type CUSTOMSCREEN on that screen");
    check_long("the windows on the public screen return all of their memory",
               AvailMem(MEMF_ANY), avail);
    UnlockPubScreen(NULL, scr);
}

/* The negative probe, apart from the battery: the bridge must stop the
   program before Intuition reads a Screen at the address of the title. */
static void bad_screen(void)
{
    struct Window *win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)title, WA_Left, 10, WA_Top, 15,
                                        WA_InnerWidth, 240, WA_InnerHeight, 80, TAG_DONE);

    check(FALSE, "WA_PubScreen of something that is not a screen unexpectedly returned");
    if (win)
        CloseWindow(win);
}

int main(int argc, char **argv)
{
    ULONG avail;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    if (check(IntuitionBase && GfxBase, "intuition.library and graphics.library 37"))
    {
        if (argc == 2 && !strcmp(argv[1], "badscreen"))
            bad_screen();
        else
        {
            window_round(1);
            avail = AvailMem(MEMF_ANY);
            window_round(2);
            check_long("the second window returns all of its memory", AvailMem(MEMF_ANY), avail);
            public_screen();
        }
    }
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_window");
}

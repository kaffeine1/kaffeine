/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Intuition and graphics manual thunks shared by kaffeine.library and the
    host dispatch tests. The graphics calls here take the 68k RastPort of a
    window or a screen (kaffeine_rastport.c). Any other RastPort, such as one
    that the program set up itself, stops the program with return code 20
    instead of reaching native graphics.library.
*/

#include <aros/debug.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/graphics.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_fontattr.h"
#include "kaffeine_thunks_gfx.h"

extern void *shadow_lookup(struct KaffeineContext *ctx, ULONG m68k_addr);

/* The 68k TextExtent of the NDK */
enum { G_TE_WIDTH = 0, G_TE_HEIGHT = 2, G_TE_MINX = 4, G_TE_MINY = 6, G_TE_MAXX = 8, G_TE_MAXY = 10 };

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* Stop the program with return code 20. The thunk returns it, so that it
   reaches D0, from which the CPU loop takes the exit code. */
static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* For the generated thunks: the native RastPort behind the 68k RastPort of
   a window or a screen, or as before the native object of another shadow
   or the guest bytes themselves. */
struct RastPort *resolve_rp(struct KaffeineContext *ctx, ULONG m68k_rp)
{
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, m68k_rp);

    if (rp) return rp;
    rp = (struct RastPort *)shadow_lookup(ctx, m68k_rp);
    if (rp) return rp;
    return (struct RastPort *)m68k_to_host(ctx, m68k_rp);
}

/* For the manual thunks: only the 68k RastPort of a window or a screen.
   For anything else the program stops, and the thunk returns
   ctx->exit_code. */
static struct RastPort *rastport(struct KaffeineContext *ctx, ULONG guest, const char *call)
{
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);

    if (!rp)
    {
        bug("[kaffeine] %s(%08lx): not the RastPort of a window or a screen\n",
            call, (unsigned long)guest);
        refuse(ctx, "only the RastPort of a window or a screen reaches graphics.library");
    }
    return rp;
}

/* The count of Text(), TextLength() and TextExtent() is a WORD, and their
   loops read UWORD(count) characters. */
static CONST_STRPTR guest_text(struct KaffeineContext *ctx, ULONG string, ULONG count)
{
    count &= 0xFFFF;
    if (!count)
        return (CONST_STRPTR)"";
    return guest_range(ctx, string, count) ? (CONST_STRPTR)ctx->mem + string : NULL;
}

static void put_textextent(struct KaffeineContext *ctx, ULONG guest, const struct TextExtent *te)
{
    m68k_write16(ctx, guest + G_TE_WIDTH, te->te_Width);
    m68k_write16(ctx, guest + G_TE_HEIGHT, te->te_Height);
    m68k_write16(ctx, guest + G_TE_MINX, (UWORD)te->te_Extent.MinX);
    m68k_write16(ctx, guest + G_TE_MINY, (UWORD)te->te_Extent.MinY);
    m68k_write16(ctx, guest + G_TE_MAXX, (UWORD)te->te_Extent.MaxX);
    m68k_write16(ctx, guest + G_TE_MAXY, (UWORD)te->te_Extent.MaxY);
}

/* intuition -816: SetWindowPointerA(A0=window, A1=taglist) */
IPTR thunk_intuition_SetWindowPointerA(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0; /* pointer shape is cosmetic, skip to avoid X11 blocking */
}

/* graphics -72: OpenFont(A0=textAttr) -> D0=font, a 68k TextFont */
IPTR thunk_gfx_OpenFont(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineFontAttr input;

    if (!Kaffeine_FontAttrIn(ctx, THUNK_A(0), &input))
        return 0;
    return Kaffeine_FontAdopt(ctx, OpenFont((struct TextAttr *)&input.attr));
}

/* graphics -78: CloseFont(A1=textFont) */
IPTR thunk_gfx_CloseFont(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct TextFont *font;

    if (!guest)
        return 0;
    font = Kaffeine_FontNative(ctx, guest);
    if (!font)
    {
        bug("[kaffeine] CloseFont(%08lx): not a font of graphics.library\n", (unsigned long)guest);
        return 0;
    }
    Kaffeine_FontClosed(ctx, guest);
    return 0;
}

/* graphics -66: SetFont(A1=rp, A0=textFont). As AROS, a NULL font is the
   default font. */
IPTR thunk_gfx_SetFont(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    struct TextFont *font = NULL;
    struct RastPort *rp;

    if (guest && !(font = Kaffeine_FontNative(ctx, guest)))
        return refuse(ctx, "SetFont(): the font is not one of graphics.library");
    rp = rastport(ctx, THUNK_A(1), "SetFont");
    if (!rp)
        return ctx->exit_code;
    SetFont(rp, font);
    return 0;
}

/* graphics -54: TextLength(A1=rp, A0=string, D0=count) -> D0 */
IPTR thunk_gfx_TextLength(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    struct RastPort *rp;

    if (!text)
        return refuse(ctx, "TextLength(): the text is not in guest memory");
    rp = rastport(ctx, THUNK_A(1), "TextLength");
    return rp ? (IPTR)(LONG)TextLength(rp, text, THUNK_D(0)) : (IPTR)ctx->exit_code;
}

/* graphics -60: Text(A1=rp, A0=string, D0=count) */
IPTR thunk_gfx_Text(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    struct RastPort *rp;

    if (!text)
        return refuse(ctx, "Text(): the text is not in guest memory");
    rp = rastport(ctx, THUNK_A(1), "Text");
    if (!rp)
        return ctx->exit_code;
    Text(rp, text, THUNK_D(0));
    return 0;
}

/* graphics -690: TextExtent(A1=rp, A0=string, D0=count, A2=textExtent).
   The native result goes to the 68k TextExtent field by field. */
IPTR thunk_gfx_TextExtent(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    ULONG extent = THUNK_A(2);
    struct TextExtent te;
    struct RastPort *rp;

    if (!text || !guest_range(ctx, extent, M68K_SIZEOF_TEXTEXTENT))
        return refuse(ctx, "TextExtent(): the text or the TextExtent is not in guest memory");
    rp = rastport(ctx, THUNK_A(1), "TextExtent");
    if (!rp)
        return ctx->exit_code;
    TextExtent(rp, text, THUNK_D(0), &te);
    put_textextent(ctx, extent, &te);
    return 0;
}

/* graphics -696: TextFit(A1=rp, A0=string, D0=strLen, A2=textExtent,
   A3=constrainingExtent, D1=strDirection, D2=constrainingBitWidth,
   D3=constrainingBitHeight) -> D0. AROS reads strLen characters from
   string on, a WORD strDirection apart. */
IPTR thunk_gfx_TextFit(struct KaffeineContext *ctx, void *cpu)
{
    ULONG string = THUNK_A(0), length = THUNK_D(0) & 0xFFFF;
    ULONG extent = THUNK_A(2), constraint = THUNK_A(3);
    LONG step = (WORD)THUNK_D(1);
    struct TextExtent te, limit;
    struct RastPort *rp;
    ULONG fitted;

    if (length)
    {
        /* first and last character, which bound every one in between */
        long long last = (long long)string + (long long)(length - 1) * step;

        if (!guest_range(ctx, string, 1) || last < 0 || last >= ctx->mem_size ||
            !guest_range(ctx, (ULONG)last, 1))
            return refuse(ctx, "TextFit(): the text is not in guest memory");
    }
    if (!guest_range(ctx, extent, M68K_SIZEOF_TEXTEXTENT) ||
        (constraint && !guest_range(ctx, constraint, M68K_SIZEOF_TEXTEXTENT)))
        return refuse(ctx, "TextFit(): a TextExtent is not in guest memory");
    if (constraint)
    {
        limit.te_Width = m68k_read16(ctx, constraint + G_TE_WIDTH);
        limit.te_Height = m68k_read16(ctx, constraint + G_TE_HEIGHT);
        limit.te_Extent.MinX = (WORD)m68k_read16(ctx, constraint + G_TE_MINX);
        limit.te_Extent.MinY = (WORD)m68k_read16(ctx, constraint + G_TE_MINY);
        limit.te_Extent.MaxX = (WORD)m68k_read16(ctx, constraint + G_TE_MAXX);
        limit.te_Extent.MaxY = (WORD)m68k_read16(ctx, constraint + G_TE_MAXY);
    }
    rp = rastport(ctx, THUNK_A(1), "TextFit");
    if (!rp)
        return ctx->exit_code;
    fitted = TextFit(rp, length ? (CONST_STRPTR)ctx->mem + string : (CONST_STRPTR)"",
                     length, &te, constraint ? &limit : NULL, step,
                     THUNK_D(2), THUNK_D(3));
    put_textextent(ctx, extent, &te);
    return fitted;
}

/* graphics -240: Move(A1=rp, D0=x, D1=y) */
IPTR thunk_gfx_Move(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "Move");

    if (!rp)
        return ctx->exit_code;
    Move(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    return 0;
}

/* graphics -246: Draw(A1=rp, D0=x, D1=y) */
IPTR thunk_gfx_Draw(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "Draw");

    if (!rp)
        return ctx->exit_code;
    Draw(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    return 0;
}

/* graphics -306: RectFill(A1=rp, D0=xMin, D1=yMin, D2=xMax, D3=yMax) */
IPTR thunk_gfx_RectFill(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "RectFill");

    if (!rp)
        return ctx->exit_code;
    RectFill(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1), (WORD)THUNK_D(2), (WORD)THUNK_D(3));
    return 0;
}

/* graphics -318: ReadPixel(A1=rp, D0=x, D1=y) -> D0 */
IPTR thunk_gfx_ReadPixel(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "ReadPixel");

    return rp ? (IPTR)ReadPixel(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1)) : (IPTR)ctx->exit_code;
}

/* graphics -324: WritePixel(A1=rp, D0=x, D1=y) -> D0 */
IPTR thunk_gfx_WritePixel(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "WritePixel");

    return rp ? (IPTR)WritePixel(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1)) : (IPTR)ctx->exit_code;
}

/* graphics -342: SetAPen(A1=rp, D0=pen) */
IPTR thunk_gfx_SetAPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "SetAPen");

    if (!rp)
        return ctx->exit_code;
    SetAPen(rp, THUNK_D(0));
    return 0;
}

/* graphics -348: SetBPen(A1=rp, D0=pen) */
IPTR thunk_gfx_SetBPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "SetBPen");

    if (!rp)
        return ctx->exit_code;
    SetBPen(rp, THUNK_D(0));
    return 0;
}

/* graphics -354: SetDrMd(A1=rp, D0=mode) */
IPTR thunk_gfx_SetDrMd(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "SetDrMd");

    if (!rp)
        return ctx->exit_code;
    SetDrMd(rp, THUNK_D(0));
    return 0;
}

/* graphics -894: SetABPenDrMd(A1=rp, D0=apen, D1=bpen, D2=mode) */
IPTR thunk_gfx_SetABPenDrMd(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "SetABPenDrMd");

    if (!rp)
        return ctx->exit_code;
    SetABPenDrMd(rp, THUNK_D(0), THUNK_D(1), THUNK_D(2));
    return 0;
}

/* graphics -858: GetAPen(A0=rp) -> D0 */
IPTR thunk_gfx_GetAPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(0), "GetAPen");

    return rp ? (IPTR)GetAPen(rp) : (IPTR)ctx->exit_code;
}

/* graphics -864: GetBPen(A0=rp) -> D0 */
IPTR thunk_gfx_GetBPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(0), "GetBPen");

    return rp ? (IPTR)GetBPen(rp) : (IPTR)ctx->exit_code;
}

/* graphics -870: GetDrMd(A0=rp) -> D0 */
IPTR thunk_gfx_GetDrMd(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(0), "GetDrMd");

    return rp ? (IPTR)GetDrMd(rp) : (IPTR)ctx->exit_code;
}

/* graphics -876: GetOutlinePen(A0=rp) -> D0 */
IPTR thunk_gfx_GetOutlinePen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(0), "GetOutlinePen");

    return rp ? (IPTR)GetOutlinePen(rp) : (IPTR)ctx->exit_code;
}

/* graphics -396: ScrollRaster(A1=rp, D0=dx, D1=dy, D2=xMin, D3=yMin, D4=xMax, D5=yMax) */
IPTR thunk_gfx_ScrollRaster(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = rastport(ctx, THUNK_A(1), "ScrollRaster");

    if (!rp)
        return ctx->exit_code;
    ScrollRaster(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1),
                 (WORD)THUNK_D(2), (WORD)THUNK_D(3),
                 (WORD)THUNK_D(4), (WORD)THUNK_D(5));
    return 0;
}

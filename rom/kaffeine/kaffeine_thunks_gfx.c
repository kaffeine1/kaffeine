/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Intuition and graphics manual thunks shared by kaffeine.library and the
    host dispatch tests. The graphics calls here take the 68k RastPort of a
    window or a screen (kaffeine_rastport.c). ReadPixel also reads bounded,
    unlayered guest planar bitmaps directly. Caller-owned classic RastPorts support bounded initialization and font-only
    operations through temporary native structures. Other guest-built RastPorts
    stop with return code 20 instead of reaching native graphics.library.
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

/* The 68k TextExtent, RastPort and TextFont of the NDK */
enum { G_TE_WIDTH = 0, G_TE_HEIGHT = 2, G_TE_MINX = 4, G_TE_MINY = 6, G_TE_MAXX = 8, G_TE_MAXY = 10 };
enum { G_RP_FONT = 52, G_RP_TXHEIGHT = 58, G_RP_TXWIDTH = 60,
       G_RP_TXBASELINE = 62, G_RP_TXSPACING = 64, G_LN_NAME = 10 };

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* Whether two guest ranges share a byte, without wrapping near 2^32 */
static BOOL overlaps(ULONG a, ULONG a_size, ULONG b, ULONG b_size)
{
    return (unsigned long long)a < (unsigned long long)b + b_size &&
           (unsigned long long)b < (unsigned long long)a + a_size;
}

/* Whether a guest range shares a byte with the block of a 68k TextFont,
   whose name and tables the bridge owns */
static BOOL in_font(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->fonts[i].guest &&
            overlaps(addr, size, ctx->fonts[i].guest, ctx->fonts[i].allocation_size))
            return TRUE;
    return FALSE;
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

/* A caller-owned classic RastPort may be used for font operations without
   registering a persistent native object. Never reinterpret its bytes as
   native pointers, or accept the storage of a registered graphics object. */
static BOOL plain_rastport(struct KaffeineContext *ctx, ULONG guest)
{
    UWORD i;
    if ((guest & 1) || !guest_range(ctx, guest, M68K_SIZEOF_RASTPORT) ||
        in_font(ctx, guest, M68K_SIZEOF_RASTPORT))
        return FALSE;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == guest ||
            (ctx->shadow_map[i].type == KAFFEINE_SHADOW_RASTPORT &&
             overlaps(guest, M68K_SIZEOF_RASTPORT, ctx->shadow_map[i].m68k_addr, M68K_SIZEOF_RASTPORT)))
            return FALSE;
    return TRUE;
}

/* graphics -198: initialize exactly the classic 100 bytes, using a
   temporary native RastPort. The default font has a context-owned name
   and snapshot; no native pointer or temporary native object escapes. */
IPTR thunk_gfx_InitRastPort(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1), font;
    struct RastPort rp;
    if (!plain_rastport(ctx, guest))
        return refuse(ctx, "InitRastPort(): not caller-owned classic RastPort storage");
    InitRastPort(&rp);
    if (!rp.Font || !(font = Kaffeine_FontGuest(ctx, rp.Font)))
        return refuse(ctx, "InitRastPort(): no guest copy of the default font");
    Kaffeine_FontPin(ctx, font);
    memset(ctx->mem + guest, 0, M68K_SIZEOF_RASTPORT);
    Kaffeine_RastPortOut(ctx, guest, &rp);
    return 0;
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
    ULONG guest = THUNK_A(0), port = THUNK_A(1), shown;
    struct TextFont *font = NULL;
    struct RastPort *rp, temporary;

    if (guest && !(font = Kaffeine_FontNative(ctx, guest)))
        return refuse(ctx, "SetFont(): the font is not one of graphics.library");
    rp = Kaffeine_RastPortIn(ctx, port);
    if (!rp)
    {
        if (!plain_rastport(ctx, port))
            return refuse(ctx, "SetFont(): not supported RastPort storage");
        memset(&temporary, 0, sizeof(temporary));
        SetFont(&temporary, font);
        shown = Kaffeine_FontGuest(ctx, temporary.Font);
        if (!shown) return refuse(ctx, "SetFont(): no guest font snapshot");
        if (!guest) Kaffeine_FontPin(ctx, shown);
        m68k_write32(ctx, port + G_RP_FONT, shown);
        m68k_write16(ctx, port + G_RP_TXWIDTH, temporary.TxWidth);
        m68k_write16(ctx, port + G_RP_TXHEIGHT, temporary.TxHeight);
        m68k_write16(ctx, port + G_RP_TXBASELINE, temporary.TxBaseline);
        return 0;
    }
    SetFont(rp, font);
    return 0;
}

/* graphics -474: AskFont(A1=rp, A0=textAttr). The classic 8-byte TextAttr
   gets the attributes of native AskFont() of the RastPort, which as AROS
   are those of the font itself (tf_Style, not the RastPort's AlgoStyle),
   big-endian: name, YSize, Style, Flags. FSF_TAGGED is not passed on, as
   the 68k TextFont has no extension and an 8-byte TextAttr no tags. The
   name is the copy in the 68k TextFont of that font (Kaffeine_FontGuest()),
   never a native pointer; it stays while a 68k RastPort shows the font or
   the program holds it open, as the name of a native TextFont does. The
   TextAttr must lie whole and word-aligned in guest memory, outside the
   RastPort and every 68k TextFont, and the RastPort must show the 68k
   TextFont of its native font; anything else, a RastPort without a font
   and a font without a 68k TextFont stop the program before native
   AskFont(). */
IPTR thunk_gfx_AskFont(struct KaffeineContext *ctx, void *cpu)
{
    ULONG attr = THUNK_A(0), guest = THUNK_A(1), font, name = 0;
    struct TextAttr ta;
    struct TextFont *tf;
    struct RastPort *rp, temporary;

    if ((attr & 1) || !guest_range(ctx, attr, M68K_SIZEOF_TEXTATTR) ||
        overlaps(attr, M68K_SIZEOF_TEXTATTR, guest, M68K_SIZEOF_RASTPORT) ||
        in_font(ctx, attr, M68K_SIZEOF_TEXTATTR))
        return refuse(ctx, "AskFont(): the TextAttr is not free guest memory");
    rp = Kaffeine_RastPortIn(ctx, guest);
    if (!rp)
    {
        if (!plain_rastport(ctx, guest) ||
            !(tf = Kaffeine_FontNative(ctx, m68k_read32(ctx, guest + G_RP_FONT))))
            return refuse(ctx, "AskFont(): no registered font in the classic RastPort");
        memset(&temporary, 0, sizeof(temporary));
        temporary.Font = tf;
        rp = &temporary;
    }
    /* A font that the program wrote to rp->Font itself is not the one that
       native graphics.library has; native AskFont() of no font crashes. */
    tf = rp->Font;
    if (!tf || m68k_read32(ctx, guest + G_RP_FONT) != Kaffeine_FontFind(ctx, tf))
        return refuse(ctx, "AskFont(): the RastPort does not show its native font");
    font = Kaffeine_FontGuest(ctx, tf);
    /* a new 68k TextFont may not take the TextAttr's place either */
    if (!font || in_font(ctx, attr, M68K_SIZEOF_TEXTATTR))
        return refuse(ctx, "AskFont(): no 68k TextFont for the font of the RastPort");
    AskFont(rp, &ta);
    /* Kaffeine_FontGuest() copies the name right after the TextFont. */
    if ((const char *)ta.ta_Name != (const char *)tf->tf_Message.mn_Node.ln_Name ||
        m68k_read32(ctx, font + G_LN_NAME) != (ta.ta_Name ? font + M68K_SIZEOF_TEXTFONT : 0))
        return refuse(ctx, "AskFont(): the font name has no 68k copy");
    if (ta.ta_Name)
        name = font + M68K_SIZEOF_TEXTFONT;
    m68k_write32(ctx, attr + M68K_TA_NAME, name);
    m68k_write16(ctx, attr + M68K_TA_YSIZE, ta.ta_YSize);
    ctx->mem[attr + M68K_TA_STYLE] = ta.ta_Style & (UBYTE)~FSF_TAGGED;
    ctx->mem[attr + M68K_TA_FLAGS] = ta.ta_Flags;
    return 0;
}

/* graphics -54: TextLength(A1=rp, A0=string, D0=count) -> D0 */
IPTR thunk_gfx_TextLength(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    ULONG guest = THUNK_A(1);
    struct RastPort *rp, temporary;
    struct TextFont *font;

    if (!text)
        return refuse(ctx, "TextLength(): the text is not in guest memory");
    rp = Kaffeine_RastPortIn(ctx, guest);
    if (!rp)
    {
        if (!plain_rastport(ctx, guest) ||
            !(font = Kaffeine_FontNative(ctx, m68k_read32(ctx, guest + G_RP_FONT))))
            return refuse(ctx, "TextLength(): no registered font in the classic RastPort");
        /* Native TextLength reads only Font and signed TxSpacing. */
        memset(&temporary, 0, sizeof(temporary));
        temporary.Font = font;
        temporary.TxSpacing = (WORD)m68k_read16(ctx, guest + G_RP_TXSPACING);
        rp = &temporary;
    }
    return (IPTR)(LONG)TextLength(rp, text, THUNK_D(0));
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

/* Read-only classic planar access. No guest structure or plane pointer is
   converted into a native RastPort, BitMap or graphics driver object. */
static IPTR guest_read_pixel(struct KaffeineContext *ctx, ULONG guest, WORD x, WORD y)
{
    ULONG bitmap, row, rows, depth, bytes, offset, pen = 0, planes[8];
    unsigned i;
    UBYTE mask;

    if ((guest & 1) || !guest_range(ctx, guest, 100) || shadow_lookup(ctx, guest) ||
        m68k_read32(ctx, guest) != 0)
        return refuse(ctx, "ReadPixel: unsupported guest RastPort");
    bitmap = m68k_read32(ctx, guest + 4);
    if ((bitmap & 1) || !guest_range(ctx, bitmap, 40) || shadow_lookup(ctx, bitmap))
        return refuse(ctx, "ReadPixel: invalid guest BitMap");
    row = m68k_read16(ctx, bitmap);
    rows = m68k_read16(ctx, bitmap + 2);
    depth = ctx->mem[bitmap + 5];
    /* Support the plain classic layout (Flags 0 or BMF_STANDARD). Extended
       formats, layers and driver-owned bitmaps require their own adapters. */
    if (!row || (row & 1) || !rows || !depth || depth > 8 ||
        (ctx->mem[bitmap + 4] & ~8U) || m68k_read16(ctx, bitmap + 6))
        return refuse(ctx, "ReadPixel: unsupported guest planar BitMap");
    /* Both factors are UWORDs, so this product cannot overflow ULONG. */
    bytes = row * rows;
    for (i = 0; i < depth; ++i)
    {
        planes[i] = m68k_read32(ctx, bitmap + 8 + 4 * i);
        if (!guest_range(ctx, planes[i], bytes))
            return refuse(ctx, "ReadPixel: guest bitplane outside arena");
    }
    if (x < 0 || y < 0 || (ULONG)y >= rows || ((ULONG)x >> 3) >= row)
        return refuse(ctx, "ReadPixel: coordinate outside guest planar BitMap");
    offset = (ULONG)y * row + ((ULONG)x >> 3);
    mask = 0x80U >> (x & 7);
    for (i = 0; i < depth; ++i)
        if (ctx->mem[planes[i] + offset] & mask)
            pen |= 1UL << i;
    return pen;
}

/* graphics -318: ReadPixel(A1=rp, D0=x, D1=y) -> D0 */
IPTR thunk_gfx_ReadPixel(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);

    if (rp)
        return (IPTR)ReadPixel(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    return guest_read_pixel(ctx, guest, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
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

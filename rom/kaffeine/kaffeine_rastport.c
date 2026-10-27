/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    RastPorts and fonts of 68k code. The RastPort of a window or a screen
    is a 68k RastPort in the guest arena that stands for the native one.
    Before a graphics call, the fields that a program may write go to the
    native RastPort: the pens, the drawing mode, the write mask, the line
    pattern, the pen position and the text spacing. Right after the call,
    the fields that programs read come back, among them the font as a 68k
    TextFont with the metrics of the native one. Pointers to native layers,
    bitmaps and area buffers are not shown to 68k code: those fields stay
    as the program left them. The screen and window bridges may publish
    a typed screen BitMap handle there; drawing preserves that identity.
    68k TextFonts live in a table of their own.
    The font that a window or a screen comes with stays for the whole
    program, because programs keep rp->Font to set it back later; a font
    that the program opened goes once it is closed and no 68k RastPort
    shows it.
*/

#include "kaffeine_intern.h"
#include "kaffeine_rastport.h"

#include <aros/debug.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/graphics.h>
#include <string.h>

/* The 68k RastPort and TextFont of the NDK */
enum
{
    G_RP_MASK = 24, G_RP_FGPEN = 25, G_RP_BGPEN = 26, G_RP_AOLPEN = 27,
    G_RP_DRAWMODE = 28, G_RP_AREAPTSZ = 29, G_RP_LINPATCNT = 30, G_RP_FLAGS = 32,
    G_RP_LINEPTRN = 34, G_RP_CP_X = 36, G_RP_CP_Y = 38, G_RP_MINTERMS = 40,
    G_RP_PENWIDTH = 48, G_RP_PENHEIGHT = 50, G_RP_FONT = 52, G_RP_ALGOSTYLE = 56,
    G_RP_TXFLAGS = 57, G_RP_TXHEIGHT = 58, G_RP_TXWIDTH = 60, G_RP_TXBASELINE = 62,
    G_RP_TXSPACING = 64, G_RASTPORT_SIZE = 100,
    G_LN_TYPE = 8, G_LN_NAME = 10,
    G_TF_YSIZE = 20, G_TF_STYLE = 22, G_TF_FLAGS = 23, G_TF_XSIZE = 24,
    G_TF_BASELINE = 26, G_TF_BOLDSMEAR = 28, G_TF_ACCESSORS = 30,
    G_TF_LOCHAR = 32, G_TF_HICHAR = 33, G_TF_CHARDATA = 34, G_TF_MODULO = 38,
    G_TF_CHARLOC = 40, G_TF_CHARSPACE = 44, G_TF_CHARKERN = 48
};

/* ── Shadow map ──────────────────────────────────────────────────── */

void *Kaffeine_ShadowNative(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{
    UWORD i;

    if (!guest)
        return NULL;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == guest && ctx->shadow_map[i].type == type)
            return ctx->shadow_map[i].native_ptr;
    return NULL;
}

ULONG Kaffeine_ShadowGuest(struct KaffeineContext *ctx, const void *native, UWORD type)
{
    UWORD i;

    if (!native)
        return 0;
    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].native_ptr == native && ctx->shadow_map[i].type == type)
            return ctx->shadow_map[i].m68k_addr;
    return 0;
}

BOOL Kaffeine_ShadowAdd(struct KaffeineContext *ctx, ULONG guest, void *native, UWORD type)
{
    if (ctx->num_shadows >= KAFFEINE_MAX_SHADOWS)
    {
        bug("[kaffeine] shadow table full, no shadow for %p\n", native);
        return FALSE;
    }
    ctx->shadow_map[ctx->num_shadows].m68k_addr = guest;
    ctx->shadow_map[ctx->num_shadows].native_ptr = native;
    ctx->shadow_map[ctx->num_shadows].type = type;
    ctx->num_shadows++;
    return TRUE;
}

void Kaffeine_ShadowForget(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == guest && ctx->shadow_map[i].type == type)
        {
            ctx->shadow_map[i] = ctx->shadow_map[--ctx->num_shadows];
            return;
        }
}

struct Screen *Kaffeine_ScreenNative(struct KaffeineContext *ctx, ULONG guest)
{
    struct Screen *scr = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_SCREEN);

    return scr ? scr : Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_PUBSCREEN);
}

ULONG Kaffeine_ScreenGuest(struct KaffeineContext *ctx, const struct Screen *native)
{
    ULONG guest = Kaffeine_ShadowGuest(ctx, native, KAFFEINE_SHADOW_SCREEN);

    return guest ? guest : Kaffeine_ShadowGuest(ctx, native, KAFFEINE_SHADOW_PUBSCREEN);
}

/* ── Fonts ───────────────────────────────────────────────────────── */

static int font_index(struct KaffeineContext *ctx, ULONG guest, const struct TextFont *native)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->fonts[i].guest && (guest ? ctx->fonts[i].guest == guest
                                          : ctx->fonts[i].native == native))
            return i;
    return -1;
}

struct TextFont *Kaffeine_FontNative(struct KaffeineContext *ctx, ULONG guest)
{
    int i = guest ? font_index(ctx, guest, NULL) : -1;

    return i < 0 ? NULL : ctx->fonts[i].native;
}

ULONG Kaffeine_FontFind(struct KaffeineContext *ctx, const struct TextFont *font)
{
    int i = font ? font_index(ctx, 0, font) : -1;

    return i < 0 ? 0 : ctx->fonts[i].guest;
}

/* Reserve a four-byte-aligned part without overflowing a guest address or
   exceeding the arena. There is one owned block for the entire snapshot. */
static BOOL font_part(struct KaffeineContext *ctx, ULONG *total, ULONG size, ULONG *offset)
{
    if (*total > ctx->mem_size || ctx->mem_size - *total < 3)
        return FALSE;
    *offset = (*total + 3) & ~3UL;
    if (*offset > ctx->mem_size || size > ctx->mem_size - *offset)
        return FALSE;
    *total = *offset + size;
    return TRUE;
}

ULONG Kaffeine_FontGuest(struct KaffeineContext *ctx, struct TextFont *font)
{
    ULONG guest, name_size = 0, total, bitmap = 0, loc = 0, space = 0, kern = 0;
    ULONG bitmap_size = 0, records = 0, n;
    BOOL data;
    const char *name;
    int i;

    if (!font || !ctx->mem)
        return 0;
    data = font->tf_CharData || font->tf_CharLoc || font->tf_CharSpace || font->tf_CharKern;
    /* ColorTextFont has more fields and planes; a TextFont prefix would
       misrepresent its bitmap. Completely table-less borrowed objects
       retain their existing metadata-only representation. */
    if (data && (font->tf_Style & FSF_COLORFONT))
    {
        bug("[kaffeine] color TextFont %p is unsupported\n", font);
        return 0;
    }
    i = font_index(ctx, 0, font);
    if (i < 0)
    {
        for (i = 0; i < KAFFEINE_MAX_FONTS && ctx->fonts[i].guest; i++)
            ;
        if (i == KAFFEINE_MAX_FONTS)
        {
            bug("[kaffeine] font table full, no 68k TextFont for %p\n", font);
            return 0;
        }
        /* A completely table-less native object remains a metadata-only
           font, as used by existing callers. Partial bitmap objects fail. */
        if (data)
        {
            if (!font->tf_CharData || !font->tf_CharLoc || !font->tf_YSize ||
                !font->tf_Modulo || font->tf_HiChar < font->tf_LoChar)
                return 0;
            records = (ULONG)font->tf_HiChar - font->tf_LoChar + 2;
            if ((ULONG)font->tf_YSize > ctx->mem_size / font->tf_Modulo)
                return 0;
            bitmap_size = (ULONG)font->tf_YSize * font->tf_Modulo;
        }
        name = font->tf_Message.mn_Node.ln_Name;
        if (name)
        {
            size_t length = strlen(name);

            if (ctx->mem_size <= M68K_SIZEOF_TEXTFONT ||
                length >= ctx->mem_size - M68K_SIZEOF_TEXTFONT)
                return 0;
            name_size = (ULONG)length + 1;
        }
        total = M68K_SIZEOF_TEXTFONT + name_size;
        if (total > ctx->mem_size || (data &&
            (!font_part(ctx, &total, bitmap_size, &bitmap) ||
             !font_part(ctx, &total, records * 4, &loc) ||
             (font->tf_CharSpace && !font_part(ctx, &total, records * 2, &space)) ||
             (font->tf_CharKern && !font_part(ctx, &total, records * 2, &kern)))))
            return 0;
        guest = Kaffeine_MirrorAlloc(ctx, total);
        if (!guest)
            return 0;
        if (name_size)
        {
            memcpy(ctx->mem + guest + M68K_SIZEOF_TEXTFONT, name, name_size);
            m68k_write32(ctx, guest + G_LN_NAME, guest + M68K_SIZEOF_TEXTFONT);
        }
        if (data)
        {
            memcpy(ctx->mem + guest + bitmap, font->tf_CharData, bitmap_size);
            m68k_write32(ctx, guest + G_TF_CHARDATA, guest + bitmap);
            m68k_write32(ctx, guest + G_TF_CHARLOC, guest + loc);
            if (space) m68k_write32(ctx, guest + G_TF_CHARSPACE, guest + space);
            if (kern) m68k_write32(ctx, guest + G_TF_CHARKERN, guest + kern);
            for (n = 0; n < records; n++)
            {
                m68k_write32(ctx, guest + loc + n * 4, ((const ULONG *)font->tf_CharLoc)[n]);
                if (space)
                    m68k_write16(ctx, guest + space + n * 2,
                                 (UWORD)((const WORD *)font->tf_CharSpace)[n]);
                if (kern)
                    m68k_write16(ctx, guest + kern + n * 2,
                                 (UWORD)((const WORD *)font->tf_CharKern)[n]);
            }
        }
        ctx->fonts[i].guest = guest;
        ctx->fonts[i].allocation_size = total;
        ctx->fonts[i].native = font;
        ctx->fonts[i].opens = 0;
        ctx->fonts[i].pinned = FALSE;
        ctx->fonts[i].retained = FALSE;
    }
    guest = ctx->fonts[i].guest;
    ctx->mem[guest + G_LN_TYPE] = font->tf_Message.mn_Node.ln_Type;
    m68k_write16(ctx, guest + G_TF_YSIZE, font->tf_YSize);
    /* Native AROS ExtendFont sets FSF_TAGGED with its private extension.
       The guest snapshot has no TextFontExtension or reply-port pointer,
       so publish only the intrinsic style until that extension is bridged. */
    ctx->mem[guest + G_TF_STYLE] = font->tf_Style & (UBYTE)~FSF_TAGGED;
    ctx->mem[guest + G_TF_FLAGS] = font->tf_Flags;
    m68k_write16(ctx, guest + G_TF_XSIZE, font->tf_XSize);
    m68k_write16(ctx, guest + G_TF_BASELINE, font->tf_Baseline);
    m68k_write16(ctx, guest + G_TF_BOLDSMEAR, font->tf_BoldSmear);
    m68k_write16(ctx, guest + G_TF_ACCESSORS, font->tf_Accessors);
    ctx->mem[guest + G_TF_LOCHAR] = font->tf_LoChar;
    ctx->mem[guest + G_TF_HICHAR] = font->tf_HiChar;
    m68k_write16(ctx, guest + G_TF_MODULO, font->tf_Modulo);
    return guest;
}

static BOOL font_referenced(struct KaffeineContext *ctx, int i)
{
    UWORD s;

    if (ctx->fonts[i].pinned)
        return TRUE;
    for (s = 0; s < ctx->num_shadows; s++)
        if (ctx->shadow_map[s].type == KAFFEINE_SHADOW_RASTPORT &&
            (m68k_read32(ctx, ctx->shadow_map[s].m68k_addr + G_RP_FONT) == ctx->fonts[i].guest ||
             ((struct RastPort *)ctx->shadow_map[s].native_ptr)->Font == ctx->fonts[i].native))
            return TRUE;
    return FALSE;
}

/* Keep both the snapshot and its last owned native open while a RastPort
   shows it. Native CloseFont can remove a non-ROM font at zero accessors. */
static void font_release(struct KaffeineContext *ctx, ULONG guest)
{
    int i = guest ? font_index(ctx, guest, NULL) : -1;

    if (i < 0 || ctx->fonts[i].opens || font_referenced(ctx, i))
        return;
    if (ctx->fonts[i].retained)
        CloseFont(ctx->fonts[i].native);
    Kaffeine_MirrorRelease(ctx, guest);
    memset(&ctx->fonts[i], 0, sizeof(ctx->fonts[i]));
}

static void font_sweep(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->fonts[i].guest && ctx->fonts[i].retained)
            font_release(ctx, ctx->fonts[i].guest);
}

void Kaffeine_FontOpened(struct KaffeineContext *ctx, ULONG guest)
{
    int i = guest ? font_index(ctx, guest, NULL) : -1;

    if (i >= 0)
    {
        /* A new native open replaces the last open held for a RastPort. */
        if (ctx->fonts[i].retained)
        {
            CloseFont(ctx->fonts[i].native);
            ctx->fonts[i].retained = FALSE;
        }
        ctx->fonts[i].opens++;
        m68k_write16(ctx, guest + G_TF_ACCESSORS, ctx->fonts[i].native->tf_Accessors);
    }
}

ULONG Kaffeine_FontAdopt(struct KaffeineContext *ctx, struct TextFont *font)
{
    ULONG guest;
    int i;

    /* OpenFont and OpenDiskFont never claim to open a color font, even
       when a collaborator supplies a metadata-only native object. */
    if (font && (font->tf_Style & FSF_COLORFONT))
    {
        bug("[kaffeine] opening a color TextFont is unsupported\n");
        CloseFont(font);
        return 0;
    }
    guest = Kaffeine_FontGuest(ctx, font);
    i = guest ? font_index(ctx, guest, NULL) : -1;

    if (font && (i < 0 || ctx->fonts[i].opens == (ULONG)~0UL))
    {
        CloseFont(font);
        return 0;
    }
    Kaffeine_FontOpened(ctx, guest);
    return guest;
}

void Kaffeine_FontClosed(struct KaffeineContext *ctx, ULONG guest)
{
    int i = guest ? font_index(ctx, guest, NULL) : -1;

    if (i < 0)
        return;
    if (!ctx->fonts[i].opens)
    {
        /* Also used internally to release an unreferenced borrowed
           snapshot. A repeated guest close acquires no native close. */
        font_release(ctx, guest);
        return;
    }
    ctx->fonts[i].opens--;
    if (!ctx->fonts[i].opens && font_referenced(ctx, i))
        ctx->fonts[i].retained = TRUE;
    else
        CloseFont(ctx->fonts[i].native);
    if (ctx->fonts[i].opens || ctx->fonts[i].retained)
        m68k_write16(ctx, guest + G_TF_ACCESSORS, ctx->fonts[i].native->tf_Accessors);
    font_release(ctx, guest);
}

/* End of the context, after its native UI calls have finished. Only opens
   acquired by OpenFont/OpenDiskFont are ours to close, never borrowed ones. */
void Kaffeine_FontsCleanup(struct KaffeineContext *ctx)
{
    int i;

    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
    {
        if (!ctx->fonts[i].guest)
            continue;
        while (ctx->fonts[i].opens)
        {
            ctx->fonts[i].opens--;
            CloseFont(ctx->fonts[i].native);
        }
        if (ctx->fonts[i].retained)
            CloseFont(ctx->fonts[i].native);
        Kaffeine_MirrorRelease(ctx, ctx->fonts[i].guest);
        memset(&ctx->fonts[i], 0, sizeof(ctx->fonts[i]));
    }
}

/* ── RastPorts ───────────────────────────────────────────────────── */

void Kaffeine_RastPortOut(struct KaffeineContext *ctx, ULONG guest, struct RastPort *rp)
{
    UBYTE *g = ctx->mem + guest;
    ULONG i, old_font;

    g[G_RP_MASK] = rp->Mask;
    g[G_RP_FGPEN] = (UBYTE)rp->FgPen;
    g[G_RP_BGPEN] = (UBYTE)rp->BgPen;
    g[G_RP_AOLPEN] = (UBYTE)rp->AOlPen;
    g[G_RP_DRAWMODE] = (UBYTE)rp->DrawMode;
    g[G_RP_AREAPTSZ] = (UBYTE)rp->AreaPtSz;
    g[G_RP_LINPATCNT] = (UBYTE)rp->linpatcnt;
    m68k_write16(ctx, guest + G_RP_FLAGS, rp->Flags);
    m68k_write16(ctx, guest + G_RP_LINEPTRN, rp->LinePtrn);
    m68k_write16(ctx, guest + G_RP_CP_X, (UWORD)rp->cp_x);
    m68k_write16(ctx, guest + G_RP_CP_Y, (UWORD)rp->cp_y);
    for (i = 0; i < 8; i++)
        g[G_RP_MINTERMS + i] = rp->minterms[i];
    m68k_write16(ctx, guest + G_RP_PENWIDTH, (UWORD)rp->PenWidth);
    m68k_write16(ctx, guest + G_RP_PENHEIGHT, (UWORD)rp->PenHeight);
    /* Replace the guest reference before reclaiming closed fonts. Also
       check native RastPorts, so a guest edit of rp->Font cannot make a
       still displayed font disappear, or hide its later last release.
       Reclaim before publishing a new font to make table space available. */
    old_font = m68k_read32(ctx, guest + G_RP_FONT);
    m68k_write32(ctx, guest + G_RP_FONT, 0);
    font_release(ctx, old_font);
    font_sweep(ctx);
    m68k_write32(ctx, guest + G_RP_FONT, Kaffeine_FontGuest(ctx, rp->Font));
    g[G_RP_ALGOSTYLE] = rp->AlgoStyle;
    g[G_RP_TXFLAGS] = rp->TxFlags;
    m68k_write16(ctx, guest + G_RP_TXHEIGHT, rp->TxHeight);
    m68k_write16(ctx, guest + G_RP_TXWIDTH, rp->TxWidth);
    m68k_write16(ctx, guest + G_RP_TXBASELINE, rp->TxBaseline);
    m68k_write16(ctx, guest + G_RP_TXSPACING, (UWORD)rp->TxSpacing);
}

BOOL Kaffeine_RastPortAttach(struct KaffeineContext *ctx, ULONG guest, struct RastPort *native)
{
    int i;

    if (!guest || guest >= ctx->mem_size || ctx->mem_size - guest < G_RASTPORT_SIZE ||
        !Kaffeine_ShadowAdd(ctx, guest, native, KAFFEINE_SHADOW_RASTPORT))
        return FALSE;
    memset(ctx->mem + guest, 0, G_RASTPORT_SIZE);
    Kaffeine_RastPortOut(ctx, guest, native);
    /* the font of the window or screen stays for the program */
    i = font_index(ctx, m68k_read32(ctx, guest + G_RP_FONT), NULL);
    if (i >= 0)
        ctx->fonts[i].pinned = TRUE;
    return TRUE;
}

ULONG Kaffeine_RastPortCreate(struct KaffeineContext *ctx, struct RastPort *native)
{
    ULONG guest = Kaffeine_HeapAlloc(ctx, G_RASTPORT_SIZE, MEMF_PUBLIC);

    if (guest && !Kaffeine_RastPortAttach(ctx, guest, native))
    {
        Kaffeine_HeapFree(ctx, guest, G_RASTPORT_SIZE);
        guest = 0;
    }
    return guest;
}

void Kaffeine_RastPortDetach(struct KaffeineContext *ctx, ULONG guest)
{
    ULONG i;

    if (!Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_RASTPORT))
        return;
    Kaffeine_ShadowForget(ctx, guest, KAFFEINE_SHADOW_RASTPORT);
    for (i = 0; i < KAFFEINE_MAX_SETTLE; i++)
        if (ctx->settle_rastports[i] == guest)
            ctx->settle_rastports[i] = 0;
    /* The font it showed may be left to nobody. */
    font_release(ctx, m68k_read32(ctx, guest + G_RP_FONT));
    font_sweep(ctx);
}

void Kaffeine_RastPortDelete(struct KaffeineContext *ctx, ULONG guest)
{
    if (!Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_RASTPORT))
        return;
    Kaffeine_RastPortDetach(ctx, guest);
    Kaffeine_HeapFree(ctx, guest, G_RASTPORT_SIZE);
}

static void settle(struct KaffeineContext *ctx)
{
    ULONG i, guest;
    struct RastPort *rp;

    for (i = 0; i < KAFFEINE_MAX_SETTLE; i++)
    {
        guest = ctx->settle_rastports[i];
        ctx->settle_rastports[i] = 0;
        rp = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_RASTPORT);
        if (rp)
            Kaffeine_RastPortOut(ctx, guest, rp);
    }
}

struct RastPort *Kaffeine_RastPortIn(struct KaffeineContext *ctx, ULONG guest)
{
    struct RastPort *rp = Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_RASTPORT);
    const UBYTE *g;
    ULONG i;

    if (!rp)
        return NULL;
    g = ctx->mem + guest;
    rp->Mask = g[G_RP_MASK];
    rp->FgPen = (BYTE)g[G_RP_FGPEN];
    rp->BgPen = (BYTE)g[G_RP_BGPEN];
    rp->AOlPen = (BYTE)g[G_RP_AOLPEN];
    rp->DrawMode = (BYTE)g[G_RP_DRAWMODE];
    rp->AreaPtSz = (BYTE)g[G_RP_AREAPTSZ];
    rp->linpatcnt = (BYTE)g[G_RP_LINPATCNT];
    rp->Flags = m68k_read16(ctx, guest + G_RP_FLAGS);
    rp->LinePtrn = m68k_read16(ctx, guest + G_RP_LINEPTRN);
    rp->cp_x = (WORD)m68k_read16(ctx, guest + G_RP_CP_X);
    rp->cp_y = (WORD)m68k_read16(ctx, guest + G_RP_CP_Y);
    rp->PenWidth = (WORD)m68k_read16(ctx, guest + G_RP_PENWIDTH);
    rp->PenHeight = (WORD)m68k_read16(ctx, guest + G_RP_PENHEIGHT);
    rp->AlgoStyle = g[G_RP_ALGOSTYLE];
    rp->TxSpacing = (WORD)m68k_read16(ctx, guest + G_RP_TXSPACING);
    /* Settle it right after the thunk, once even when used twice. */
    for (i = 0; i < KAFFEINE_MAX_SETTLE; i++)
        if (!ctx->settle_rastports[i] || ctx->settle_rastports[i] == guest)
        {
            ctx->settle_rastports[i] = guest;
            break;
        }
    ctx->after_thunk = settle;
    return rp;
}

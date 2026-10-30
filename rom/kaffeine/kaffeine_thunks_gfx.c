/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Intuition and graphics manual thunks shared by kaffeine.library and the
    host dispatch tests. The graphics calls here take the 68k RastPort of a
    window or a screen (kaffeine_rastport.c). ReadPixel also reads bounded,
    unlayered guest planar bitmaps directly. Caller-owned classic RastPorts support bounded initialization and font-only
    operations through temporary native structures, and the pointer-free
    state calls (pens, drawing mode, Move) through a temporary native
    RastPort with only its scalars. Draw(), RectFill(), WritePixel(),
    Text(), BltTemplate() and a layered ReadPixel() on a caller-owned one,
    one that it built with InitRastPort() over its own planar BitMap or a
    copy of the RastPort of its window, go through such a temporary too,
    on the C stack for that call alone, whose Layer and BitMap the layer
    bridge gives (layer_ops->draw_target): a native BitMap of the call over
    the checked guest planes, or the real native layer of the window, which
    clips as for the window's own RastPort. Only the changed scalars come
    back; the program's pointers, font, RP_User and reserved bytes stay,
    and so does the RastPort that it copied. Other guest-built RastPorts,
    area patterns and other drawing calls on a caller-owned one stop with
    return code 20 instead of reaching native graphics.library. RectFill
    ignores unused TmpRas/AreaInfo. Text accepts a bounded classic TmpRas
    through a per-call native descriptor; AreaInfo remains unsupported.
    AllocRaster() planes are ordinary guest heap allocations.
*/

#include <aros/debug.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <hidd/gfx.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_messages.h"
#include "kaffeine_copy.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_fontattr.h"
#include "kaffeine_thunks_gfx.h"

extern void *shadow_lookup(struct KaffeineContext *ctx, ULONG m68k_addr);

/* The 68k TextExtent, RastPort and TextFont of the NDK */
enum { G_TE_WIDTH = 0, G_TE_HEIGHT = 2, G_TE_MINX = 4, G_TE_MINY = 6, G_TE_MAXX = 8, G_TE_MAXY = 10 };
enum { G_RP_LAYER = 0, G_RP_BITMAP = 4, G_RP_AREAPTRN = 8, G_RP_TMPRAS = 12,
       G_RP_AREAINFO = 16, G_RP_GELSINFO = 20,
       G_RP_FONT = 52, G_RP_TXHEIGHT = 58, G_RP_TXWIDTH = 60,
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
   a window or a screen. Anything else, the shadow of another object and
   raw guest bytes included, stops the program and gives NULL, for which
   the thunk returns ctx->exit_code before any native call. */
struct RastPort *resolve_rp(struct KaffeineContext *ctx, ULONG m68k_rp)
{
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, m68k_rp);

    if (!rp)
    {
        bug("[kaffeine] resolve_rp(%08lx): not the RastPort of a window or a screen\n",
            (unsigned long)m68k_rp);
        refuse(ctx, "only the RastPort of a window or a screen reaches graphics.library");
    }
    return rp;
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

/* Intuition follows these classic chains itself. Reuse the checked
   two-pass copies used by requester/gadget rendering, including native
   WORD border coordinates. Planar image bytes stay in the guest arena.
   No guest structure or unconverted pointer reaches native Intuition. */
static IPTR draw_classic(struct KaffeineContext *ctx, void *cpu, BOOL image)
{
    ULONG guest = THUNK_A(1);
    struct RastPort *rp = rastport(ctx, THUNK_A(0), image ? "DrawImage" : "DrawBorder");
    struct KaffeineCopy copy;
    APTR block, native;

    if (!rp) return ctx->exit_code;
    if (!guest) return 0;
    memset(&copy, 0, sizeof(copy));
    if (image) Kaffeine_CopyImages(ctx, &copy, guest);
    else Kaffeine_CopyBorders(ctx, &copy, guest);
    if (copy.why)
    {
        bug("[kaffeine] %s(): %s\n", image ? "DrawImage" : "DrawBorder", copy.why);
        return refuse(ctx, "Intuition drawing data that cannot be converted");
    }
    block = AllocVec(copy.used, MEMF_CLEAR);
    if (!block) return refuse(ctx, "no memory for Intuition drawing data");
    Kaffeine_CopyFill(&copy, block);
    native = image ? (APTR)Kaffeine_CopyImages(ctx, &copy, guest) :
                     (APTR)Kaffeine_CopyBorders(ctx, &copy, guest);
    if (copy.why || !native)
    {
        FreeVec(block);
        return refuse(ctx, "Intuition drawing data changed during conversion");
    }
    ctx->bitmap_blits++;
    if (image) DrawImage(rp, (struct Image *)native, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    else DrawBorder(rp, (struct Border *)native, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    WaitBlit();
    ctx->bitmap_blits--;
    FreeVec(block);
    return 0;
}

IPTR thunk_intuition_DrawImage(struct KaffeineContext *ctx, void *cpu)
{ return draw_classic(ctx, cpu, TRUE); }
IPTR thunk_intuition_DrawBorder(struct KaffeineContext *ctx, void *cpu)
{ return draw_classic(ctx, cpu, FALSE); }

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

/* size bytes of ordinary guest memory of the program, in the heap, a hunk
   or the stack alike, sharing no byte with a registered object (a 68k
   RastPort, Window or Screen among them), a file, a port, a protected
   IORequest, a queued message, a 68k TextFont, free or bridge-owned heap
   memory, a library base with its jump table or the memory snapshot. */
static BOOL program_memory(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    UWORD i;

    if (!Kaffeine_MessageMemory(ctx, addr, size) || !Kaffeine_HeapGuestRange(ctx, addr, size) ||
        in_font(ctx, addr, size) ||
        (ctx->memory_info && overlaps(addr, size, ctx->memory_info, KAFFEINE_MEMINFO_SIZE)))
        return FALSE;
    for (i = 0; i < ctx->num_libs; i++)
        if (ctx->libs[i].m68k_addr >= ctx->libs[i].jt_start &&
            overlaps(addr, size, ctx->libs[i].jt_start,
                     ctx->libs[i].m68k_addr - ctx->libs[i].jt_start + ctx->libs[i].pos_size))
            return FALSE;
    return TRUE;
}

/* Storage of a caller-owned classic RastPort for state and drawing calls:
   100 word aligned bytes of ordinary guest memory of the program. */
static BOOL state_storage(struct KaffeineContext *ctx, ULONG guest)
{
    return !(guest & 1) && program_memory(ctx, guest, M68K_SIZEOF_RASTPORT);
}

/* The native RastPort for a state call (SetAPen() ... Move()): that of a
   window or a screen, settled after the thunk as before, or else a
   temporary one in *temporary for a caller-owned classic RastPort, with
   its scalars only and a copy in *before (Kaffeine_RastPortStateIn()).
   Anything else stops the program, and the thunk returns ctx->exit_code. */
static struct RastPort *state_rastport(struct KaffeineContext *ctx, ULONG guest,
                                       struct RastPort *temporary, struct RastPort *before,
                                       const char *call)
{
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);

    if (rp)
        return rp;
    if (!state_storage(ctx, guest))
    {
        bug("[kaffeine] %s(%08lx): not a RastPort of the program\n", call, (unsigned long)guest);
        refuse(ctx, "a RastPort state call on unsupported storage");
        return NULL;
    }
    Kaffeine_RastPortStateIn(ctx, guest, temporary);
    *before = *temporary;
    return temporary;
}

/* After the native state call: the changed scalars of a temporary back. */
static void state_return(struct KaffeineContext *ctx, ULONG guest, const struct RastPort *rp,
                         const struct RastPort *temporary, const struct RastPort *before)
{
    if (rp == temporary)
        Kaffeine_RastPortStateOut(ctx, guest, before, rp);
}

/* A temporary native RastPort for drawing through a caller-owned classic
   RastPort, on the C stack for one thunk: the scalars as for the state
   calls, the native font of its 68k TextFont, and the target that the
   layer bridge gives (layer_ops->draw_target). Without a Layer, adapter is
   the native BitMap of this call over the guest planes, width and height
   are its pixels, and planes and plane_size the guest bytes that drawing
   may write. Nothing of it outlives the thunk. */
struct DrawTemporary
{
    struct RastPort rp, before;
    struct BitMap adapter;
    struct TmpRas tmpras;
    ULONG bitmap, planes[8], plane_size, width, height;
    UBYTE depth;
};

static BOOL draw_refuse(struct KaffeineContext *ctx, ULONG guest, const char *call, const char *why)
{
    bug("[kaffeine] %s(%08lx): %s\n", call, (unsigned long)guest, why);
    refuse(ctx, "a drawing call that the bridge cannot take");
    return FALSE;
}

enum DrawPolicy
{
    DRAW_REQUIRE_AREA_STATE_NULL,
    DRAW_RECTFILL_UNUSED_AREA_STATE,
    DRAW_TEXT_TYPED_TMPRAS
};

/* The temporary of a drawing call (Draw() ... BltTemplate()) for the
   caller-owned classic RastPort at guest, checked whole before any native
   call: the storage of a state call; no AreaPtrn. Only RectFill permits
   guest TmpRas/AreaInfo values: neither its solid nor patterned native path
   uses them. StateIn leaves both native pointers NULL and StateOut leaves
   all guest pointer bytes untouched. Text may adapt TmpRas separately,
   after its complete target and source validation; it still refuses
   AreaInfo. Other operations retain their checks. These calls never read GelsInfo;
   its guest value stays untouched and the native temporary keeps NULL.
   For text, a Font that is a
   68k TextFont of graphics, whose native font it gets (other calls get it
   too, or NULL, as they never read it); the target of the layer bridge;
   and for a planar BitMap, a descriptor and planes that are ordinary
   memory of the program, apart from each other and from the RastPort. No
   68k pointer is taken for a native one. FALSE after stopping the program
   (20). */
static BOOL draw_rastport(struct KaffeineContext *ctx, ULONG guest, BOOL text,
                          enum DrawPolicy policy, struct DrawTemporary *t, const char *call)
{
    const struct KaffeineLayerOps *ops = ctx->layer_ops;
    ULONG i, j;

    if (!state_storage(ctx, guest))
        return draw_refuse(ctx, guest, call, "not a RastPort of the program");
    if (m68k_read32(ctx, guest + G_RP_AREAPTRN) ||
        (policy != DRAW_RECTFILL_UNUSED_AREA_STATE && m68k_read32(ctx, guest + G_RP_AREAINFO)) ||
        (policy != DRAW_RECTFILL_UNUSED_AREA_STATE && policy != DRAW_TEXT_TYPED_TMPRAS &&
         m68k_read32(ctx, guest + G_RP_TMPRAS)))
        return draw_refuse(ctx, guest, call, "unsupported area state in a caller-owned RastPort");
    Kaffeine_RastPortStateIn(ctx, guest, &t->rp);
    t->rp.Font = Kaffeine_FontNative(ctx, m68k_read32(ctx, guest + G_RP_FONT));
    if (text && !t->rp.Font)
        return draw_refuse(ctx, guest, call, "no 68k TextFont of graphics in the RastPort");
    if (!ops || !ops->draw_target)
        return draw_refuse(ctx, guest, call, "drawing needs the layer bridge");
    memset(&t->adapter, 0, sizeof(t->adapter));
    t->bitmap = m68k_read32(ctx, guest + G_RP_BITMAP);
    if (!ops->draw_target(ctx, m68k_read32(ctx, guest + G_RP_LAYER), t->bitmap, &t->rp, &t->adapter))
    {
        if (ctx->running)
            refuse(ctx, "no drawing target for the RastPort");
        return FALSE;
    }
    t->depth = 0;
    t->plane_size = t->width = t->height = 0;
    memset(t->planes, 0, sizeof(t->planes));
    if (!t->rp.Layer)
    {
        if (t->rp.BitMap != &t->adapter || !t->adapter.Depth || t->adapter.Depth > 8)
            return draw_refuse(ctx, guest, call, "the planar target is not this call's own BitMap");
        t->depth = t->adapter.Depth;
        /* at most 4094 * 32767 bytes and 32752 pixels (layers_private_bitmap()) */
        t->plane_size = (ULONG)t->adapter.BytesPerRow * t->adapter.Rows;
        t->width = (ULONG)t->adapter.BytesPerRow * 8;
        t->height = t->adapter.Rows;
        if ((t->bitmap & 1) || !program_memory(ctx, t->bitmap, M68K_SIZEOF_BITMAP) ||
            overlaps(t->bitmap, M68K_SIZEOF_BITMAP, guest, M68K_SIZEOF_RASTPORT))
            return draw_refuse(ctx, guest, call, "the BitMap is not ordinary memory of the program");
        for (i = 0; i < t->depth; i++)
        {
            t->planes[i] = (ULONG)(t->adapter.Planes[i] - ctx->mem);
            if (!program_memory(ctx, t->planes[i], t->plane_size) ||
                overlaps(t->planes[i], t->plane_size, guest, M68K_SIZEOF_RASTPORT) ||
                overlaps(t->planes[i], t->plane_size, t->bitmap, M68K_SIZEOF_BITMAP))
                return draw_refuse(ctx, guest, call, "a plane is not ordinary memory of the program");
            /* the same plane twice, as some programs give, is not taken yet */
            for (j = 0; j < i; j++)
                if (overlaps(t->planes[i], t->plane_size, t->planes[j], t->plane_size))
                    return draw_refuse(ctx, guest, call, "planes that share bytes");
        }
    }
    t->before = t->rp;
    return TRUE;
}

/* Whether guest bytes that a drawing call only reads, its text or
   template, share none with the RastPort, nor with the BitMap and planes
   of a planar target */
static BOOL draw_source(const struct DrawTemporary *t, ULONG guest, ULONG addr, ULONG size)
{
    ULONG i;

    if (overlaps(addr, size, guest, M68K_SIZEOF_RASTPORT))
        return FALSE;
    if (t->rp.Layer)
        return TRUE;
    if (overlaps(addr, size, t->bitmap, M68K_SIZEOF_BITMAP))
        return FALSE;
    for (i = 0; i < t->depth; i++)
        if (overlaps(addr, size, t->planes[i], t->plane_size))
            return FALSE;
    return TRUE;
}

/* Text's monochrome renderer uses TmpRas only when it fits its own
   computed raster; a smaller positive descriptor legitimately falls back
   to native AllocRaster. Never duplicate font geometry here. The signed
   native Size is compared with ULONG rassize, so reject negative bits
   rather than let their unsigned promotion claim a huge writable buffer.
   NULL/zero buffers and capacities above this bounded profile are refused. */
#define TEXT_TMPRAS_MAX_SIZE (16UL * 1024 * 1024)
static BOOL text_tmpras(struct KaffeineContext *ctx, ULONG guest, ULONG string, ULONG count,
                        struct DrawTemporary *t)
{
    ULONG descriptor = m68k_read32(ctx, guest + G_RP_TMPRAS), buffer, size;
    if (!descriptor)
        return TRUE;
    if ((descriptor & 1) || !program_memory(ctx, descriptor, 8) ||
        !draw_source(t, guest, descriptor, 8) || (count && overlaps(descriptor, 8, string, count)) ||
        overlaps(descriptor, 8, t->bitmap, M68K_SIZEOF_BITMAP))
        return draw_refuse(ctx, guest, "Text", "not an ordinary disjoint classic TmpRas");
    buffer = m68k_read32(ctx, descriptor);
    size = m68k_read32(ctx, descriptor + 4);
    if (!buffer || (buffer & 1) || !size || size > TEXT_TMPRAS_MAX_SIZE ||
        !program_memory(ctx, buffer, size) || !draw_source(t, guest, buffer, size) ||
        overlaps(buffer, size, descriptor, 8) || (count && overlaps(buffer, size, string, count)) ||
        overlaps(buffer, size, t->bitmap, M68K_SIZEOF_BITMAP))
        return draw_refuse(ctx, guest, "Text", "not a positive bounded disjoint TmpRas buffer");
    /* A private layer may draw into guest planes even with a non-NULL
       Layer. Until that provenance is described here, admit only managed
       native bitmaps for this case; unlayered planes were checked above. */
    if (t->rp.Layer && (!t->rp.BitMap || !IS_HIDD_BM(t->rp.BitMap)))
        return draw_refuse(ctx, guest, "Text", "TmpRas needs a managed native layered target");
    t->tmpras.RasPtr = ctx->mem + buffer;
    t->tmpras.Size = (LONG)size;
    t->rp.TmpRas = &t->tmpras;
    return TRUE;
}

/* Around the native call on a temporary: no LayerInfo of the program is
   disposed of and no screen of it closes until the blitter has finished
   with the call's BitMap; then exactly the changed scalars go back. */
static void draw_begin(struct KaffeineContext *ctx)
{
    ctx->bitmap_blits++;
}

static void draw_end(struct KaffeineContext *ctx, ULONG guest, const struct DrawTemporary *t)
{
    WaitBlit();
    ctx->bitmap_blits--;
    Kaffeine_RastPortStateOut(ctx, guest, &t->before, &t->rp);
}

/* A rectangle on a planar target, clipped to its BitMap as graphics does;
   FALSE when nothing of it is inside, also for an empty one. */
static BOOL draw_clip(const struct DrawTemporary *t, LONG *x0, LONG *y0, LONG *x1, LONG *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= (LONG)t->width) *x1 = (LONG)t->width - 1;
    if (*y1 >= (LONG)t->height) *y1 = (LONG)t->height - 1;
    return *x0 <= *x1 && *y0 <= *y1;
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

/* graphics -468: classic TmpRas is exactly two LONGs. This initializer
   only stores opaque guest bits; a drawing consumer must validate any
   buffer it actually uses. Never pass either guest address to native code. */
IPTR thunk_gfx_InitTmpRas(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(0);
    if ((guest & 1) || !program_memory(ctx, guest, 8))
        return refuse(ctx, "InitTmpRas(): not caller-owned classic TmpRas storage");
    m68k_write32(ctx, guest, THUNK_A(1));
    m68k_write32(ctx, guest + 4, THUNK_D(0));
    return guest;
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

/* Native metric queries read Font and signed TxSpacing only. A caller's
   classic RastPort needs no layer or bitmap: resolve its live font into
   a zeroed per-call temporary, never import its other pointer fields. */
static struct RastPort *metric_rastport(struct KaffeineContext *ctx, ULONG guest,
                                       struct RastPort *temporary)
{
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct TextFont *font;

    if (rp)
    {
        if (rp->Font)
            return rp;
    }
    else if (state_storage(ctx, guest) &&
             (font = Kaffeine_FontNative(ctx, m68k_read32(ctx, guest + G_RP_FONT))))
    {
        memset(temporary, 0, sizeof(*temporary));
        temporary->Font = font;
        temporary->TxSpacing = (WORD)m68k_read16(ctx, guest + G_RP_TXSPACING);
        return temporary;
    }
    refuse(ctx, "text metrics: no live font in supported RastPort storage");
    return NULL;
}

static BOOL metric_output(struct KaffeineContext *ctx, ULONG extent, ULONG port)
{
    return !(extent & 1) && program_memory(ctx, extent, M68K_SIZEOF_TEXTEXTENT) &&
           !overlaps(extent, M68K_SIZEOF_TEXTEXTENT, port, M68K_SIZEOF_RASTPORT);
}

/* graphics -54: TextLength(A1=rp, A0=string, D0=count) -> D0 */
IPTR thunk_gfx_TextLength(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    ULONG guest = THUNK_A(1);
    struct RastPort *rp, temporary;

    if (!text)
        return refuse(ctx, "TextLength(): the text is not in guest memory");
    rp = metric_rastport(ctx, guest, &temporary);
    if (!rp)
        return ctx->exit_code;
    return (IPTR)(LONG)TextLength(rp, text, THUNK_D(0));
}

/* graphics -60: Text(A1=rp, A0=string, D0=count). On a caller-owned
   RastPort with a 68k TextFont of graphics, through its temporary; the
   text shares no byte with the RastPort or a planar target, and the pen
   position comes back. */
IPTR thunk_gfx_Text(struct KaffeineContext *ctx, void *cpu)
{
    ULONG string = THUNK_A(0), guest = THUNK_A(1), count = THUNK_D(0) & 0xFFFF;
    CONST_STRPTR text = guest_text(ctx, string, THUNK_D(0));
    struct DrawTemporary t;
    struct RastPort *rp;

    if (!text)
        return refuse(ctx, "Text(): the text is not in guest memory");
    rp = Kaffeine_RastPortIn(ctx, guest);
    if (rp)
    {
        Text(rp, text, THUNK_D(0));
        return 0;
    }
    if (!draw_rastport(ctx, guest, TRUE, DRAW_TEXT_TYPED_TMPRAS, &t, "Text"))
        return ctx->exit_code;
    if (count && !draw_source(&t, guest, string, count))
        return refuse(ctx, "Text(): the text shares bytes with the RastPort or its BitMap");
    if (!text_tmpras(ctx, guest, string, count, &t))
        return ctx->exit_code;
    draw_begin(ctx);
    Text(&t.rp, text, THUNK_D(0));
    draw_end(ctx, guest, &t);
    return 0;
}

/* graphics -690: TextExtent(A1=rp, A0=string, D0=count, A2=textExtent).
   The native result goes to the 68k TextExtent field by field. */
IPTR thunk_gfx_TextExtent(struct KaffeineContext *ctx, void *cpu)
{
    CONST_STRPTR text = guest_text(ctx, THUNK_A(0), THUNK_D(0));
    ULONG extent = THUNK_A(2);
    struct TextExtent te;
    struct RastPort *rp, temporary;

    if (!text || !metric_output(ctx, extent, THUNK_A(1)))
        return refuse(ctx, "TextExtent(): the text or the TextExtent is not in guest memory");
    rp = metric_rastport(ctx, THUNK_A(1), &temporary);
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
    struct RastPort *rp, temporary;
    ULONG fitted;

    if (length)
    {
        /* first and last character, which bound every one in between */
        long long last = (long long)string + (long long)(length - 1) * step;

        if (!guest_range(ctx, string, 1) || last < 0 || last >= ctx->mem_size ||
            !guest_range(ctx, (ULONG)last, 1))
            return refuse(ctx, "TextFit(): the text is not in guest memory");
    }
    if (!metric_output(ctx, extent, THUNK_A(1)) ||
        (constraint && ((constraint & 1) || !guest_range(ctx, constraint, M68K_SIZEOF_TEXTEXTENT) ||
                        overlaps(extent, M68K_SIZEOF_TEXTEXTENT, constraint, M68K_SIZEOF_TEXTEXTENT))))
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
    rp = metric_rastport(ctx, THUNK_A(1), &temporary);
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
    ULONG guest = THUNK_A(1);
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, guest, &temporary, &before, "Move");

    if (!rp)
        return ctx->exit_code;
    Move(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    state_return(ctx, guest, rp, &temporary, &before);
    return 0;
}

/* graphics -246: Draw(A1=rp, D0=x, D1=y). On a caller-owned RastPort
   through its temporary, which graphics clips; the pen position comes
   back. */
IPTR thunk_gfx_Draw(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct DrawTemporary t;

    if (rp)
    {
        Draw(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
        return 0;
    }
    if (!draw_rastport(ctx, guest, FALSE, DRAW_REQUIRE_AREA_STATE_NULL, &t, "Draw"))
        return ctx->exit_code;
    draw_begin(ctx);
    Draw(&t.rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    draw_end(ctx, guest, &t);
    return 0;
}

/* graphics -306: RectFill(A1=rp, D0=xMin, D1=yMin, D2=xMax, D3=yMax). On
   a caller-owned RastPort through its temporary; on a planar target
   clipped to the BitMap first, and nothing native for a rectangle that is
   empty or wholly outside it. */
IPTR thunk_gfx_RectFill(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    LONG x0 = (WORD)THUNK_D(0), y0 = (WORD)THUNK_D(1), x1 = (WORD)THUNK_D(2), y1 = (WORD)THUNK_D(3);
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct DrawTemporary t;

    if (rp)
    {
        RectFill(rp, (WORD)x0, (WORD)y0, (WORD)x1, (WORD)y1);
        return 0;
    }
    if (!draw_rastport(ctx, guest, FALSE, DRAW_RECTFILL_UNUSED_AREA_STATE, &t, "RectFill"))
        return ctx->exit_code;
    if (!t.rp.Layer && !draw_clip(&t, &x0, &y0, &x1, &y1))
        return 0;
    draw_begin(ctx);
    RectFill(&t.rp, (WORD)x0, (WORD)y0, (WORD)x1, (WORD)y1);
    draw_end(ctx, guest, &t);
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

/* graphics -318: ReadPixel(A1=rp, D0=x, D1=y) -> D0. A caller-owned
   RastPort with a Layer, such as a copy of a window's, reads its real
   native layer through a temporary, as for drawing; one without a Layer
   is read from the guest planes alone. */
IPTR thunk_gfx_ReadPixel(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct DrawTemporary t;
    LONG pen;

    if (rp)
        return (IPTR)ReadPixel(rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
    if (!(guest & 1) && guest_range(ctx, guest, M68K_SIZEOF_RASTPORT) &&
        m68k_read32(ctx, guest + G_RP_LAYER))
    {
        if (!draw_rastport(ctx, guest, FALSE, DRAW_REQUIRE_AREA_STATE_NULL, &t, "ReadPixel"))
            return ctx->exit_code;
        draw_begin(ctx);
        pen = ReadPixel(&t.rp, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
        draw_end(ctx, guest, &t);
        return (IPTR)pen;
    }
    return guest_read_pixel(ctx, guest, (WORD)THUNK_D(0), (WORD)THUNK_D(1));
}

/* graphics -324: WritePixel(A1=rp, D0=x, D1=y) -> D0. On a caller-owned
   RastPort through its temporary; outside a planar target -1, as graphics
   gives, with no native call. */
IPTR thunk_gfx_WritePixel(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    LONG x = (WORD)THUNK_D(0), y = (WORD)THUNK_D(1), result;
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct DrawTemporary t;

    if (rp)
        return (IPTR)WritePixel(rp, (WORD)x, (WORD)y);
    if (!draw_rastport(ctx, guest, FALSE, DRAW_REQUIRE_AREA_STATE_NULL, &t, "WritePixel"))
        return ctx->exit_code;
    if (!t.rp.Layer && (x < 0 || y < 0 || x >= (LONG)t.width || y >= (LONG)t.height))
        return (IPTR)(LONG)-1;
    draw_begin(ctx);
    result = WritePixel(&t.rp, (WORD)x, (WORD)y);
    draw_end(ctx, guest, &t);
    return (IPTR)result;
}

/* graphics -36: BltTemplate(A0=source, D0=xSrc, D1=srcMod, A1=destRP,
   D2=xDest, D3=yDest, D4=xSize, D5=ySize), WORD scalars. The template is
   read-only guest bytes, the bit xSrc of a row of srcMod bytes on: xSrc
   and srcMod must not be negative, and nothing native runs for an empty
   size. On a caller-owned RastPort through its temporary; on a planar
   target the rectangle is clipped to the BitMap first, the source moved
   along with it, whole words of xSrc go to the source address, and a
   rectangle wholly outside draws nothing. Every template byte that
   graphics may read, ySize - 1 rows of srcMod and the words of the last
   row, all in 64 bits, must lie in guest memory, and for a temporary
   apart from the RastPort and a planar target's BitMap and planes. */
IPTR thunk_gfx_BltTemplate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG source = THUNK_A(0), guest = THUNK_A(1);
    LONG xs = (WORD)THUNK_D(0), mod = (WORD)THUNK_D(1);
    LONG xd = (WORD)THUNK_D(2), yd = (WORD)THUNK_D(3), w = (WORD)THUNK_D(4), h = (WORD)THUNK_D(5);
    unsigned long long first, span;
    struct RastPort *rp = Kaffeine_RastPortIn(ctx, guest);
    struct DrawTemporary t;

    if (!rp && !draw_rastport(ctx, guest, FALSE, DRAW_REQUIRE_AREA_STATE_NULL, &t, "BltTemplate"))
        return ctx->exit_code;
    if (xs < 0 || mod < 0)
        return refuse(ctx, "BltTemplate(): a negative source offset or modulo");
    if (w <= 0 || h <= 0)
        return 0;
    if (!source)
        return refuse(ctx, "BltTemplate(): no template");
    first = source;
    if (!rp && !t.rp.Layer)
    {
        if (xd < 0) { xs -= xd; w += xd; xd = 0; }
        if (yd < 0) { first += (unsigned long long)(-yd) * (ULONG)mod; h += yd; yd = 0; }
        if (xd >= (LONG)t.width || yd >= (LONG)t.height || w <= 0 || h <= 0)
            return 0;
        if (w > (LONG)t.width - xd) w = (LONG)t.width - xd;
        if (h > (LONG)t.height - yd) h = (LONG)t.height - yd;
        /* xSrc stays a WORD: whole words of it move the source */
        first += (unsigned long long)(xs >> 4) * 2;
        xs &= 15;
    }
    span = (unsigned long long)(h - 1) * (ULONG)mod + (((unsigned long long)xs + (ULONG)w + 15) >> 4) * 2;
    if (first >= ctx->mem_size || span > ctx->mem_size - first)
        return refuse(ctx, "BltTemplate(): the template is not in guest memory");
    if (!rp && !draw_source(&t, guest, (ULONG)first, (ULONG)span))
        return refuse(ctx, "BltTemplate(): the template shares bytes with the RastPort or its BitMap");
    if (rp)
    {
        BltTemplate((PLANEPTR)(ctx->mem + first), (WORD)xs, (WORD)mod, rp,
                    (WORD)xd, (WORD)yd, (WORD)w, (WORD)h);
        return 0;
    }
    draw_begin(ctx);
    BltTemplate((PLANEPTR)(ctx->mem + first), (WORD)xs, (WORD)mod, &t.rp,
                (WORD)xd, (WORD)yd, (WORD)w, (WORD)h);
    draw_end(ctx, guest, &t);
    return 0;
}

/* graphics -342: SetAPen(A1=rp, D0=pen) */
IPTR thunk_gfx_SetAPen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, guest, &temporary, &before, "SetAPen");

    if (!rp)
        return ctx->exit_code;
    SetAPen(rp, THUNK_D(0));
    state_return(ctx, guest, rp, &temporary, &before);
    return 0;
}

/* graphics -348: SetBPen(A1=rp, D0=pen) */
IPTR thunk_gfx_SetBPen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, guest, &temporary, &before, "SetBPen");

    if (!rp)
        return ctx->exit_code;
    SetBPen(rp, THUNK_D(0));
    state_return(ctx, guest, rp, &temporary, &before);
    return 0;
}

/* graphics -354: SetDrMd(A1=rp, D0=mode) */
IPTR thunk_gfx_SetDrMd(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, guest, &temporary, &before, "SetDrMd");

    if (!rp)
        return ctx->exit_code;
    SetDrMd(rp, THUNK_D(0));
    state_return(ctx, guest, rp, &temporary, &before);
    return 0;
}

/* graphics -894: SetABPenDrMd(A1=rp, D0=apen, D1=bpen, D2=mode) */
IPTR thunk_gfx_SetABPenDrMd(struct KaffeineContext *ctx, void *cpu)
{
    ULONG guest = THUNK_A(1);
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, guest, &temporary, &before, "SetABPenDrMd");

    if (!rp)
        return ctx->exit_code;
    SetABPenDrMd(rp, THUNK_D(0), THUNK_D(1), THUNK_D(2));
    state_return(ctx, guest, rp, &temporary, &before);
    return 0;
}

/* graphics -858: GetAPen(A0=rp) -> D0 */
IPTR thunk_gfx_GetAPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, THUNK_A(0), &temporary, &before, "GetAPen");

    return rp ? (IPTR)GetAPen(rp) : (IPTR)ctx->exit_code;
}

/* graphics -864: GetBPen(A0=rp) -> D0 */
IPTR thunk_gfx_GetBPen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, THUNK_A(0), &temporary, &before, "GetBPen");

    return rp ? (IPTR)GetBPen(rp) : (IPTR)ctx->exit_code;
}

/* graphics -870: GetDrMd(A0=rp) -> D0 */
IPTR thunk_gfx_GetDrMd(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, THUNK_A(0), &temporary, &before, "GetDrMd");

    return rp ? (IPTR)GetDrMd(rp) : (IPTR)ctx->exit_code;
}

/* graphics -876: GetOutlinePen(A0=rp) -> D0 */
IPTR thunk_gfx_GetOutlinePen(struct KaffeineContext *ctx, void *cpu)
{
    struct RastPort temporary, before;
    struct RastPort *rp = state_rastport(ctx, THUNK_A(0), &temporary, &before, "GetOutlinePen");

    return rp ? (IPTR)GetOutlinePen(rp) : (IPTR)ctx->exit_code;
}

/* The bytes of a plane of width x height pixels, as RASSIZE() of
   graphics/gfx.h: rows rounded up to 16 pixels. 0 for an empty plane, a
   row beyond the 16-bit mask of RASSIZE(), or more than the arena. */
static ULONG raster_size(struct KaffeineContext *ctx, ULONG width, ULONG height)
{
    unsigned long long row = (((unsigned long long)width + 15) >> 4) * 2;
    unsigned long long size = row * height;

    return row > 0xFFFE || size > ctx->mem_size ? 0 : (ULONG)size;
}

/* graphics -492: AllocRaster(D0=width, D1=height) -> D0. As AROS,
   AllocMem(RASSIZE()) of chip memory, here an ordinary allocation of the
   guest heap, which FreeRaster() and FreeMem() of that size free. No
   native plane is made. 0 for an empty, overflowing or oversized plane. */
IPTR thunk_gfx_AllocRaster(struct KaffeineContext *ctx, void *cpu)
{
    ULONG size = raster_size(ctx, THUNK_D(0), THUNK_D(1));

    return size ? Kaffeine_HeapAlloc(ctx, size, MEMF_CHIP | MEMF_PUBLIC) : 0;
}

/* graphics -120: InitGels(A0=head, A1=tail, A2=gelsInfo). Initialize the
   classic 60-byte VSprite sentinels and 38-byte GelsInfo in guest memory,
   with 32-bit big-endian links. This is list initialization only; GELS
   drawing and collision handlers cannot run through the native ABI.
   Preflight every complete object and every overlap before any write. */
IPTR thunk_gfx_InitGels(struct KaffeineContext *ctx, void *cpu)
{
    ULONG head = THUNK_A(0), tail = THUNK_A(1), info = THUNK_A(2), collision;

    if (((head | tail | info) & 1) ||
        !program_memory(ctx, head, 60) || !program_memory(ctx, tail, 60) ||
        !program_memory(ctx, info, 38) || overlaps(head, 60, tail, 60) ||
        overlaps(head, 60, info, 38) || overlaps(tail, 60, info, 38))
        return refuse(ctx, "InitGels(): invalid or overlapping classic structures");
    collision = m68k_read32(ctx, info + 18);
    if (collision && ((collision & 1) || !program_memory(ctx, collision, 64) ||
                      overlaps(collision, 64, head, 60) || overlaps(collision, 64, tail, 60) ||
                      overlaps(collision, 64, info, 38)))
        return refuse(ctx, "InitGels(): invalid or overlapping collision table");

    m68k_write32(ctx, head, tail);
    m68k_write32(ctx, head + 4, 0);
    m68k_write32(ctx, head + 8, 0); /* DrawPath; native AROS calls this IntVSprite */
    m68k_write32(ctx, head + 12, tail);
    m68k_write16(ctx, head + 16, 0x8000);
    m68k_write16(ctx, head + 18, 0x8000);
    m68k_write16(ctx, head + 22, 0x8000);
    m68k_write16(ctx, head + 24, 0x8000);
    m68k_write32(ctx, tail, 0);
    m68k_write32(ctx, tail + 4, head);
    m68k_write32(ctx, tail + 8, 0);
    m68k_write16(ctx, tail + 16, 0x7FFF);
    m68k_write16(ctx, tail + 18, 0x7FFF);
    m68k_write16(ctx, tail + 22, 0x7FFF);
    m68k_write16(ctx, tail + 24, 0x7FFF);
    m68k_write32(ctx, info + 2, head);
    m68k_write32(ctx, info + 6, tail);
    if (collision)
        m68k_write32(ctx, collision, 0);
    return 0;
}

/* No raw VSprite, Bob, animation list or guest collision callback may
   reach native graphics. InitGels alone does not register native GELS. */
IPTR thunk_gfx_GelsUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "unsupported GELS drawing or collision call");
}

/* graphics -498: FreeRaster(A0=p, D0=width, D1=height). As AROS, with the
   UWORD width and height, FreeMem(p, RASSIZE()); nothing for NULL. Only
   exactly an ordinary heap allocation of that size is freed; anything
   else stops the program before any memory changes. */
IPTR thunk_gfx_FreeRaster(struct KaffeineContext *ctx, void *cpu)
{
    ULONG plane = THUNK_A(0), size;

    if (!plane)
        return 0;
    size = raster_size(ctx, (UWORD)THUNK_D(0), (UWORD)THUNK_D(1));
    if (!size || !Kaffeine_HeapFreeExact(ctx, plane, size))
        return refuse(ctx, "FreeRaster(): not a raster of that size from AllocRaster()");
    return 0;
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

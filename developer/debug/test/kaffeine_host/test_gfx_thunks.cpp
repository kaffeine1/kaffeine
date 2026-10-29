/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Check the production Intuition and graphics thunks through literal LVOs,
    the real dispatcher and heap: the ABI slots of the tables, OpenFont() and
    CloseFont() with 68k TextFonts, the 68k RastPort of a window, which gets
    the native fields back after every call, also after a generated thunk,
    and gives the native one the fields that the program wrote, the text
    calls with their 68k TextExtents, AskFont() into a classic 68k
    TextAttr, bounded guest planar ReadPixel and the refusal of unsupported
    RastPorts, also by generated thunks, the pointer-free state calls on
    RastPorts that the program built or copied, AllocRaster() and
    FreeRaster() in the guest heap, and drawing through temporaries of such
    RastPorts, with the manual BltTemplate(): over the program's own planes,
    pre-clipped, or a copied window RastPort through the layer that a
    stand-in layer bridge names, and every refusal before native graphics.
    The native graphics.library acts as AROS does and records every call;
    its WritePixel() can set a pixel through the converted BitMap.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include <intuition/intuition.h>

#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/graphics.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_layers.h"
#include "kaffeine_thunks_gfx.h"
#include "kaffeine_dispatch.h"

/* ── The native side ─────────────────────────────────────────────── */

static TextFont topaz, other;
static char topaz_name[] = "topaz.font", other_name[] = "other.font";
static RastPort native_rp;

struct GfxCalls
{
    unsigned any = 0, init_rp = 0;
    unsigned open_font = 0, close_font = 0, set_font = 0;
    std::string font_name;
    UWORD font_ysize = 0;
    UBYTE font_style = 0, font_flags = 0;
    TextFont *open_result = &topaz, *closed = nullptr;
    std::string text;           /* characters that the text calls read */
    WORD fill_fg = 0, fill_drmd = 0;
    WORD fill[4] = {};
    WORD draw_from_x = 0, draw_from_y = 0;
    WORD dx = 0, dy = 0, xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    ULONG apen = 0, bpen = 0, drmd = 0;
    unsigned ask_font = 0;
    RastPort *ask_rp = nullptr;
    bool ask_wrong_name = false; /* a native AskFont() with another name */
    unsigned state = 0;         /* state calls, with the RastPort each saw */
    RastPort *state_rp = nullptr;
    RastPort seen = {};
    /* drawing calls: the RastPort, a copy of it and of a BitMap that is no
       fake pointer, the program's blits in progress, WaitBlit() calls */
    unsigned draws = 0, waits = 0;
    RastPort *draw_rp = nullptr;
    RastPort drawn = {};
    BitMap drawn_bm = {};
    UWORD drawn_blits = 0;
    bool paint = false;         /* WritePixel() sets the pixel in a real BitMap */
    UWORD draw_flags = 0;       /* Draw() toggles these Flags */
    const UBYTE *tmpl = nullptr;
    WORD tmpl_args[6] = {};     /* xSrc, srcMod, xDest, yDest, xSize, ySize */
};
static GfxCalls gfx;
struct ClassicDrawCalls
{
    unsigned images=0,borders=0,waits=0;
    WORD x=0,y=0;
    std::vector<Image> image;
    std::vector<Border> border;
    std::vector<std::vector<WORD>> pairs;
};
static ClassicDrawCalls classic_draw;
static KaffeineContext *drawing_context;
extern "C" void DrawImage(RastPort *rp, Image *image, WORD x, WORD y)
{
    assert(rp==&native_rp && drawing_context && drawing_context->bitmap_blits==1);
    ++classic_draw.images;classic_draw.x=x;classic_draw.y=y;
    for(;image;image=image->NextImage)classic_draw.image.push_back(*image);
}
extern "C" void DrawBorder(RastPort *rp, Border *border, WORD x, WORD y)
{
    assert(rp==&native_rp && drawing_context && drawing_context->bitmap_blits==1);
    ++classic_draw.borders;classic_draw.x=x;classic_draw.y=y;
    for(;border;border=border->NextBorder)
    {
        classic_draw.border.push_back(*border);
        classic_draw.pairs.emplace_back();
        if(border->Count>0)classic_draw.pairs.back().assign(border->XY,border->XY+2*border->Count);
    }
}

static KaffeineContext *live;

static void reset_native()
{
    std::memset(&topaz, 0, sizeof(topaz));
    topaz.tf_Message.mn_Node.ln_Name = topaz_name;
    topaz.tf_Message.mn_Node.ln_Type = NT_FONT;
    topaz.tf_YSize = 8;
    topaz.tf_XSize = 8;
    topaz.tf_Baseline = 6;
    topaz.tf_BoldSmear = 1;
    topaz.tf_Accessors = 3;
    topaz.tf_LoChar = 32;
    topaz.tf_HiChar = 255;
    topaz.tf_Modulo = 192;
    topaz.tf_Flags = FPF_ROMFONT;
    std::memset(&other, 0, sizeof(other));
    other.tf_Message.mn_Node.ln_Name = other_name;
    other.tf_YSize = 11;
    other.tf_XSize = 7;
    other.tf_Baseline = 8;
    other.tf_LoChar = 32;
    other.tf_HiChar = 126;
    other.tf_Flags = FPF_PROPORTIONAL;
    std::memset(&native_rp, 0, sizeof(native_rp));
    native_rp.Mask = 0xFF;
    native_rp.FgPen = 1;
    native_rp.BgPen = 0;
    native_rp.AOlPen = 1;
    native_rp.DrawMode = JAM2;
    native_rp.LinePtrn = 0xFFFF;
    native_rp.cp_x = 4;
    native_rp.cp_y = 9;
    native_rp.minterms[0] = 0xCA;
    native_rp.Font = &topaz;
    native_rp.TxHeight = 8;
    native_rp.TxWidth = 8;
    native_rp.TxBaseline = 6;
    native_rp.Layer = reinterpret_cast<struct Layer *>(uintptr_t(0x123456789ABCULL));
    native_rp.BitMap = reinterpret_cast<struct BitMap *>(uintptr_t(0x23456789ABCDULL));
    gfx = {};
}

extern "C" struct TextFont *OpenFont(const struct TextAttr *ta)
{
    ++gfx.any; ++gfx.open_font;
    gfx.font_name = ta && ta->ta_Name ? ta->ta_Name : "";
    gfx.font_ysize = ta ? ta->ta_YSize : 0;
    gfx.font_style = ta ? ta->ta_Style : 0;
    gfx.font_flags = ta ? ta->ta_Flags : 0;
    return gfx.font_name == "other.font" ? &other : gfx.open_result;
}

extern "C" void CloseFont(struct TextFont *font) { ++gfx.any; ++gfx.close_font; gfx.closed = font; }

extern "C" void SetFont(struct RastPort *rp, struct TextFont *font)
{
    ++gfx.any; ++gfx.set_font;
    if (!font)
        font = &topaz;
    rp->Font = font;
    rp->TxWidth = font->tf_XSize;
    rp->TxHeight = font->tf_YSize;
    rp->TxBaseline = font->tf_Baseline;
}

extern "C" void InitRastPort(struct RastPort *rp)
{
    ++gfx.any; ++gfx.init_rp;
    std::memset(rp, 0, sizeof(*rp));
    rp->Mask = 0xFF; rp->FgPen = -1; rp->AOlPen = -1;
    rp->DrawMode = JAM2; rp->LinePtrn = 0xFFFF;
    for (auto &m : rp->minterms) m = 0xCA;
    SetFont(rp, &topaz);
}

/* As AROS: the font's own attributes, whatever the RastPort's soft style */
extern "C" void AskFont(struct RastPort *rp, struct TextAttr *ta)
{
    ++gfx.any; ++gfx.ask_font;
    gfx.ask_rp = rp;
    ta->ta_Name = gfx.ask_wrong_name ? other_name : rp->Font->tf_Message.mn_Node.ln_Name;
    ta->ta_YSize = rp->Font->tf_YSize;
    ta->ta_Style = rp->Font->tf_Style;
    ta->ta_Flags = rp->Font->tf_Flags;
}

/* As AROS, the loops read UWORD(count) characters of a WORD count. */
static UWORD read_text(CONST_STRPTR string, ULONG count)
{
    WORD n = (WORD)count;
    UWORD read = 0;

    while (n--)
        gfx.text.push_back(string[read++]);
    return read;
}

extern "C" WORD TextLength(struct RastPort *rp, CONST_STRPTR string, ULONG count)
{
    ++gfx.any;
    return (WORD)(read_text(string, count) * (rp->Font->tf_XSize + rp->TxSpacing));
}

/* A drawing call: what it saw, before it changes anything. The BitMap of
   the window's native RastPort is a fake pointer and is not read. */
static void draw_call(struct RastPort *rp)
{
    ++gfx.any; ++gfx.draws;
    gfx.draw_rp = rp;
    gfx.drawn = *rp;
    gfx.drawn_bm = {};
    if (rp != &native_rp && rp->BitMap)
        gfx.drawn_bm = *rp->BitMap;
    gfx.drawn_blits = live ? live->bitmap_blits : 0;
}

extern "C" void Text(struct RastPort *rp, CONST_STRPTR string, ULONG count)
{
    draw_call(rp);
    rp->cp_x += read_text(string, count) * rp->TxWidth;
}

extern "C" void TextExtent(struct RastPort *rp, CONST_STRPTR string, ULONG count,
                           struct TextExtent *te)
{
    ++gfx.any;
    te->te_Width = read_text(string, count) * rp->TxWidth;
    te->te_Height = rp->Font->tf_YSize;
    te->te_Extent.MinX = 0;
    te->te_Extent.MinY = -(WORD)rp->Font->tf_Baseline;
    te->te_Extent.MaxX = te->te_Width - 1;
    te->te_Extent.MaxY = te->te_Height - 1 - rp->Font->tf_Baseline;
}

/* TextFit() of AROS for a fixed-width font: the constraint is checked whole,
   and the characters are read one at a time, a WORD direction apart. */
extern "C" ULONG TextFit(struct RastPort *rp, CONST_STRPTR string, ULONG strLen,
                         struct TextExtent *te, struct TextExtent *limit,
                         LONG direction, ULONG bitWidth, ULONG bitHeight)
{
    TextFont *tf = rp->Font;
    UWORD length = (UWORD)strLen;
    ULONG fitted = 0;

    ++gfx.any;
    std::memset(te, 0, sizeof(*te));
    if (length && (UWORD)bitHeight >= tf->tf_YSize)
    {
        te->te_Height = tf->tf_YSize;
        te->te_Extent.MinY = -(WORD)tf->tf_Baseline;
        te->te_Extent.MaxY = tf->tf_YSize - tf->tf_Baseline - 1;
        if (!limit || (limit->te_Extent.MinY <= te->te_Extent.MinY &&
                       limit->te_Extent.MaxY >= te->te_Extent.MaxY &&
                       limit->te_Height >= te->te_Height))
            while (length--)
            {
                WORD width = te->te_Width + rp->TxWidth, maxx = width - 1;

                gfx.text.push_back(*string);
                string += (WORD)direction;
                if ((ULONG)(maxx + 1) > (UWORD)bitWidth)
                    break;
                if (limit && (limit->te_Extent.MinX > 0 || limit->te_Extent.MaxX < maxx ||
                              limit->te_Width < width))
                    break;
                te->te_Width = width;
                te->te_Extent.MaxX = maxx;
                ++fitted;
            }
    }
    if (!fitted)
        std::memset(te, 0, sizeof(*te));
    return fitted;
}

/* Private Flags of rom/graphics/gfxfuncsupport.h */
enum { RPF_NO_PENS = 1 << 14, RPF_REMAP_COLORFONTS = 1 << 13 };

static void state_call(struct RastPort *rp)
{
    ++gfx.any; ++gfx.state;
    gfx.state_rp = rp;
    gfx.seen = *rp;
}

/* GenMinterms() of rom/graphics/gfxfuncsupport.c */
static void gen_minterms(struct RastPort *rp)
{
    if (rp->DrawMode & COMPLEMENT)
    {
        for (auto &m : rp->minterms) m = (rp->DrawMode & INVERSVID) ? 0x6a : 0x9a;
        return;
    }
    for (int i = 0; i < 8; i++)
    {
        UBYTE a = ((rp->FgPen >> i) & 1) * 3;
        UBYTE b = (rp->DrawMode & JAM2) ? ((rp->BgPen >> i) & 1) * 3 : 2;
        a = (rp->DrawMode & INVERSVID) ? (b << 2) | a : (a << 2) | b;
        rp->minterms[i] = (a << 4) | 0x0a;
    }
}

/* As rom/graphics/move.c */
extern "C" void Move(struct RastPort *rp, WORD x, WORD y)
{
    state_call(rp);
    rp->cp_x = x;
    rp->cp_y = y;
    rp->linpatcnt = 15;
    rp->Flags |= FRST_DOT;
}

extern "C" void Draw(struct RastPort *rp, WORD x, WORD y)
{
    draw_call(rp);
    gfx.draw_from_x = rp->cp_x;
    gfx.draw_from_y = rp->cp_y;
    rp->cp_x = x;
    rp->cp_y = y;
    rp->Flags ^= gfx.draw_flags;
}

extern "C" void RectFill(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1)
{
    draw_call(rp);
    gfx.fill_fg = rp->FgPen;
    gfx.fill_drmd = rp->DrawMode;
    gfx.fill[0] = x0; gfx.fill[1] = y0; gfx.fill[2] = x1; gfx.fill[3] = y1;
}

extern "C" LONG ReadPixel(struct RastPort *rp, WORD x, WORD y)
{
    draw_call(rp);
    return x == 3000 ? -1 : x + y;
}

/* With gfx.paint, the pixel of a planar BitMap of the bridge gets FgPen,
   as JAM2 would, through the native plane pointers */
extern "C" LONG WritePixel(struct RastPort *rp, WORD x, WORD y)
{
    draw_call(rp);
    if (gfx.paint)
    {
        const BitMap *bm = rp->BitMap;
        assert(rp != &native_rp && !rp->Layer && x >= 0 && y >= 0 &&
               x < bm->BytesPerRow * 8 && y < bm->Rows);
        for (int p = 0; p < bm->Depth; ++p)
        {
            UBYTE &b = bm->Planes[p][y * bm->BytesPerRow + (x >> 3)];
            const UBYTE mask = UBYTE(0x80 >> (x & 7));
            b = (rp->FgPen >> p) & 1 ? b | mask : b & ~mask;
        }
    }
    return x == 3000 ? -1 : 0;
}

extern "C" void BltTemplate(PLANEPTR source, WORD xSrc, WORD srcMod, struct RastPort *rp,
                            WORD xDest, WORD yDest, WORD xSize, WORD ySize)
{
    draw_call(rp);
    gfx.tmpl = source;
    const WORD args[] = { xSrc, srcMod, xDest, yDest, xSize, ySize };
    std::memcpy(gfx.tmpl_args, args, sizeof(args));
}

/* The drawing of a temporary has finished before its BitMap goes. */
extern "C" void WaitBlit(void)
{
    ++gfx.waits;
    ++classic_draw.waits;
    assert(live && live->bitmap_blits == 1);
    if(drawing_context)assert(drawing_context->bitmap_blits==1);
}

/* As setapen.c, setbpen.c, setdrmd.c and setabpendrmd.c (UBYTE pens and
   mode, ULONG for SetABPenDrMd()). SetAPen() leaves GenMinterms() out. */
extern "C" void SetAPen(struct RastPort *rp, ULONG pen)
{
    state_call(rp);
    rp->FgPen = (BYTE)pen; rp->linpatcnt = 15; rp->Flags &= ~RPF_NO_PENS;
}
extern "C" void SetBPen(struct RastPort *rp, ULONG pen)
{
    state_call(rp);
    rp->BgPen = (BYTE)pen; rp->linpatcnt = 15; rp->Flags &= ~RPF_NO_PENS;
    gen_minterms(rp);
}
extern "C" void SetDrMd(struct RastPort *rp, ULONG mode)
{
    state_call(rp);
    rp->DrawMode = (BYTE)mode; rp->linpatcnt = 15;
    gen_minterms(rp);
}

extern "C" void SetABPenDrMd(struct RastPort *rp, ULONG apen, ULONG bpen, ULONG mode)
{
    state_call(rp);
    gfx.apen = apen; gfx.bpen = bpen; gfx.drmd = mode;
    rp->FgPen = (BYTE)apen;
    rp->BgPen = (BYTE)bpen;
    rp->DrawMode = (BYTE)mode;
    rp->linpatcnt = 15;
    rp->Flags &= ~RPF_NO_PENS;
    gen_minterms(rp);
}

extern "C" ULONG GetAPen(struct RastPort *rp) { state_call(rp); return (UBYTE)rp->FgPen; }
extern "C" ULONG GetBPen(struct RastPort *rp) { state_call(rp); return (UBYTE)rp->BgPen; }
extern "C" ULONG GetDrMd(struct RastPort *rp) { state_call(rp); return (UBYTE)rp->DrawMode; }
extern "C" ULONG GetOutlinePen(struct RastPort *rp) { state_call(rp); return (UBYTE)rp->AOlPen; }

/* AllocRaster() planes are guest heap allocations: native ones never. */
extern "C" PLANEPTR AllocRaster(ULONG, ULONG) { assert(false); return nullptr; }
extern "C" void FreeRaster(PLANEPTR, ULONG, ULONG) { assert(false); }

extern "C" void ScrollRaster(struct RastPort *, WORD dx, WORD dy,
                             WORD xMin, WORD yMin, WORD xMax, WORD yMax)
{
    ++gfx.any;
    gfx.dx = dx; gfx.dy = dy;
    gfx.xmin = xMin; gfx.ymin = yMin; gfx.xmax = xMax; gfx.ymax = yMax;
}

extern "C" void *shadow_lookup(struct KaffeineContext *ctx, ULONG m68k_addr)
{
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == m68k_addr)
            return ctx->shadow_map[i].native_ptr;
    return nullptr;
}

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* ── The guest side ──────────────────────────────────────────────── */

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };

static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }

/* These dispatch entries use production generated C bodies, extracted
   verbatim. Native collaborators count calls and one changes the pen. */
static unsigned generated_native;
extern "C" {
IPTR gfx_generated_PolyDraw(KaffeineContext *, void *);
IPTR gfx_generated_SetRast(KaffeineContext *, void *);
IPTR gfx_generated_ClipBlit(KaffeineContext *, void *);
void PolyDraw(RastPort *rp, WORD, WORD *) { ++generated_native; rp->cp_x=77; rp->cp_y=88; }
void SetRast(RastPort *, UBYTE) { ++generated_native; }
void ClipBlit(RastPort *, WORD, WORD, RastPort *, WORD, WORD, WORD, WORD, UBYTE) { ++generated_native; }
}

static const M68KThunkEntry intuition_manual[] = { KAFFEINE_INTUITION_COMPAT_THUNKS };
static const M68KThunkEntry intuition_generated[] = { { 108, poison }, { 114, poison }, { 816, poison } };
static const M68KThunkEntry graphics_compat[] = { KAFFEINE_GRAPHICS_COMPAT_THUNKS };
static const M68KThunkEntry graphics_rastport[] = { KAFFEINE_GRAPHICS_RASTPORT_THUNKS };
static const M68KThunkEntry graphics_manual[] =
    { KAFFEINE_GRAPHICS_COMPAT_THUNKS, KAFFEINE_GRAPHICS_RASTPORT_THUNKS };
static const M68KThunkEntry graphics_generated[] =
{
    { 36, poison }, { 54, poison }, { 60, poison }, { 66, poison }, { 72, poison }, { 78, poison },
    { 96, poison },
    { 102, poison },
    { 108, poison },
    { 114, poison },
    { 120, poison },
    { 126, poison },
    { 132, poison },
    { 138, poison },
    { 144, poison },
    { 150, poison },
    { 156, poison },
    { 162, poison },
    { 168, poison },
    { 174, poison },
    { 600, poison },
    { 198, poison }, { 234, gfx_generated_SetRast }, { 240, poison }, { 246, poison }, { 306, poison },
    { 318, poison }, { 324, poison },
    { 336, gfx_generated_PolyDraw }, { 342, poison }, { 348, poison }, { 354, poison },
    { 396, poison }, { 474, poison }, { 492, poison }, { 498, poison }, { 552, gfx_generated_ClipBlit },
    { 690, poison }, { 696, poison }, { 858, poison }, { 864, poison },
    { 870, poison }, { 876, poison }, { 894, poison }
};

/* Guest offsets of the NDK structures. */
enum
{
    LN_TYPE = 8, LN_NAME = 10,
    TF_YSIZE = 20, TF_STYLE = 22, TF_FLAGS = 23, TF_XSIZE = 24, TF_BASELINE = 26,
    TF_BOLDSMEAR = 28, TF_ACCESSORS = 30, TF_LOCHAR = 32, TF_HICHAR = 33,
    TF_CHARDATA = 34, TF_MODULO = 38,
    RP_LAYER = 0, RP_BITMAP = 4, RP_MASK = 24, RP_FGPEN = 25, RP_BGPEN = 26,
    RP_AOLPEN = 27, RP_DRAWMODE = 28, RP_LINPATCNT = 30, RP_LINEPTRN = 34,
    RP_CP_X = 36, RP_CP_Y = 38, RP_MINTERMS = 40, RP_FONT = 52, RP_ALGOSTYLE = 56, RP_TXHEIGHT = 58,
    RP_TXWIDTH = 60, RP_TXBASELINE = 62, RP_TXSPACING = 64,
    TE_WIDTH = 0, TE_HEIGHT = 2, TE_MINX = 4, TE_MINY = 6, TE_MAXX = 8, TE_MAXY = 10
};

struct Fixture
{
    static constexpr ULONG base = 0x1000, stack = 0xF00;
    static constexpr ULONG text = 0x2000, extent = 0x2100, limit = 0x2120;
    static constexpr ULONG attr = 0x2200, name = 0x2240, raw = 0x2300;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x10000> memory;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    ULONG heap_avail;

    Fixture()
    {
        reset_native();
        memory.fill(0xA5);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.heap_start = 0x4000;
        ctx.heap_end = 0xF000;
        assert(Kaffeine_HeapInit(&ctx));
        heap_avail = Kaffeine_HeapAvail(&ctx, 0);
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = base;
        lib.jt_start = base - 6 * 512;
        std::strcpy(lib.name, "graphics.library");
        lib.thunks = graphics_manual;
        lib.num_thunks = std::size(graphics_manual);
        lib.gen_thunks = graphics_generated;
        lib.num_gen_thunks = std::size(graphics_generated);
        /* Tests that only change the name start with an untagged TextAttr.
           Poisoning ta_Style would request a TTextAttr with a bad tag pointer. */
        std::memset(memory.data() + attr, 0, M68K_SIZEOF_TEXTATTR);
        m68k_write32(&ctx, stack, 0x76543210);
        live = &ctx;
    }
    ~Fixture() { live = nullptr; }

    void put(ULONG addr, const char *s)
    {
        std::memcpy(memory.data() + addr, s, std::strlen(s) + 1);
    }

    /* A literal LVO through the dispatcher, with A0, A1... and D0, D1...
       from the lists; only D0 may change. */
    ULONG call(ULONG lvo, std::initializer_list<ULONG> a = {}, std::initializer_list<ULONG> d = {})
    {
        unsigned i = 0;

        for (i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        i = 0;
        for (ULONG v : a) state.a[i++] = v;
        i = 0;
        for (ULONG v : d) state.d[i++] = v;
        state.a[7] = stack;
        const auto before = state;
        ctx.running = TRUE; ctx.exit_code = 0;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);
        assert(!ctx.after_thunk);
        return state.d[0];
    }
    /* stopped with rc 20 in D0, where the CPU loop takes the exit code */
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    ULONG rastport()
    {
        ULONG rp = Kaffeine_RastPortCreate(&ctx, &native_rp);
        assert(rp);
        return rp;
    }
    UWORD word(ULONG addr) const { return m68k_read16(const_cast<KaffeineContext *>(&ctx), addr); }
    ULONG lng(ULONG addr) const { return m68k_read32(const_cast<KaffeineContext *>(&ctx), addr); }
};

static ULONG guest_font(Fixture &f, TextFont *font)
{
    return Kaffeine_FontFind(&f.ctx, font);
}

static void test_registration()
{
    /* AddGList() and AddGadget() are in the gadget table of
       kaffeine_gadgets.c since gadget lists reach windows. */
    assert(std::size(intuition_manual) == 3);
    assert(intuition_manual[0].lvo == 108 && intuition_manual[0].thunk == thunk_intuition_DrawBorder);
    assert(intuition_manual[1].lvo == 114 && intuition_manual[1].thunk == thunk_intuition_DrawImage);
    assert(intuition_manual[2].lvo == 816 && intuition_manual[2].thunk == thunk_intuition_SetWindowPointerA);

    assert(std::size(graphics_compat) == 3);
    for (const auto &entry : graphics_compat)
        assert((entry.lvo == 72 && entry.thunk == thunk_gfx_OpenFont) ||
               (entry.lvo == 396 && entry.thunk == thunk_gfx_ScrollRaster) ||
               (entry.lvo == 894 && entry.thunk == thunk_gfx_SetABPenDrMd));
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } rastport[] =
    {
        { 36, thunk_gfx_BltTemplate },
        { 54, thunk_gfx_TextLength }, { 60, thunk_gfx_Text }, { 66, thunk_gfx_SetFont },
        { 78, thunk_gfx_CloseFont },
        { 96, thunk_gfx_GelsUnsupported },
        { 102, thunk_gfx_GelsUnsupported },
        { 108, thunk_gfx_GelsUnsupported },
        { 114, thunk_gfx_GelsUnsupported },
        { 120, thunk_gfx_InitGels },
        { 126, thunk_gfx_GelsUnsupported },
        { 132, thunk_gfx_GelsUnsupported },
        { 138, thunk_gfx_GelsUnsupported },
        { 144, thunk_gfx_GelsUnsupported },
        { 150, thunk_gfx_GelsUnsupported },
        { 156, thunk_gfx_GelsUnsupported },
        { 162, thunk_gfx_GelsUnsupported },
        { 168, thunk_gfx_GelsUnsupported },
        { 174, thunk_gfx_GelsUnsupported },
        { 600, thunk_gfx_GelsUnsupported },
        { 198, thunk_gfx_InitRastPort }, { 240, thunk_gfx_Move }, { 246, thunk_gfx_Draw },
        { 306, thunk_gfx_RectFill }, { 318, thunk_gfx_ReadPixel }, { 324, thunk_gfx_WritePixel },
        { 342, thunk_gfx_SetAPen }, { 348, thunk_gfx_SetBPen }, { 354, thunk_gfx_SetDrMd },
        { 474, thunk_gfx_AskFont }, { 492, thunk_gfx_AllocRaster }, { 498, thunk_gfx_FreeRaster },
        { 690, thunk_gfx_TextExtent }, { 696, thunk_gfx_TextFit }, { 858, thunk_gfx_GetAPen },
        { 864, thunk_gfx_GetBPen }, { 870, thunk_gfx_GetDrMd }, { 876, thunk_gfx_GetOutlinePen }
    };
    assert(std::size(graphics_rastport) == std::size(rastport));
    for (unsigned i = 0; i < std::size(rastport); ++i)
        assert(graphics_rastport[i].lvo == rastport[i].lvo &&
               graphics_rastport[i].thunk == rastport[i].thunk);
    for (const auto &c : graphics_compat)
        for (const auto &r : graphics_rastport)
            assert(c.lvo != r.lvo);
}

static void test_intuition()
{
    Fixture f;
    f.ctx.libs[0].thunks = intuition_manual;
    f.ctx.libs[0].num_thunks = std::size(intuition_manual);
    f.ctx.libs[0].gen_thunks = intuition_generated;
    f.ctx.libs[0].num_gen_thunks = std::size(intuition_generated);
    /* -816 is SetWindowPointerA(), which returns 0 as a no-op instead of
       falling through to the generated thunk. */
    assert(f.call(816, { 0x300 }) == 0U && !f.stopped());
    assert(!gfx.any);
}

static void test_fonts()
{
    Fixture f;

    /* OpenFont() reads the TextAttr at A0, not the decoy at A1. */
    f.put(Fixture::name, "topaz.font");
    f.put(Fixture::name + 0x20, "wrong.font");
    m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
    m68k_write16(&f.ctx, Fixture::attr + M68K_TA_YSIZE, 8);
    f.memory[Fixture::attr + M68K_TA_STYLE] = 0x02;
    f.memory[Fixture::attr + M68K_TA_FLAGS] = 0x41;
    m68k_write32(&f.ctx, Fixture::attr + 0x20 + M68K_TA_NAME, Fixture::name + 0x20);
    m68k_write16(&f.ctx, Fixture::attr + 0x20 + M68K_TA_YSIZE, 16);
    const ULONG font = f.call(72, { Fixture::attr, Fixture::attr + 0x20 });
    assert(gfx.open_font == 1 && gfx.font_name == "topaz.font" && gfx.font_ysize == 8 &&
           gfx.font_style == 0x02 && gfx.font_flags == 0x41);

    /* a 68k TextFont in the guest heap with the metrics and the name */
    assert(font >= f.ctx.heap_start && font < f.ctx.heap_end);
    assert(guest_font(f, &topaz) == font);
    assert(f.word(font + TF_YSIZE) == 8 && f.word(font + TF_XSIZE) == 8 &&
           f.word(font + TF_BASELINE) == 6 && f.word(font + TF_BOLDSMEAR) == 1 &&
           f.word(font + TF_ACCESSORS) == 3 && f.word(font + TF_MODULO) == 192);
    assert(f.memory[font + TF_LOCHAR] == 32 && f.memory[font + TF_HICHAR] == 255 &&
           f.memory[font + TF_FLAGS] == FPF_ROMFONT && f.memory[font + TF_STYLE] == 0 &&
           f.memory[font + LN_TYPE] == NT_FONT);
    assert(f.lng(font + TF_CHARDATA) == 0);
    const ULONG font_name = f.lng(font + LN_NAME);
    assert(font_name == font + M68K_SIZEOF_TEXTFONT &&
           std::strcmp(reinterpret_cast<char *>(f.memory.data() + font_name), "topaz.font") == 0);

    /* the same native font gives the same 68k TextFont, with fresh fields */
    topaz.tf_Accessors = 4;
    assert(f.call(72, { Fixture::attr }) == font && f.word(font + TF_ACCESSORS) == 4);

    /* CloseFont() of a 68k TextFont closes the native font; the second
       close of the two opens frees the 68k TextFont, which no RastPort
       shows. Anything else closes nothing. */
    assert(f.call(78, { 0, font }) == 0 && gfx.close_font == 1 && gfx.closed == &topaz);
    assert(guest_font(f, &topaz) == font);
    assert(f.call(78, { 0, font }) == 0 && gfx.close_font == 2);
    assert(!guest_font(f, &topaz) && Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);
    f.call(78, { 0, font });
    f.call(78, { 0, Fixture::raw });
    f.call(78, { 0, 0 });
    assert(gfx.close_font == 2 && !f.stopped());

    /* A failed open gives 0. */
    gfx.open_result = nullptr;
    assert(f.call(72, { Fixture::attr }) == 0 && gfx.open_font == 3);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.heap_avail);

    /* Null or out-of-bounds TextAttr or name: no native call */
    const unsigned opens = gfx.open_font;
    for (ULONG bad : { 0U, static_cast<ULONG>(f.memory.size() - 7), 0xFFFFFFFFU })
        assert(f.call(72, { bad, Fixture::attr }) == 0);
    f.memory[f.memory.size() - 1] = 'z';
    for (ULONG bad : { 0xFFFFFFFFU, static_cast<ULONG>(f.memory.size() - 1),
                       static_cast<ULONG>(f.memory.size()) })
    {
        m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, bad);
        assert(f.call(72, { Fixture::attr }) == 0);
    }
    assert(gfx.open_font == opens);
}

static void test_rastport_fields()
{
    Fixture f;
    const ULONG rp = f.rastport();
    const ULONG font = guest_font(f, &topaz);

    /* the fields of the native RastPort, without native pointers */
    assert(font && f.lng(rp + RP_FONT) == font);
    assert(f.lng(rp + RP_LAYER) == 0 && f.lng(rp + RP_BITMAP) == 0);
    assert(f.memory[rp + RP_MASK] == 0xFF && f.memory[rp + RP_FGPEN] == 1 &&
           f.memory[rp + RP_BGPEN] == 0 && f.memory[rp + RP_AOLPEN] == 1 &&
           f.memory[rp + RP_DRAWMODE] == JAM2 && f.memory[rp + RP_MINTERMS] == 0xCA);
    assert(f.word(rp + RP_LINEPTRN) == 0xFFFF && f.word(rp + RP_CP_X) == 4 &&
           f.word(rp + RP_CP_Y) == 9 && f.word(rp + RP_TXHEIGHT) == 8 &&
           f.word(rp + RP_TXWIDTH) == 8 && f.word(rp + RP_TXBASELINE) == 6);
    for (ULONG i = 66; i < 100; ++i)
        assert(f.memory[rp + i] == 0);

    /* Delete frees the RastPort; the font that the window came with stays,
       because programs keep rp->Font to set it back. */
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    Kaffeine_RastPortDelete(&f.ctx, rp);
    assert(!Kaffeine_ShadowNative(&f.ctx, rp, KAFFEINE_SHADOW_RASTPORT));
    assert(guest_font(f, &topaz) == font && Kaffeine_HeapAvail(&f.ctx, 0) > avail);

    /* Attach fills a RastPort inside another guest block, Detach forgets it */
    const ULONG screen = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_SCREEN, 0);
    assert(Kaffeine_RastPortAttach(&f.ctx, screen + M68K_SCR_RASTPORT, &native_rp));
    assert(f.lng(screen + M68K_SCR_RASTPORT + RP_FONT) == guest_font(f, &topaz));
    assert(!Kaffeine_RastPortAttach(&f.ctx, f.memory.size() - 99, &native_rp));
    Kaffeine_RastPortDetach(&f.ctx, screen + M68K_SCR_RASTPORT);
    assert(!Kaffeine_ShadowNative(&f.ctx, screen + M68K_SCR_RASTPORT, KAFFEINE_SHADOW_RASTPORT));
    Kaffeine_HeapFree(&f.ctx, screen, M68K_SIZEOF_SCREEN);
    assert(guest_font(f, &topaz) == font);
}

static void test_pens()
{
    Fixture f;
    const ULONG rp = f.rastport();

    /* setters and getters, with the fields back after each call */
    f.call(342, { 0, rp }, { 3 });
    assert(native_rp.FgPen == 3 && f.memory[rp + RP_FGPEN] == 3 &&
           f.memory[rp + RP_LINPATCNT] == 15);
    assert(f.call(858, { rp }) == 3);
    f.call(348, { 0, rp }, { 0xFE });
    assert(f.call(864, { rp }) == 0xFE && f.memory[rp + RP_BGPEN] == 0xFE);
    f.call(354, { 0, rp }, { 0x101 });
    assert(f.call(870, { rp }) == 1 && f.memory[rp + RP_DRAWMODE] == 1);
    f.call(894, { 0, rp }, { 5, 6, 2 });
    assert(gfx.apen == 5 && gfx.bpen == 6 && gfx.drmd == 2 &&
           f.memory[rp + RP_FGPEN] == 5 && f.memory[rp + RP_BGPEN] == 6 &&
           f.memory[rp + RP_DRAWMODE] == 2);

    /* Fields that the program writes reach the native RastPort first. */
    f.memory[rp + RP_FGPEN] = 9;
    f.memory[rp + RP_DRAWMODE] = JAM1;
    f.memory[rp + RP_AOLPEN] = 4;
    m68k_write16(&f.ctx, rp + RP_TXSPACING, 2);
    f.call(306, { 0, rp }, { 1, 2, 30, 40 });
    assert(gfx.fill_fg == 9 && gfx.fill_drmd == JAM1 && gfx.fill[0] == 1 && gfx.fill[1] == 2 &&
           gfx.fill[2] == 30 && gfx.fill[3] == 40);
    assert(native_rp.TxSpacing == 2 && f.call(876, { rp }) == 4 && f.call(858, { rp }) == 9);

    /* the pen position, written and read back */
    f.call(240, { 0, rp }, { 0xFFFF0010, 20 });
    assert(f.word(rp + RP_CP_X) == 16 && f.word(rp + RP_CP_Y) == 20);
    m68k_write16(&f.ctx, rp + RP_CP_X, 50);
    f.call(246, { 0, rp }, { 60, 70 });
    assert(gfx.draw_from_x == 50 && gfx.draw_from_y == 20);
    assert(f.word(rp + RP_CP_X) == 60 && f.word(rp + RP_CP_Y) == 70);

    /* pixels, with WORD coordinates and LONG results */
    assert(f.call(318, { 0, rp }, { 0x00010005, 6 }) == 11);
    assert(f.call(318, { 0, rp }, { 3000, 1 }) == 0xFFFFFFFFU);
    assert(f.call(324, { 0, rp }, { 3000, 1 }) == 0xFFFFFFFFU);
    assert(f.call(324, { 0, rp }, { 5, 1 }) == 0);

    /* ScrollRaster() takes signed WORDs. */
    f.call(396, { 0, rp }, { 0xFFFE, 5, 10, 20 });
    assert(gfx.dx == -2 && gfx.dy == 5 && gfx.xmin == 10 && gfx.ymin == 20);
    assert(!f.stopped());
}

static void test_generated_settle()
{
    Fixture f;
    const ULONG rp = f.rastport();

    /* A generated thunk moves the native pen: the 68k RastPort follows right
       after it, and the next call starts from there. */
    f.call(336, { 0, rp });
    assert(native_rp.cp_x == 77 && f.word(rp + RP_CP_X) == 77 && f.word(rp + RP_CP_Y) == 88);
    f.call(246, { 0, rp }, { 1, 2 });
    assert(gfx.draw_from_x == 77 && gfx.draw_from_y == 88);

    /* resolve_rp() gives nothing but a 68k RastPort: the shadow of a window,
       raw guest bytes, NULL and the end of the arena stop the program. */
    int native_window;
    f.ctx.shadow_map[f.ctx.num_shadows].m68k_addr = Fixture::raw + 0x40;
    f.ctx.shadow_map[f.ctx.num_shadows].native_ptr = &native_window;
    f.ctx.shadow_map[f.ctx.num_shadows++].type = KAFFEINE_SHADOW_WINDOW;
    for (ULONG bad : { Fixture::raw + 0x40, Fixture::raw, 0U, static_cast<ULONG>(f.memory.size()),
                       rp + 2 })
    {
        f.ctx.running = TRUE; f.ctx.exit_code = 0;
        assert(resolve_rp(&f.ctx, bad) == nullptr);
        assert(!f.ctx.running && f.ctx.exit_code == 20 && !f.ctx.after_thunk);
    }

    /* A deleted RastPort is not settled any more. */
    assert(Kaffeine_RastPortIn(&f.ctx, rp) == &native_rp && f.ctx.after_thunk);
    Kaffeine_RastPortDelete(&f.ctx, rp);
    std::memset(f.memory.data() + rp, 0x5A, 100);
    f.ctx.after_thunk(&f.ctx);
    f.ctx.after_thunk = nullptr;
    assert(f.memory[rp + RP_FGPEN] == 0x5A);
}

static void test_text()
{
    Fixture f;
    const ULONG rp = f.rastport();

    f.put(Fixture::text, "Hello world");
    assert(f.call(54, { Fixture::text, rp }, { 5 }) == 40 && gfx.text == "Hello");
    /* the count is a WORD: its high half is not read */
    gfx.text.clear();
    assert(f.call(54, { Fixture::text, rp }, { 0x00070005 }) == 40 && gfx.text == "Hello");

    /* Text() moves the pen of the 68k RastPort. */
    f.call(240, { 0, rp }, { 10, 20 });
    gfx.text.clear();
    f.call(60, { Fixture::text, rp }, { 3 });
    assert(gfx.text == "Hel" && f.word(rp + RP_CP_X) == 34);

    /* TextExtent() writes the 68k TextExtent, big-endian, with signed fields */
    std::memset(f.memory.data() + Fixture::extent, 0xEE, 16);
    f.call(690, { Fixture::text, rp, Fixture::extent }, { 5 });
    assert(f.word(Fixture::extent + TE_WIDTH) == 40 && f.word(Fixture::extent + TE_HEIGHT) == 8 &&
           f.word(Fixture::extent + TE_MINX) == 0 && f.word(Fixture::extent + TE_MINY) == 0xFFFA &&
           f.word(Fixture::extent + TE_MAXX) == 39 && f.word(Fixture::extent + TE_MAXY) == 1);
    assert(f.memory[Fixture::extent + 12] == 0xEE);

    /* no characters, no string needed */
    gfx.text.clear();
    assert(f.call(54, { 0, rp }, { 0x10000 }) == 0 && gfx.text.empty() && !f.stopped());

    /* text or TextExtent outside guest memory: stop before graphics.library */
    const unsigned calls = gfx.any;
    f.put(f.memory.size() - 3, "ab");
    f.call(54, { static_cast<ULONG>(f.memory.size() - 3), rp }, { 4 });
    assert(f.stopped());
    f.call(60, { static_cast<ULONG>(f.memory.size() - 3), rp }, { 4 });
    assert(f.stopped());
    f.call(690, { Fixture::text, rp, static_cast<ULONG>(f.memory.size() - 11) }, { 5 });
    assert(f.stopped());
    f.call(690, { Fixture::text, rp, 0 }, { 5 });
    assert(f.stopped() && gfx.any == calls);
    assert(f.call(54, { static_cast<ULONG>(f.memory.size() - 3), rp }, { 3 }) == 24 && !f.stopped());
}

static void test_textfit()
{
    Fixture f;
    const ULONG rp = f.rastport();

    f.put(Fixture::text, "Hello world");
    /* forward, 5 characters of 8 pixels in 40 pixels */
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, 0 }, { 11, 1, 40, 8 }) == 5);
    assert(gfx.text == "Hello " && f.word(Fixture::extent + TE_WIDTH) == 40 &&
           f.word(Fixture::extent + TE_MINY) == 0xFFFA && f.word(Fixture::extent + TE_MAXX) == 39);
    /* backward from the last character, with the WORD direction in D1 */
    gfx.text.clear();
    assert(f.call(696, { Fixture::text + 10, rp, Fixture::extent, 0 }, { 11, 0xFFFF, 24, 8 }) == 3);
    assert(gfx.text == "dlro");
    /* a 68k constraining extent, read whole: 16 pixels wide fit two */
    auto limit = [&](UWORD width, UWORD height, WORD minx, WORD miny, WORD maxx, WORD maxy) {
        m68k_write16(&f.ctx, Fixture::limit + TE_WIDTH, width);
        m68k_write16(&f.ctx, Fixture::limit + TE_HEIGHT, height);
        m68k_write16(&f.ctx, Fixture::limit + TE_MINX, (UWORD)minx);
        m68k_write16(&f.ctx, Fixture::limit + TE_MINY, (UWORD)miny);
        m68k_write16(&f.ctx, Fixture::limit + TE_MAXX, (UWORD)maxx);
        m68k_write16(&f.ctx, Fixture::limit + TE_MAXY, (UWORD)maxy);
    };
    gfx.text.clear();
    limit(16, 8, 0, -6, 15, 1);
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, Fixture::limit }, { 11, 1, 400, 8 }) == 2);
    /* its MaxX narrower than its width, its height or MaxY too small */
    limit(400, 8, 0, -6, 23, 1);
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, Fixture::limit }, { 11, 1, 400, 8 }) == 3);
    limit(400, 7, 0, -6, 399, 1);
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, Fixture::limit }, { 11, 1, 400, 8 }) == 0);
    limit(400, 8, 0, -6, 399, 0);
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, Fixture::limit }, { 11, 1, 400, 8 }) == 0);
    /* too little height: nothing fits, and the TextExtent is cleared */
    assert(f.call(696, { Fixture::text, rp, Fixture::extent, 0 }, { 5, 1, 40, 7 }) == 0);
    for (ULONG i = 0; i < 12; ++i)
        assert(f.memory[Fixture::extent + i] == 0);
    assert(!f.stopped());

    /* characters, TextExtent or constraint outside guest memory */
    const unsigned calls = gfx.any;
    f.call(696, { 0x10, rp, Fixture::extent, 0 }, { 32, 0xFFFF, 400, 8 });
    assert(f.stopped());
    f.call(696, { static_cast<ULONG>(f.memory.size() - 4), rp, Fixture::extent, 0 }, { 5, 1, 400, 8 });
    assert(f.stopped());
    f.call(696, { Fixture::text, rp, static_cast<ULONG>(f.memory.size() - 4) }, { 5, 1, 400, 8 });
    assert(f.stopped());
    f.call(696, { Fixture::text, rp, Fixture::extent, 0xFFFFFFF0 }, { 5, 1, 400, 8 });
    assert(f.stopped() && gfx.any == calls);
    /* no characters: no string needed */
    assert(f.call(696, { 0, rp, Fixture::extent, 0 }, { 0x10000, 1, 400, 8 }) == 0 && !f.stopped());
}

static void test_setfont()
{
    Fixture f;
    const ULONG rp = f.rastport();

    f.put(Fixture::name, "other.font");
    m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    const ULONG font = f.call(72, { Fixture::attr });
    assert(font && guest_font(f, &other) == font);
    f.call(66, { font, rp });
    assert(native_rp.Font == &other && f.lng(rp + RP_FONT) == font &&
           f.word(rp + RP_TXHEIGHT) == 11 && f.word(rp + RP_TXWIDTH) == 7 &&
           f.word(rp + RP_TXBASELINE) == 8);
    /* closed but still shown by the RastPort: it stays */
    f.call(78, { 0, font });
    assert(guest_font(f, &other) == font);
    /* back to the font the window came with, which stayed; the closed font
       goes */
    const ULONG window_font = guest_font(f, &topaz);
    assert(window_font);
    f.call(66, { window_font, rp });
    assert(native_rp.Font == &topaz && f.lng(rp + RP_FONT) == window_font);
    assert(!guest_font(f, &other) && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
    /* NULL is the default font, as AROS has it */
    f.call(66, { font, rp });
    assert(f.stopped());
    f.call(66, { 0, rp });
    assert(native_rp.Font == &topaz && f.lng(rp + RP_FONT) == window_font && !f.stopped());
    /* anything else stops the program */
    const unsigned sets = gfx.set_font;
    f.call(66, { Fixture::raw, rp });
    assert(f.stopped() && gfx.set_font == sets);
}

static void test_refusals()
{
    Fixture f;
    int native_window;

    /* raw guest bytes and the shadow of a window are no RastPorts */
    f.ctx.shadow_map[f.ctx.num_shadows].m68k_addr = Fixture::raw + 0x40;
    f.ctx.shadow_map[f.ctx.num_shadows].native_ptr = &native_window;
    f.ctx.shadow_map[f.ctx.num_shadows++].type = KAFFEINE_SHADOW_WINDOW;
    f.put(Fixture::text, "Hello");
    for (ULONG bad : { Fixture::raw, Fixture::raw + 0x40, 0U })
    {
        /* every other argument valid, so that the RastPort is what stops it */
        for (ULONG lvo : { 54U, 60U, 240U, 246U, 306U, 318U, 324U, 342U, 348U, 354U,
                           396U, 474U, 690U, 696U, 894U })
        {
            assert(f.call(lvo, { Fixture::text, bad, Fixture::extent, 0 }, { 1, 1, 400, 8 }) == 20);
            assert(f.stopped());
        }
        if (bad != Fixture::raw)
            assert(f.call(66, { 0, bad }) == 20 && f.stopped());
        for (ULONG lvo : { 858U, 864U, 870U, 876U })
        {
            assert(f.call(lvo, { bad }) == 20);
            assert(f.stopped());
        }
    }
    assert(!gfx.any);
}

/* ── AskFont() ───────────────────────────────────────────────────── */

using Memory = std::array<UBYTE, 0x10000>;

/* Generated poison, different for every call, so that no stale byte can
   pass for a written one */
static ULONG poison_seed = 0x9E3779B9;
static void poison_bytes(Fixture &f, ULONG addr, ULONG size)
{
    for (ULONG i = 0; i < size; ++i)
    {
        poison_seed ^= poison_seed << 13;
        poison_seed ^= poison_seed >> 17;
        poison_seed ^= poison_seed << 5;
        f.memory[addr + i] = UBYTE(poison_seed >> 11);
    }
}

/* the classic TextAttr, big-endian: name32, size16, style8, flags8 */
static void expect_attr(const Fixture &f, ULONG out, ULONG name, UWORD ysize, UBYTE style, UBYTE flags)
{
    const UBYTE want[M68K_SIZEOF_TEXTATTR] =
        { UBYTE(name >> 24), UBYTE(name >> 16), UBYTE(name >> 8), UBYTE(name),
          UBYTE(ysize >> 8), UBYTE(ysize), style, flags };
    assert(std::memcmp(f.memory.data() + out, want, sizeof(want)) == 0);
}

/* Only the TextAttr changed, and the 68k RastPort that is settled after
   the call (rp 0 for none). */
static void expect_changes_within(const Memory &before, const Fixture &f, ULONG out, ULONG rp)
{
    for (ULONG i = 0; i < before.size(); ++i)
        if (before[i] != f.memory[i])
            assert((out && i >= out && i < out + M68K_SIZEOF_TEXTATTR) ||
                   (rp && i >= rp && i < rp + M68K_SIZEOF_RASTPORT));
}

/* No half of a native pointer reaches the name. */
static void expect_no_native_pointer(const Fixture &f, ULONG out)
{
    for (const void *p : { static_cast<const void *>(topaz_name), static_cast<const void *>(other_name),
                           static_cast<const void *>(&topaz), static_cast<const void *>(&other),
                           static_cast<const void *>(&native_rp) })
    {
        const uint64_t v = reinterpret_cast<uintptr_t>(p);
        for (ULONG half : { ULONG(v), ULONG(v >> 32) })
            assert(!half || f.lng(out) != half);
    }
}

static ULONG font_size(const Fixture &f, ULONG font)
{
    for (const auto &entry : f.ctx.fonts)
        if (entry.guest == font)
            return entry.allocation_size;
    assert(false);
    return 0;
}

static void test_classic_rastport_init()
{
    Fixture f;
    const ULONG rp = Fixture::raw;
    assert(!f.call(198, {0, rp}) && !f.stopped());
    assert(gfx.init_rp == 1 && !f.ctx.num_shadows);
    const ULONG font = f.lng(rp + RP_FONT);
    assert(font && Kaffeine_FontNative(&f.ctx, font) == &topaz);
    UBYTE want[M68K_SIZEOF_RASTPORT] = {};
    want[RP_MASK] = want[RP_FGPEN] = want[RP_AOLPEN] = 0xFF;
    want[RP_DRAWMODE] = JAM2;
    want[RP_LINEPTRN] = want[RP_LINEPTRN + 1] = 0xFF;
    for (unsigned i = 0; i < 8; ++i) want[RP_MINTERMS + i] = 0xCA;
    want[RP_FONT] = UBYTE(font >> 24); want[RP_FONT + 1] = UBYTE(font >> 16);
    want[RP_FONT + 2] = UBYTE(font >> 8); want[RP_FONT + 3] = UBYTE(font);
    want[RP_TXHEIGHT + 1] = 8; want[RP_TXWIDTH + 1] = 8; want[RP_TXBASELINE + 1] = 6;
    assert(!std::memcmp(f.memory.data() + rp, want, sizeof(want)));
    for (unsigned i = 0; i < 16; ++i)
        assert(f.memory[rp - 1 - i] == 0xA5 && f.memory[rp + sizeof(want) + i] == 0xA5);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    const ULONG edge = f.memory.size() - M68K_SIZEOF_RASTPORT;
    assert(!f.call(198, {0, edge}));
    assert(f.lng(edge + RP_FONT) == font && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
    const ULONG owned = f.rastport();
    for (ULONG bad : {0U, rp + 1, edge + 2, 0xFFFFFFF0U, owned, owned + 2, font})
    {
        const auto before = f.memory; const auto calls = gfx.any;
        assert(f.call(198, {0, bad}) == 20 && f.stopped());
        assert(before == f.memory && gfx.any == calls);
    }
    Kaffeine_RastPortDelete(&f.ctx, owned);
    assert(Kaffeine_FontNative(&f.ctx, font) == &topaz);
    Kaffeine_FontsCleanup(&f.ctx);
    assert(!Kaffeine_FontNative(&f.ctx, font) && !gfx.close_font);
}

static void test_classic_rastport_fonts()
{
    Fixture f;
    const ULONG rp = Fixture::raw, copy = rp + 0x100, attr = Fixture::attr;
    assert(!f.call(198, {0, rp}));
    const ULONG default_font = f.lng(rp + RP_FONT);
    f.put(Fixture::name, "other.font");
    m68k_write32(&f.ctx, attr, Fixture::name);
    const ULONG font = f.call(72, {attr});
    assert(font && font != default_font);
    const auto before = f.memory;
    assert(!f.call(66, {font, rp}) && !f.stopped());
    assert(f.lng(rp + RP_FONT) == font && f.word(rp + RP_TXHEIGHT) == 11 &&
           f.word(rp + RP_TXWIDTH) == 7 && f.word(rp + RP_TXBASELINE) == 8);
    for (ULONG i = 0; i < before.size(); ++i)
        if (before[i] != f.memory[i])
            assert((i >= rp + RP_FONT && i < rp + RP_FONT + 4) ||
                   (i >= rp + RP_TXHEIGHT && i < rp + RP_TXBASELINE + 2));
    std::memcpy(f.memory.data() + copy, f.memory.data() + rp, M68K_SIZEOF_RASTPORT);
    f.put(Fixture::text, "abc");
    m68k_write16(&f.ctx, copy + RP_TXSPACING, UWORD(-2));
    const auto measured_before = f.memory;
    assert(f.call(54, {Fixture::text, copy}, {3}) == 15 && !f.stopped());
    assert(f.memory == measured_before);
    assert(!f.call(54, {0, copy}, {0}) && !f.stopped());
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    for (unsigned i = 0; i < 64; ++i)
    {
        const ULONG source = i & 1 ? rp : copy;
        poison_bytes(f, attr, M68K_SIZEOF_TEXTATTR);
        const auto query_before = f.memory;
        assert(!f.call(474, {attr, source}) && !f.stopped());
        expect_attr(f, attr, font + M68K_SIZEOF_TEXTFONT, 11, other.tf_Style, other.tf_Flags);
        expect_changes_within(query_before, f, attr, 0);
        assert(gfx.ask_rp != &native_rp && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
        expect_no_native_pointer(f, attr);
    }
    assert(!f.call(66, {0, rp}) && f.lng(rp + RP_FONT) == default_font);
    const auto calls = gfx.any;
    for (ULONG bad : {0U, rp + 1, f.ctx.mem_size - 2, 0xFFFFFFF0U})
        assert(f.call(66, {font, bad}) == 20 && f.stopped());
    assert(gfx.any == calls);
    f.call(78, {0, font});
    assert(!Kaffeine_FontNative(&f.ctx, font));
    const auto closed_before = f.memory;
    assert(f.call(474, {attr, copy}) == 20 && f.stopped());
    assert(closed_before == f.memory && gfx.any == calls + 1);
    assert(f.call(54, {Fixture::text, copy}, {3}) == 20 && f.stopped());
    assert(closed_before == f.memory && gfx.any == calls + 1);
    Kaffeine_FontsCleanup(&f.ctx);
}

static void test_askfont()
{
    Fixture f;
    constexpr ULONG out = 0x2A00;

    /* An extended bold underlined font; the RastPort has another soft style
       and text height, which AskFont() does not report. */
    topaz.tf_Style = FSF_UNDERLINED | 0x02 | FSF_TAGGED;
    topaz.tf_Flags = FPF_ROMFONT | FPF_DESIGNED;
    native_rp.AlgoStyle = 0x04;
    native_rp.TxHeight = 13;
    const ULONG rp = f.rastport();
    const ULONG font = guest_font(f, &topaz);
    const ULONG name = font + M68K_SIZEOF_TEXTFONT;
    assert(font && f.lng(font + LN_NAME) == name);
    f.memory[rp + RP_ALGOSTYLE] = 0x05;

    /* Repeated calls: the same name, no allocation, no new shadow or font,
       no generated thunk (Fixture::call()), A0 the TextAttr and A1 the
       RastPort, every other register preserved. */
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    const UWORD shadows = f.ctx.num_shadows;
    for (unsigned n = 0; n < 64; ++n)
    {
        poison_bytes(f, out - 16, 40);
        const Memory before = f.memory;
        assert(f.call(474, { out, rp }) == 0 && !f.stopped());
        assert(gfx.ask_font == n + 1 && gfx.any == n + 1 && gfx.ask_rp == &native_rp);
        expect_attr(f, out, name, 8, FSF_UNDERLINED | 0x02, FPF_ROMFONT | FPF_DESIGNED);
        expect_changes_within(before, f, out, rp);
        expect_no_native_pointer(f, out);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && f.ctx.num_shadows == shadows &&
               guest_font(f, &topaz) == font);
    }
    /* the name is the one of the 68k TextFont that rp->Font shows */
    assert(f.lng(out) == f.lng(f.lng(rp + RP_FONT) + LN_NAME));
    assert(std::strcmp(reinterpret_cast<char *>(f.memory.data() + name), "topaz.font") == 0);
    /* the RastPort's own style reached it, and both stay as they were */
    assert(native_rp.AlgoStyle == 0x05 && f.memory[rp + RP_ALGOSTYLE] == 0x05 &&
           f.word(rp + RP_TXHEIGHT) == 13);

    /* the name stays across other calls on the RastPort */
    f.put(Fixture::text, "Hello");
    f.call(60, { Fixture::text, rp }, { 5 });
    f.call(342, { 0, rp }, { 3 });
    assert(std::strcmp(reinterpret_cast<char *>(f.memory.data() + f.lng(out)), "topaz.font") == 0);

    /* the last eight bytes of the arena */
    const ULONG edge = f.ctx.mem_size - M68K_SIZEOF_TEXTATTR;
    poison_bytes(f, edge - 8, 16);
    const Memory before = f.memory;
    assert(f.call(474, { edge, rp }) == 0 && !f.stopped());
    expect_attr(f, edge, name, 8, FSF_UNDERLINED | 0x02, FPF_ROMFONT | FPF_DESIGNED);
    expect_changes_within(before, f, edge, rp);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail);
}

static void test_askfont_fonts()
{
    Fixture f;
    constexpr ULONG out = 0x2A00, kept = 0x2A20;
    const ULONG rp = f.rastport();
    const ULONG window_font = guest_font(f, &topaz);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);

    /* an italic proportional font of the program on the RastPort */
    other.tf_Style = 0x04;
    f.put(Fixture::name, "other.font");
    m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
    const ULONG font = f.call(72, { Fixture::attr });
    assert(font && font != window_font);
    f.call(66, { font, rp });
    poison_bytes(f, out, M68K_SIZEOF_TEXTATTR);
    assert(f.call(474, { out, rp }) == 0 && !f.stopped());
    const ULONG other_copy = font + M68K_SIZEOF_TEXTFONT;
    expect_attr(f, out, other_copy, 11, 0x04, FPF_PROPORTIONAL);
    expect_no_native_pointer(f, out);

    /* closed but still shown: the same copy */
    f.call(78, { 0, font });
    poison_bytes(f, kept, M68K_SIZEOF_TEXTATTR);
    assert(f.call(474, { kept, rp }) == 0 && !f.stopped());
    expect_attr(f, kept, other_copy, 11, 0x04, FPF_PROPORTIONAL);
    assert(std::strcmp(reinterpret_cast<char *>(f.memory.data() + other_copy), "other.font") == 0);

    /* back to the window's font: its own copy; the closed font goes */
    f.call(66, { window_font, rp });
    poison_bytes(f, out, M68K_SIZEOF_TEXTATTR);
    assert(f.call(474, { out, rp }) == 0 && !f.stopped());
    expect_attr(f, out, window_font + M68K_SIZEOF_TEXTFONT, 8, 0, FPF_ROMFONT);
    assert(!guest_font(f, &other) && Kaffeine_HeapAvail(&f.ctx, 0) == avail);

    /* a font without a name gives a NULL name, as native AskFont() */
    TextFont anon;
    std::memset(&anon, 0, sizeof(anon));
    anon.tf_YSize = 6;
    anon.tf_Style = FSF_UNDERLINED;
    anon.tf_Flags = FPF_DESIGNED;
    gfx.open_result = &anon;
    f.put(Fixture::name, "anon.font");
    const ULONG anon_font = f.call(72, { Fixture::attr });
    assert(anon_font && f.lng(anon_font + LN_NAME) == 0);
    f.call(66, { anon_font, rp });
    poison_bytes(f, out, M68K_SIZEOF_TEXTATTR);
    assert(f.call(474, { out, rp }) == 0 && !f.stopped());
    expect_attr(f, out, 0, 6, FSF_UNDERLINED, FPF_DESIGNED);
    f.call(78, { 0, anon_font });
    f.call(66, { window_font, rp });
    assert(!guest_font(f, &anon) && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
}

static void test_askfont_refusals()
{
    constexpr ULONG out = 0x2A00;

    /* Stopped before native AskFont(), and with no byte of guest memory
       changed; after the RastPort was taken, only it is settled. */
    auto refused = [](Fixture &f, ULONG a0, ULONG a1, ULONG settled) {
        poison_bytes(f, out, M68K_SIZEOF_TEXTATTR);
        const Memory before = f.memory;
        const unsigned asks = gfx.ask_font, calls = gfx.any;
        assert(f.call(474, { a0, a1 }) == 20 && f.stopped());
        assert(gfx.ask_font == asks && gfx.any == calls);
        if (settled)
            expect_changes_within(before, f, 0, settled);
        else
            assert(f.memory == before);
    };

    /* the TextAttr: NULL, odd, partly or wholly outside the arena, on the
       RastPort, or on a 68k TextFont, shown or not, with its name */
    {
        Fixture f;
        const ULONG rp = f.rastport();
        const ULONG font = guest_font(f, &topaz);
        f.put(Fixture::name, "other.font");
        m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
        const ULONG unshown = f.call(72, { Fixture::attr });
        assert(unshown);
        const ULONG size = f.ctx.mem_size;
        for (ULONG bad : { 0U, out + 1, size - 7, size - 6, size - 1, size, 0xFFFFFFF8U, 0xFFFFFFFEU,
                           rp, rp + 92, rp - 6, rp + 98, font, font + M68K_SIZEOF_TEXTFONT,
                           (font + font_size(f, font) - 1) & ~1U, unshown + 4 })
            refused(f, bad, rp, 0);
        assert(!gfx.ask_font);
    }

    /* the RastPort: NULL, raw guest bytes, inside a 68k RastPort, the shadow
       of a window, and A0 and A1 swapped */
    {
        Fixture f;
        int native_window;
        const ULONG rp = f.rastport();
        f.ctx.shadow_map[f.ctx.num_shadows++] = { Fixture::raw + 0x40, &native_window, KAFFEINE_SHADOW_WINDOW };
        for (ULONG bad : { 0U, Fixture::raw, rp + 2, Fixture::raw + 0x40 })
            refused(f, out, bad, 0);
        refused(f, rp, out, 0);
    }

    /* the font: none natively, a guest rp->Font that is not the 68k TextFont
       of the native font, a color font that has no 68k TextFont, a full
       font table */
    {
        Fixture f;
        const ULONG rp = f.rastport();
        native_rp.Font = nullptr;
        refused(f, out, rp, rp);
    }
    for (int field = 0; field < 3; ++field)
    {
        Fixture f;
        const ULONG rp = f.rastport();
        f.put(Fixture::name, "other.font");
        m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
        const ULONG unshown = f.call(72, { Fixture::attr });
        m68k_write32(&f.ctx, rp + RP_FONT, field == 0 ? 0U : field == 1 ? Fixture::raw : unshown);
        refused(f, out, rp, rp);
    }
    {
        Fixture f;
        const ULONG rp = f.rastport();
        static UBYTE planes[8];
        topaz.tf_Style = FSF_COLORFONT;
        topaz.tf_CharData = planes;
        refused(f, out, rp, rp);
    }
    {
        Fixture f;
        const ULONG rp = f.rastport();
        for (unsigned i = 0; i < KAFFEINE_MAX_FONTS; ++i)
            if (!f.ctx.fonts[i].guest)
            {
                f.ctx.fonts[i].guest = 0xE000 + 8 * i;
                f.ctx.fonts[i].allocation_size = 4;
            }
        native_rp.Font = &other;
        m68k_write32(&f.ctx, rp + RP_FONT, 0);
        refused(f, out, rp, rp);
    }

    /* After native AskFont(): a name that is not the font's, or a 68k
       TextFont whose name is no more its copy, write nothing. */
    for (int wrong = 0; wrong < 2; ++wrong)
    {
        Fixture f;
        const ULONG rp = f.rastport();
        if (wrong)
            m68k_write32(&f.ctx, guest_font(f, &topaz) + LN_NAME, Fixture::raw);
        else
            gfx.ask_wrong_name = true;
        poison_bytes(f, out, M68K_SIZEOF_TEXTATTR);
        const Memory before = f.memory;
        assert(f.call(474, { out, rp }) == 20 && f.stopped() && gfx.ask_font == 1);
        expect_changes_within(before, f, 0, rp);
    }
}

static void test_guest_planar_read()
{
    constexpr ULONG rp=Fixture::raw,bm=0x2400,first_plane=0x2808;
    const std::array<UBYTE,32> colors={
        1,2,4,8,16,32,64,128,255,0,128,1,170,85,240,15,
        17,34,51,68,90,102,119,136,153,165,187,204,221,238,254,127};
    auto color=[&](unsigned x,unsigned y)->ULONG {
        return y==0?colors[x]:y==1?255-colors[x]:(x*37+11)&255;
    };
    auto setup=[&](Fixture &f,unsigned depth) {
        std::memset(f.memory.data()+rp,0,100);
        std::memset(f.memory.data()+bm,0,40);
        m68k_write32(&f.ctx,rp+4,bm);
        m68k_write16(&f.ctx,bm,4);m68k_write16(&f.ctx,bm+2,3);
        f.memory[bm+4]=depth==2?8:0;f.memory[bm+5]=depth;
        for(unsigned p=0;p<depth;++p)
        {
            ULONG plane=first_plane+p*32;
            std::memset(f.memory.data()+plane,0,12);
            m68k_write32(&f.ctx,bm+8+p*4,plane);
            for(unsigned y=0;y<3;++y)for(unsigned x=0;x<32;++x)
                if(color(x,y)&(1U<<p))f.memory[plane+y*4+(x>>3)]|=0x80>>(x&7);
        }
    };
    for(unsigned depth:{1U,2U,8U})
    {
        Fixture f;setup(f,depth);
        if(depth<8)m68k_write32(&f.ctx,bm+36,0xFFFFFFFE);
        auto before=f.memory;ULONG mask=(1U<<depth)-1;
        for(unsigned y=0;y<3;++y)for(unsigned x=0;x<32;++x)
        {
            assert(f.call(318,{0xDEADBEEF,rp},{x,y})==(color(x,y)&mask));
            assert(f.ctx.running && !f.ctx.exit_code && f.memory==before && !gfx.any);
        }
        for(auto xy:{std::array<ULONG,2>{0xFFFFFFFF,0},{0,0xFFFFFFFF},
                     {0x8000,0},{0,0x8000},{32,0},{0,3},{32767,0},{0,32767}})
            assert(f.call(318,{0,rp},{xy[0],xy[1]})==20 && f.stopped() && f.memory==before && !gfx.any);
        assert(f.call(318,{0,rp},{0x10007,0xFFFF0001})==(color(7,1)&mask));
        assert(f.call(318,{0,rp},{0x1FFFF,0})==20 && f.stopped() && f.memory==before && !gfx.any);
    }
    for(unsigned bad=0;bad<22;++bad)
    {
        Fixture f;setup(f,8);ULONG address=rp;int foreign;
        switch(bad)
        {
            case 0:address=0;break;
            case 1:address=rp+1;break;
            case 2:address=f.ctx.mem_size-98;break;
            case 3:m68k_write32(&f.ctx,rp,0x12345678);break;
            case 4:m68k_write32(&f.ctx,rp+4,0);break;
            case 5:m68k_write32(&f.ctx,rp+4,bm+1);break;
            case 6:m68k_write32(&f.ctx,rp+4,f.ctx.mem_size-38);break;
            case 7:m68k_write16(&f.ctx,bm,0);break;
            case 8:m68k_write16(&f.ctx,bm,1);break;
            case 9:m68k_write16(&f.ctx,bm+2,0);break;
            case 10:f.memory[bm+5]=0;break;
            case 11:f.memory[bm+5]=9;break;
            case 12:f.memory[bm+4]=4;break;
            case 13:f.memory[bm+4]=0x80;break;
            case 14:m68k_write16(&f.ctx,bm+6,1);break;
            case 15:m68k_write32(&f.ctx,bm+36,f.ctx.mem_size-8);break;
            case 16:m68k_write32(&f.ctx,bm+8,0xFFFFFFFE);break;
            case 17:m68k_write16(&f.ctx,bm,65534);m68k_write16(&f.ctx,bm+2,65535);break;
            case 18:f.ctx.shadow_map[f.ctx.num_shadows++]={rp,&foreign,KAFFEINE_SHADOW_WINDOW};break;
            case 19:f.ctx.shadow_map[f.ctx.num_shadows++]={bm,&foreign,KAFFEINE_SHADOW_WINDOW};break;
            case 20:m68k_write32(&f.ctx,bm+8,0);break;
            case 21:m68k_write32(&f.ctx,bm+8,0xFFFFFFFF);break;
        }
        const auto before=f.memory;
        assert(f.call(318,{0,address},{0,0})==20 && f.stopped() && f.memory==before && !gfx.any);
    }
    Fixture f;setup(f,8);auto before=f.memory;
    assert(f.call(324,{0,rp},{0,0})==20 && f.stopped() && f.memory==before && !gfx.any);
    assert(f.call(306,{0,rp},{0,0,1,1})==20 && f.stopped() && f.memory==before && !gfx.any);
}

/* ── Generated thunks, state of caller-owned RastPorts, rasters ──── */

enum { RP_AREAPTSZ = 29, RP_FLAGS = 32, RP_PENWIDTH = 48, RP_PENHEIGHT = 50, RP_TXFLAGS = 57 };

/* Generated SetRast(), BltTemplate() and ClipBlit() (two RastPorts) reach
   native graphics only with 68k RastPorts; anything else stops them before
   it, every RastPort argument checked. */
static void test_generated_guard()
{
    Fixture f;
    const ULONG rp = f.rastport(), raw = Fixture::raw + 0x100;
    int native_window;
    f.ctx.shadow_map[f.ctx.num_shadows++] = { Fixture::raw + 0x40, &native_window, KAFFEINE_SHADOW_WINDOW };
    std::memset(f.memory.data() + raw, 0, M68K_SIZEOF_RASTPORT);
    f.put(Fixture::text, "\xFF\x81\x81\xFF");
    generated_native = 0;
    assert(f.call(234, { 0, rp }, { 1 }) == 0 && !f.stopped() && generated_native == 1);
    assert(f.call(552, { rp, rp }) == 0 && !f.stopped() && generated_native == 2);
    for (ULONG bad : { Fixture::raw + 0x40, raw, 0U, rp + 2, static_cast<ULONG>(f.memory.size()) })
    {
        const Memory before = f.memory;
        assert(f.call(234, { 0, bad }, { 1 }) == 20 && f.stopped());
        assert(f.call(36, { Fixture::text, bad }, { 0, 2, 0, 0, 8, 2 }) == 20 && f.stopped());
        assert(f.call(552, { bad, rp }) == 20 && f.stopped());
        assert(f.memory == before);
        assert(f.call(552, { rp, bad }) == 20 && f.stopped());
        expect_changes_within(before, f, 0, rp);
        assert(generated_native == 2 && !gfx.any);
    }
}

/* The classic RastPort that a program built for itself: its pointers,
   RP_User, the reserved bytes, the pad byte and guard bytes on each side
   poisoned; scalars known, with private and unknown Flags bits. */
static Memory state_rastport(Fixture &f, ULONG rp, ULONG guard)
{
    poison_bytes(f, rp - guard, M68K_SIZEOF_RASTPORT + 2 * guard);
    UBYTE *g = f.memory.data() + rp;
    g[RP_MASK] = 0x0F; g[RP_FGPEN] = 1; g[RP_BGPEN] = 2; g[RP_AOLPEN] = 3;
    g[RP_DRAWMODE] = JAM1; g[RP_AREAPTSZ] = 0xFC; g[RP_LINPATCNT] = 7;
    m68k_write16(&f.ctx, rp + RP_FLAGS, 0xC090 | NOCROSSFILL);
    m68k_write16(&f.ctx, rp + RP_LINEPTRN, 0xF0F0);
    m68k_write16(&f.ctx, rp + RP_CP_X, 0x0102);
    m68k_write16(&f.ctx, rp + RP_CP_Y, 0xFFFE);
    for (unsigned i = 0; i < 8; ++i) g[RP_MINTERMS + i] = 0x11 + i;
    m68k_write16(&f.ctx, rp + RP_PENWIDTH, 1);
    m68k_write16(&f.ctx, rp + RP_PENHEIGHT, 1);
    g[RP_ALGOSTYLE] = 1; g[RP_TXFLAGS] = 2;
    m68k_write16(&f.ctx, rp + RP_TXHEIGHT, 9);
    m68k_write16(&f.ctx, rp + RP_TXWIDTH, 7);
    m68k_write16(&f.ctx, rp + RP_TXBASELINE, 6);
    m68k_write16(&f.ctx, rp + RP_TXSPACING, 0xFFFF);
    return f.memory;
}

/* What native graphics saw: a temporary of the bridge, outside guest
   memory and never a registered one, with every pointer NULL and only the
   public Flags. */
static void expect_temporary(const Fixture &f, UWORD flags)
{
    const RastPort &s = gfx.seen;
    const auto at = reinterpret_cast<uintptr_t>(gfx.state_rp);
    const auto mem = reinterpret_cast<uintptr_t>(f.memory.data());
    assert(gfx.state_rp != &native_rp && (at + sizeof(RastPort) <= mem || at >= mem + f.memory.size()));
    assert(!s.Layer && !s.BitMap && !s.AreaPtrn && !s.TmpRas && !s.AreaInfo && !s.GelsInfo &&
           !s.Font && !s.RP_User);
    assert(s.Flags == flags);
}

static void set_minterms(Memory &want, ULONG rp, std::initializer_list<UBYTE> m)
{
    unsigned i = 0;
    for (UBYTE v : m) want[rp + RP_MINTERMS + i++] = v;
}

/* State calls on a caller-owned classic RastPort, outside the heap as in
   a hunk or on the stack, in an AllocMem() block and at an offset in one:
   native graphics gets the scalars, and exactly the changed ones come
   back, big-endian, as rom/graphics computes them. */
static void test_classic_rastport_state()
{
    for (int where = 0; where < 3; ++where)
    {
        Fixture f;
        const ULONG rp = where == 0 ? Fixture::raw : where == 1
            ? Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT, 0)
            : Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT + 64, 0) + 32;
        assert(rp > 32);
        const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
        /* guards inside the arena's own bytes or the block, never on a
           heap header */
        Memory want = state_rastport(f, rp, where == 1 ? 0 : 16);

        /* SetAPen(): FgPen as a UBYTE and linpatcnt; no GenMinterms() */
        assert(f.call(342, { 0, rp }, { 0x1F3 }) == 0 && !f.stopped());
        want[rp + RP_FGPEN] = 0xF3; want[rp + RP_LINPATCNT] = 15;
        assert(f.memory == want);
        expect_temporary(f, NOCROSSFILL);
        assert(gfx.seen.FgPen == 1 && gfx.seen.cp_y == -2 && gfx.seen.TxSpacing == -1 &&
               gfx.seen.minterms[7] == 0x18 && gfx.seen.TxHeight == 9 && gfx.seen.Mask == 0x0F);

        /* SetBPen() in JAM1: FgPen 0xF3 gives the minterms 0xEA and 0x2A */
        assert(f.call(348, { 0, rp }, { 3 }) == 0 && !f.stopped());
        want[rp + RP_BGPEN] = 3;
        set_minterms(want, rp, { 0xEA, 0xEA, 0x2A, 0x2A, 0xEA, 0xEA, 0xEA, 0xEA });
        assert(f.memory == want);

        /* SetDrMd() takes the low byte: COMPLEMENT | INVERSVID */
        assert(f.call(354, { 0, rp }, { 0x106 }) == 0 && !f.stopped());
        want[rp + RP_DRAWMODE] = COMPLEMENT | INVERSVID;
        set_minterms(want, rp, { 0x6A, 0x6A, 0x6A, 0x6A, 0x6A, 0x6A, 0x6A, 0x6A });
        assert(f.memory == want);

        /* SetABPenDrMd(): BgPen is already 3, so only its byte stays */
        assert(f.call(894, { 0, rp }, { 5, 3, JAM2 }) == 0 && !f.stopped());
        want[rp + RP_FGPEN] = 5; want[rp + RP_DRAWMODE] = JAM2;
        set_minterms(want, rp, { 0xFA, 0x3A, 0xCA, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A });
        assert(f.memory == want && gfx.apen == 5 && gfx.bpen == 3 && gfx.drmd == JAM2);

        /* Move() with signed WORD coordinates from the low halves; FRST_DOT
           joins the program's own bits */
        f.memory[rp + RP_LINPATCNT] = 3; want[rp + RP_LINPATCNT] = 3;
        assert(f.call(240, { 0, rp }, { 0xFFFFFFFB, 0x12340007 }) == 0 && !f.stopped());
        want[rp + RP_CP_X] = 0xFF; want[rp + RP_CP_X + 1] = 0xFB;
        want[rp + RP_CP_Y] = 0x00; want[rp + RP_CP_Y + 1] = 0x07;
        want[rp + RP_LINPATCNT] = 15;
        want[rp + RP_FLAGS] = 0xC0; want[rp + RP_FLAGS + 1] = 0x90 | NOCROSSFILL | FRST_DOT;
        assert(f.memory == want);
        expect_temporary(f, NOCROSSFILL);

        /* getters read the guest each time and write nothing */
        assert(f.call(858, { rp }) == 5 && f.call(864, { rp }) == 3 &&
               f.call(870, { rp }) == JAM2 && f.call(876, { rp }) == 3);
        expect_temporary(f, NOCROSSFILL | FRST_DOT);
        f.memory[rp + RP_FGPEN] = 0x80; want[rp + RP_FGPEN] = 0x80;
        assert(f.call(858, { rp }) == 0x80 && f.memory == want && !f.stopped());

        /* nothing registered, allocated, settled or closed */
        assert(!f.ctx.num_shadows && Kaffeine_HeapAvail(&f.ctx, 0) == avail);
        assert(!f.ctx.settle_rastports[0] && !f.ctx.settle_rastports[1]);
        assert(gfx.state == 10 && gfx.any == 10 && !gfx.close_font);

        /* drawing on it still stops before native graphics */
        f.put(Fixture::text, "Hello");
        m68k_write32(&f.ctx, rp + RP_LAYER, 0x12345678);
        want = f.memory;
        assert(f.call(246, { 0, rp }, { 1, 1 }) == 20 && f.stopped());
        assert(f.call(306, { 0, rp }, { 0, 0, 1, 1 }) == 20 && f.stopped());
        assert(f.call(324, { 0, rp }, { 0, 0 }) == 20 && f.stopped());
        assert(f.call(318, { 0, rp }, { 0, 0 }) == 20 && f.stopped());
        assert(f.call(60, { Fixture::text, rp }, { 5 }) == 20 && f.stopped());
        assert(f.call(396, { 0, rp }, { 1, 1, 0, 0, 8, 8 }) == 20 && f.stopped());
        assert(f.call(36, { Fixture::text, rp }, { 0, 2, 0, 0, 8, 2 }) == 20 && f.stopped());
        assert(f.memory == want && gfx.any == 10 && !generated_calls);
    }
}

/* A copy of a window's 68k RastPort is the program's own: the state calls
   change the copy, never the window's native RastPort or its 68k one, and
   the copied font pointer is neither followed nor written. */
static void test_copied_rastport_state()
{
    Fixture f;
    const ULONG rp = f.rastport(), copy = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT, 0);
    assert(copy);
    std::memcpy(f.memory.data() + copy, f.memory.data() + rp, M68K_SIZEOF_RASTPORT);
    const BYTE fg = native_rp.FgPen, lp = native_rp.linpatcnt;
    const WORD x = native_rp.cp_x, y = native_rp.cp_y;
    const UWORD flags = native_rp.Flags;
    Memory want = f.memory;
    assert(f.call(342, { 0, copy }, { 7 }) == 0 && f.call(240, { 0, copy }, { 10, 20 }) == 0);
    want[copy + RP_FGPEN] = 7; want[copy + RP_LINPATCNT] = 15;
    want[copy + RP_CP_X + 1] = 10; want[copy + RP_CP_Y + 1] = 20;
    want[copy + RP_FLAGS + 1] |= FRST_DOT;
    want[copy + RP_CP_X] = want[copy + RP_CP_Y] = 0;
    assert(f.memory == want && !f.stopped());
    expect_temporary(f, 0);
    assert(native_rp.FgPen == fg && native_rp.linpatcnt == lp && native_rp.cp_x == x &&
           native_rp.cp_y == y && native_rp.Flags == flags && native_rp.Font == &topaz);
    /* the window's own RastPort still goes the registered way */
    assert(f.call(342, { 0, rp }, { 4 }) == 0 && gfx.state_rp == &native_rp && native_rp.FgPen == 4);
    assert(f.memory[copy + RP_FGPEN] == 7 && f.memory[rp + RP_FGPEN] == 4);
}

/* A registered RastPort: native private Flags stay native and are never
   shown; the program's own bits stay in its 68k RastPort, and only the
   public ones reach native graphics. */
static void test_registered_private_flags()
{
    Fixture f;
    native_rp.Flags = RPF_NO_PENS | RPF_REMAP_COLORFONTS | 0x0100 | AREAOUTLINE;
    const ULONG rp = f.rastport();
    assert(f.word(rp + RP_FLAGS) == AREAOUTLINE);
    m68k_write16(&f.ctx, rp + RP_FLAGS, 0xFFFF);
    assert(f.call(354, { 0, rp }, { JAM2 }) == 0);
    assert(native_rp.Flags == (RPF_NO_PENS | RPF_REMAP_COLORFONTS | 0x0100 |
                               FRST_DOT | ONE_DOT | DBUFFER | AREAOUTLINE | NOCROSSFILL));
    assert(gfx.seen.Flags == native_rp.Flags && f.word(rp + RP_FLAGS) == 0xFFFF);
    m68k_write16(&f.ctx, rp + RP_FLAGS, 0);
    assert(f.call(342, { 0, rp }, { 2 }) == 0);
    assert(native_rp.Flags == (RPF_REMAP_COLORFONTS | 0x0100) && f.word(rp + RP_FLAGS) == 0);
    m68k_write16(&f.ctx, rp + RP_FLAGS, RPF_NO_PENS | 0x0010);
    assert(f.call(240, { 0, rp }, { 1, 1 }) == 0 && !f.stopped());
    assert(native_rp.Flags == (RPF_REMAP_COLORFONTS | 0x0100 | FRST_DOT));
    assert(f.word(rp + RP_FLAGS) == (RPF_NO_PENS | 0x0010 | FRST_DOT));
}

/* Storage that is not the program's own classic RastPort stops every
   state call before native graphics, with no byte changed. */
static void test_state_refusals()
{
    Fixture f;
    const ULONG owned = f.rastport(), font = guest_font(f, &topaz);
    const ULONG block = Kaffeine_HeapAlloc(&f.ctx, 200, 0);
    const ULONG mirror = Kaffeine_MirrorAlloc(&f.ctx, 200);
    const ULONG pool = Kaffeine_PoolCreate(&f.ctx, 0, 1024, 512);
    /* freed last, so that no later allocation takes its place */
    const ULONG freed = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT, 0);
    Kaffeine_HeapFree(&f.ctx, freed, M68K_SIZEOF_RASTPORT);
    int native_window;
    f.ctx.shadow_map[f.ctx.num_shadows++] = { 0x2600, &native_window, KAFFEINE_SHADOW_WINDOW };
    f.ctx.memory_info = 0x3000;
    f.ctx.io_requests[0].guest = 0x3400;
    f.ctx.io_requests[0].capacity = 48;
    const ULONG base = f.ctx.libs[0].m68k_addr, size = f.ctx.mem_size;
    assert(font && freed && block && mirror && pool);
    for (ULONG bad : { 0U, Fixture::raw + 1, size - 98, size - 2, 0xFFFFFFF0U,
                       owned + 2, owned - 50, font, font + 4, freed, block - 8, mirror, pool,
                       0x2600U - 98, 0x2600U + 100, 0x3000U - 40, 0x3400U + 46,
                       f.ctx.libs[0].jt_start + 6, base - 50 })
    {
        const Memory before = f.memory;
        for (ULONG lvo : { 240U, 342U, 348U, 354U, 894U })
            assert(f.call(lvo, { 0, bad }, { 1, 1, 1 }) == 20 && f.stopped());
        for (ULONG lvo : { 858U, 864U, 870U, 876U })
            assert(f.call(lvo, { bad }) == 20 && f.stopped());
        assert(f.memory == before && !gfx.any);
    }
    /* the payload of the same allocation is fine */
    assert(f.call(342, { 0, block }, { 1 }) == 0 && !f.stopped() && gfx.state == 1);
}

/* AllocRaster() and FreeRaster(): guest heap planes of exactly RASSIZE()
   bytes, never native ones; FreeMem() of the guest frees them too. */
static ULONG io_keeps_calls;
static BOOL io_keeps(KaffeineContext *, ULONG, ULONG, BOOL) { ++io_keeps_calls; return FALSE; }

static void test_rasters()
{
    Fixture f;
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);
    /* 320 x 200: 40 bytes a row */
    const ULONG p = f.call(492, {}, { 320, 200 });
    assert(p >= f.ctx.heap_start && p < f.ctx.heap_end && !(p & 1) && !f.stopped());
    assert(Kaffeine_HeapAvail(&f.ctx, 0) <= avail - 8000);
    /* wrong sizes, interior, NULL-less refusal: nothing freed */
    for (auto args : { std::array<ULONG, 3>{ p, 321, 200 }, { p, 320, 199 }, { p, 0, 200 },
                       { p, 320, 0 }, { p + 8, 320, 200 }, { p - 8, 320, 200 }, { p + 1, 320, 200 } })
    {
        const Memory before = f.memory;
        const ULONG left = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.call(498, { args[0] }, { args[1], args[2] }) == 20 && f.stopped());
        assert(f.memory == before && Kaffeine_HeapAvail(&f.ctx, 0) == left);
    }
    /* still allocated while asynchronous I/O keeps it */
    f.ctx.io_release = io_keeps;
    assert(f.call(498, { p }, { 320, 200 }) == 20 && f.stopped() && io_keeps_calls == 1);
    f.ctx.io_release = nullptr;
    /* 310 rounds to the same 40 bytes; D0 and D1 are UWORDs, as in AROS */
    assert(f.call(498, { p }, { 0x7FFF0136, 0x000100C8 }) == 0 && !f.stopped());
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail);
    /* twice: refused */
    assert(f.call(498, { p }, { 320, 200 }) == 20 && f.stopped());
    /* NULL does nothing */
    assert(f.call(498, { 0 }, { 320, 200 }) == 0 && !f.stopped());

    /* the guest's FreeMem() of a raster, FreeRaster() of an AllocMem() */
    const ULONG q = f.call(492, {}, { 17, 3 });
    assert(q && !f.stopped());
    Kaffeine_HeapFree(&f.ctx, q, 12);
    const ULONG r = Kaffeine_HeapAlloc(&f.ctx, 12, 0);
    assert(f.call(498, { r }, { 32, 3 }) == 0 && !f.stopped());
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail);

    /* a mirror, a pooled block, an AllocAbs() block of the same size */
    const ULONG mirror = Kaffeine_MirrorAlloc(&f.ctx, 64);
    const ULONG pool = Kaffeine_PoolCreate(&f.ctx, 0, 1024, 512);
    const ULONG pooled = Kaffeine_PoolAlloc(&f.ctx, pool, 64);
    /* the lowest free heap address, from a probe allocation */
    const ULONG probe = Kaffeine_HeapAlloc(&f.ctx, 64, 0);
    Kaffeine_HeapFree(&f.ctx, probe, 64);
    const ULONG absolute = Kaffeine_HeapAllocAbs(&f.ctx, 64, (probe - 8) & ~7U);
    assert(mirror && pool && pooled && absolute);
    for (ULONG bad : { mirror, pooled, absolute, pool })
    {
        const Memory before = f.memory;
        const ULONG left = Kaffeine_HeapAvail(&f.ctx, 0);
        assert(f.call(498, { bad }, { 16, bad == pool ? 8U : 32U }) == 20 && f.stopped());
        assert(f.memory == before && Kaffeine_HeapAvail(&f.ctx, 0) == left);
    }
    Kaffeine_MirrorRelease(&f.ctx, mirror);
    Kaffeine_PoolDelete(&f.ctx, pool);
    Kaffeine_HeapFree(&f.ctx, absolute, 64);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail);

    /* empty, overflowing, beyond the 16-bit row of RASSIZE(), beyond the
       arena or the heap: 0 and nothing allocated */
    for (auto wh : { std::array<ULONG, 2>{ 0, 200 }, { 320, 0 }, { 0xFFFFFFFF, 0xFFFFFFFF },
                     { 0xFFFFFFF0, 1 }, { 0x80000, 1 }, { 0x7FFF0, 0xFFFF }, { 64000, 200 },
                     { 16, 0x7000 } })
    {
        assert(f.call(492, {}, { wh[0], wh[1] }) == 0 && !f.stopped());
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail);
    }
    assert(!gfx.any);
}

static void classic_intuition(Fixture &f)
{
    std::strcpy(f.ctx.libs[0].name,"intuition.library");
    f.ctx.libs[0].thunks=intuition_manual;f.ctx.libs[0].num_thunks=std::size(intuition_manual);
    f.ctx.libs[0].gen_thunks=intuition_generated;f.ctx.libs[0].num_gen_thunks=std::size(intuition_generated);
    drawing_context=&f.ctx;classic_draw={};
}
static void test_classic_drawimage()
{
    Fixture f;const ULONG rp=f.rastport(),im=0x2100,next=0x2140,data=0x2180;
    classic_intuition(f);std::memset(f.memory.data()+im,0,20);std::memset(f.memory.data()+next,0,20);
    m68k_write16(&f.ctx,im,-7);m68k_write16(&f.ctx,im+2,9);
    m68k_write16(&f.ctx,im+4,8);m68k_write16(&f.ctx,im+6,2);m68k_write16(&f.ctx,im+8,1);
    m68k_write32(&f.ctx,im+10,data);f.memory[im+14]=1;f.memory[im+15]=2;
    m68k_write32(&f.ctx,im+16,next);
    m68k_write16(&f.ctx,next+4,3);m68k_write16(&f.ctx,next+6,4);
    m68k_write32(&f.ctx,next+10,0xFFFFFFFF);f.memory[next+15]=5;
    const Memory original=f.memory;
    assert(f.call(114,{rp,im},{0x1234FFFD,0x56780005})==0&&!f.stopped());
    assert(classic_draw.images==1&&classic_draw.borders==0&&classic_draw.waits==1);
    assert(classic_draw.x==-3&&classic_draw.y==5&&classic_draw.image.size()==2);
    const auto &a=classic_draw.image[0];const auto &b=classic_draw.image[1];
    assert(a.LeftEdge==-7&&a.TopEdge==9&&a.Width==8&&a.Height==2&&a.Depth==1);
    assert(a.ImageData==reinterpret_cast<UWORD*>(f.memory.data()+data)&&a.PlanePick==1&&a.PlaneOnOff==2);
    assert(b.Width==3&&b.Height==4&&b.Depth==0&&!b.ImageData&&b.PlaneOnOff==5&&!b.NextImage);
    assert(f.memory==original&&!f.ctx.bitmap_blits);
    classic_draw={};assert(f.call(114,{rp,0})==0&&!classic_draw.images&&!classic_draw.waits);
    for(int bad=0;bad<4;bad++)
    {
        f.memory=original;classic_draw={};
        if(bad==0)m68k_write16(&f.ctx,im+8,0xFFFF); // BOOPSI class, no raw native object
        if(bad==1)m68k_write32(&f.ctx,im+10,0xFFFF); // incomplete/unaligned plane
        if(bad==2)m68k_write32(&f.ctx,im+16,im); // bounded cycle
        if(bad==3)m68k_write16(&f.ctx,im+4,0xFFFF); // negative width
        const Memory before=f.memory;
        assert(f.call(114,{rp,im})==20&&f.stopped()&&!classic_draw.images&&!classic_draw.waits);
        assert(f.memory==before&&!f.ctx.bitmap_blits);
    }
    f.memory=original;classic_draw={};
    assert(f.call(114,{Fixture::raw,im})==20&&f.stopped()&&!classic_draw.images);
    drawing_context=nullptr;
}
static void test_classic_drawborder()
{
    Fixture f;const ULONG rp=f.rastport(),bd=0x2100,next=0x2140,xy=0x2180;
    classic_intuition(f);std::memset(f.memory.data()+bd,0,16);std::memset(f.memory.data()+next,0,16);
    m68k_write16(&f.ctx,bd,-2);m68k_write16(&f.ctx,bd+2,11);
    f.memory[bd+4]=0xA1;f.memory[bd+5]=0xA2;f.memory[bd+6]=JAM1;f.memory[bd+7]=2;
    m68k_write32(&f.ctx,bd+8,xy);m68k_write32(&f.ctx,bd+12,next);
    m68k_write16(&f.ctx,xy,-7);m68k_write16(&f.ctx,xy+2,10);
    m68k_write16(&f.ctx,xy+4,30);m68k_write16(&f.ctx,xy+6,-40);
    f.memory[next+7]=0xFF;m68k_write32(&f.ctx,next+8,0xFFFFFFFF); // unused when Count < 1
    const Memory original=f.memory;
    assert(f.call(108,{rp,bd},{0xFFFF0003,0x1234FFFC})==0&&!f.stopped());
    assert(classic_draw.borders==1&&classic_draw.images==0&&classic_draw.waits==1);
    assert(classic_draw.x==3&&classic_draw.y==-4&&classic_draw.border.size()==2);
    const auto &a=classic_draw.border[0];const auto &b=classic_draw.border[1];
    assert(a.LeftEdge==-2&&a.TopEdge==11&&a.FrontPen==0xA1&&a.BackPen==0xA2&&a.DrawMode==JAM1&&a.Count==2);
    assert(classic_draw.pairs[0]==std::vector<WORD>({-7,10,30,-40}));
    assert(b.Count==-1&&!b.XY&&!b.NextBorder&&classic_draw.pairs[1].empty());
    assert(f.memory==original&&!f.ctx.bitmap_blits);
    classic_draw={};assert(f.call(108,{rp,0})==0&&!classic_draw.borders&&!classic_draw.waits);
    for(int bad=0;bad<3;bad++)
    {
        f.memory=original;classic_draw={};
        if(bad==0)m68k_write32(&f.ctx,bd+8,0xFFFC);
        if(bad==1)m68k_write32(&f.ctx,bd+12,bd);
        if(bad==2)m68k_write32(&f.ctx,bd+8,0);
        const Memory before=f.memory;
        assert(f.call(108,{rp,bd})==20&&f.stopped()&&!classic_draw.borders&&!classic_draw.waits);
        assert(f.memory==before&&!f.ctx.bitmap_blits);
    }
    drawing_context=nullptr;
}

/* ── Drawing through caller-owned RastPorts ──────────────────────── */

/* The layer bridge, as kaffeine_layers.c answers (test_layers and
   test_layers_private check its own side): without a Layer, the classic
   BitMap converted into the adapter over the guest planes, unchecked, so
   that the checks of the graphics thunks alone stand between it and
   native graphics here; with one, only the window layer that the test
   names, with the BitMap that it publishes. */
static int window_layer_storage;
static Layer *const window_layer = reinterpret_cast<Layer *>(&window_layer_storage);
static BitMap window_bm;
static struct { ULONG layer, bitmap; unsigned calls; } bridge;
static BOOL fake_draw_target(KaffeineContext *ctx, ULONG layer, ULONG bitmap, RastPort *rp, BitMap *adapter)
{
    ++bridge.calls;
    assert(!rp->Layer && !rp->BitMap);
    if (!layer && bitmap)
    {
        adapter->BytesPerRow = m68k_read16(ctx, bitmap);
        adapter->Rows = m68k_read16(ctx, bitmap + 2);
        adapter->Flags = BMF_STANDARD;
        adapter->Depth = ctx->mem[bitmap + 5];
        for (int p = 0; p < 8; ++p)
            adapter->Planes[p] = p < adapter->Depth ? ctx->mem + m68k_read32(ctx, bitmap + 8 + 4 * p) : nullptr;
        rp->BitMap = adapter;
        return TRUE;
    }
    if (layer && layer == bridge.layer && bitmap == bridge.bitmap)
    {
        rp->Layer = window_layer;
        rp->BitMap = &window_bm;
        return TRUE;
    }
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return FALSE;
}
/* every other operation NULL, as an older table has them */
static KaffeineLayerOps bridge_ops;

/* Caller 1: InitRastPort() into an AllocMem() block, a 32 x 16 BitMap of
   depth 2 whose planes come from AllocRaster(), its RastPort.BitMap set */
struct Planar { ULONG rp, bm, planes[2]; };
static Planar planar(Fixture &f)
{
    Planar p;
    p.rp = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT, 0);
    p.bm = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_BITMAP, 0);
    assert(p.rp && p.bm);
    assert(f.call(198, { 0, p.rp }) == 0 && !f.stopped());
    std::memset(f.memory.data() + p.bm, 0, M68K_SIZEOF_BITMAP);
    m68k_write16(&f.ctx, p.bm, 4);
    m68k_write16(&f.ctx, p.bm + 2, 16);
    f.memory[p.bm + 4] = BMF_STANDARD;
    f.memory[p.bm + 5] = 2;
    for (int i = 0; i < 2; ++i)
    {
        p.planes[i] = f.call(492, {}, { 32, 16 });
        assert(p.planes[i] && !f.stopped());
        std::memset(f.memory.data() + p.planes[i], 0, 64);
        m68k_write32(&f.ctx, p.bm + 8 + 4 * i, p.planes[i]);
    }
    m68k_write32(&f.ctx, p.rp + RP_BITMAP, p.bm);
    bridge_ops.draw_target = fake_draw_target;
    f.ctx.layer_ops = &bridge_ops;
    bridge = {};
    gfx.draws = gfx.waits = 0;
    return p;
}

static bool outside(const Fixture &f, const void *p)
{
    const auto at = reinterpret_cast<uintptr_t>(p), mem = reinterpret_cast<uintptr_t>(f.memory.data());
    return at < mem || at >= mem + f.memory.size();
}

/* Every pixel of the planes, a rectangle, a template, a line and text,
   through a temporary over the very guest planes: native graphics sees
   no Layer, the converted BitMap, the native font and no other pointer;
   pre-clipped arguments, -1 without a native call outside the BitMap, and
   exactly the changed scalars back, big-endian. */
static void test_draw_planar()
{
    Fixture f;
    const Planar p = planar(f);
    const ULONG rp = p.rp, font = guest_font(f, &topaz);
    assert(font && f.lng(rp + RP_FONT) == font && !f.lng(rp + RP_LAYER));
    m68k_write32(&f.ctx, rp + 20, 0xFFFFFFF1U); /* unused GelsInfo */
    /* RP_User, the reserved bytes and the pad byte are the program's, so
       are its private Flags bits */
    poison_bytes(f, rp + 66, 34);
    f.memory[rp + 31] = 0x77;
    m68k_write16(&f.ctx, rp + RP_FLAGS, 0x8100);
    const std::vector<UBYTE> kept(f.memory.begin() + rp + 66, f.memory.begin() + rp + 100);
    const ULONG avail = Kaffeine_HeapAvail(&f.ctx, 0);

    /* each of the 512 pixels in a pen of its own */
    auto pen = [](ULONG x, ULONG y) -> ULONG { return (x * 3 + y * 5 + (x >> 3)) & 3; };
    Memory want = f.memory;
    gfx.paint = true;
    for (ULONG y = 0; y < 16; ++y)
        for (ULONG x = 0; x < 32; ++x)
        {
            assert(f.call(342, { 0, rp }, { pen(x, y) }) == 0);
            assert(f.call(324, { 0, rp }, { x, y }) == 0 && !f.stopped());
            for (int i = 0; i < 2; ++i)
                if ((pen(x, y) >> i) & 1)
                    want[p.planes[i] + y * 4 + (x >> 3)] |= UBYTE(0x80 >> (x & 7));
        }
    gfx.paint = false;
    want[rp + RP_FGPEN] = UBYTE(pen(31, 15));
    want[rp + RP_LINPATCNT] = 15;
    assert(f.memory == want);
    assert(gfx.draws == 512 && gfx.waits == 512 && !f.ctx.bitmap_blits && bridge.calls == 512);
    const RastPort &s = gfx.drawn;
    assert(outside(f, gfx.draw_rp) && !s.Layer && s.BitMap && outside(f, s.BitMap));
    assert(s.Font == &topaz && !s.AreaPtrn && !s.TmpRas && !s.AreaInfo && !s.GelsInfo && !s.RP_User);
    assert(s.Flags == 0 && s.FgPen == BYTE(pen(31, 15)) && s.Mask == 0xFF && gfx.drawn_blits == 1);
    assert(gfx.drawn_bm.BytesPerRow == 4 && gfx.drawn_bm.Rows == 16 && gfx.drawn_bm.Depth == 2 &&
           gfx.drawn_bm.Planes[0] == f.ctx.mem + p.planes[0] &&
           gfx.drawn_bm.Planes[1] == f.ctx.mem + p.planes[1] && !gfx.drawn_bm.Planes[2]);
    /* the read-only guest ReadPixel() sees the same planes */
    for (ULONG y = 0; y < 16; ++y)
        for (ULONG x = 0; x < 32; ++x)
            assert(f.call(318, { 0, rp }, { x, y }) == pen(x, y));
    assert(gfx.draws == 512 && f.memory == want);

    /* outside the BitMap: -1, nothing native */
    for (const auto &xy : { std::array<ULONG, 2>{ 32, 0 }, { 0, 16 }, { 0xFFFF, 0 }, { 0, 0xFFFFFFFF },
                            { 0x7FFF, 0 }, { 0x10000, 16 } })
        assert(f.call(324, { 0, rp }, { xy[0], xy[1] }) == 0xFFFFFFFFU && !f.stopped());
    assert(gfx.draws == 512 && f.memory == want);

    /* RectFill(): clipped to the BitMap; nothing for an empty or outside one */
    assert(f.call(306, { 0, rp }, { 0xFFFB, 0xFFFD, 40, 20 }) == 0 && gfx.draws == 513);
    assert(gfx.fill[0] == 0 && gfx.fill[1] == 0 && gfx.fill[2] == 31 && gfx.fill[3] == 15);
    assert(f.call(306, { 0, rp }, { 10, 2, 12, 3 }) == 0 && gfx.draws == 514);
    assert(gfx.fill[0] == 10 && gfx.fill[1] == 2 && gfx.fill[2] == 12 && gfx.fill[3] == 3);
    for (const auto &r : { std::array<ULONG, 4>{ 32, 0, 40, 5 }, { 0, 16, 5, 20 },
                           { 0xFFF6, 0xFFF6, 0xFFFF, 0xFFFF }, { 5, 5, 4, 5 }, { 0, 0, 0x8000, 3 } })
        assert(f.call(306, { 0, rp }, { r[0], r[1], r[2], r[3] }) == 0 && !f.stopped());
    assert(gfx.draws == 514 && gfx.waits == 514);

    /* BltTemplate(): clipped, the source moved along, whole words of xSrc
       into the address */
    const ULONG tmpl = Fixture::text;
    for (ULONG i = 0; i < 64; ++i)
        f.memory[tmpl + i] = want[tmpl + i] = UBYTE(i * 37 + 1);
    assert(f.call(36, { tmpl, rp }, { 3, 2, 0xFFFE, 0xFFFF, 10, 4 }) == 0 && gfx.draws == 515);
    assert(gfx.tmpl == f.ctx.mem + tmpl + 2 && gfx.tmpl_args[0] == 5 && gfx.tmpl_args[1] == 2 &&
           gfx.tmpl_args[2] == 0 && gfx.tmpl_args[3] == 0 && gfx.tmpl_args[4] == 8 && gfx.tmpl_args[5] == 3);
    assert(f.call(36, { tmpl, rp }, { 20, 4, 30, 14, 10, 5 }) == 0 && gfx.draws == 516);
    assert(gfx.tmpl == f.ctx.mem + tmpl + 2 && gfx.tmpl_args[0] == 4 && gfx.tmpl_args[1] == 4 &&
           gfx.tmpl_args[2] == 30 && gfx.tmpl_args[3] == 14 && gfx.tmpl_args[4] == 2 && gfx.tmpl_args[5] == 2);
    assert(gfx.drawn.DrawMode == JAM2 && !gfx.drawn.Layer);
    /* wholly outside, or empty: nothing, also without a template */
    for (const auto &a : { std::array<ULONG, 7>{ tmpl, 0, 2, 32, 0, 8, 1 }, { tmpl, 0, 2, 0xFFF8, 0, 8, 1 },
                           { tmpl, 0, 2, 0, 16, 8, 1 }, { 0, 0, 2, 0, 0, 0, 5 }, { 0, 0, 2, 0, 0, 5, 0xFFFF } })
        assert(f.call(36, { a[0], rp }, { a[1], a[2], a[3], a[4], a[5], a[6] }) == 0 && !f.stopped());
    assert(gfx.draws == 516 && gfx.waits == 516 && f.memory == want);

    /* Move() and Draw(): BE pen position in, the changed one and only the
       public Flags back */
    assert(f.call(240, { 0, rp }, { 0xFFFFFFFB, 2 }) == 0);
    gfx.draw_flags = ONE_DOT | RPF_NO_PENS;
    assert(f.call(246, { 0, rp }, { 0x1000A, 12 }) == 0 && !f.stopped());
    gfx.draw_flags = 0;
    assert(gfx.draw_from_x == -5 && gfx.draw_from_y == 2 && gfx.drawn.cp_x == -5 && !gfx.drawn.Layer);
    assert(f.word(rp + RP_CP_X) == 10 && f.word(rp + RP_CP_Y) == 12);
    assert(f.word(rp + RP_FLAGS) == (0x8100 | FRST_DOT | ONE_DOT));

    /* Text() with the native font of its 68k TextFont */
    f.put(Fixture::text + 0x80, "Hi");
    gfx.text.clear();
    assert(f.call(60, { Fixture::text + 0x80, rp }, { 2 }) == 0 && !f.stopped());
    assert(gfx.text == "Hi" && gfx.drawn.Font == &topaz && gfx.drawn.TxWidth == 8);
    assert(f.word(rp + RP_CP_X) == 26 && f.lng(rp + RP_FONT) == font);

    /* the program's bytes stay; nothing registered, settled or allocated */
    assert(std::equal(kept.begin(), kept.end(), f.memory.begin() + rp + 66) && f.memory[rp + 31] == 0x77);
    assert(f.lng(rp + RP_LAYER) == 0 && f.lng(rp + RP_BITMAP) == p.bm);
    for (ULONG off = 8; off < 20; off += 4)
        assert(f.lng(rp + off) == 0);
    assert(f.lng(rp + 20) == 0xFFFFFFF1U);
    assert(!f.ctx.num_shadows && !f.ctx.settle_rastports[0] && !f.ctx.settle_rastports[1]);
    assert(Kaffeine_HeapAvail(&f.ctx, 0) == avail && !f.ctx.bitmap_blits && gfx.waits == gfx.draws);
}

/* Anything that the bridge cannot draw through stops the program before
   native graphics, with no byte changed: pointer fields that graphics
   would follow, planes that alias each other, the RastPort, the BitMap or
   bridge memory, freed planes, no layer bridge or target, text without a
   68k TextFont, and templates that are negative, missing, too long or on
   the target. */
static void test_draw_refusals()
{
    enum { CASES = 14 };
    for (int c = 0; c < CASES; ++c)
    {
        Fixture f;
        const Planar p = planar(f);
        const ULONG rp = p.rp;
        switch (c)
        {
            case 0: m68k_write32(&f.ctx, rp + 8, Fixture::text); break;      /* AreaPtrn */
            case 1: m68k_write32(&f.ctx, rp + 12, Fixture::text); break;     /* TmpRas */
            case 2: m68k_write32(&f.ctx, rp + 16, Fixture::text); break;     /* AreaInfo */
            case 3: m68k_write32(&f.ctx, p.bm + 12, f.ctx.heap_start); break; /* heap metadata */
            case 4: m68k_write32(&f.ctx, p.bm + 12, p.planes[0]); break;     /* the same plane twice */
            case 5:
            {
                const ULONG block = Kaffeine_HeapAlloc(&f.ctx, 128, 0);
                m68k_write32(&f.ctx, p.bm + 8, block);
                m68k_write32(&f.ctx, p.bm + 12, block + 32);
                break;
            }
            case 6: m68k_write32(&f.ctx, p.bm + 12, rp); break;              /* a plane over the RastPort */
            case 7:
            {
                /* a plane over the BitMap, both in one block */
                const ULONG block = Kaffeine_HeapAlloc(&f.ctx, 200, 0);
                std::memcpy(f.memory.data() + block, f.memory.data() + p.bm, M68K_SIZEOF_BITMAP);
                m68k_write32(&f.ctx, block + 12, block + 32);
                m68k_write32(&f.ctx, rp + RP_BITMAP, block);
                break;
            }
            case 8: m68k_write32(&f.ctx, p.bm + 12, Kaffeine_MirrorAlloc(&f.ctx, 64)); break;
            case 9: m68k_write32(&f.ctx, p.bm + 12, guest_font(f, &topaz)); break;
            case 10:
            {
                const ULONG mirror = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_BITMAP);
                std::memcpy(f.memory.data() + mirror, f.memory.data() + p.bm, M68K_SIZEOF_BITMAP);
                m68k_write32(&f.ctx, rp + RP_BITMAP, mirror);
                break;
            }
            case 11: assert(f.call(498, { p.planes[1] }, { 32, 16 }) == 0); break;   /* freed */
            case 12: f.ctx.layer_ops = nullptr; break;
            case 13: m68k_write32(&f.ctx, rp + RP_LAYER, Fixture::raw); break;   /* no target */
        }
        f.put(Fixture::text + 0x80, "Hi");
        const Memory with_text = f.memory;
        assert(f.call(246, { 0, rp }, { 1, 1 }) == 20 && f.stopped());
        assert(f.call(306, { 0, rp }, { 0, 0, 5, 5 }) == 20 && f.stopped());
        assert(f.call(324, { 0, rp }, { 1, 1 }) == 20 && f.stopped());
        assert(f.call(60, { Fixture::text + 0x80, rp }, { 2 }) == 20 && f.stopped());
        assert(f.call(36, { Fixture::text, rp }, { 0, 2, 0, 0, 8, 2 }) == 20 && f.stopped());
        assert(f.memory == with_text && gfx.draws == 0 && gfx.waits == 0 && !f.ctx.bitmap_blits);
    }

    /* Text() needs a 68k TextFont of graphics; the other calls never read it */
    {
        Fixture f;
        const Planar p = planar(f);
        f.put(Fixture::text + 0x80, "Hi");
        for (ULONG font : { 0U, Fixture::raw })
        {
            m68k_write32(&f.ctx, p.rp + RP_FONT, font);
            assert(f.call(60, { Fixture::text + 0x80, p.rp }, { 2 }) == 20 && f.stopped() && !gfx.draws);
        }
        assert(f.call(306, { 0, p.rp }, { 0, 0, 3, 3 }) == 0 && gfx.draws == 1 && !gfx.drawn.Font);
        m68k_write32(&f.ctx, p.rp + RP_FONT, guest_font(f, &topaz));
        /* text on the target or in the RastPort */
        const Memory before = f.memory;
        assert(f.call(60, { p.planes[1] + 8, p.rp }, { 2 }) == 20 && f.stopped());
        assert(f.call(60, { p.rp + 70, p.rp }, { 2 }) == 20 && f.stopped());
        assert(f.call(60, { p.bm + 38, p.rp }, { 4 }) == 20 && f.stopped());
        assert(f.memory == before && gfx.draws == 1);
    }

    /* templates */
    {
        Fixture f;
        const Planar p = planar(f);
        const ULONG end = f.ctx.mem_size;
        const Memory before = f.memory;
        for (const auto &a : { std::array<ULONG, 7>{ Fixture::text, 0, 0xFFFE, 0, 0, 8, 2 },   /* srcMod < 0 */
                               { Fixture::text, 0xFFFF, 2, 0, 0, 8, 2 },                     /* xSrc < 0 */
                               { 0, 0, 2, 0, 0, 8, 2 },                                      /* no template */
                               { end - 0x10, 0, 0x7FFF, 0, 0, 8, 0x7FFF },                   /* too long */
                               { end - 1, 0, 2, 0, 0, 16, 1 },                               /* the last word */
                               { 0xFFFFFFF0, 0, 2, 0, 0, 8, 1 },
                               { p.planes[0] + 4, 0, 2, 0, 0, 8, 2 },                        /* on a plane */
                               { p.rp + 90, 0, 2, 0, 0, 8, 2 },                              /* on the RastPort */
                               { p.bm + 36, 0, 2, 0, 0, 8, 4 } })                            /* on the BitMap */
            assert(f.call(36, { a[0], p.rp }, { a[1], a[2], a[3], a[4], a[5], a[6] }) == 20 && f.stopped());
        assert(f.memory == before && !gfx.draws);
        /* exactly the bytes graphics may read: one word for 16 pixels */
        assert(f.call(36, { end - 2, p.rp }, { 0, 2, 0, 0, 16, 1 }) == 0 && gfx.draws == 1 &&
               gfx.tmpl == f.ctx.mem + end - 2);
    }
}

/* Caller 2: a CopyMem() of the 68k RastPort of a window, whose Layer and
   BitMap handles stay as the bridge published them, with its own font,
   pens and pen position: native graphics draws through a temporary with
   the window's real native layer, which clips it, and that layer's
   BitMap; the window's native and 68k RastPorts stay as they were. A copy
   whose handles no longer match draws nothing. */
static void test_draw_copied_window()
{
    Fixture f;
    const ULONG rp = f.rastport(), layer = Fixture::raw + 0x200;
    bridge_ops.draw_target = fake_draw_target;
    f.ctx.layer_ops = &bridge_ops;
    m68k_write32(&f.ctx, rp + RP_LAYER, layer);   /* as the layer bridge publishes it */
    bridge = { layer, 0, 0 };
    const ULONG copy = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_RASTPORT, 0);
    assert(copy);
    std::memcpy(f.memory.data() + copy, f.memory.data() + rp, M68K_SIZEOF_RASTPORT);
    f.put(Fixture::name, "other.font");
    m68k_write32(&f.ctx, Fixture::attr + M68K_TA_NAME, Fixture::name);
    const ULONG font = f.call(72, { Fixture::attr });
    assert(font && guest_font(f, &other) == font);
    UBYTE native_before[sizeof(RastPort)];
    std::memcpy(native_before, &native_rp, sizeof(RastPort));
    const std::vector<UBYTE> window_before(f.memory.begin() + rp, f.memory.begin() + rp + 100);
    gfx.draws = gfx.waits = 0;

    assert(f.call(342, { 0, copy }, { 5 }) == 0 && f.call(66, { font, copy }) == 0);
    assert(f.call(240, { 0, copy }, { 3, 4 }) == 0 && !f.stopped());
    f.put(Fixture::text, "Hi");
    gfx.text.clear();
    assert(f.call(60, { Fixture::text, copy }, { 2 }) == 0 && !f.stopped());
    assert(gfx.draw_rp != &native_rp && outside(f, gfx.draw_rp) && gfx.text == "Hi");
    assert(gfx.drawn.Layer == window_layer && gfx.drawn.BitMap == &window_bm && gfx.drawn.Font == &other);
    assert(gfx.drawn.FgPen == 5 && gfx.drawn.TxWidth == 7 && gfx.drawn.cp_x == 3 && gfx.drawn_blits == 1);
    assert(!gfx.drawn.AreaPtrn && !gfx.drawn.TmpRas && !gfx.drawn.AreaInfo && !gfx.drawn.GelsInfo);
    assert(f.word(copy + RP_CP_X) == 17 && f.lng(copy + RP_FONT) == font);
    /* the layer clips: nothing clipped before */
    assert(f.call(306, { 0, copy }, { 0xFFFB, 0xFFFB, 500, 500 }) == 0);
    assert(gfx.fill[0] == -5 && gfx.fill[1] == -5 && gfx.fill[2] == 500 && gfx.fill[3] == 500);
    assert(f.call(246, { 0, copy }, { 9, 9 }) == 0 && f.word(copy + RP_CP_X) == 9);
    assert(f.call(318, { 0, copy }, { 2, 3 }) == 5 && gfx.drawn.Layer == window_layer);
    assert(f.call(324, { 0, copy }, { 3000, 1 }) == 0xFFFFFFFFU);
    assert(f.call(36, { Fixture::text, copy }, { 0, 2, 0xFFFC, 0xFFFC, 8, 2 }) == 0);
    assert(gfx.tmpl == f.ctx.mem + Fixture::text && gfx.tmpl_args[2] == -4 && gfx.tmpl_args[3] == -4 &&
           gfx.tmpl_args[4] == 8 && gfx.tmpl_args[5] == 2 && gfx.drawn.Layer == window_layer);
    assert(gfx.draws == 6 && gfx.waits == 6 && bridge.calls == 6 && !f.stopped());
    /* the whole template counts on a layer */
    assert(f.call(36, { f.ctx.mem_size - 0x10, copy }, { 0, 0x7FFF, 0, 0, 8, 0x7FFF }) == 20 && f.stopped());
    /* the window's RastPorts are untouched */
    assert(!std::memcmp(native_before, &native_rp, sizeof(RastPort)));
    assert(std::equal(window_before.begin(), window_before.end(), f.memory.begin() + rp));

    /* another BitMap handle, or the window has closed */
    const Memory before = f.memory;
    m68k_write32(&f.ctx, copy + RP_BITMAP, 0x1234);
    assert(f.call(306, { 0, copy }, { 0, 0, 1, 1 }) == 20 && f.stopped());
    m68k_write32(&f.ctx, copy + RP_BITMAP, 0);
    bridge.layer = 0;
    assert(f.call(246, { 0, copy }, { 1, 1 }) == 20 && f.stopped());
    assert(f.call(318, { 0, copy }, { 1, 1 }) == 20 && f.stopped());
    assert(gfx.draws == 6 && f.memory == before);
}

/* -36 is manual: every register, WORD halves, for a window's RastPort as
   before; the generated thunk is never reached. */
static void test_blttemplate_registers()
{
    Fixture f;
    const ULONG rp = f.rastport();
    generated_native = 0;
    assert(f.call(36, { Fixture::text, rp }, { 0xABCD0001, 0x12340004, 5, 0xFFFF0006, 7, 2 }) == 0);
    assert(gfx.draw_rp == &native_rp && gfx.tmpl == f.ctx.mem + Fixture::text);
    assert(gfx.tmpl_args[0] == 1 && gfx.tmpl_args[1] == 4 && gfx.tmpl_args[2] == 5 &&
           gfx.tmpl_args[3] == 6 && gfx.tmpl_args[4] == 7 && gfx.tmpl_args[5] == 2);
    assert(!generated_native && !gfx.waits && !f.stopped());
    /* a negative modulo or an unbounded template stops it there too */
    assert(f.call(36, { Fixture::text, rp }, { 0, 0xFFFE, 0, 0, 8, 2 }) == 20 && f.stopped());
    assert(f.call(36, { f.ctx.mem_size - 1, rp }, { 0, 2, 0, 0, 16, 1 }) == 20 && f.stopped());
    assert(gfx.draws == 1 && !generated_native);
}

/* Literal classic offsets from graphics/gels.h and rastport.h, dispatched
   through -120. Compare the whole arena: sentinel links/coordinates and
   collision entry zero are the only writes, no native pointer or call. */
static void test_initgels()
{
    for (bool table : {false, true})
    {
        Fixture f;
        const ULONG head=0x2800, tail=0x2900, info=0x2A00, coll=table?0x2B00:0;
        m68k_write32(&f.ctx, info+18, coll);
        Memory want=f.memory;
        auto put16=[&](ULONG p, UWORD v){want[p]=UBYTE(v>>8);want[p+1]=UBYTE(v);};
        auto put32=[&](ULONG p, ULONG v){put16(p,UWORD(v>>16));put16(p+2,UWORD(v));};
        put32(head,tail);put32(head+4,0);put32(head+8,0);put32(head+12,tail);
        put32(tail,0);put32(tail+4,head);put32(tail+8,0);
        for (ULONG off : {16U,18U,22U,24U}) {put16(head+off,0x8000);put16(tail+off,0x7FFF);}
        put32(info+2,head);put32(info+6,tail);
        if(table)put32(coll,0);
        const unsigned native=gfx.any, generated=generated_calls;
        assert(f.call(120,{head,tail,info})==0 && !f.stopped());
        assert(f.memory==want && gfx.any==native && generated_calls==generated);
        assert(f.call(120,{head,tail,info})==0 && f.memory==want);
    }
    for (unsigned which=0;which<16;which++)
    {
        Fixture f;
        ULONG head=0x2800,tail=0x2900,info=0x2A00,coll=0x2B00;
        switch(which)
        {
            case 0:head=0;break;
            case 1:tail++;break;
            case 2:info=f.ctx.mem_size-36;break;
            case 3:tail=head+58;break;
            case 4:info=head+2;break;
            case 5:coll=info+36;break;
            case 6:coll=tail+58;break;
            case 7:coll=0xFFFFFFFEU;break;
            case 8:coll++;break;
            case 9:head=Kaffeine_MirrorAlloc(&f.ctx,60);break;
            case 10:tail=guest_font(f,&topaz);break;
            case 11:info=f.rastport();break;
            case 12:coll=Kaffeine_HeapAlloc(&f.ctx,62,0);break;
            case 13:head=Kaffeine_HeapAlloc(&f.ctx,58,0);break;
            case 14:tail=Kaffeine_HeapAlloc(&f.ctx,60,0);assert(Kaffeine_HeapFreeExact(&f.ctx,tail,60));break;
            case 15:info=Kaffeine_HeapAlloc(&f.ctx,36,0);break;
        }
        if (info && info+22<=f.ctx.mem_size)m68k_write32(&f.ctx,info+18,coll);
        const Memory before=f.memory;
        assert(f.call(120,{head,tail,info})==20 && f.stopped() && f.memory==before);
        assert(!gfx.any);
    }
    for (ULONG vector : {96U,102U,108U,114U,126U,132U,138U,144U,150U,156U,162U,168U,174U,600U})
    {
        Fixture f;const Memory before=f.memory;const unsigned generated=generated_calls;
        assert(f.call(vector,{0xFFFFFFFFU,0xFFFFFFFFU,0xFFFFFFFFU})==20 && f.stopped());
        assert(f.memory==before && !gfx.any && generated_calls==generated);
    }
}

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "Intuition and graphics tables use their ABI slots", test_registration },
        { "SetWindowPointerA is at -816 and does nothing", test_intuition },
        { "DrawImage converts classic chains and preserves planar byte identity", test_classic_drawimage },
        { "DrawBorder converts chains and signed WORD coordinates before Intuition", test_classic_drawborder },
        { "OpenFont() and CloseFont() with 68k TextFonts", test_fonts },
        { "A 68k RastPort gets the native fields without native pointers", test_rastport_fields },
        { "Pens, modes and the pen position both ways", test_pens },
        { "A generated thunk is settled right after it", test_generated_settle },
        { "TextLength(), Text() and TextExtent() with 68k text and TextExtent", test_text },
        { "TextFit() both ways and with a 68k constraint", test_textfit },
        { "SetFont() takes a 68k TextFont", test_setfont },
        { "Unsupported RastPorts stop before native graphics.library", test_refusals },
        { "InitRastPort initializes exactly classic caller storage and bounds", test_classic_rastport_init },
        { "Caller-owned RastPorts support bounded font operations and copied queries", test_classic_rastport_fonts },
        { "AskFont() fills a classic TextAttr with the font's own attributes", test_askfont },
        { "AskFont() follows the font of the RastPort with stable 68k names", test_askfont_fonts },
        { "AskFont() stops on bad TextAttrs, RastPorts and fonts before writing", test_askfont_refusals },
        { "ReadPixel reads bounded guest planar bytes without native calls or mutation", test_guest_planar_read },
        { "Generated thunks reach graphics only with 68k RastPorts", test_generated_guard },
        { "State calls on caller-owned RastPorts write back exactly the changed scalars", test_classic_rastport_state },
        { "State calls on a copied window RastPort leave the window's alone", test_copied_rastport_state },
        { "Native private RastPort Flags are neither shown nor taken", test_registered_private_flags },
        { "State calls stop on storage that is not the program's own", test_state_refusals },
        { "AllocRaster() and FreeRaster() use exact guest heap blocks", test_rasters },
        { "Drawing on a program's planar BitMap goes through the very guest planes, pre-clipped", test_draw_planar },
        { "Drawing stops before native graphics on pointers, aliases, bridge memory and bad templates", test_draw_refusals },
        { "A copied window RastPort draws through the real layer and leaves the window's alone", test_draw_copied_window },
        { "Classic InitGels is bounded and other GELS calls stop before native graphics", test_initgels },
        { "BltTemplate() is manual with every register; window RastPorts as before", test_blttemplate_registers }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Intuition and graphics thunks: %zu groups passed\n", std::size(tests));
    return 0;
}

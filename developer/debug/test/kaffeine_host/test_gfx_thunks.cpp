/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Check the production Intuition and graphics thunks through literal LVOs,
    the real dispatcher and heap: the ABI slots of the tables, OpenFont() and
    CloseFont() with 68k TextFonts, the 68k RastPort of a window, which gets
    the native fields back after every call, also after a generated thunk,
    and gives the native one the fields that the program wrote, the text
    calls with their 68k TextExtents, bounded guest planar ReadPixel and
    the refusal of unsupported RastPorts. The native graphics.library acts as AROS does and
    records every call.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/graphics.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks_gfx.h"
#include "kaffeine_dispatch.h"

/* ── The native side ─────────────────────────────────────────────── */

static TextFont topaz, other;
static char topaz_name[] = "topaz.font", other_name[] = "other.font";
static RastPort native_rp;

struct GfxCalls
{
    unsigned any = 0;
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
};
static GfxCalls gfx;

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
    return (WORD)(read_text(string, count) * rp->TxWidth);
}

extern "C" void Text(struct RastPort *rp, CONST_STRPTR string, ULONG count)
{
    ++gfx.any;
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

extern "C" void Move(struct RastPort *rp, WORD x, WORD y)
{
    ++gfx.any;
    rp->cp_x = x;
    rp->cp_y = y;
    rp->linpatcnt = 15;
}

extern "C" void Draw(struct RastPort *rp, WORD x, WORD y)
{
    ++gfx.any;
    gfx.draw_from_x = rp->cp_x;
    gfx.draw_from_y = rp->cp_y;
    rp->cp_x = x;
    rp->cp_y = y;
}

extern "C" void RectFill(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1)
{
    ++gfx.any;
    gfx.fill_fg = rp->FgPen;
    gfx.fill_drmd = rp->DrawMode;
    gfx.fill[0] = x0; gfx.fill[1] = y0; gfx.fill[2] = x1; gfx.fill[3] = y1;
}

extern "C" LONG ReadPixel(struct RastPort *, WORD x, WORD y)
{
    ++gfx.any;
    return x == 3000 ? -1 : x + y;
}

extern "C" LONG WritePixel(struct RastPort *, WORD x, WORD)
{
    ++gfx.any;
    return x == 3000 ? -1 : 0;
}

extern "C" void SetAPen(struct RastPort *rp, ULONG pen) { ++gfx.any; rp->FgPen = (BYTE)pen; rp->linpatcnt = 15; }
extern "C" void SetBPen(struct RastPort *rp, ULONG pen) { ++gfx.any; rp->BgPen = (BYTE)pen; rp->linpatcnt = 15; }
extern "C" void SetDrMd(struct RastPort *rp, ULONG mode) { ++gfx.any; rp->DrawMode = (BYTE)mode; rp->linpatcnt = 15; }

extern "C" void SetABPenDrMd(struct RastPort *rp, ULONG apen, ULONG bpen, ULONG mode)
{
    ++gfx.any;
    gfx.apen = apen; gfx.bpen = bpen; gfx.drmd = mode;
    rp->FgPen = (BYTE)apen;
    rp->BgPen = (BYTE)bpen;
    rp->DrawMode = (BYTE)mode;
}

extern "C" ULONG GetAPen(struct RastPort *rp) { ++gfx.any; return (UBYTE)rp->FgPen; }
extern "C" ULONG GetBPen(struct RastPort *rp) { ++gfx.any; return (UBYTE)rp->BgPen; }
extern "C" ULONG GetDrMd(struct RastPort *rp) { ++gfx.any; return (UBYTE)rp->DrawMode; }
extern "C" ULONG GetOutlinePen(struct RastPort *rp) { ++gfx.any; return (UBYTE)rp->AOlPen; }

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

/* A generated drawing thunk, as PolyDraw() through resolve_rp(): it moves
   the native pen. */
static IPTR generated_draw(KaffeineContext *ctx, void *cpu)
{
    struct RastPort *rp = resolve_rp(ctx, THUNK_A(1));

    rp->cp_x = 77;
    rp->cp_y = 88;
    return 0;
}

static const M68KThunkEntry intuition_manual[] = { KAFFEINE_INTUITION_COMPAT_THUNKS };
static const M68KThunkEntry intuition_generated[] = { { 816, poison } };
static const M68KThunkEntry graphics_compat[] = { KAFFEINE_GRAPHICS_COMPAT_THUNKS };
static const M68KThunkEntry graphics_rastport[] = { KAFFEINE_GRAPHICS_RASTPORT_THUNKS };
static const M68KThunkEntry graphics_manual[] =
    { KAFFEINE_GRAPHICS_COMPAT_THUNKS, KAFFEINE_GRAPHICS_RASTPORT_THUNKS };
static const M68KThunkEntry graphics_generated[] =
{
    { 54, poison }, { 60, poison }, { 66, poison }, { 72, poison }, { 78, poison },
    { 240, poison }, { 246, poison }, { 306, poison }, { 318, poison }, { 324, poison },
    { 336, generated_draw }, { 342, poison }, { 348, poison }, { 354, poison },
    { 396, poison }, { 690, poison }, { 696, poison }, { 858, poison }, { 864, poison },
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
    RP_CP_X = 36, RP_CP_Y = 38, RP_MINTERMS = 40, RP_FONT = 52, RP_TXHEIGHT = 58,
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
    }

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
    assert(std::size(intuition_manual) == 1);
    assert(intuition_manual[0].lvo == 816 && intuition_manual[0].thunk == thunk_intuition_SetWindowPointerA);

    assert(std::size(graphics_compat) == 3);
    for (const auto &entry : graphics_compat)
        assert((entry.lvo == 72 && entry.thunk == thunk_gfx_OpenFont) ||
               (entry.lvo == 396 && entry.thunk == thunk_gfx_ScrollRaster) ||
               (entry.lvo == 894 && entry.thunk == thunk_gfx_SetABPenDrMd));
    const struct { ULONG lvo; IPTR (*thunk)(KaffeineContext *, void *); } rastport[] =
    {
        { 54, thunk_gfx_TextLength }, { 60, thunk_gfx_Text }, { 66, thunk_gfx_SetFont },
        { 78, thunk_gfx_CloseFont }, { 240, thunk_gfx_Move }, { 246, thunk_gfx_Draw },
        { 306, thunk_gfx_RectFill }, { 318, thunk_gfx_ReadPixel }, { 324, thunk_gfx_WritePixel },
        { 342, thunk_gfx_SetAPen }, { 348, thunk_gfx_SetBPen }, { 354, thunk_gfx_SetDrMd },
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

    /* resolve_rp() as before for anything but a 68k RastPort */
    int native_window;
    f.ctx.shadow_map[f.ctx.num_shadows].m68k_addr = Fixture::raw + 0x40;
    f.ctx.shadow_map[f.ctx.num_shadows].native_ptr = &native_window;
    f.ctx.shadow_map[f.ctx.num_shadows++].type = KAFFEINE_SHADOW_WINDOW;
    assert(resolve_rp(&f.ctx, Fixture::raw + 0x40) == reinterpret_cast<RastPort *>(&native_window));
    assert(resolve_rp(&f.ctx, Fixture::raw) ==
           reinterpret_cast<RastPort *>(f.memory.data() + Fixture::raw));
    assert(resolve_rp(&f.ctx, 0) == nullptr);
    assert(resolve_rp(&f.ctx, f.memory.size()) == nullptr);
    assert(!f.ctx.after_thunk);

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
                           396U, 690U, 696U, 894U })
        {
            assert(f.call(lvo, { Fixture::text, bad, Fixture::extent, 0 }, { 1, 1, 400, 8 }) == 20);
            assert(f.stopped());
        }
        assert(f.call(66, { 0, bad }) == 20 && f.stopped());
        for (ULONG lvo : { 858U, 864U, 870U, 876U })
        {
            assert(f.call(lvo, { bad }) == 20);
            assert(f.stopped());
        }
    }
    assert(!gfx.any);
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

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "Intuition and graphics tables use their ABI slots", test_registration },
        { "SetWindowPointerA is at -816 and does nothing", test_intuition },
        { "OpenFont() and CloseFont() with 68k TextFonts", test_fonts },
        { "A 68k RastPort gets the native fields without native pointers", test_rastport_fields },
        { "Pens, modes and the pen position both ways", test_pens },
        { "A generated thunk is settled right after it", test_generated_settle },
        { "TextLength(), Text() and TextExtent() with 68k text and TextExtent", test_text },
        { "TextFit() both ways and with a 68k constraint", test_textfit },
        { "SetFont() takes a 68k TextFont", test_setfont },
        { "Unsupported RastPorts stop before native graphics.library", test_refusals },
        { "ReadPixel reads bounded guest planar bytes without native calls or mutation", test_guest_planar_read }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Intuition and graphics thunks: %zu groups passed\n", std::size(tests));
    return 0;
}

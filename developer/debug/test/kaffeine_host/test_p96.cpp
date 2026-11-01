/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Picasso96API.library of kaffeine_p96.c through the real dispatcher with
    literal LVOs: p96BestModeIDTagList() and p96WritePixelArray() against
    recorded cybergraphics.library calls, the halt of the 26 other LVOs, the
    write as the one manual vector that passes a held LayerInfo or refresh
    scope (ctx->layer_ops->info_locked), and Kaffeine_P96Open() with its
    version, reopen and capacity rules. The program memory check, the
    RastPort lookup and the base setup are collaborators that reproduce the
    production rules they stand for.
*/
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_p96.h"

#include <cybergraphx/cybergraphics.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>

static unsigned checks, failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    if (failures < 40) std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

/* P96 numbers, from workbench/devs/diskimage/include/amigaos/libraries/Picasso96.h */
enum
{
    RGBFB_NONE, RGBFB_CLUT, RGBFB_R8G8B8, RGBFB_B8G8R8, RGBFB_R5G6B5PC, RGBFB_R5G5B5PC,
    RGBFB_A8R8G8B8, RGBFB_A8B8G8R8, RGBFB_R8G8B8A8, RGBFB_B8G8R8A8, RGBFB_R5G6B5,
    RGBFB_R5G5B5, RGBFB_B5G6R5PC, RGBFB_B5G5R5PC, RGBFB_Y4U2V2, RGBFB_Y4U1V1
};
static const ULONG P96BIDTAG_Dummy = TAG_USER + 96;
static const ULONG P96BIDTAG_FormatsAllowed = P96BIDTAG_Dummy + 1;
static const ULONG P96BIDTAG_FormatsForbidden = P96BIDTAG_Dummy + 2;
static const ULONG P96BIDTAG_NominalWidth = P96BIDTAG_Dummy + 3;
static const ULONG P96BIDTAG_NominalHeight = P96BIDTAG_Dummy + 4;
static const ULONG P96BIDTAG_Depth = P96BIDTAG_Dummy + 5;
static const ULONG P96BIDTAG_VideoCompatible = P96BIDTAG_Dummy + 6;
static const ULONG INVALID = 0xffffffffUL;
static const ULONG ALL = 0xffffffffUL;    /* every format, or every bit of an ID */

enum : ULONG
{
    MEM_SIZE = 0x10000,
    PROG_LO = 0x1000, PROG_HI = 0xf000,     /* program memory for the collaborator */
    P96_BASE = 0xf800,
    RP_GUEST = 0x1200, RP_NOBITMAP = 0x1300,
    RI = 0x1400, TAGS = 0x1500, TAGS2 = 0x1600, BUF = 0x2000,
    RETURN_PC = 0x76543210
};

/* ── collaborators ── */

static ULONG forbid_lo, forbid_hi;          /* a range the program does not own */
static unsigned program_calls;

extern "C" BOOL Kaffeine_ProgramMemory(KaffeineContext *, ULONG addr, ULONG size)
{
    ++program_calls;
    if (!size || addr < PROG_LO || addr > PROG_HI || size > PROG_HI - addr)
        return FALSE;
    return !(forbid_hi > forbid_lo && addr < forbid_hi && forbid_lo < addr + size);
}

static struct RastPort native_rp, nobitmap_rp;
static struct BitMap *const SOME_BITMAP = reinterpret_cast<struct BitMap *>(0x1234);

extern "C" struct RastPort *Kaffeine_RastPortIn(KaffeineContext *, ULONG guest)
{
    if (guest == RP_GUEST) return &native_rp;
    if (guest == RP_NOBITMAP) return &nobitmap_rp;
    return nullptr;
}

/* A display database for the mode calls, with the fields that BestModeIDA()
   and the bridge read. The monitor of a mode is the upper word of its ID,
   as AROS gives each display driver an ID base of its own. */
struct DbMode
{
    ULONG id;
    UWORD width, height, maxdepth;
    ULONG pixfmt, bppix;
    bool cyber = true;
    ULONG flags = 0;
    UBYTE gun = 8;                      /* RedBits, GreenBits and BlueBits */
    bool not_available = false;
    ULONG dims_bytes = sizeof(struct DimensionInfo);
};
static std::vector<DbMode> db;
static ULONG default_monitor;           /* upper word of its IDs */
static std::vector<ULONG> disabled_monitors;
static ULONG extra_ids;                 /* IDs, not of cybergraphics, before db */
static bool ids_cycle;                  /* NextDisplayInfo() starts again after db */
static unsigned next_calls, probe_calls;
static const ULONG EXTRA_BASE = 0x7fff0000;
/* SPECIAL_FLAGS of graphics/modeid.h: DUALPF, PF2PRI, HAM, EXTRAHALFBRITE */
static const ULONG SPECIAL = (1UL << 1) | (1UL << 2) | (1UL << 3) | (1UL << 12);
static const ULONG DIPF_HAM = 1UL << 3;

static DbMode &add_mode(ULONG id, UWORD width, UWORD height, UWORD maxdepth, ULONG pixfmt,
                        ULONG bppix)
{
    db.push_back({ .id = id, .width = width, .height = height, .maxdepth = maxdepth,
                   .pixfmt = pixfmt, .bppix = bppix });
    return db.back();
}

static const DbMode *find_mode(ULONG id)
{
    for (const auto &m : db)
        if (m.id == id)
            return &m;
    return nullptr;
}

/* BestModeIDA() of rom/graphics/bestmodeida.c over db: the default monitor
   first, the enabled other ones only when it gives nothing, the filters
   (NotAvailable, the special flags against the default DIPFMustNotHave, 4
   bits a gun), the sizes and depth as minimums and the ranking in which
   the first of equal modes stays. */
static ULONG native_best(ULONG width, ULONG height, ULONG depth, ULONG monitorid, ULONG mask)
{
    ULONG found = INVALID;
    UWORD fd = 0xffff, fw = 0xffff, fh = 0xffff;
    auto scan = [&](ULONG monitor)
    {
        for (const auto &m : db)
        {
            if ((m.id >> 16) != monitor ||
                (monitorid != INVALID && (monitorid & mask) != (m.id & mask)) ||
                m.not_available || (m.flags & SPECIAL) || m.gun < 4)
                continue;
            if (m.maxdepth >= depth && m.width >= width && m.height >= height &&
                m.maxdepth <= fd && m.width <= fw && m.height <= fh &&
                (m.maxdepth < fd || m.width < fw || m.height < fh))
            {
                found = m.id;
                fd = m.maxdepth;
                fw = m.width;
                fh = m.height;
            }
        }
    };
    scan(default_monitor);
    if (found == INVALID)
    {
        std::vector<ULONG> seen;
        for (const auto &m : db)
        {
            ULONG monitor = m.id >> 16;
            bool enabled = std::find(disabled_monitors.begin(), disabled_monitors.end(),
                                     monitor) == disabled_monitors.end();
            if (monitor != default_monitor && enabled &&
                std::find(seen.begin(), seen.end(), monitor) == seen.end())
            {
                seen.push_back(monitor);
                scan(monitor);
            }
        }
    }
    return found;
}

struct ModeCall { ULONG width, height, depth; unsigned tags; };
static std::vector<ModeCall> mode_calls;

/* cybergraphics.library's BestCModeIDTagList(), which hands its size and
   depth on to BestModeIDA(); the bridge must give all three, and values
   that the UWORDs and the UBYTE of BestModeIDA() hold. */
extern "C" ULONG BestCModeIDTagList(struct TagItem *tags)
{
    ModeCall call = { 0, 0, 0, 0 };
    unsigned n = 0;

    for (; tags[n].ti_Tag != TAG_DONE; ++n)
    {
        CHECK(n < 3);
        if (tags[n].ti_Tag == CYBRBIDTG_NominalWidth) call.width = (ULONG)tags[n].ti_Data;
        else if (tags[n].ti_Tag == CYBRBIDTG_NominalHeight) call.height = (ULONG)tags[n].ti_Data;
        else if (tags[n].ti_Tag == CYBRBIDTG_Depth) call.depth = (ULONG)tags[n].ti_Data;
        else CHECK(!"an unexpected cybergraphics tag");
    }
    call.tags = n;
    mode_calls.push_back(call);
    CHECK(n == 3);
    CHECK(call.width >= 1 && call.width <= 0xffff && call.height >= 1 && call.height <= 0xffff);
    CHECK(call.depth >= 1 && call.depth <= 24);
    return native_best(call.width, call.height, call.depth, INVALID, ALL);
}

/* The probe of the bridge: a MonitorID with size and depth 1. The first
   pass matches the whole ID, the AROS one the monitor only. */
extern "C" ULONG BestModeIDA(struct TagItem *tags)
{
    ULONG monitorid = INVALID, width = 0, height = 0, depth = 0, id;

    ++probe_calls;
    for (unsigned n = 0; tags[n].ti_Tag != TAG_DONE; ++n)
    {
        if (tags[n].ti_Tag == BIDTAG_MonitorID) monitorid = (ULONG)tags[n].ti_Data;
        else if (tags[n].ti_Tag == BIDTAG_NominalWidth) width = (ULONG)tags[n].ti_Data;
        else if (tags[n].ti_Tag == BIDTAG_NominalHeight) height = (ULONG)tags[n].ti_Data;
        else if (tags[n].ti_Tag == BIDTAG_Depth) depth = (ULONG)tags[n].ti_Data;
        else CHECK(!"an unexpected BestModeIDA() tag");
    }
    CHECK(monitorid != INVALID && width == 1 && height == 1 && depth == 1);
    id = native_best(width, height, depth, monitorid, ALL);
    return id != INVALID ? id : native_best(width, height, depth, monitorid, 0xffff0000UL);
}

extern "C" ULONG NextDisplayInfo(ULONG last)
{
    std::vector<ULONG> ids;

    ++next_calls;
    for (ULONG k = 0; k < extra_ids; ++k)
        ids.push_back(EXTRA_BASE + k);
    for (const auto &m : db)
        ids.push_back(m.id);
    if (ids.empty())
        return INVALID;
    if (last == INVALID)
        return ids[0];
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == last)
            return i + 1 < ids.size() ? ids[i + 1] : ids_cycle ? ids[0] : INVALID;
    CHECK(!"NextDisplayInfo() of an unknown ID");
    return INVALID;
}

extern "C" BOOL IsCyberModeID(ULONG id)
{
    if (id >= EXTRA_BASE && id < EXTRA_BASE + extra_ids)
        return FALSE;
    const DbMode *m = find_mode(id);
    CHECK(m != nullptr);
    return m && m->cyber;
}

extern "C" ULONG GetDisplayInfoData(DisplayInfoHandle handle, UBYTE *buf, ULONG size, ULONG tag,
                                    ULONG id)
{
    const DbMode *m = find_mode(id);
    struct DimensionInfo dims = {};

    CHECK(handle == nullptr && tag == DTAG_DIMS && size == sizeof(dims) && m != nullptr);
    if (!m)
        return 0;
    dims.MaxDepth = m->maxdepth;
    dims.Nominal.MaxX = (WORD)(m->width - 1);
    dims.Nominal.MaxY = (WORD)(m->height - 1);
    std::memcpy(buf, &dims, m->dims_bytes < size ? m->dims_bytes : size);
    return m->dims_bytes;
}

/* A mode that is not of cybergraphics.library still answers, with 0 for
   PIXFMT_LUT8, so that the bridge must ask IsCyberModeID() first. */
extern "C" ULONG GetCyberIDAttr(ULONG attribute, ULONG id)
{
    const DbMode *m = find_mode(id);

    CHECK(m != nullptr);
    if (!m)
        return 0;
    return attribute == CYBRIDATTR_PIXFMT ? (m->cyber ? m->pixfmt : 0) :
           attribute == CYBRIDATTR_BPPIX ? (m->cyber ? m->bppix : 1) : 0;
}

struct PixelCall
{
    const UBYTE *src; UWORD srcx, srcy, srcmod; struct RastPort *rp;
    UWORD destx, desty, width, height; UBYTE format;
};
static std::vector<PixelCall> pixel_calls;

extern "C" ULONG WritePixelArray(APTR src, UWORD srcx, UWORD srcy, UWORD srcmod, struct RastPort *rp,
                                 UWORD destx, UWORD desty, UWORD width, UWORD height, UBYTE format)
{
    pixel_calls.push_back({ static_cast<const UBYTE *>(src), srcx, srcy, srcmod, rp,
                            destx, desty, width, height, format });
    return (ULONG)width * height;
}

/* setup_base() of kaffeine_memory.c as it is: a full library map still
   gives a base, which no lookup then finds. */
static unsigned setup_calls;
static bool fail_setup;

extern "C" ULONG Kaffeine_FindLibBase(KaffeineContext *ctx, const char *name)
{
    for (UWORD i = 0; i < ctx->num_libmap; ++i)
        if (!std::strcmp(ctx->libmap[i].name, name))
            return ctx->libmap[i].m68k_base;
    return 0;
}

extern "C" ULONG Kaffeine_SetupLibBaseSized(KaffeineContext *ctx, UWORD id, const char *name,
                                            UWORD vectors, UWORD pos_size,
                                            const M68KThunkEntry *thunks, ULONG num_thunks,
                                            const M68KThunkEntry *gen, ULONG num_gen)
{
    ++setup_calls;
    CHECK(id == ctx->num_libs && vectors == KAFFEINE_MAX_LVO && pos_size == 34);
    CHECK(thunks == kaffeine_thunks_p96 && num_thunks == kaffeine_thunks_p96_count);
    CHECK(gen == nullptr && num_gen == 0);
    if (fail_setup || ctx->num_libs >= KAFFEINE_MAX_LIBS)
        return 0;
    auto &lib = ctx->libs[ctx->num_libs++];
    lib.m68k_addr = P96_BASE;
    lib.jt_start = P96_BASE - 6u * vectors;
    lib.pos_size = pos_size;
    lib.num_vectors = vectors;
    lib.thunks = thunks;
    lib.num_thunks = num_thunks;
    std::snprintf(lib.name, sizeof(lib.name), "%s", name);
    if (ctx->num_libmap < KAFFEINE_MAX_LIBMAP)
    {
        auto &entry = ctx->libmap[ctx->num_libmap++];
        std::snprintf(entry.name, sizeof(entry.name), "%s", name);
        entry.m68k_base = P96_BASE;
    }
    return P96_BASE;
}

/* ── the CPU and the fixture ── */

struct TestCPU { ULONG d[8] = {}, a[8] = {}; UWORD sr = 0x2715; ULONG pc = 0; };
static ULONG get_d(void *p, int n) { return static_cast<TestCPU *>(p)->d[n]; }
static ULONG get_a(void *p, int n) { return static_cast<TestCPU *>(p)->a[n]; }
static void set_d(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->d[n] = v; }
static void set_a(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->a[n] = v; }
static UWORD get_sr(void *p) { return static_cast<TestCPU *>(p)->sr; }
static void set_sr(void *p, UWORD v) { static_cast<TestCPU *>(p)->sr = v; }
static ULONG get_pc(void *p) { return static_cast<TestCPU *>(p)->pc; }
static void set_pc(void *p, ULONG v) { static_cast<TestCPU *>(p)->pc = v; }
static void execute(void *) { std::abort(); }
static const KaffeineCPUOps cpu_ops = {
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

struct Fixture
{
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory = std::vector<UBYTE>(MEM_SIZE, 0);
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        ctx.mem = memory.data();
        ctx.mem_size = MEM_SIZE;
        ctx.running = TRUE;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = P96_BASE;
        lib.jt_start = P96_BASE - 6u * KAFFEINE_MAX_LVO;
        lib.pos_size = 34;
        std::strcpy(lib.name, "Picasso96API.library");
        lib.thunks = kaffeine_thunks_p96;
        lib.num_thunks = kaffeine_thunks_p96_count;
        forbid_lo = forbid_hi = 0;
        program_calls = setup_calls = 0;
        fail_setup = false;
        mode_calls.clear();
        db.clear();
        default_monitor = 0x5003;
        disabled_monitors.clear();
        extra_ids = 0;
        ids_cycle = false;
        next_calls = probe_calls = 0;
        pixel_calls.clear();
        native_rp = {};
        native_rp.BitMap = SOME_BITMAP;
        nobitmap_rp = {};
    }

    void put32(ULONG at, ULONG v) { m68k_write32(&ctx, at, v); }
    void put16(ULONG at, UWORD v) { m68k_write16(&ctx, at, v); }

    void tags(ULONG at, std::initializer_list<ULONG> items)
    {
        for (ULONG v : items) { put32(at, v); at += 4; }
    }

    void render_info(ULONG memory_at, WORD bpr, ULONG format)
    {
        put32(RI, memory_at);
        put16(RI + 4, (UWORD)bpr);
        put16(RI + 6, 0);
        put32(RI + 8, format);
    }

    ULONG call(ULONG lvo, ULONG a0, ULONG a1 = 0, std::initializer_list<ULONG> d = {})
    {
        for (int i = 0; i < 8; ++i) { state.d[i] = 0xd0d0d000u + i; state.a[i] = 0xa0a0a000u + i; }
        int n = 0;
        for (ULONG v : d) state.d[n++] = v;
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[7] = 0x100;
        m68k_write32(&ctx, 0x100, RETURN_PC);
        KaffeineDispatchResult result = {};
        CHECK(Kaffeine_DispatchLibraryCall(&ctx, &cpu, P96_BASE - lvo, &result));
        CHECK(result.return_pc == RETURN_PC && state.a[7] == 0x104);
        return state.d[0];
    }

    bool stopped() const { return !ctx.running && ctx.exit_code == 20; }
};

/* ── the table ── */

static void test_table()
{
    CHECK(kaffeine_thunks_p96_count == 28);
    for (ULONG i = 0; i < kaffeine_thunks_p96_count; ++i)
        CHECK(kaffeine_thunks_p96[i].lvo == 30 + 6 * i && kaffeine_thunks_p96[i].thunk != nullptr);
    CHECK(kaffeine_thunks_p96[kaffeine_thunks_p96_count].thunk == nullptr);
}

static void test_unsupported()
{
    for (ULONG lvo = 30; lvo <= 192; lvo += 6)
    {
        if (lvo == 60 || lvo == 102)
            continue;
        Fixture f;
        ULONG d0 = f.call(lvo, TAGS, RP_GUEST, { 1, 2, 3, 4, 5, 6 });
        CHECK(f.stopped());
        CHECK(d0 == 20);
        CHECK(mode_calls.empty() && pixel_calls.empty() && program_calls == 0);
    }
}

/* ── p96BestModeIDTagList() ── */

static const ULONG AMIPIXELSHOP_FORMATS =
    (1UL << RGBFB_A8R8G8B8) | (1UL << RGBFB_B8G8R8A8) | (1UL << RGBFB_R8G8B8A8);
static const ULONG TRUECOLOR = (1UL << RGBFB_R8G8B8) | (1UL << RGBFB_B8G8R8);
static const ULONG TRUEALPHA = (1UL << RGBFB_A8R8G8B8) | (1UL << RGBFB_A8B8G8R8) |
                               (1UL << RGBFB_R8G8B8A8) | (1UL << RGBFB_B8G8R8A8);
static const ULONG ARGB = 1UL << RGBFB_A8R8G8B8;
static const ULONG P96BIDTAG_PabloIVCompatible = P96BIDTAG_Dummy + 7;
static const ULONG P96BIDTAG_PalomaIVCompatible = P96BIDTAG_Dummy + 8;

static ULONG best(Fixture &f, std::initializer_list<ULONG> items)
{
    f.tags(TAGS, items);
    return f.call(60, TAGS);
}

/* The answer the bridge must give, built apart from it: the mode of
   BestCModeIDTagList() when its format is allowed, else, in database order,
   the modes of cybergraphics with a full DimensionInfo, the size and depth,
   an allowed format (and four bytes for 25 to 32) that BestModeIDA() takes
   by itself, of which the first that no later one beats in the ranking. */
static ULONG oracle(ULONG width, ULONG height, ULONG depth, ULONG mask)
{
    bool four = depth > 24;
    ULONG w = width ? width : 1, h = height ? height : 1, d = four ? 24 : depth ? depth : 1;
    auto ok = [&](const DbMode &m)
    {
        static const ULONG rgb[] = { RGBFB_CLUT, RGBFB_R5G5B5, ALL, RGBFB_R5G5B5PC,
                                     RGBFB_B5G5R5PC, RGBFB_R5G6B5, ALL, RGBFB_R5G6B5PC,
                                     RGBFB_B5G6R5PC, RGBFB_R8G8B8, RGBFB_B8G8R8, RGBFB_A8R8G8B8,
                                     RGBFB_B8G8R8A8, RGBFB_R8G8B8A8 };
        ULONG format = m.pixfmt < 14 ? rgb[m.pixfmt] :
                       m.pixfmt == PIXFMT_ABGR32 || m.pixfmt == PIXFMT_0BGR32 ? RGBFB_A8B8G8R8 :
                       m.pixfmt == PIXFMT_0RGB32 ? RGBFB_A8R8G8B8 :
                       m.pixfmt == PIXFMT_BGR032 ? RGBFB_B8G8R8A8 :
                       m.pixfmt == PIXFMT_RGB032 ? RGBFB_R8G8B8A8 : ALL;
        return m.cyber && format != ALL && (mask & (1UL << format)) && (!four || m.bppix == 4);
    };
    ULONG first = native_best(w, h, d, INVALID, ALL);
    if (first == INVALID || ok(*find_mode(first)))
        return first;
    const DbMode *pick = nullptr;
    for (const auto &m : db)
    {
        if (!m.cyber || m.dims_bytes < offsetof(struct DimensionInfo, MaxOScan) ||
            m.width < w || m.height < h || m.maxdepth < d || !ok(m))
            continue;
        if (native_best(1, 1, 1, m.id, ALL) != m.id)
            continue;
        if (!pick || (m.maxdepth <= pick->maxdepth && m.width <= pick->width &&
                      m.height <= pick->height &&
                      (m.maxdepth < pick->maxdepth || m.width < pick->width ||
                       m.height < pick->height)))
            pick = &m;
    }
    return pick ? pick->id : INVALID;
}

static void test_best_mode()
{
    {   /* AmiPixelShop's tags: 32-bit storage is 24 bits on four bytes */
        Fixture f;
        add_mode(0x50031000, 640, 480, 24, PIXFMT_ARGB32, 4);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 32, P96BIDTAG_FormatsAllowed, AMIPIXELSHOP_FORMATS,
                        TAG_DONE, 0 }) == 0x50031000 && f.ctx.running);
        CHECK(mode_calls.size() == 1 && mode_calls[0].width == 640 &&
              mode_calls[0].height == 480 && mode_calls[0].depth == 24);
        CHECK(next_calls == 0 && probe_calls == 0);
    }
    {   /* the case of the review: a 3-byte RGB24 mode ranked before a
           4-byte ARGB32 one of the same size and depth */
        Fixture f;
        add_mode(0x50030001, 800, 600, 24, PIXFMT_RGB24, 3);
        add_mode(0x50030002, 800, 600, 24, PIXFMT_ARGB32, 4);
        CHECK(best(f, { P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == 0x50030002 && f.ctx.running);
        CHECK(mode_calls.size() == 1 && mode_calls[0].depth == 24 && next_calls > 0 &&
              probe_calls == 1);
        CHECK(best(f, { P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUEALPHA,
                        TAG_DONE, 0 }) == 0x50030002);
        next_calls = 0;
        CHECK(best(f, { P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUECOLOR,
                        TAG_DONE, 0 }) == 0x50030001 && next_calls == 0);
        /* depths that no format has, or that BestModeIDA() would cut */
        for (ULONG depth : { 33u, 64u, 256u, 0x118u, 0x10018u, 0xffffffffu })
        {
            mode_calls.clear();
            next_calls = 0;
            CHECK(best(f, { P96BIDTAG_Depth, depth, TAG_DONE, 0 }) == INVALID);
            CHECK(f.ctx.running && mode_calls.empty() && next_calls == 0);
        }
    }
    {   /* AmiPixelShop on a 1920 by 1080 Workbench */
        Fixture f;
        add_mode(0x50031001, 1920, 1080, 24, PIXFMT_RGB24, 3);
        add_mode(0x50031002, 1920, 1080, 24, PIXFMT_BGRA32, 4);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 1920, P96BIDTAG_NominalHeight, 1080,
                        P96BIDTAG_Depth, 32, P96BIDTAG_FormatsAllowed, AMIPIXELSHOP_FORMATS,
                        TAG_DONE, 0 }) == 0x50031002);
    }
    {   /* a forbidden winner: the next mode in the ranking */
        Fixture f;
        add_mode(0x50032001, 800, 600, 24, PIXFMT_ARGB32, 4);
        add_mode(0x50032002, 1024, 768, 24, PIXFMT_ARGB32, 4);
        add_mode(0x50032003, 800, 600, 24, PIXFMT_BGRA32, 4);
        CHECK(best(f, { P96BIDTAG_Depth, 24, P96BIDTAG_FormatsForbidden, ARGB,
                        TAG_DONE, 0 }) == 0x50032003);
    }
    {   /* 4-byte modes of MaxDepth 32 and 24 of the same size: depth 32 asks
           for 24, so the second now; both have four bytes */
        Fixture f;
        add_mode(0x50033001, 800, 600, 32, PIXFMT_ARGB32, 4);
        add_mode(0x50033002, 800, 600, 24, PIXFMT_BGRA32, 4);
        CHECK(best(f, { P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == 0x50033002);
    }
    {   /* the filters of BestModeIDA() through the probe, and the bridge's
           own: a smaller eligible mode that one of them rejects is skipped */
        for (int filter = 0; filter < 8; ++filter)
        {
            Fixture f;
            add_mode(0x50034001, 640, 480, 24, PIXFMT_RGB24, 3);     /* the first choice */
            DbMode &m = add_mode(filter == 1 ? 0x50040001 : 0x50034002, 700, 500, 24,
                                 PIXFMT_ARGB32, 4);
            switch (filter)
            {
                case 0: m.flags = DIPF_HAM; break;
                case 1: disabled_monitors.push_back(0x5004); break;  /* not the default */
                case 2: m.cyber = false; break;
                case 3: m.pixfmt = PIXFMT_BGR15; m.bppix = 2; break;
                case 4: m.gun = 3; break;
                case 5: m.not_available = true; break;
                case 6: m.dims_bytes = offsetof(struct DimensionInfo, MaxOScan) - 1; break;
                case 7: m.bppix = 3; break;                          /* not four bytes */
            }
            add_mode(0x50034003, 800, 600, 24, PIXFMT_ARGB32, 4);
            CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                            P96BIDTAG_Depth, (ULONG)(filter == 7 ? 32 : 24),
                            P96BIDTAG_FormatsAllowed, filter == 3 ? ~TRUECOLOR : TRUEALPHA,
                            TAG_DONE, 0 }) == 0x50034003);
            CHECK(f.ctx.running);
        }
        /* a disabled monitor's mode with no other: INVALID_ID */
        Fixture f;
        add_mode(0x50035001, 640, 480, 24, PIXFMT_RGB24, 3);
        add_mode(0x50050001, 800, 600, 24, PIXFMT_ARGB32, 4);
        disabled_monitors.push_back(0x5005);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUEALPHA,
                        TAG_DONE, 0 }) == INVALID && f.ctx.running && next_calls > 0);
        /* the same monitor enabled: its mode */
        disabled_monitors.clear();
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUEALPHA,
                        TAG_DONE, 0 }) == 0x50050001);
    }
    {   /* the probe gives another ID of the same monitor: not the candidate */
        Fixture f;
        add_mode(0x50036001, 640, 480, 24, PIXFMT_RGB24, 3);
        add_mode(0x50060001, 700, 500, 24, PIXFMT_ARGB32, 4).flags = DIPF_HAM;
        add_mode(0x50060002, 1024, 768, 8, PIXFMT_LUT8, 1);
        add_mode(0x50036003, 800, 600, 24, PIXFMT_ARGB32, 4);
        CHECK(native_best(1, 1, 1, 0x50060001, ALL) == INVALID &&
              native_best(1, 1, 1, 0x50060001, 0xffff0000UL) == 0x50060002);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUEALPHA,
                        TAG_DONE, 0 }) == 0x50036003 && probe_calls == 2);
    }
    {   /* a first choice that is not of cybergraphics, whose GetCyberIDAttr()
           says LUT8, is not taken for CLUT */
        Fixture f;
        add_mode(0x50037001, 640, 480, 8, PIXFMT_LUT8, 1).cyber = false;
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        TAG_DONE, 0 }) == INVALID);
        add_mode(0x50037002, 800, 600, 8, PIXFMT_LUT8, 1);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        TAG_DONE, 0 }) == 0x50037002);
    }
    {   /* modes that are not comparable: the first stays, as in BestModeIDA() */
        for (int reversed = 0; reversed < 2; ++reversed)
        {
            Fixture f;
            add_mode(0x50038001, 640, 480, 24, PIXFMT_RGB24, 3);
            add_mode(reversed ? 0x50038003 : 0x50038002, reversed ? 800 : 1024,
                     reversed ? 768 : 600, 24, PIXFMT_ARGB32, 4);
            add_mode(reversed ? 0x50038002 : 0x50038003, reversed ? 1024 : 800,
                     reversed ? 600 : 768, 24, PIXFMT_ARGB32, 4);
            CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                            P96BIDTAG_Depth, 24, P96BIDTAG_FormatsAllowed, TRUEALPHA,
                            TAG_DONE, 0 }) == (reversed ? 0x50038003 : 0x50038002));
            CHECK(next_calls > 0);
        }
    }
    {   /* equal modes: the first stays */
        Fixture f;
        add_mode(0x5003a101, 640, 480, 24, PIXFMT_RGB24, 3);
        add_mode(0x5003a102, 800, 600, 24, PIXFMT_ARGB32, 4);
        add_mode(0x5003a103, 800, 600, 24, PIXFMT_BGRA32, 4);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == 0x5003a102 && probe_calls == 1);
    }
    {   /* a grid of requests on one display: the bridge gives the answer of
           the oracle, and the first choice untouched whenever it is eligible */
        static const ULONG widths[] = { 0, 1, 640, 800, 1024, 1280 };
        static const ULONG heights[] = { 0, 480, 600, 768, 1024 };
        static const ULONG depths[] = { 0, 1, 8, 15, 16, 24, 25, 32, 99 };
        static const ULONG masks[] = {
            ALL, 1UL << RGBFB_CLUT, (1UL << RGBFB_R5G6B5) | (1UL << RGBFB_R5G6B5PC),
            TRUECOLOR, TRUEALPHA, ARGB, (1UL << RGBFB_CLUT) | TRUEALPHA,
            AMIPIXELSHOP_FORMATS
        };
        Fixture f;
        add_mode(0x50039001, 640, 480, 8, PIXFMT_LUT8, 1);
        add_mode(0x50039002, 640, 480, 16, PIXFMT_RGB16PC, 2);
        add_mode(0x50039003, 800, 600, 24, PIXFMT_RGB24, 3);
        add_mode(0x50039004, 800, 600, 24, PIXFMT_ARGB32, 4);
        add_mode(0x50039005, 1024, 768, 8, PIXFMT_LUT8, 1);
        add_mode(0x50039006, 1024, 768, 24, PIXFMT_BGRA32, 4);
        add_mode(0x50039007, 1280, 1024, 16, PIXFMT_RGB16, 2);
        add_mode(0x50039008, 1280, 1024, 32, PIXFMT_RGBA32, 4);
        for (ULONG w : widths)
            for (ULONG h : heights)
                for (ULONG d : depths)
                    for (ULONG mask : masks)
                    {
                        ULONG want = d > 32 ? INVALID : oracle(w, h, d, mask);
                        unsigned calls = next_calls;
                        CHECK(best(f, { P96BIDTAG_NominalWidth, w, P96BIDTAG_NominalHeight, h,
                                        P96BIDTAG_Depth, d, P96BIDTAG_FormatsAllowed, mask,
                                        TAG_DONE, 0 }) == want);
                        if (d <= 32)
                        {
                            ULONG first = native_best(w ? w : 1, h ? h : 1,
                                                      d > 24 ? 24 : d ? d : 1, INVALID, ALL);
                            if (first == want)
                                CHECK(next_calls == calls);
                        }
                    }
        CHECK(f.ctx.running);
    }
    {   /* nothing eligible: no mode of the depth, or none of the formats */
        Fixture f;
        add_mode(0x5003a001, 800, 600, 8, PIXFMT_LUT8, 1);
        add_mode(0x5003a002, 800, 600, 16, PIXFMT_RGB16, 2);
        CHECK(best(f, { P96BIDTAG_Depth, 24, TAG_DONE, 0 }) == INVALID && next_calls == 0);
        CHECK(best(f, { P96BIDTAG_FormatsAllowed, TRUEALPHA, TAG_DONE, 0 }) == INVALID &&
              next_calls > 0 && f.ctx.running);
    }
    {   /* depths: 0 is 1, absent is 8, 24 any storage, 25 and 32 four bytes */
        Fixture f;
        add_mode(0x5003b001, 800, 600, 24, PIXFMT_RGB24, 3);
        CHECK(best(f, { P96BIDTAG_Depth, 0, TAG_DONE, 0 }) == 0x5003b001 &&
              !mode_calls.empty() && mode_calls.back().depth == 1);
        CHECK(best(f, { TAG_DONE, 0 }) == 0x5003b001 && !mode_calls.empty() &&
              mode_calls.back().depth == 8);
        CHECK(best(f, { P96BIDTAG_Depth, 24, TAG_DONE, 0 }) == 0x5003b001);
        CHECK(best(f, { P96BIDTAG_Depth, 25, TAG_DONE, 0 }) == INVALID &&
              !mode_calls.empty() && mode_calls.back().depth == 24);
        CHECK(best(f, { P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == INVALID);
        add_mode(0x5003b002, 800, 600, 24, PIXFMT_0RGB32, 4);
        CHECK(best(f, { P96BIDTAG_Depth, 25, TAG_DONE, 0 }) == 0x5003b002);
        CHECK(best(f, { P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == 0x5003b002);
    }
    {   /* sizes: 0 is 1, absent is 800 by 600, 0xffff goes on, more does not */
        Fixture f;
        add_mode(0x5003c001, 800, 600, 8, PIXFMT_LUT8, 1);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 0, P96BIDTAG_NominalHeight, 0,
                        TAG_DONE, 0 }) == 0x5003c001 && !mode_calls.empty() &&
              mode_calls.back().width == 1 && mode_calls.back().height == 1);
        CHECK(best(f, { TAG_DONE, 0 }) == 0x5003c001 && !mode_calls.empty() &&
              mode_calls.back().width == 800 && mode_calls.back().height == 600 &&
              mode_calls.back().tags == 3);
        CHECK(f.call(60, 0) == 0x5003c001 && !mode_calls.empty() &&
              mode_calls.back().width == 800);
        for (ULONG tag : { P96BIDTAG_NominalWidth, P96BIDTAG_NominalHeight })
        {
            mode_calls.clear();
            CHECK(best(f, { tag, 0xffff, TAG_DONE, 0 }) == INVALID && mode_calls.size() == 1);
            for (ULONG size : { 0x10000u, 0x10320u, 0x101e0u, 0xffffffffu })
            {
                mode_calls.clear();
                CHECK(best(f, { tag, size, TAG_DONE, 0 }) == INVALID && mode_calls.empty());
            }
        }
        CHECK(f.ctx.running);
    }
    {   /* the card tags: TRUE asks for what no mode of AROS is, FALSE nothing;
           and a mask with no format left */
        Fixture f;
        add_mode(0x5003d001, 800, 600, 8, PIXFMT_LUT8, 1);
        for (ULONG tag : { P96BIDTAG_VideoCompatible, P96BIDTAG_PabloIVCompatible,
                           P96BIDTAG_PalomaIVCompatible })
        {
            mode_calls.clear();
            CHECK(best(f, { tag, 1, TAG_DONE, 0 }) == INVALID && mode_calls.empty());
            CHECK(best(f, { tag, 0, TAG_DONE, 0 }) == 0x5003d001 && mode_calls.size() == 1);
        }
        mode_calls.clear();
        CHECK(best(f, { P96BIDTAG_FormatsAllowed, 1UL << RGBFB_CLUT,
                        P96BIDTAG_FormatsForbidden, 1UL << RGBFB_CLUT, TAG_DONE, 0 }) == INVALID &&
              mode_calls.empty() && f.ctx.running);
    }
    {   /* the search looks at 512 display IDs: the 512th found, a 513th
           gives INVALID_ID, and so does a database that starts again */
        for (ULONG extra : { 510u, 511u })
        {
            Fixture f;
            extra_ids = extra;
            add_mode(0x5003e001, 640, 480, 24, PIXFMT_RGB24, 3);
            add_mode(0x5003e002, 800, 600, 24, PIXFMT_ARGB32, 4);
            CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                            P96BIDTAG_Depth, 32, TAG_DONE, 0 }) ==
                  (extra == 510 ? 0x5003e002 : INVALID));
            CHECK(next_calls == 513 && f.ctx.running);
        }
        Fixture f;
        ids_cycle = true;
        add_mode(0x5003e001, 640, 480, 24, PIXFMT_RGB24, 3);
        add_mode(0x5003e002, 800, 600, 24, PIXFMT_ARGB32, 4);
        CHECK(best(f, { P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
                        P96BIDTAG_Depth, 32, TAG_DONE, 0 }) == INVALID &&
              next_calls == 513 && f.ctx.running);
    }
    {   /* every pixel format with a Picasso96 name, each allowed alone */
        static const struct { ULONG pixfmt, rgb; } names[] = {
            { PIXFMT_LUT8, RGBFB_CLUT }, { PIXFMT_RGB15, RGBFB_R5G5B5 },
            { PIXFMT_RGB15PC, RGBFB_R5G5B5PC }, { PIXFMT_BGR15PC, RGBFB_B5G5R5PC },
            { PIXFMT_RGB16, RGBFB_R5G6B5 }, { PIXFMT_RGB16PC, RGBFB_R5G6B5PC },
            { PIXFMT_BGR16PC, RGBFB_B5G6R5PC }, { PIXFMT_RGB24, RGBFB_R8G8B8 },
            { PIXFMT_BGR24, RGBFB_B8G8R8 }, { PIXFMT_ARGB32, RGBFB_A8R8G8B8 },
            { PIXFMT_BGRA32, RGBFB_B8G8R8A8 }, { PIXFMT_RGBA32, RGBFB_R8G8B8A8 },
            { PIXFMT_ABGR32, RGBFB_A8B8G8R8 }, { PIXFMT_0RGB32, RGBFB_A8R8G8B8 },
            { PIXFMT_BGR032, RGBFB_B8G8R8A8 }, { PIXFMT_RGB032, RGBFB_R8G8B8A8 },
            { PIXFMT_0BGR32, RGBFB_A8B8G8R8 },
        };
        for (const auto &n : names)
        {
            Fixture f;
            add_mode(0x5003f001, 800, 600, 8, n.pixfmt, 4);
            CHECK(best(f, { P96BIDTAG_FormatsAllowed, (ULONG)(1UL << n.rgb), TAG_DONE, 0 }) ==
                  0x5003f001 && f.ctx.running);
            CHECK(best(f, { P96BIDTAG_FormatsForbidden, (ULONG)(1UL << n.rgb), TAG_DONE, 0 }) ==
                  INVALID && f.ctx.running);
        }
        /* BGR15 and BGR16 have no Picasso96 name */
        for (ULONG pixfmt : { PIXFMT_BGR15, PIXFMT_BGR16 })
        {
            Fixture f;
            add_mode(0x5003f002, 800, 600, 16, pixfmt, 2);
            CHECK(best(f, { TAG_DONE, 0 }) == INVALID && f.ctx.running);
        }
    }
    {   /* ModeID 0 is a mode, returned as 0 */
        Fixture f;
        add_mode(0, 800, 600, 8, PIXFMT_LUT8, 1);
        CHECK(best(f, { TAG_DONE, 0 }) == 0 && f.ctx.running);
        CHECK(f.call(60, 0) == 0 && f.ctx.running);
    }
    {   /* TAG_SKIP passes over items it does not read, readable or not */
        Fixture f;
        add_mode(0x50038000, 800, 600, 16, PIXFMT_RGB16, 2);
        forbid_lo = TAGS + 8; forbid_hi = TAGS + 24;
        CHECK(best(f, { TAG_SKIP, 2, P96BIDTAG_Depth, 4, P96BIDTAG_Depth, 4,
                        P96BIDTAG_Depth, 16, TAG_DONE, 0 }) == 0x50038000 && f.ctx.running);
        CHECK(mode_calls.size() == 1 && mode_calls[0].depth == 16);
    }
    {   /* TAG_IGNORE, TAG_SKIP, TAG_MORE and a card tag that is FALSE */
        Fixture f;
        add_mode(0x50034000, 1024, 768, 16, PIXFMT_RGB16PC, 2);
        f.tags(TAGS, { TAG_IGNORE, 999, TAG_SKIP, 1, P96BIDTAG_Depth, 4,
                       P96BIDTAG_VideoCompatible, 0, TAG_MORE, TAGS2 });
        f.tags(TAGS2, { P96BIDTAG_Depth, 16, P96BIDTAG_NominalWidth, 1024, TAG_DONE, 0 });
        CHECK(f.call(60, TAGS) == 0x50034000 && f.ctx.running);
        CHECK(mode_calls.size() == 1 && mode_calls[0].depth == 16 && mode_calls[0].width == 1024);
    }
    {   /* P96_TAG_STEPS: 64 items with the TAG_DONE, not 65 */
        for (ULONG items : { 64u, 65u })
        {
            Fixture f;
            add_mode(0x50039000, 800, 600, 8, PIXFMT_LUT8, 1);
            for (ULONG k = 0; k + 1 < items; ++k)
                f.tags(TAGS + 8 * k, { TAG_IGNORE, k });
            f.tags(TAGS + 8 * (items - 1), { TAG_DONE, 0 });
            ULONG d0 = f.call(60, TAGS);
            CHECK(items == 64 ? d0 == 0x50039000 && f.ctx.running : f.stopped());
        }
    }
    {   /* lists that are not in program memory, or that do not end */
        for (ULONG at : { TAGS + 1, (ULONG)PROG_HI - 4, 0x10u })
        {
            Fixture f;
            f.call(60, at);
            CHECK(f.stopped() && mode_calls.empty());
        }
        Fixture f;
        f.tags(TAGS, { TAG_MORE, TAGS });
        f.call(60, TAGS);
        CHECK(f.stopped() && mode_calls.empty());
        Fixture g;
        forbid_lo = TAGS2; forbid_hi = TAGS2 + 8;
        g.tags(TAGS, { TAG_MORE, TAGS2 });
        g.call(60, TAGS);
        CHECK(g.stopped() && mode_calls.empty());
        /* TAG_SKIP counts that would wrap the address, forwards or back */
        for (ULONG skip : { 0x20000000u, 0x1ffffffeu, 0xffffffffu, (ULONG)MEM_SIZE / 8 })
        {
            Fixture h;
            add_mode(0, 800, 600, 8, PIXFMT_LUT8, 1);
            h.tags(TAGS, { TAG_DONE, 0, TAG_SKIP, skip, TAG_DONE, 0 });
            h.call(60, TAGS + 8);
            CHECK(h.stopped() && mode_calls.empty());
        }
    }
}

/* ── p96WritePixelArray() ── */

static void test_write_pixels()
{
    struct { ULONG rgb; UBYTE rect; ULONG bpp; } formats[] = {
        { RGBFB_A8R8G8B8, RECTFMT_ARGB32, 4 }, { RGBFB_B8G8R8A8, RECTFMT_BGRA32, 4 },
        { RGBFB_R8G8B8A8, RECTFMT_RGBA32, 4 }, { RGBFB_A8B8G8R8, RECTFMT_ABGR32, 4 },
        { RGBFB_R8G8B8, RECTFMT_RGB24, 3 }, { RGBFB_B8G8R8, RECTFMT_BGR24, 3 },
    };
    for (const auto &fm : formats)
    {   /* the first source pixel, the stride and the rest as they are */
        Fixture f;
        f.render_info(BUF, 16 * 4, fm.rgb);
        CHECK(f.call(102, RI, RP_GUEST, { 2, 1, 5, 7, 8, 4 }) == 0 && f.ctx.running);
        CHECK(pixel_calls.size() == 1);
        if (pixel_calls.size() == 1)
        {   /* the fields of that one call, only when it was made */
            const auto &c = pixel_calls[0];
            CHECK(c.src == f.memory.data() + BUF + 1 * 64 + 2 * fm.bpp);
            CHECK(c.srcx == 0 && c.srcy == 0 && c.srcmod == 64 && c.rp == &native_rp);
            CHECK(c.destx == 5 && c.desty == 7 && c.width == 8 && c.height == 4 &&
                  c.format == fm.rect);
        }
    }
    {   /* dirty upper words */
        Fixture f;
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0xdead0002, 0xbeef0001, 0x12340005, 0x56780007,
                                    0x9abc0008, 0xdef00004 });
        CHECK(f.ctx.running && pixel_calls.size() == 1);
        CHECK(pixel_calls.size() == 1 && pixel_calls[0].destx == 5 && pixel_calls[0].width == 8 &&
              pixel_calls[0].height == 4);
    }
    for (ULONG bad : { (ULONG)RGBFB_NONE, (ULONG)RGBFB_CLUT, (ULONG)RGBFB_R5G6B5PC,
                       (ULONG)RGBFB_R5G5B5PC, (ULONG)RGBFB_R5G6B5, (ULONG)RGBFB_R5G5B5,
                       (ULONG)RGBFB_B5G6R5PC, (ULONG)RGBFB_B5G5R5PC, (ULONG)RGBFB_Y4U2V2,
                       (ULONG)RGBFB_Y4U1V1, 0x100u + RGBFB_A8R8G8B8, 0xffffffffu })
    {   /* formats that are not copied stop, even for an empty rectangle */
        for (ULONG width : { 8u, 0u })
        {
            Fixture f;
            f.render_info(BUF, 64, bad);
            f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, width, 4 });
            CHECK(f.stopped() && pixel_calls.empty());
        }
    }
    {   /* an empty rectangle reads no pixel and needs no stride */
        static const ULONG sizes[][2] = { { 0, 4 }, { 8, 0 }, { 0, 0 } };
        for (const auto &size : sizes)
        {
            Fixture f;
            f.render_info(0, -1, RGBFB_A8R8G8B8);
            CHECK(f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, size[0], size[1] }) == 0);
            CHECK(f.ctx.running && pixel_calls.empty());
        }
    }
    for (ULONG ri : { 0u, RI + 1, (ULONG)PROG_HI - 8, 0x10u })
    {   /* no RenderInfo of the program, empty or not */
        for (ULONG width : { 8u, 0u })
        {
            Fixture f;
            f.render_info(BUF, 64, RGBFB_A8R8G8B8);
            f.call(102, ri, RP_GUEST, { 0, 0, 0, 0, width, 4 });
            CHECK(f.stopped() && pixel_calls.empty());
        }
    }
    for (ULONG rp : { 0u, (ULONG)RP_NOBITMAP, (ULONG)RP_GUEST + 2 })
    {   /* not the RastPort of a window or a screen, or one without a BitMap */
        for (ULONG width : { 8u, 0u })
        {
            Fixture f;
            f.render_info(BUF, 64, RGBFB_A8R8G8B8);
            f.call(102, RI, rp, { 0, 0, 0, 0, width, 4 });
            CHECK(f.stopped() && pixel_calls.empty());
        }
    }
    for (WORD bpr : { (WORD)0, (WORD)-64, (WORD)39, (WORD)-32768 })
    {   /* rows shorter than SrcX + SizeX pixels: 2 + 8 pixels need 40 bytes */
        Fixture f;
        f.render_info(BUF, bpr, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 2, 0, 0, 0, 8, 4 });
        CHECK(f.stopped() && pixel_calls.empty());
    }
    {   /* 40 bytes are enough */
        Fixture f;
        f.render_info(BUF, 40, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 2, 0, 0, 0, 8, 4 });
        CHECK(f.ctx.running && pixel_calls.size() == 1);
    }
    {   /* the destination stays inside the WORD coordinates of a RastPort */
        Fixture f;
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0, 32760, 32764, 8, 4 });
        CHECK(f.ctx.running && pixel_calls.size() == 1);
        Fixture g;
        g.render_info(BUF, 64, RGBFB_A8R8G8B8);
        g.call(102, RI, RP_GUEST, { 0, 0, 32761, 0, 8, 4 });
        CHECK(g.stopped() && pixel_calls.empty());
        Fixture h;
        h.render_info(BUF, 64, RGBFB_A8R8G8B8);
        h.call(102, RI, RP_GUEST, { 0, 0, 0, 65535, 8, 1 });
        CHECK(h.stopped() && pixel_calls.empty());
    }
    {   /* the last source byte at the end of program memory, and one past */
        const ULONG rows = 4, bpr = 64, span = (rows - 1) * bpr + 8 * 4;
        Fixture f;
        f.render_info(PROG_HI - span, bpr, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, rows });
        CHECK(f.ctx.running && pixel_calls.size() == 1);
        Fixture g;
        g.render_info(PROG_HI - span + 1, bpr, RGBFB_A8R8G8B8);
        g.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, rows });
        CHECK(g.stopped() && pixel_calls.empty());
    }
    for (ULONG memory_at : { 0u, 0xfffffff0u, 0x80000000u, (ULONG)MEM_SIZE })
    {   /* Memory NULL, or rows past the guest memory, with 64-bit sums */
        Fixture f;
        f.render_info(memory_at, 0x7ffc, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0xffff, 0, 0, 8, 0xffff });
        CHECK(f.stopped() && pixel_calls.empty());
    }
    {   /* a source row on memory the program does not own */
        Fixture f;
        forbid_lo = BUF + 2 * 64; forbid_hi = BUF + 2 * 64 + 4;
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, 4 });
        CHECK(f.stopped() && pixel_calls.empty());
    }
}

/* A held LayerInfo or refresh scope of the layer bridge, as the dispatcher
   asks it through Kaffeine_LayerInfoLocked() */
static BOOL info_held(KaffeineContext *) { return TRUE; }
static KaffeineLayerOps locked_ops()
{
    KaffeineLayerOps ops = {};
    ops.info_locked = info_held;
    return ops;
}

static void test_write_pixels_in_layer_info_scope()
{
    static const KaffeineLayerOps held = locked_ops();
    {   /* the manual write reaches WritePixelArray() as without the scope */
        Fixture f;
        f.ctx.layer_ops = &held;
        f.render_info(BUF, 16 * 4, RGBFB_A8R8G8B8);
        CHECK(f.call(102, RI, RP_GUEST, { 2, 1, 5, 7, 8, 4 }) == 0 && f.ctx.running);
        CHECK(pixel_calls.size() == 1);
        if (pixel_calls.size() == 1)
        {   /* the fields of that one call, only when it was made */
            const auto &c = pixel_calls[0];
            CHECK(c.src == f.memory.data() + BUF + 1 * 64 + 2 * 4 && c.srcmod == 64 &&
                  c.rp == &native_rp);
            CHECK(c.destx == 5 && c.desty == 7 && c.width == 8 && c.height == 4 &&
                  c.format == RECTFMT_ARGB32);
        }
    }
    /* and keeps every refusal of its own: RenderInfo, RastPort, format,
       stride, destination and source rows */
    struct Bad { ULONG ri, rp, memory_at; WORD bpr; ULONG format, d[6]; };
    const Bad bad[] = {
        { 0, RP_GUEST, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI + 1, RP_GUEST, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI, 0, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI, RP_NOBITMAP, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI, RP_GUEST + 2, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI, RP_GUEST, BUF, 64, RGBFB_R5G6B5, { 0, 0, 0, 0, 8, 4 } },
        { RI, RP_GUEST, BUF, 39, RGBFB_A8R8G8B8, { 2, 0, 0, 0, 8, 4 } },
        { RI, RP_GUEST, BUF, 64, RGBFB_A8R8G8B8, { 0, 0, 32761, 0, 8, 4 } },
        { RI, RP_GUEST, PROG_HI - 3 * 64 - 8 * 4 + 1, 64, RGBFB_A8R8G8B8, { 0, 0, 0, 0, 8, 4 } },
        { RI, RP_GUEST, 0xfffffff0u, 0x7ffc, RGBFB_A8R8G8B8, { 0, 0xffff, 0, 0, 8, 0xffff } },
    };
    for (const auto &b : bad)
    {
        Fixture f;
        f.ctx.layer_ops = &held;
        f.render_info(b.memory_at, b.bpr, b.format);
        CHECK(f.call(102, b.ri, b.rp, { b.d[0], b.d[1], b.d[2], b.d[3], b.d[4], b.d[5] }) == 20);
        CHECK(f.stopped() && pixel_calls.empty());
    }
    {   /* a source row on memory the program does not own */
        Fixture f;
        f.ctx.layer_ops = &held;
        forbid_lo = BUF + 2 * 64; forbid_hi = BUF + 2 * 64 + 4;
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, 4 });
        CHECK(f.stopped() && pixel_calls.empty());
    }
    {   /* p96BestModeIDTagList() answers without the scope and stops in it,
           before cybergraphics or the tag list is read */
        Fixture f;
        CHECK(best(f, { TAG_DONE }) != 20 && f.ctx.running && mode_calls.size() == 1);
        Fixture g;
        g.ctx.layer_ops = &held;
        CHECK(best(g, { TAG_DONE }) == 20 && g.stopped());
        CHECK(mode_calls.empty() && program_calls == 0);
    }
    for (ULONG lvo = 30; lvo <= 192; lvo += 6)
    {   /* every other vector of the library stops in the scope */
        if (lvo == 102)
            continue;
        Fixture f;
        f.ctx.layer_ops = &held;
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        CHECK(f.call(lvo, RI, RP_GUEST, { 0, 0, 0, 0, 8, 4 }) == 20 && f.stopped());
        CHECK(mode_calls.empty() && pixel_calls.empty() && program_calls == 0);
    }
    {   /* the write thunk as a generated entry: it runs without the scope
           and stops in it, as only a manual vector 102 passes */
        static const M68KThunkEntry no_manual[] = { { 0, nullptr } };
        const M68KThunkEntry generated[] = { { 102, kaffeine_thunks_p96[12].thunk } };
        CHECK(kaffeine_thunks_p96[12].lvo == 102);
        for (bool scope : { false, true })
        {
            Fixture f;
            f.ctx.libs[0].thunks = no_manual;
            f.ctx.libs[0].num_thunks = 0;
            f.ctx.libs[0].gen_thunks = generated;
            f.ctx.libs[0].num_gen_thunks = 1;
            if (scope)
                f.ctx.layer_ops = &held;
            f.render_info(BUF, 64, RGBFB_A8R8G8B8);
            f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, 4 });
            CHECK(scope ? f.stopped() && pixel_calls.empty()
                        : f.ctx.running && pixel_calls.size() == 1);
        }
    }
    for (const char *name : { "picasso96api.library", "Picasso96API.libraryX", "Picasso96API" })
    {   /* the exact library name: vector 102 of any other library stops */
        Fixture f;
        f.ctx.layer_ops = &held;
        std::snprintf(f.ctx.libs[0].name, sizeof(f.ctx.libs[0].name), "%s", name);
        f.render_info(BUF, 64, RGBFB_A8R8G8B8);
        f.call(102, RI, RP_GUEST, { 0, 0, 0, 0, 8, 4 });
        CHECK(f.stopped() && pixel_calls.empty());
    }
}

/* ── Kaffeine_P96Open() ── */

static void test_open()
{
    {   /* versions 0 to 2 get the base, made once, with a Library of version 2 */
        Fixture f;
        f.ctx.num_libs = 0;
        ULONG base = Kaffeine_P96Open(&f.ctx, 0);
        CHECK(base == P96_BASE && setup_calls == 1);
        CHECK(f.memory[base + 8] == 9);                                /* NT_LIBRARY */
        CHECK(m68k_read16(&f.ctx, base + 16) == 6 * KAFFEINE_MAX_LVO);  /* lib_NegSize */
        CHECK(m68k_read16(&f.ctx, base + 18) == 34);                    /* lib_PosSize */
        CHECK(m68k_read16(&f.ctx, base + 20) == 2);                     /* lib_Version */
        CHECK(m68k_read16(&f.ctx, base + 22) == 0);                     /* lib_Revision */
        CHECK(Kaffeine_P96Open(&f.ctx, 1) == base && Kaffeine_P96Open(&f.ctx, 2) == base);
        CHECK(setup_calls == 1 && f.ctx.num_libs == 1);
        /* a later version is refused, before and after the first open */
        CHECK(Kaffeine_P96Open(&f.ctx, 3) == 0 && Kaffeine_P96Open(&f.ctx, 0xffff) == 0);
        CHECK(setup_calls == 1);
        /* the base is found again when the library map has filled up since */
        f.ctx.num_libmap = KAFFEINE_MAX_LIBMAP;
        CHECK(Kaffeine_P96Open(&f.ctx, 2) == base && setup_calls == 1);
    }
    {
        Fixture f;
        f.ctx.num_libs = 0;
        CHECK(Kaffeine_P96Open(&f.ctx, 3) == 0 && setup_calls == 0 && f.ctx.num_libs == 0);
    }
    {   /* a full library map: nothing is made that no lookup would find */
        Fixture f;
        f.ctx.num_libs = 3;
        f.ctx.num_libmap = KAFFEINE_MAX_LIBMAP;
        const auto before = f.memory;
        CHECK(Kaffeine_P96Open(&f.ctx, 2) == 0);
        CHECK(setup_calls == 0 && f.ctx.num_libs == 3 && f.ctx.num_libmap == KAFFEINE_MAX_LIBMAP);
        CHECK(f.memory == before);
    }
    {   /* no room for one more base, by count or in the base region */
        Fixture f;
        f.ctx.num_libs = KAFFEINE_MAX_LIBS;
        CHECK(Kaffeine_P96Open(&f.ctx, 2) == 0 && setup_calls == 0);
        Fixture g;
        g.ctx.num_libs = 0;
        fail_setup = true;
        const auto before = g.memory;
        CHECK(Kaffeine_P96Open(&g.ctx, 2) == 0 && setup_calls == 1);
        CHECK(g.ctx.num_libs == 0 && g.memory == before);
    }
}

int main()
{
    test_table();
    test_unsupported();
    test_best_mode();
    test_write_pixels();
    test_write_pixels_in_layer_info_scope();
    test_open();
    std::printf("Picasso96API.library thunks: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

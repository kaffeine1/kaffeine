/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public 68k diskfont/graphics LVOs through the production C dispatcher,
    thunks, font bookkeeping and heap. Inputs and expected BE output bytes
    are constructed independently of the production conversion helpers.
    Native collaborators record and mutate only their native input copies.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <utility/tagitem.h>
#include "kaffeine_intern.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks_diskfont.h"
#include "kaffeine_thunks_gfx.h"

static std::map<APTR, ULONG> blocks;
static unsigned allocation_calls;
static int fail_after = -1;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocation_calls;
    if (fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    void *p = std::malloc(size);
    assert(p && blocks.emplace(p, size).second);
    std::memset(p, flags & MEMF_CLEAR ? 0 : 0xA5, size);
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    assert(p && blocks.count(p) && blocks.at(p) == size);
    blocks.erase(p);
    std::free(p);
}

static void be16(UBYTE *p, unsigned v) { p[0] = UBYTE(v >> 8); p[1] = UBYTE(v); }
static void be32(UBYTE *p, ULONG v)
{
    p[0] = UBYTE(v >> 24); p[1] = UBYTE(v >> 16); p[2] = UBYTE(v >> 8); p[3] = UBYTE(v);
}
static ULONG read32(const UBYTE *p)
{
    return ULONG(p[0]) << 24 | ULONG(p[1]) << 16 | ULONG(p[2]) << 8 | p[3];
}

/* Independent monochrome fixture, including the undefined character.
   The native tables have native byte order and a guard at either end. */
struct Fonts
{
    TextFont mono = {}, system = {};
    char name[32] = "kaffeinedfprobe.font";
    char system_name[32] = "host-system.font";
    std::array<UBYTE, 18> bits = {0xC1, 0x00,0x00,0x5B,0x60,0xB6,0xA0,0xFB,0x60,
                                0xB6,0xA0,0xBB,0x60,0x00,0x00,0x00,0x00, 0xC2};
    std::array<ULONG, 6> loc = {0x11223344, 0x00000003,0x00030003,0x00060003,0x00090003,0x55667788};
    std::array<WORD, 6> space = {0x1234, 4,5,4,4,0x2345};
    std::array<WORD, 6> kern = {0x3456, 0,-1,1,0,0x4567};

    Fonts()
    {
        mono.tf_Message.mn_Node.ln_Name = name;
        mono.tf_Message.mn_Node.ln_Type = 12;
        mono.tf_Message.mn_Node.ln_Pri = -3;
        mono.tf_Message.mn_Node.ln_Succ = reinterpret_cast<Node *>(uintptr_t(0x123456781234ULL));
        mono.tf_Message.mn_Node.ln_Pred = reinterpret_cast<Node *>(uintptr_t(0x234567892345ULL));
        mono.tf_Message.mn_ReplyPort = reinterpret_cast<MsgPort *>(uintptr_t(0x3456789A3456ULL));
        mono.tf_Message.mn_Length = 0x1234;
        mono.tf_YSize = 8; mono.tf_XSize = 4; mono.tf_Baseline = 6; mono.tf_BoldSmear = 1;
        mono.tf_LoChar = 65; mono.tf_HiChar = 67; mono.tf_Modulo = 2;
        mono.tf_Flags = 0x62;
        mono.tf_CharData = bits.data() + 1; mono.tf_CharLoc = loc.data() + 1;
        mono.tf_CharSpace = space.data() + 1; mono.tf_CharKern = kern.data() + 1;
        system.tf_Message.mn_Node.ln_Name = system_name;
        system.tf_Message.mn_Node.ln_Type = 12;
        system.tf_YSize = 8; system.tf_XSize = 8; system.tf_Baseline = 6;
        system.tf_LoChar = 32; system.tf_HiChar = 126;
        system.tf_Flags = 1; system.tf_Accessors = 1;
    }
};

struct Request
{
    bool disk;
    std::string name;
    UWORD size;
    UBYTE style, flags;
    bool null_tags;
    std::vector<std::pair<ULONG, IPTR>> tags;
};
static std::vector<Request> requests;
static std::map<TextFont *, unsigned> owned_opens;
static unsigned closes, sets, poisoned;
static TextFont *open_result;
static TextFont *default_font;
static KaffeineContext *active;
static bool mutate_input;

static bool guest_pointer(const void *ptr)
{
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr), start = reinterpret_cast<uintptr_t>(active->mem);
    return p >= start && p - start < active->mem_size;
}

static TextFont *record_open(TextAttr *ta, bool disk)
{
    assert(active && ta && !guest_pointer(ta));
    assert(guest_pointer(ta->ta_Name));
    Request r{disk, ta->ta_Name, ta->ta_YSize, ta->ta_Style, ta->ta_Flags, true, {}};
    if (ta->ta_Style & 0x80)
    {
        auto *tta = reinterpret_cast<TTextAttr *>(ta);
        r.null_tags = !tta->tta_Tags;
        if (tta->tta_Tags)
        {
            assert(!guest_pointer(tta->tta_Tags));
            unsigned i = 0;
            while (tta->tta_Tags[i].ti_Tag)
            {
                assert(i < 64 && tta->tta_Tags[i].ti_Tag == 0x80000001UL);
                r.tags.emplace_back(tta->tta_Tags[i].ti_Tag, tta->tta_Tags[i].ti_Data);
                ++i;
            }
            assert(tta->tta_Tags[i].ti_Data == 0);
            if (mutate_input)
            {
                tta->tta_Tags[0].ti_Tag = 0xDEADBEEF;
                tta->tta_Tags[0].ti_Data = IPTR(0x12345678ABCDEF01ULL);
            }
        }
        if (mutate_input) tta->tta_Tags = nullptr;
    }
    requests.push_back(r);
    if (mutate_input)
    {
        ta->ta_Name = const_cast<char *>("changed by native backend");
        ta->ta_YSize = 0xFF00; ta->ta_Style = 0xFF; ta->ta_Flags = 0xAA;
    }
    if (open_result)
    {
        ++owned_opens[open_result];
        ++open_result->tf_Accessors;
    }
    return open_result;
}
extern "C" TextFont *OpenDiskFont(TextAttr *ta) { return record_open(ta, true); }
extern "C" TextFont *OpenFont(const TextAttr *ta) { return record_open(const_cast<TextAttr *>(ta), false); }
extern "C" void AskFont(RastPort *, TextAttr *) { assert(false); }
extern "C" void InitRastPort(RastPort *) { assert(false); }

extern "C" void CloseFont(TextFont *font)
{
    assert(font && owned_opens[font] && font->tf_Accessors);
    --owned_opens[font]; --font->tf_Accessors; ++closes;
}
extern "C" void SetFont(RastPort *rp, TextFont *font)
{
    ++sets;
    if (!font) font = default_font;
    assert(font && font->tf_Accessors);
    rp->Font = font; rp->TxHeight = font->tf_YSize;
    rp->TxWidth = font->tf_XSize; rp->TxBaseline = font->tf_Baseline;
}

/* No drawing is needed by this fixture. Any unexpected native call fails. */
extern "C" WORD TextLength(RastPort *, CONST_STRPTR, ULONG) { std::abort(); }
extern "C" void Text(RastPort *, CONST_STRPTR, ULONG) { std::abort(); }
extern "C" void TextExtent(RastPort *, CONST_STRPTR, ULONG, struct TextExtent *) { std::abort(); }
extern "C" ULONG TextFit(RastPort *, CONST_STRPTR, ULONG, struct TextExtent *, struct TextExtent *, LONG, ULONG, ULONG) { std::abort(); }
extern "C" void Move(RastPort *, WORD, WORD) { std::abort(); }
extern "C" void Draw(RastPort *, WORD, WORD) { std::abort(); }
extern "C" void RectFill(RastPort *, WORD, WORD, WORD, WORD) { std::abort(); }
extern "C" LONG ReadPixel(RastPort *, WORD, WORD) { std::abort(); }
extern "C" LONG WritePixel(RastPort *, WORD, WORD) { std::abort(); }
extern "C" void SetAPen(RastPort *, ULONG) { std::abort(); }
extern "C" void SetBPen(RastPort *, ULONG) { std::abort(); }
extern "C" void SetDrMd(RastPort *, ULONG) { std::abort(); }
extern "C" void SetABPenDrMd(RastPort *, ULONG, ULONG, ULONG) { std::abort(); }
extern "C" ULONG GetAPen(RastPort *) { std::abort(); }
extern "C" ULONG GetBPen(RastPort *) { std::abort(); }
extern "C" ULONG GetDrMd(RastPort *) { std::abort(); }
extern "C" ULONG GetOutlinePen(RastPort *) { std::abort(); }
extern "C" void ScrollRaster(RastPort *, WORD, WORD, WORD, WORD, WORD, WORD) { std::abort(); }
extern "C" void *shadow_lookup(KaffeineContext *ctx, ULONG addr)
{
    for (unsigned i = 0; i < ctx->num_shadows; ++i)
        if (ctx->shadow_map[i].m68k_addr == addr) return ctx->shadow_map[i].native_ptr;
    return nullptr;
}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }

struct CPUState { ULONG d[8] = {}, a[8] = {}, pc = 0x77889900; UWORD sr = 0xA71F; };
static ULONG get_d(void *s, int n) { return static_cast<CPUState *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<CPUState *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<CPUState *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<CPUState *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<CPUState *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<CPUState *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<CPUState *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<CPUState *>(s)->pc = v; }
static const KaffeineCPUOps ops = {get_d,get_a,set_d,set_a,get_sr,set_sr,get_pc,set_pc,nullptr,nullptr};
static IPTR poison(KaffeineContext *, void *) { ++poisoned; std::abort(); }
static const M68KThunkEntry graphics[] = {KAFFEINE_GRAPHICS_COMPAT_THUNKS, KAFFEINE_GRAPHICS_RASTPORT_THUNKS};
static const M68KThunkEntry disk_poison[] = {{30,poison}};
static const M68KThunkEntry gfx_poison[] = {{66,poison},{72,poison},{78,poison}};

struct Fixture
{
    static constexpr ULONG arena = 0x20000, attr = 0x5000, name = 0x5100, tags = 0x5200;
    static constexpr ULONG diskbase = 0x2000, gfxbase = 0x4000, stack = 0x7000;
    KaffeineContext *context;
    KaffeineContext &ctx;
    CPUState state;
    KaffeineCPU cpu{&ops, &state};
    ULONG initial_avail;

    Fixture() : context(static_cast<KaffeineContext *>(AllocMem(sizeof(KaffeineContext), MEMF_CLEAR))), ctx(*context)
    {
        ctx.mem = static_cast<UBYTE *>(AllocMem(arena + KAFFEINE_MEM_GUARD, 0));
        ctx.mem_size = arena; ctx.heap_start = 0x8000; ctx.heap_end = 0x18000;
        assert(Kaffeine_HeapInit(&ctx));
        initial_avail = Kaffeine_HeapAvail(&ctx, 0);
        ctx.num_libs = 2;
        ctx.libs[0].m68k_addr = diskbase; ctx.libs[0].jt_start = diskbase - 512 * 6;
        std::strcpy(ctx.libs[0].name, "diskfont.library");
        ctx.libs[0].thunks = kaffeine_thunks_diskfont_manual;
        ctx.libs[0].num_thunks = kaffeine_thunks_diskfont_manual_count;
        ctx.libs[0].gen_thunks = disk_poison; ctx.libs[0].num_gen_thunks = 1;
        ctx.libs[1].m68k_addr = gfxbase; ctx.libs[1].jt_start = gfxbase - 512 * 6;
        std::strcpy(ctx.libs[1].name, "graphics.library");
        ctx.libs[1].thunks = graphics; ctx.libs[1].num_thunks = std::size(graphics);
        ctx.libs[1].gen_thunks = gfx_poison; ctx.libs[1].num_gen_thunks = std::size(gfx_poison);
        be32(ctx.mem + stack, 0x12345678);
        put_attr();
    }
    ~Fixture()
    {
        fail_after = -1;
        active = &ctx;
        Kaffeine_FontsCleanup(&ctx);
        for (unsigned i = 0; i < KAFFEINE_MEM_GUARD; ++i) assert(ctx.mem[arena + i] == 0xA5);
        Kaffeine_DestroyContext(context);
        active = nullptr;
    }
    void put_attr(ULONG at = attr, ULONG string = name, unsigned size = 8, UBYTE style = 0, UBYTE flags = 0x40)
    {
        std::strcpy(reinterpret_cast<char *>(ctx.mem + name), "PROGDIR:kaffeinedfprobe.font");
        be32(ctx.mem + at, string); be16(ctx.mem + at + 4, size);
        ctx.mem[at + 6] = style; ctx.mem[at + 7] = flags;
    }
    void tag(ULONG at, ULONG id, ULONG value) { be32(ctx.mem + at, id); be32(ctx.mem + at + 4, value); }
    ULONG lng(ULONG at) const { return read32(ctx.mem + at); }
    std::vector<UBYTE> bytes() const { return {ctx.mem, ctx.mem + arena + KAFFEINE_MEM_GUARD}; }
    ULONG call(bool disk, ULONG lvo, ULONG a0 = 0, ULONG a1 = 0)
    {
        active = &ctx;
        for (unsigned i = 0; i < 8; ++i) { state.d[i] = 0xDD000000 + i; state.a[i] = 0xAA000000 + i; }
        state.a[0] = a0; state.a[1] = a1; state.a[7] = stack;
        CPUState saved = state;
        KaffeineDispatchResult result{};
        ctx.running = TRUE; ctx.exit_code = 0;
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, (disk ? diskbase : gfxbase) - lvo, &result));
        assert(result.return_pc == 0x12345678 && !result.supervisor);
        assert(ctx.running && !ctx.exit_code && !ctx.after_thunk);
        assert(state.a[7] == stack + 4 && state.sr == saved.sr && state.pc == saved.pc);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == saved.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == saved.a[i]);
        assert(!poisoned);
        return state.d[0];
    }
    ULONG open(bool disk = true, ULONG at = attr) { return call(disk, disk ? 30 : 72, at, attr + 0x40); }
    void close(ULONG font) { assert(call(false, 78, 0xDEADBEEF, font) == 0); }
    void invalid(bool disk, ULONG at)
    {
        auto before = bytes();
        auto count = requests.size(); auto native_allocs = allocation_calls;
        auto old_closes = closes;
        assert(!open(disk, at));
        assert(bytes() == before && requests.size() == count && closes == old_closes);
        assert(allocation_calls == native_allocs && Kaffeine_HeapAvail(&ctx, 0) == initial_avail);
    }
};

static void reset(Fonts &fonts)
{
    assert(blocks.empty());
    for (auto [font, count] : owned_opens) { (void)font; assert(count == 0); }
    requests.clear(); owned_opens.clear(); closes = sets = poisoned = 0;
    allocation_calls = 0; fail_after = -1; mutate_input = true;
    open_result = &fonts.mono; default_font = &fonts.system;
}

static void check_bitmap(Fixture &f, ULONG font, bool fixed = false, UBYTE style = 0)
{
    assert(font >= f.ctx.heap_start && font + 52 <= f.ctx.heap_end && !(font & 3));
    std::array<UBYTE, 52> expected{};
    expected[8] = 12; be32(expected.data() + 10, f.lng(font + 10));
    be16(expected.data() + 20, 8); expected[22] = style; expected[23] = fixed ? 0x42 : 0x62;
    be16(expected.data() + 24, 4); be16(expected.data() + 26, 6);
    be16(expected.data() + 28, 1); be16(expected.data() + 30, 1);
    expected[32] = 65; expected[33] = 67; be16(expected.data() + 38, 2);
    for (unsigned pos : {34U,40U,44U,48U}) be32(expected.data() + pos, f.lng(font + pos));
    assert(!std::memcmp(f.ctx.mem + font, expected.data(), expected.size()));
    const ULONG name = f.lng(font + 10), bitmap = f.lng(font + 34), loc = f.lng(font + 40);
    const ULONG space = f.lng(font + 44), kern = f.lng(font + 48);
    std::vector<std::pair<ULONG,ULONG>> spans = {{font,52},{name,21},{bitmap,16},{loc,16}};
    if (!fixed) { spans.emplace_back(space,8); spans.emplace_back(kern,8); }
    else assert(!space && !kern);
    for (auto [p, size] : spans)
    {
        assert(p >= font && p < f.ctx.heap_end && size <= f.ctx.heap_end - p);
        assert((p == name || !(p & 3)));
        assert(p != ULONG(reinterpret_cast<uintptr_t>(open_result)));
    }
    std::sort(spans.begin(), spans.end());
    for (unsigned i = 1; i < spans.size(); ++i) assert(spans[i-1].first + spans[i-1].second <= spans[i].first);
    assert(!std::memcmp(f.ctx.mem + name, "kaffeinedfprobe.font", 21));
    static constexpr UBYTE bitmap_bytes[] = {0,0,0x5B,0x60,0xB6,0xA0,0xFB,0x60,0xB6,0xA0,0xBB,0x60,0,0,0,0};
    static constexpr UBYTE loc_bytes[] = {0,0,0,3, 0,3,0,3, 0,6,0,3, 0,9,0,3};
    static constexpr UBYTE space_bytes[] = {0,4,0,5,0,4,0,4};
    static constexpr UBYTE kern_bytes[] = {0,0,0xFF,0xFF,0,1,0,0};
    assert(!std::memcmp(f.ctx.mem + bitmap, bitmap_bytes, 16));
    assert(!std::memcmp(f.ctx.mem + loc, loc_bytes, 16));
    if (!fixed)
    {
        assert(!std::memcmp(f.ctx.mem + space, space_bytes, 8));
        assert(!std::memcmp(f.ctx.mem + kern, kern_bytes, 8));
    }
    auto entry = std::find_if(std::begin(f.ctx.fonts), std::end(f.ctx.fonts), [font](auto &e){return e.guest == font;});
    assert(entry != std::end(f.ctx.fonts) && entry->allocation_size >= 52);
    for (auto [p, size] : spans) assert(p + size <= font + entry->allocation_size);
    /* All allocated payload bytes are accounted for; alignment padding is zero. */
    std::vector<UBYTE> payload(entry->allocation_size, 0);
    for (auto [p, size] : spans) std::memcpy(payload.data() + p - font, f.ctx.mem + p, size);
    assert(!std::memcmp(f.ctx.mem + font, payload.data(), payload.size()));
}

static void test_bitmap_identity()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        auto before = f.bytes();
        const ULONG font = f.open(); assert(font);
        check_bitmap(f, font);
        assert(requests.size() == 1 && requests[0].disk && requests[0].name == "PROGDIR:kaffeinedfprobe.font");
        assert(requests[0].size == 8 && !requests[0].style && requests[0].flags == 0x40 && requests[0].null_tags);
        assert(!std::memcmp(f.ctx.mem, before.data(), f.ctx.heap_start));
        assert(!std::memcmp(f.ctx.mem + f.ctx.heap_end, before.data() + f.ctx.heap_end, Fixture::arena - f.ctx.heap_end));
        assert(Kaffeine_FontNative(&f.ctx, font) == &fonts.mono);
        assert(f.open(false) == font && f.open() == font && owned_opens[&fonts.mono] == 3);
        f.close(font); f.close(font); assert(closes == 2 && Kaffeine_FontFind(&f.ctx, &fonts.mono) == font);
        f.close(font); assert(closes == 3 && !Kaffeine_FontFind(&f.ctx, &fonts.mono));
        f.close(font); f.close(0); f.close(Fixture::name); assert(closes == 3);
        assert(Kaffeine_HeapAvail(&f.ctx, 0) == f.initial_avail);
        assert(fonts.bits.front() == 0xC1 && fonts.bits.back() == 0xC2);
        assert(fonts.loc.front() == 0x11223344 && fonts.loc.back() == 0x55667788);
        assert(fonts.space.front() == 0x1234 && fonts.kern.back() == 0x4567);
    }
    assert(blocks.empty() && !owned_opens[&fonts.mono]);
}

static void test_native_extension_marker()
{
    /* Literal native/guest pairs: the native extension stays private while
       the actual style bits survive. No expected byte comes from masking. */
    for (const auto &[native_style, guest_style] :
         {std::pair<UBYTE, UBYTE>{0x80, 0x00}, {0x85, 0x05}})
    {
        Fonts fonts; reset(fonts);
        fonts.mono.tf_Style = native_style;
        const auto extension = fonts.mono.tf_Message.mn_ReplyPort;
        {
            Fixture f;
            const ULONG font = f.open(); assert(font);
            check_bitmap(f, font, false, guest_style);
            assert(f.lng(font + 14) == 0);
            assert(fonts.mono.tf_Style == native_style);
            assert(fonts.mono.tf_Message.mn_ReplyPort == extension);
            assert(f.open(false) == font);
            assert(f.ctx.mem[font + 22] == guest_style && f.lng(font + 14) == 0);
            f.close(font); f.close(font);
            assert(closes == 2 && Kaffeine_HeapAvail(&f.ctx, 0) == f.initial_avail);
        }
        assert(blocks.empty() && !owned_opens[&fonts.mono]);
    }
}

static void test_tags()
{
    Fonts fonts; reset(fonts);
    for (bool disk : {true,false})
    {
        Fixture f;
        f.put_attr(Fixture::attr, Fixture::name, 0x1234, 0x85, 0xE2);
        be32(f.ctx.mem + Fixture::attr + 8, 0);
        ULONG font = f.open(disk); assert(font);
        assert(requests.back().null_tags && requests.back().tags.empty()); f.close(font);
        be32(f.ctx.mem + Fixture::attr + 8, Fixture::tags);
        f.tag(Fixture::tags, 1, 0xFFFFFFFF);             // IGNORE
        f.tag(Fixture::tags + 8, 3, 1);                  // SKIP one invalid tag
        f.tag(Fixture::tags + 16, 0x8000F00D, 0xFFFFFFFC);
        f.tag(Fixture::tags + 24, 0x80000001, 0x1234ABCD);
        f.tag(Fixture::tags + 32, 2, Fixture::tags + 64); // MORE
        f.tag(Fixture::tags + 40, 0x8000F00D, 0xFFFFFFFF);// unreachable
        f.tag(Fixture::tags + 64, 0x80000001, 0xFFFF8000);
        f.tag(Fixture::tags + 72, 0x80000001, Fixture::name); // small scalar, not a pointer
        f.tag(Fixture::tags + 80, 0, 0xFFFFFFFF);         // DONE data is ignored
        auto before = f.bytes(); font = f.open(disk); assert(font);
        const auto &r = requests.back();
        assert(r.disk == disk && r.size == 0x1234 && r.style == 0x85 && r.flags == 0xE2 && !r.null_tags);
        assert((r.tags == std::vector<std::pair<ULONG,IPTR>>{{0x80000001,0x1234ABCD},{0x80000001,0xFFFF8000},{0x80000001,Fixture::name}}));
        assert(!std::memcmp(f.ctx.mem + Fixture::attr, before.data() + Fixture::attr, 0x400));
        f.close(font);
        f.tag(Fixture::tags, 2, 0); // NULL MORE is a terminated list
        font = f.open(disk); assert(font && requests.back().tags.empty()); f.close(font);
        f.tag(Fixture::tags, 0, 0);
        font = f.open(disk); assert(font && requests.back().tags.empty()); f.close(font);
        for (unsigned i = 0; i < 64; ++i) f.tag(Fixture::tags + i*8, 0x80000001, 0x80000000 + i);
        f.tag(Fixture::tags + 64*8, 0, 0);
        font = f.open(disk); assert(font && requests.back().tags.size() == 64); f.close(font);
        f.tag(Fixture::tags + 64*8, 0x80000001, 1); f.tag(Fixture::tags + 65*8, 0, 0);
        f.invalid(disk, Fixture::attr);
    }
    assert(blocks.empty());
}

static void test_invalid_inputs()
{
    Fonts fonts; reset(fonts);
    for (bool disk : {true,false})
    {
        Fixture f;
        for (ULONG at : {0U,Fixture::arena,Fixture::arena-7,0xFFFFFFFFU,0xFFFFFFFCU}) f.invalid(disk, at);
        for (ULONG name : {0U,Fixture::arena,0xFFFFFFFFU,Fixture::arena-1})
        {
            be32(f.ctx.mem + Fixture::attr, name); f.invalid(disk, Fixture::attr);
        }
        f.put_attr();
        std::memset(f.ctx.mem + Fixture::arena - 30, 'N', 30);
        be32(f.ctx.mem + Fixture::attr, Fixture::arena - 30); f.invalid(disk, Fixture::attr);
        f.put_attr(Fixture::arena - 8, Fixture::name, 8, 0x80); f.invalid(disk, Fixture::arena-8);
        for (ULONG list : {Fixture::arena-7,Fixture::arena,0xFFFFFFF8U,0xFFFFFFFFU})
        {
            f.put_attr(Fixture::attr, Fixture::name, 8, 0x80); be32(f.ctx.mem + Fixture::attr + 8, list);
            f.invalid(disk, Fixture::attr);
        }
        be32(f.ctx.mem + Fixture::attr + 8, Fixture::tags);
        /* Unknown tags must fail by policy even in an otherwise valid,
           terminated list, independently of malformed-list refusals. */
        f.tag(Fixture::tags + 8, 0, 0);
        for (auto [tag, data] : std::vector<std::pair<ULONG,ULONG>>{
                 {0x80000002,0},{0x8000FFFF,Fixture::name},{2,Fixture::tags},
                 {2,Fixture::arena-7},{3,0xFFFFFFFF},{3,0x20000000}})
        { f.tag(Fixture::tags,tag,data); f.invalid(disk, Fixture::attr); }
        f.tag(Fixture::tags,0x80000001,0x00480048);
        f.tag(Fixture::tags+8,2,Fixture::tags); f.invalid(disk, Fixture::attr);
        f.tag(Fixture::tags,2,Fixture::tags+16); f.tag(Fixture::tags+16,2,Fixture::tags);
        f.invalid(disk, Fixture::attr);
        be32(f.ctx.mem + Fixture::attr + 8, Fixture::arena-8);
        for (ULONG tag : {1U,3U,0x80000001U})
        { f.tag(Fixture::arena-8,tag,0); f.invalid(disk, Fixture::attr); }
        f.tag(Fixture::arena-8,0,0); // complete final TagItem is valid
        ULONG font = f.open(disk); assert(font); f.close(font);
        f.put_attr(Fixture::arena-12, Fixture::name, 0xFFFF, 0x80, 0xFF);
        be32(f.ctx.mem + Fixture::arena-4, 0); // complete final TTextAttr is valid
        font=f.open(disk,Fixture::arena-12); assert(font);
        assert(requests.back().size==0xFFFF && requests.back().flags==0xFF && requests.back().null_tags);
        f.close(font);
        /* Exact-end untagged TextAttr, and exact-end NUL-terminated name. */
        f.put_attr(Fixture::arena-8); font = f.open(disk,Fixture::arena-8); assert(font); f.close(font);
        f.put_attr(); f.ctx.mem[Fixture::arena-2]='x'; f.ctx.mem[Fixture::arena-1]=0;
        be32(f.ctx.mem + Fixture::attr,Fixture::arena-2);
        font=f.open(disk); assert(font && requests.back().name == "x"); f.close(font);
        f.ctx.mem[Fixture::arena-1]=0;
        be32(f.ctx.mem+Fixture::attr,Fixture::arena-1); // existing OpenFont accepts empty names
        font=f.open(disk); assert(font && requests.back().name.empty()); f.close(font);
    }
}

static void test_lifetimes()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        RastPort native{}, native2{}; native.Font = native2.Font = &fonts.system;
        const ULONG rp = Kaffeine_RastPortCreate(&f.ctx,&native), rp2 = Kaffeine_RastPortCreate(&f.ctx,&native2);
        assert(rp && rp2);
        const ULONG original = f.lng(rp+52), avail = Kaffeine_HeapAvail(&f.ctx,0);
        f.close(original); assert(!closes); // borrowed, pinned, no guest open
        ULONG font = f.open(); assert(font);
        assert(f.call(false,66,font,rp)==0 && f.call(false,66,font,rp2)==0);
        f.close(font); assert(!closes && owned_opens[&fonts.mono]==1 && f.lng(rp+52)==font);
        auto bitmap = f.lng(font+34); assert(f.ctx.mem[bitmap+2]==0x5B);
        f.close(font); assert(!closes); // duplicate close of a retained shadow
        assert(f.open(false)==font && closes==1 && owned_opens[&fonts.mono]==1);
        f.close(font); assert(closes==1);
        f.call(false,66,original,rp); assert(Kaffeine_FontFind(&f.ctx,&fonts.mono)==font);
        f.call(false,66,font,rp); // restore the retained font
        f.call(false,66,original,rp2); assert(Kaffeine_FontFind(&f.ctx,&fonts.mono)==font);
        f.call(false,66,0,rp); // default restores the borrowed font
        assert(closes==2 && !Kaffeine_FontFind(&f.ctx,&fonts.mono));
        assert(Kaffeine_HeapAvail(&f.ctx,0)==avail);
        font=f.open(); f.call(false,66,font,rp); f.close(font);
        Kaffeine_RastPortDelete(&f.ctx,rp); assert(!Kaffeine_FontFind(&f.ctx,&fonts.mono) && closes==3);
        Kaffeine_RastPortDelete(&f.ctx,rp2);
        /* A font initially shown by a RastPort is pinned for later restore. */
        font=f.open(); native.Font=&fonts.mono;
        const ULONG pinned_rp=Kaffeine_RastPortCreate(&f.ctx,&native); assert(pinned_rp);
        f.close(font); Kaffeine_RastPortDelete(&f.ctx,pinned_rp);
        assert(Kaffeine_FontFind(&f.ctx,&fonts.mono)==font && owned_opens[&fonts.mono]==1);
        Kaffeine_FontsCleanup(&f.ctx); assert(closes==4 && !Kaffeine_FontFind(&f.ctx,&fonts.mono));
        Kaffeine_FontsCleanup(&f.ctx); assert(closes==4 && Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
        font=f.open(); assert(f.open(false)==font); // teardown closes both unfinished opens
    }
    assert(closes==6 && !owned_opens[&fonts.mono] && blocks.empty());
    {
        Fixture a, b;
        const ULONG af=a.open(), bf=b.open(); assert(af && bf);
        a.close(af); assert(!Kaffeine_FontFind(&a.ctx,&fonts.mono) && Kaffeine_FontFind(&b.ctx,&fonts.mono)==bf);
        b.close(bf); assert(!owned_opens[&fonts.mono]);
    }
    assert(blocks.empty());
}

static void test_failures_and_forms()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        auto before=f.bytes(); const auto allocs=allocation_calls;
        fail_after=0; assert(!f.open()); fail_after=-1;
        assert(closes==1 && !owned_opens[&fonts.mono] && f.bytes()==before);
        assert(allocation_calls==allocs+1 && Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
        const ULONG all=Kaffeine_HeapAlloc(&f.ctx,f.initial_avail,0); assert(all);
        before=f.bytes(); assert(!f.open() && closes==2 && f.bytes()==before);
        Kaffeine_HeapFree(&f.ctx,all,f.initial_avail);
        open_result=nullptr; before=f.bytes(); assert(!f.open() && closes==2 && f.bytes()==before);
        open_result=&fonts.mono;
        for (unsigned form=0; form<9; ++form)
        {
            TextFont saved=fonts.mono;
            switch(form)
            {
                case 0: fonts.mono.tf_Style=0x40; break;
                case 1: fonts.mono.tf_CharData=nullptr; break;
                case 2: fonts.mono.tf_CharLoc=nullptr; break;
                case 3: fonts.mono.tf_Modulo=0; break;
                case 4: fonts.mono.tf_YSize=0; break;
                case 5: fonts.mono.tf_LoChar=68; break;
                case 6: fonts.mono.tf_Modulo=fonts.mono.tf_YSize=0xFFFF; break;
                case 7: fonts.mono.tf_Modulo=1024; fonts.mono.tf_YSize=128; break;
                case 8:
                    fonts.mono.tf_Style=0x40;
                    fonts.mono.tf_CharData=fonts.mono.tf_CharLoc=fonts.mono.tf_CharSpace=fonts.mono.tf_CharKern=nullptr;
                    break;
            }
            before=f.bytes(); const auto n=closes; assert(!f.open() && closes==n+1 && f.bytes()==before);
            assert(!Kaffeine_FontFind(&f.ctx,&fonts.mono) && !owned_opens[&fonts.mono]);
            fonts.mono=saved;
        }
        fonts.mono.tf_CharSpace=fonts.mono.tf_CharKern=nullptr; fonts.mono.tf_Flags=0x42;
        ULONG font=f.open(); assert(font); check_bitmap(f,font,true); f.close(font);
        /* Signed values retain all bits on hosts with unsigned plain char. */
        fonts.mono.tf_CharSpace=fonts.space.data()+1; fonts.mono.tf_CharKern=fonts.kern.data()+1;
        fonts.space[1]=-32768; fonts.kern[1]=-257;
        font=f.open(); assert(font);
        const ULONG space=f.lng(font+44), kern=f.lng(font+48);
        assert(f.ctx.mem[space]==0x80 && f.ctx.mem[space+1]==0 && f.ctx.mem[kern]==0xFE && f.ctx.mem[kern+1]==0xFF);
        /* The guest cannot FreeMem the shared font, its interior or tables. */
        const ULONG avail=Kaffeine_HeapAvail(&f.ctx,0);
        Kaffeine_HeapFree(&f.ctx,font,52); Kaffeine_HeapFree(&f.ctx,space,8);
        assert(Kaffeine_HeapAvail(&f.ctx,0)==avail && Kaffeine_FontNative(&f.ctx,font)==&fonts.mono);
        f.close(font); assert(Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
    }
    assert(blocks.empty());
}

static void test_geometry_and_optional_tables()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        /* 257 records exercise HiChar 255 and the undefined glyph. A
           height with both bytes set catches byte/word truncation. */
        std::array<UBYTE,1550> bits{};
        std::array<ULONG,259> loc{};
        std::array<WORD,259> space{}, kern{};
        bits.front()=0xC7; bits.back()=0xC8;
        loc.front()=0x12345678; loc.back()=0x89ABCDEF;
        space.front()=123; space.back()=124; kern.front()=125; kern.back()=126;
        for (unsigned i=0;i<1548;++i) bits[i+1]=UBYTE(i*37+11);
        for (unsigned i=0;i<257;++i)
        {
            loc[i+1]=((i%48)<<16)|1;
            space[i+1]=WORD(int(i)-129); kern[i+1]=i==256 ? -32768 : -WORD(i);
        }
        fonts.mono.tf_YSize=0x0102; fonts.mono.tf_XSize=0x0304;
        fonts.mono.tf_Baseline=0x0100; fonts.mono.tf_BoldSmear=0x0101;
        fonts.mono.tf_LoChar=0; fonts.mono.tf_HiChar=255; fonts.mono.tf_Modulo=6;
        fonts.mono.tf_CharData=bits.data()+1; fonts.mono.tf_CharLoc=loc.data()+1;
        fonts.mono.tf_CharSpace=space.data()+1; fonts.mono.tf_CharKern=kern.data()+1;
        for (unsigned optional=0;optional<3;++optional)
        {
            fonts.mono.tf_CharSpace=optional==1 ? nullptr : space.data()+1;
            fonts.mono.tf_CharKern=optional==2 ? nullptr : kern.data()+1;
            const ULONG font=f.open(); assert(font);
            const ULONG bitmap=f.lng(font+34), locations=f.lng(font+40);
            assert(!std::memcmp(f.ctx.mem+font+20,"\x01\x02",2));
            assert(!std::memcmp(f.ctx.mem+font+24,"\x03\x04\x01\x00\x01\x01",6));
            assert(f.ctx.mem[font+32]==0 && f.ctx.mem[font+33]==255);
            assert(!std::memcmp(f.ctx.mem+bitmap,bits.data()+1,1548));
            for (unsigned i=0;i<257;++i)
            {
                const UBYTE packed[]={0,UBYTE(i%48),0,1};
                assert(!std::memcmp(f.ctx.mem+locations+i*4,packed,4));
                UBYTE value[2];
                if (optional!=1)
                {
                    be16(value,unsigned(int(i)-129));
                    assert(!std::memcmp(f.ctx.mem+f.lng(font+44)+i*2,value,2));
                }
                else assert(!f.lng(font+44));
                if (optional!=2)
                {
                    be16(value,i==256 ? 0x8000 : unsigned(-int(i)));
                    assert(!std::memcmp(f.ctx.mem+f.lng(font+48)+i*2,value,2));
                }
                else assert(!f.lng(font+48));
            }
            f.close(font); assert(Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
        }
        assert(bits.front()==0xC7 && bits.back()==0xC8 && loc.front()==0x12345678 && loc.back()==0x89ABCDEF);
    }
    assert(blocks.empty());
}

static void test_rastport_guest_edits()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        RastPort native{}; native.Font=&fonts.system;
        const ULONG rp=Kaffeine_RastPortCreate(&f.ctx,&native), original=f.lng(rp+52);
        assert(rp && original);
        const ULONG avail=Kaffeine_HeapAvail(&f.ctx,0), font=f.open(); assert(font);
        f.call(false,66,font,rp); f.close(font);
        be32(f.ctx.mem+rp+52,0); // guest edits must not drop the still used native font
        f.close(font); assert(!closes && Kaffeine_FontNative(&f.ctx,font)==&fonts.mono);
        f.call(false,66,original,rp); // release uses actual references, not the overwritten pointer
        assert(closes==1 && !Kaffeine_FontFind(&f.ctx,&fonts.mono) && Kaffeine_HeapAvail(&f.ctx,0)==avail);
        /* A native default-font change can also introduce an unpinned
           borrowed snapshot. Restore must reclaim it without a native close. */
        TextFont borrowed=fonts.system;
        default_font=&borrowed;
        f.call(false,66,0,rp);
        const ULONG borrowed_guest=f.lng(rp+52);
        assert(borrowed_guest && Kaffeine_FontNative(&f.ctx,borrowed_guest)==&borrowed);
        f.call(false,66,original,rp);
        assert(!Kaffeine_FontFind(&f.ctx,&borrowed) && closes==1 && Kaffeine_HeapAvail(&f.ctx,0)==avail);
        default_font=&fonts.system;
        Kaffeine_RastPortDelete(&f.ctx,rp);
    }
    assert(blocks.empty());
}

static void test_table_exhaustion_and_cycles()
{
    Fonts fonts; reset(fonts);
    {
        Fixture f;
        std::array<TextFont,33> native{};
        std::array<ULONG,32> guest{};
        for (unsigned i=0;i<32;++i)
        {
            native[i]=fonts.mono; open_result=&native[i]; guest[i]=f.open(i%2==0); assert(guest[i]);
        }
        const auto before=f.bytes(); const unsigned n=closes;
        native[32]=fonts.mono; open_result=&native[32]; assert(!f.open() && closes==n+1 && f.bytes()==before);
        open_result=&native[7]; assert(f.open()==guest[7]); // full table still reopens an existing identity
        f.close(guest[7]);
        for (ULONG font:guest) f.close(font);
        assert(Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
        open_result=&fonts.mono;
        for (unsigned i=0;i<128;++i)
        { const ULONG font=f.open(i%2==0); assert(font); f.close(font); }
        assert(Kaffeine_HeapAvail(&f.ctx,0)==f.initial_avail);
    }
    assert(blocks.empty());
}

int main()
{
    static_assert(sizeof(void *)==8 && sizeof(IPTR)==8 && sizeof(ULONG)==4 && sizeof(WORD)==2);
    const std::pair<const char *, void(*)()> tests[] = {
        {"exact monochrome TextFont, data and shared OpenFont identity",test_bitmap_identity},
        {"native extension marker stays private and intrinsic styles survive",test_native_extension_marker},
        {"native tagged copies, scalar DPI and all tag controls",test_tags},
        {"invalid, cyclic, unsupported and exact-end inputs",test_invalid_inputs},
        {"reopens, two RastPorts, pinned fonts, contexts and cleanup",test_lifetimes},
        {"allocation rollback, color/partial refusal, fixed and signed tables",test_failures_and_forms},
        {"complete tall bitmap, 257 records and independent optional tables",test_geometry_and_optional_tables},
        {"guest RastPort pointer edits retain and release actual references",test_rastport_guest_edits},
        {"font table exhaustion and repeated open/close balance",test_table_exhaustion_and_cycles}
    };
    for (const auto &[name,test]:tests) { test(); std::printf("PASS: %s\n",name); }
    std::printf("diskfont: %zu groups passed; production thunks/dispatcher/heap, 64-bit host\n",std::size(tests));
}

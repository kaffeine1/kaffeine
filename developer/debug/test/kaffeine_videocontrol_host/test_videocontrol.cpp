/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <exec/memory.h>
#include <graphics/view.h>
#include <graphics/videocontrol.h>
#include <intuition/screens.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include "kaffeine_offsets.h"
#include "kaffeine_messages.h"
#include "kaffeine_rastport.h"
#include "kaffeine_screens.h"
#include "kaffeine_videocontrol.h"

extern "C" void video_fixture_forget(KaffeineContext *, int);
extern "C" void video_fixture_release(KaffeineContext *, Screen *);
extern "C" void video_fixture_fill(KaffeineContext *, ULONG, ULONG, Screen *);
static unsigned checks, failures;
static void check(bool okay, const char *label)
{
    ++checks;
    if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
static std::map<APTR,ULONG> allocations;
static int allocation_countdown = -1;
static unsigned allocation_calls, native_graphics_calls, native_unlocks, native_closes;
static SIPTR io_error;
static constexpr SIPTR saved_error = (SIPTR)0x12345678000000abULL;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    ++allocation_calls;
    if (!allocation_countdown) return nullptr;
    if (allocation_countdown > 0) --allocation_countdown;
    void *p = std::malloc(size ? size : 1);
    if (p) { std::memset(p, flags & MEMF_CLEAR ? 0 : 0xd9, size); allocations[p] = size; }
    io_error = 0x101;
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    auto found = allocations.find(p);
    check(found != allocations.end() && found->second == size, "native ownership has an exact single release");
    if (found != allocations.end()) allocations.erase(found);
    std::free(p); io_error = 0x102;
}
extern "C" APTR AllocVec(ULONG n, ULONG f) { return AllocMem(n, f); }
extern "C" void FreeVec(APTR p) { if (p) FreeMem(p, allocations.at(p)); }
extern "C" SIPTR IoErr(void) { return io_error; }
extern "C" SIPTR SetIoErr(SIPTR n) { SIPTR old = io_error; io_error = n; return old; }
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *, ULONG, ULONG) { return FALSE; }
extern "C" void CloseFont(TextFont *) { check(false, "fixture opened no native font to close"); }

struct Registers { std::array<ULONG,8> d{}, a{}; ULONG pc{0x1234}; UWORD sr{0x251f}; };
static ULONG get_d(void *s, int n) { return static_cast<Registers *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<Registers *>(s)->a[n]; }
static const KaffeineCPUOps ops{get_d,get_a,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
struct Guest;
static Guest *current;
struct Guest
{
    KaffeineContext *ctx;
    KaffeineCPU cpu{&ops,nullptr};
    Registers regs{};
    Screen screen{};
    ColorMap cm{};
    ViewPortExtra vpe{};
    ULONG guest, color, tags, last_vpe{};
    unsigned visitors{2};
    bool native_close_allowed{true};
    Guest(bool owned = true)
    {
        ctx = static_cast<KaffeineContext *>(AllocMem(sizeof(*ctx), MEMF_CLEAR));
        ctx->mem_size = 1024 * 1024;
        ctx->mem = static_cast<UBYTE *>(AllocMem(ctx->mem_size + KAFFEINE_MEM_GUARD, MEMF_CLEAR));
        ctx->heap_start = KAFFEINE_HEAP_BASE;
        ctx->heap_end = ctx->stack_bottom = ctx->mem_size - 65536;
        ctx->stack_top = ctx->mem_size;
        check(Kaffeine_HeapInit(ctx), "production heap initialized");
        guest = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_SCREEN + 52);
        color = guest + M68K_SIZEOF_SCREEN;
        tags = Kaffeine_HeapAlloc(ctx, 8 * (KAFFEINE_VIDEO_TAG_STEPS + 2), MEMF_CLEAR);
        screen.ViewPort.ColorMap = &cm;
        screen.ViewPort.DWidth = 640; screen.ViewPort.DHeight = 480;
        screen.ViewPort.DxOffset = 13; screen.ViewPort.DyOffset = -17;
        cm.cm_vp = &screen.ViewPort; cm.cm_vpe = &vpe; cm.Count = 256; cm.Type = 2;
        initialize(vpe);
        auto &slot = ctx->pubscreens[0];
        slot.native = &screen; slot.guest = guest; slot.colormap = color; slot.owned = owned;
        slot.locks = owned ? 0 : visitors;
        ctx->screen_release = video_fixture_release;
        check(Kaffeine_ShadowAdd(ctx, guest, &screen,
                                 owned ? KAFFEINE_SHADOW_SCREEN : KAFFEINE_SHADOW_PUBSCREEN) &&
              Kaffeine_ShadowAdd(ctx, guest + M68K_SCR_VIEWPORT, &screen.ViewPort, KAFFEINE_SHADOW_VIEWPORT),
              "real native screen and viewport identities registered");
        m68k_write32(ctx, color + 20, guest + M68K_SCR_VIEWPORT);
        m68k_write32(ctx, guest + M68K_SCR_VIEWPORT + 4, color);
        cpu.backend_state = &regs; ctx->cpu = &cpu; ctx->running = TRUE;
        for (unsigned i = 0; i < 8; ++i) { regs.d[i] = 0x11110000 + i; regs.a[i] = 0x22220000 + i; }
        query();
    }
    void initialize(ViewPortExtra &v)
    {
        v.n.xln_Type = NT_GRAPHICS; v.n.xln_Pri = -7;
        v.n.xln_Subsystem = SS_GRAPHICS; v.n.xln_Subtype = VIEWPORT_EXTRA_TYPE;
        v.n.xln_Succ = v.n.xln_Pred = reinterpret_cast<Node *>((IPTR)0x123456789000ULL);
        v.n.xln_Name = reinterpret_cast<char *>((IPTR)0x234567890000ULL);
        v.n.xln_Library = reinterpret_cast<APTR>((IPTR)0x345678900000ULL);
        v.n.xln_Init = reinterpret_cast<LONG (*)(void)>((IPTR)0x456789000000ULL);
        v.ViewPort = &screen.ViewPort;
        v.DisplayClip = {-51,-37,991,767};
        v.VecTable = v.DriverData[0] = v.DriverData[1] = reinterpret_cast<APTR>((IPTR)0x567890000000ULL);
        v.Flags = 0xa501; v.Origin[0] = {-123,234}; v.Origin[1] = {32767,-32768};
        v.cop1ptr = 0xdead1234; v.cop2ptr = 0xbeef5678;
    }
    void item(ULONG at, ULONG tag, ULONG data) { m68k_write32(ctx, at, tag); m68k_write32(ctx, at + 4, data); }
    void query(ULONG at = 0)
    {
        if (!at) at = tags;
        item(at, VTAG_VIEWPORTEXTRA_GET, 0xffffffff); item(at + 8, TAG_DONE, 0);
    }
    IPTR invoke(ULONG list = 0, ULONG colormap = 0)
    {
        current = this; regs.a[0] = colormap ? colormap : color; regs.a[1] = list ? list : tags;
        Registers before = regs; io_error = saved_error;
        IPTR result = Kaffeine_VideoControl(ctx, &cpu);
        check(regs.a == before.a && regs.d == before.d && regs.pc == before.pc && regs.sr == before.sr,
              "query preserves all guest registers, PC and CCR/X");
        check(io_error == saved_error, "query preserves full-width IoErr");
        return result;
    }
    ULONG get()
    {
        query(); check(!invoke(), "typed VPE query succeeds");
        last_vpe = m68k_read32(ctx, tags + 4); return last_vpe;
    }
    ~Guest()
    {
        allocation_countdown = -1; current = this;
        ctx->cpu = nullptr;
        Kaffeine_DestroyContext(ctx);
        current = nullptr;
    }
};

extern "C" void UnlockPubScreen(UBYTE *, Screen *screen)
{
    ++native_unlocks;
    check(current && screen == &current->screen && current->visitors,
          "only a held native screen visitor is released");
    if (current && !current->ctx->pubscreens[0].guest && current->last_vpe)
    {
        bool zero = true;
        for (unsigned i = 0; i < KAFFEINE_VPE_SIZE; ++i)
            zero &= !current->ctx->mem[current->last_vpe + i];
        check(zero && current->visitors, "VPE retires before the last native visitor disappears");
    }
    --current->visitors;
}
extern "C" BOOL CloseScreen(Screen *screen)
{
    ++native_closes;
    check(current && screen == &current->screen, "only the proven native screen is closed");
    if (!current->native_close_allowed) return FALSE;
    /* Model native Intuition freeing its Screen/ColorMap/VPE before return. */
    std::memset(screen, 0xee, sizeof(*screen));
    return TRUE;
}
extern "C" List *LockPubScreenList(void) { check(false,"fixture needs no native name lookup"); return nullptr; }
extern "C" void UnlockPubScreenList(void) { check(false,"fixture took no native list lock"); }
extern "C" APTR GfxNew(ULONG) { ++native_graphics_calls; return nullptr; }
extern "C" void GfxFree(APTR) { ++native_graphics_calls; }
extern "C" void GfxAssociate(APTR, APTR) { ++native_graphics_calls; }
extern "C" APTR GfxLookUp(APTR) { ++native_graphics_calls; return nullptr; }

static void word(std::array<UBYTE,66> &g, unsigned at, UWORD n)
{ g[at] = UBYTE(n >> 8); g[at + 1] = UBYTE(n); }
static void pointer(std::array<UBYTE,66> &g, unsigned at, ULONG n)
{ word(g,at,UWORD(n >> 16)); word(g,at+2,UWORD(n)); }

static void layout_and_identity()
{
    Guest g;
    ULONG v = g.get();
    check(v && v < g.ctx->mem_size && v != g.color && v != g.guest,
          "query returns bounded distinct guest mirror identity");
    std::array<UBYTE,66> expected{};
    expected[8] = 18; expected[9] = 0xf9; expected[14] = 2; expected[15] = 2;
    pointer(expected,24,g.guest + M68K_SCR_VIEWPORT);
    word(expected,28,UWORD(-51)); word(expected,30,UWORD(-37)); word(expected,32,991); word(expected,34,767);
    word(expected,48,0xa501); word(expected,50,UWORD(-123)); word(expected,52,234);
    word(expected,54,32767); word(expected,56,UWORD(-32768));
    check(!std::memcmp(g.ctx->mem + v, expected.data(), expected.size()),
          "all 66 classic bytes match native DisplayClip/scalars, with every raw pointer zero");
    check(m68k_read32(g.ctx,g.tags) == VTAG_VIEWPORTEXTRA_SET && m68k_read32(g.ctx,g.color + 8) == v,
          "GET becomes SET in-place and the borrowed ColorMap points to the same guest mirror");
    check(!Kaffeine_GuestBytes(g.ctx,v,66) && !Kaffeine_HeapFreeExact(g.ctx,v,66),
          "read-only mirror cannot become an ordinary output buffer or guest allocation");
    unsigned calls = allocation_calls;
    check(g.get() == v && allocation_calls == calls && g.ctx->video_extra_count == 1,
          "repeated query has stable identity and no new allocation");
    g.vpe.DisplayClip.MaxX = 1234;
    check(g.get() == v && m68k_read16(g.ctx,v + 32) == 1234,
          "native clip changes refresh the same identity after validating the previous snapshot");
    g.ctx->mem[v + 28] ^= 1; g.query();
    auto before = std::vector<UBYTE>(g.ctx->mem + g.tags,g.ctx->mem + g.tags + 16);
    check(g.invoke() == 1 && !std::memcmp(before.data(),g.ctx->mem + g.tags,16) &&
          g.ctx->running, "edited VPE snapshot refuses without output mutation or forced stop");
}

/* The real screen_fill() runs on a subsequent LockPubScreen refresh. Do
 * not issue another GET before inspecting cm_vpe: that would hide the bug. */
static void refresh_published_identity()
{
    Guest g(false);
    m68k_write32(g.ctx,g.color + 8,0x12345678);
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(!m68k_read32(g.ctx,g.color + 8), "refresh never invents an unpublished VPE from a guest pointer");
    ULONG v = g.get();
    std::array<UBYTE,66> snapshot{}; std::memcpy(snapshot.data(),g.ctx->mem + v,66);
    unsigned calls = allocation_calls;
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(m68k_read32(g.ctx,g.color + 8) == v && !std::memcmp(snapshot.data(),g.ctx->mem + v,66) &&
          allocation_calls == calls, "real Screen refresh preserves the already-published native-proven VPE identity");
    m68k_write32(g.ctx,g.color + 8,0xdeadbeef);
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(m68k_read32(g.ctx,g.color + 8) == v && allocation_calls == calls,
          "refresh uses the private published identity, never a forged guest cm_vpe value");
    g.vpe.ViewPort = nullptr;
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(!m68k_read32(g.ctx,g.color + 8), "refresh refuses a cached native VPE whose association changed");
    g.vpe.ViewPort = &g.screen.ViewPort;
    ViewPortExtra replacement{}; g.initialize(replacement); g.cm.cm_vpe = &replacement;
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(!m68k_read32(g.ctx,g.color + 8) && allocation_calls == calls,
          "refresh does not attach an old mirror to a different native VPE");
    ULONG fresh = g.get();
    check(fresh && fresh != v, "typed query alone publishes the new native VPE identity");
    g.cm.cm_vpe = nullptr;
    video_fixture_fill(g.ctx,g.guest,g.color,&g.screen);
    check(!m68k_read32(g.ctx,g.color + 8), "native NULL VPE is reflected as NULL during refresh");
    g.cm.cm_vpe = &replacement;
    video_fixture_forget(g.ctx,0);
    if (g.ctx->video_extra_guest)
        check(!g.ctx->video_extra_guest(g.ctx,&g.screen,g.color), "retired identities are never restored by the refresh hook");
}

static void tag_lists()
{
    Guest g;
    g.item(g.tags,TAG_IGNORE,0); g.item(g.tags + 8,TAG_SKIP,1);
    g.item(g.tags + 16,VTAG_VIEWPORTEXTRA_SET,0x12345678);
    g.item(g.tags + 24,TAG_MORE,g.tags + 64);
    g.item(g.tags + 64,VTAG_VIEWPORTEXTRA_GET,0xffffffff);
    g.item(g.tags + 72,VTAG_VIEWPORTEXTRA_GET,0x12345678);
    g.item(g.tags + 80,TAG_DONE,0);
    check(!g.invoke() && m68k_read32(g.ctx,g.tags + 68) == m68k_read32(g.ctx,g.tags + 76) &&
          m68k_read32(g.ctx,g.tags + 16) == VTAG_VIEWPORTEXTRA_SET,
          "IGNORE/SKIP/MORE semantics and duplicate GETs are bounded and retain skipped data");
    ULONG owned = Kaffeine_TagItemsAlloc(g.ctx,4);
    g.query(owned + 8);
    check(!g.invoke(owned + 8), "interior AllocateTagItems-owned slots are valid writable tag storage");
    Kaffeine_TagItemsFree(g.ctx,owned);
    for (unsigned mode = 0; mode < 7; ++mode)
    {
        g.query();
        if (mode == 0) g.item(g.tags + 8,0x8000ffff,0);
        if (mode == 1) g.item(g.tags + 8,VTAG_VIEWPORTEXTRA_SET,0);
        if (mode == 2) g.item(g.tags + 8,TAG_MORE,g.tags);
        if (mode == 3) g.item(g.tags + 8,TAG_SKIP,0xffffffff);
        if (mode == 4) g.item(g.tags + 8,TAG_MORE,g.ctx->mem_size - 2);
        if (mode == 5) g.item(g.tags + 8,TAG_MORE,g.tags + 2);
        if (mode == 6)
            for (unsigned i = 0; i < KAFFEINE_VIDEO_TAG_STEPS + 1; ++i)
                g.item(g.tags + i * 8,TAG_IGNORE,0);
        std::vector<UBYTE> before(g.ctx->mem,g.ctx->mem + g.ctx->mem_size);
        unsigned calls = allocation_calls;
        check(g.invoke() == 1 && !std::memcmp(before.data(),g.ctx->mem,g.ctx->mem_size) &&
              calls == allocation_calls, "unknown/SET/cyclic/overflow/overlap/overlong lists fail atomically before allocation");
    }
    g.query();
    check(g.invoke(g.color) == 1 && g.invoke(g.ctx->mem_size - 4) == 1,
          "screen snapshots and truncated cells are not writable output lists");
}

static void allocation_failures_and_provenance()
{
    for (int failure : {0,1})
    {
        Guest g;
        auto available = Kaffeine_HeapAvail(g.ctx,0); auto blocks = allocations.size();
        allocation_countdown = failure;
        check(g.invoke() == 1, "record/mirror allocation failure reports the API error");
        allocation_countdown = -1;
        check(m68k_read32(g.ctx,g.tags) == VTAG_VIEWPORTEXTRA_GET &&
              !m68k_read32(g.ctx,g.color + 8) && !g.ctx->video_extra_count &&
              Kaffeine_HeapAvail(g.ctx,0) == available && allocations.size() == blocks,
              "OOM restores guest outputs, heap, native ownership and identity capacity");
        check(g.get() != 0, "query can retry after allocation failure");
    }
    Guest g;
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        g.query();
        if (mode == 0) g.vpe.ViewPort = nullptr;
        if (mode == 1) g.vpe.n.xln_Subtype = 1;
        if (mode == 2) g.vpe.n.xln_Type = 0;
        if (mode == 3) m68k_write32(g.ctx,g.color + 20,g.guest + 2);
        if (mode == 4) m68k_write32(g.ctx,g.color + 8,0x12345678);
        check(g.invoke() == 1 && !g.ctx->video_extra_count,
              "wrong native association/header and corrupt ColorMap pointers have no provenance");
        g.initialize(g.vpe); m68k_write32(g.ctx,g.color + 20,g.guest + M68K_SCR_VIEWPORT);
        m68k_write32(g.ctx,g.color + 8,0);
    }
    g.cm.cm_vpe = nullptr;
    check(!g.invoke() && !m68k_read32(g.ctx,g.tags + 4) && !g.ctx->video_extra_count,
          "native NULL VPE remains a genuine successful NULL query without fabrication");
    g.cm.cm_vpe = &g.vpe; g.query();
    Guest other;
    m68k_write32(other.ctx,other.color + 20,other.guest + M68K_SCR_VIEWPORT);
    ULONG unregistered = other.color + 2;
    check(other.invoke(0,unregistered) == 1, "foreign/unregistered ColorMap address cannot select a native object");
    Kaffeine_ShadowForget(other.ctx,other.guest,KAFFEINE_SHADOW_SCREEN);
    check(other.invoke() == 1, "registry entry without this domain's screen identity cannot borrow native VPE");
}

static void lifetime()
{
    {
        Guest g(false);
        ULONG v = g.get();
        g.regs.a[0] = 0; g.regs.a[1] = g.guest; current = &g;
        check(!Kaffeine_UnlockPubScreen(g.ctx,&g.cpu) && g.visitors == 1 && g.get() == v,
              "nested public locks keep one VPE identity until the final holder");
        g.regs.a[0] = 0; g.regs.a[1] = g.guest;
        check(!Kaffeine_UnlockPubScreen(g.ctx,&g.cpu) && !g.visitors && !g.ctx->pubscreens[0].guest,
              "final public unlock retires both screen and VPE through real production lifecycle");
        g.query(); check(g.invoke() == 1, "retired screen ColorMap cannot query a stale VPE");
        check(!Kaffeine_HeapFreeExact(g.ctx,v,66) && !Kaffeine_HeapAllocAbs(g.ctx,66,v),
              "retired VPE mirror is quarantined against guest address reuse");
    }
    {
        Guest g;
        ULONG v = g.get(); current = &g;
        g.native_close_allowed = false; g.regs.a[0] = g.guest;
        check(!Kaffeine_CloseScreen(g.ctx,&g.cpu) && g.get() == v,
              "native close refusal preserves live mirror and screen ownership");
        g.native_close_allowed = true; g.regs.a[0] = g.guest;
        check(Kaffeine_CloseScreen(g.ctx,&g.cpu) && !g.ctx->pubscreens[0].guest,
              "successful native close retires without reading the freed native Screen/VPE");
        bool zero = true;
        for (unsigned i = 0; i < 66; ++i) zero &= !g.ctx->mem[v + i];
        check(zero, "owned close clears retired guest VPE bytes");
    }
    {
        Guest g(false);
        ULONG v = g.get(); Window window{}; window.WScreen = &g.screen;
        check(Kaffeine_ShadowAdd(g.ctx,0x2000,&window,KAFFEINE_SHADOW_WINDOW), "window holds the borrowed screen");
        for (unsigned i = 0; i < 2; ++i)
        { g.regs.a[0] = 0; g.regs.a[1] = g.guest; current = &g; Kaffeine_UnlockPubScreen(g.ctx,&g.cpu); }
        check(!g.visitors && g.get() == v, "existing window holder preserves screen/VPE after public unlock");
        Kaffeine_ShadowForget(g.ctx,0x2000,KAFFEINE_SHADOW_WINDOW);
        video_fixture_release(g.ctx,&g.screen);
        check(!g.ctx->pubscreens[0].guest, "last window-holder release retires the borrowed view");
    }
}

static void caps_and_native_refusals()
{
    Guest g;
    std::vector<std::unique_ptr<ViewPortExtra>> native_extras;
    ULONG previous = 0;
    for (unsigned i = 0; i < KAFFEINE_VPE_LIMIT; ++i)
    {
        auto next = std::make_unique<ViewPortExtra>(); g.initialize(*next);
        g.cm.cm_vpe = next.get(); native_extras.push_back(std::move(next));
        ULONG v = g.get();
        check(v && v != previous, "new native VPE gets a distinct guest identity");
        previous = v;
    }
    g.cm.cm_vpe = &g.vpe; g.query(); unsigned calls = allocation_calls;
    check(g.invoke() == 1 && calls == allocation_calls && g.ctx->video_extra_count == KAFFEINE_VPE_LIMIT &&
          m68k_read32(g.ctx,g.tags) == VTAG_VIEWPORTEXTRA_GET,
          "identity cap refuses transactionally before any native allocation");
    const M68KThunkEntry refused[] = {KAFFEINE_GRAPHICS_VIDEO_NODE_THUNKS};
    const ULONG expected[] = {660,666,672,702};
    for (unsigned i = 0; i < 4; ++i)
    {
        g.regs.a[0] = g.regs.a[1] = previous; g.regs.d[0] = VIEWPORT_EXTRA_TYPE;
        check(refused[i].lvo == expected[i] && refused[i].thunk(g.ctx,&g.cpu) == 20 &&
              !g.ctx->running && g.ctx->callback_failed && !native_graphics_calls,
              "untyped GfxNew/Free/Associate/LookUp stop before any native graphics call");
        g.ctx->running = TRUE; g.ctx->callback_failed = FALSE;
    }
    auto *mem = g.ctx->mem; g.ctx->mem = nullptr; io_error = saved_error;
    Kaffeine_VideoControlCleanup(g.ctx);
    check(!g.ctx->video_extras && !g.ctx->video_extra_count && io_error == saved_error,
          "final cleanup frees only private records, with no guest/native-object dereference");
    Kaffeine_VideoControlCleanup(g.ctx); g.ctx->mem = mem;
}

int main()
{
    static_assert(sizeof(ViewPortExtra) == 120, "fixture follows the native public 64-bit VPE layout");
    layout_and_identity(); refresh_published_identity(); tag_lists(); allocation_failures_and_provenance(); lifetime(); caps_and_native_refusals();
    check(allocations.empty() && !native_graphics_calls,
          "all native bookkeeping is balanced and no native pointer-bearing graphics API ran");
    std::printf("VideoControl production adapter: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}

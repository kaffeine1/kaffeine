/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The LockIBase() snapshot of kaffeine_intuibase.c, compiled from the
    production source, over recording collaborators: a native Intuition of
    screens, windows and menus that each case builds, a guest heap that
    counts the live mirrors, and a native IBase lock that counts its holds.

    A screen with one window and a menu strip of M menus and N items in all
    takes 3 + M + 3N views: Screen, its BitMap, the Window, every Menu, and
    for every item the MenuItem, its IntuiText and that text's string.

    Windows and Screens of the program are entries of the real shadow map
    over guest blocks of their own: the snapshot shows them as themselves,
    and the roots follow Intuition without LockIBase() through
    Kaffeine_IBaseRefresh(), with every native lock and mirror counted.
*/

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include "kaffeine_intern.h"
#include "kaffeine_intuibase.h"
#include "kaffeine_offsets.h"
#include "kaffeine_screens.h"
#include "kaffeine_shadow.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const ULONG ARENA = 8u << 20;
static const ULONG IBASE = 0x1000;        /* the guest intuition.library base */
static const ULONG HEAP = 0x10000;        /* where the mirrors start */
static const ULONG MODE = 0x00100001;
static const ULONG WINDOW_MENUSTRIP = 28; /* classic Window offset */
static const ULONG WINDOW_SIZE = 136, SCREEN_SIZE = 346;
static const ULONG OWN_A = 0x4000, OWN_SCREEN = 0x5000, OWN_PUB = 0x5400;

/* ---- native Intuition --------------------------------------------------- */

static struct IntuitionBase native_ib;
struct IntuitionBase *IntuitionBase = &native_ib;

static int native_holds;                  /* LockIBase() less UnlockIBase() */
static int lock_calls;                    /* every native LockIBase() */
static struct Window *focus_on_lock;      /* the input handler moves the focus as the lock is taken */
static const ULONG native_key = 0x1BA5E;
static int public_holds, public_drops, fronts, mode_queries;
static struct ViewPort *mode_viewport;

extern "C" ULONG LockIBase(ULONG what)
{
    (void)what;
    ++native_holds;
    ++lock_calls;
    if (focus_on_lock) { native_ib.ActiveWindow = focus_on_lock; focus_on_lock = nullptr; }
    return native_key;
}
extern "C" void UnlockIBase(ULONG key) { CHECK(key == native_key); --native_holds; }
extern "C" BOOL Kaffeine_ScreenPublicHold(struct Screen *screen) { (void)screen; ++public_holds; return TRUE; }
extern "C" void UnlockPubScreen(UBYTE *name, struct Screen *screen) { (void)name; (void)screen; ++public_drops; }
extern "C" void ScreenToFront(struct Screen *screen) { (void)screen; ++fronts; }
extern "C" ULONG GetVPModeID(struct ViewPort *vp) { ++mode_queries; mode_viewport = vp; return MODE; }

static struct Task *const exec_task = reinterpret_cast<struct Task *>(0x7000);
extern "C" struct Task *FindTask(const char *name) { (void)name; return exec_task; }

/* ---- the rest of Kaffeine ----------------------------------------------- */

static ULONG d_regs[8], a_regs[8];
extern "C" ULONG Kaffeine_GetD(void *cpu, int reg) { (void)cpu; return d_regs[reg]; }
extern "C" ULONG Kaffeine_GetA(void *cpu, int reg) { (void)cpu; return a_regs[reg]; }

static int stops;
static LONG stop_code;
extern "C" void Kaffeine_StopGuest(struct KaffeineContext *ctx, void *cpu, LONG code)
{ (void)ctx; (void)cpu; ++stops; stop_code = code; }

extern "C" ULONG Kaffeine_FindLibBase(struct KaffeineContext *ctx, const char *name)
{ (void)ctx; return std::strcmp(name, "intuition.library") ? 0 : IBASE; }

/* Nothing outside the snapshot resolves: a guest address the snapshot no
   longer holds must not reach native Intuition. */
extern "C" struct Screen *Kaffeine_ScreenNative(struct KaffeineContext *ctx, ULONG guest)
{ (void)ctx; (void)guest; return nullptr; }
extern "C" void *Kaffeine_ShadowNative(struct KaffeineContext *ctx, ULONG guest, UWORD type)
{ (void)ctx; (void)guest; (void)type; return nullptr; }

static const struct M68KStructLayout layouts[] = {
    { "Screen", 346, 0, nullptr }, { "Window", 136, 0, nullptr }, { "Layer", 160, 0, nullptr },
    { "RastPort", 100, 0, nullptr }, { "BitMap", 40, 0, nullptr }, { "Gadget", 44, 0, nullptr },
    { "IntuiText", 20, 0, nullptr }, { "StringInfo", 36, 0, nullptr }, { "PropInfo", 22, 0, nullptr },
    { "Menu", 30, 0, nullptr }, { "MenuItem", 34, 0, nullptr },
};
extern "C" const struct M68KStructLayout *shadow_find_layout(const char *name)
{
    for (const auto &layout : layouts)
        if (!std::strcmp(layout.name, name)) return &layout;
    return nullptr;
}

/* The guest heap of the mirrors: exact-size reuse of freed blocks, so that
   a bounded table also keeps the heap bounded. */
static std::map<ULONG, ULONG> mirrors;              /* live address -> size */
static std::vector<std::pair<ULONG, ULONG>> freed;  /* reusable blocks */
static ULONG heap_top;
static long mirror_budget;                          /* allocations left; -1 no limit */
static int bad_releases, allocs;                    /* allocs: every MirrorAlloc() */

extern "C" ULONG Kaffeine_MirrorAlloc(struct KaffeineContext *ctx, ULONG size)
{
    ULONG rounded = (size + 7) & ~7u, at;
    (void)ctx;
    ++allocs;
    if (!mirror_budget) return 0;
    if (mirror_budget > 0) --mirror_budget;
    for (size_t i = 0; i < freed.size(); ++i)
        if (freed[i].second == rounded)
        {
            at = freed[i].first;
            freed.erase(freed.begin() + (long)i);
            mirrors[at] = rounded;
            return at;
        }
    if (heap_top + rounded > ARENA) return 0;
    at = heap_top;
    heap_top += rounded;
    mirrors[at] = rounded;
    return at;
}

extern "C" void Kaffeine_MirrorRelease(struct KaffeineContext *ctx, ULONG addr)
{
    auto it = mirrors.find(addr);
    (void)ctx;
    if (it == mirrors.end()) { ++bad_releases; return; }
    freed.push_back({ addr, it->second });
    mirrors.erase(it);
}

/* ---- fixtures ------------------------------------------------------------ */

/* A native screen, one window on it and a menu strip of menus menus that
   share items items; never copied, its natives keep their addresses. */
struct NativeUI
{
    struct Screen screen {};
    struct Window window {};
    struct BitMap bitmap {};
    std::vector<struct Menu> menus;
    std::vector<struct MenuItem> items;
    std::vector<struct IntuiText> texts;
    std::vector<std::array<char, 16>> labels;

    NativeUI(unsigned nmenus, unsigned nitems)
        : menus(nmenus), items(nitems), texts(nitems), labels(nitems)
    {
        unsigned per = nmenus ? (nitems + nmenus - 1) / nmenus : 0;

        screen.Width = 1280;
        screen.Height = 720;
        screen.FirstWindow = &window;
        screen.RastPort.BitMap = &bitmap;
        bitmap.BytesPerRow = 1280;
        bitmap.Rows = 720;
        bitmap.Depth = 8;
        window.WScreen = &screen;
        window.MenuStrip = nmenus ? &menus[0] : nullptr;
        for (unsigned m = 0; m < nmenus; ++m)
        {
            unsigned first = m * per, end = first + per < nitems ? first + per : nitems;
            menus[m].NextMenu = m + 1 < nmenus ? &menus[m + 1] : nullptr;
            menus[m].FirstItem = first < end ? &items[first] : nullptr;
            for (unsigned k = first; k < end; ++k)
            {
                items[k].NextItem = k + 1 < end ? &items[k + 1] : nullptr;
                items[k].Flags = ITEMTEXT;
                items[k].ItemFill = &texts[k];
                std::snprintf(labels[k].data(), labels[k].size(), "item %u", k);
                texts[k].IText = reinterpret_cast<UBYTE *>(labels[k].data());
            }
        }
    }
    NativeUI(const NativeUI &) = delete;
    NativeUI &operator=(const NativeUI &) = delete;

    size_t views() const { return 3 + menus.size() + 3 * items.size(); }
    void show()
    {
        native_ib.FirstScreen = native_ib.ActiveScreen = &screen;
        native_ib.ActiveWindow = &window;
    }
};

struct Fixture
{
    std::vector<UBYTE> mem;
    KaffeineContext *ctx;

    Fixture() : mem(ARENA + KAFFEINE_MEM_GUARD, 0), ctx(new KaffeineContext())
    {
        ctx->mem = mem.data();
        ctx->mem_size = ARENA;
        ctx->exec_task = exec_task;
        ctx->guest_exec_task = 0x500;
        ctx->running = TRUE;
        mirrors.clear();
        freed.clear();
        heap_top = HEAP;
        mirror_budget = -1;
        bad_releases = allocs = 0;
        native_holds = lock_calls = public_holds = public_drops = fronts = mode_queries = 0;
        stops = 0;
        stop_code = 0;
        focus_on_lock = nullptr;
        native_ib = {};
    }
    ~Fixture()
    {
        Kaffeine_IBaseCleanup(ctx);
        delete ctx;
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    IPTR lock() { d_regs[0] = 0; return Kaffeine_LockIBase(ctx, nullptr); }
    IPTR unlock(ULONG key = native_key) { a_regs[0] = key; return Kaffeine_UnlockIBase(ctx, nullptr); }
    ULONG active_window() { return m68k_read32(ctx, IBASE + 52); }
    ULONG active_screen() { return m68k_read32(ctx, IBASE + 56); }
    ULONG first_screen() { return m68k_read32(ctx, IBASE + 60); }
    ULONG read(ULONG addr) { return m68k_read32(ctx, addr); }
    UWORD read16(ULONG addr) { return m68k_read16(ctx, addr); }
    void refresh() { Kaffeine_IBaseRefresh(ctx); }
    /* A Window or Screen of the program: a shadow-map entry over its own
       guest block, filled with a pattern that only token() may change */
    ULONG own(void *native, UWORD type, ULONG guest, ULONG size)
    {
        UWORD i = ctx->num_shadows++;
        ctx->shadow_map[i].m68k_addr = guest;
        ctx->shadow_map[i].native_ptr = native;
        ctx->shadow_map[i].type = type;
        ctx->shadow_map[i].idcmp_window = 0;
        std::memset(ctx->mem + guest, 0xA5, size);
        return guest;
    }
    void forget(ULONG guest)
    {
        for (UWORD i = 0; i < ctx->num_shadows; ++i)
            if (ctx->shadow_map[i].m68k_addr == guest)
            { ctx->shadow_map[i] = ctx->shadow_map[--ctx->num_shadows]; return; }
    }
    /* Every byte of an owned block outside the fields token() writes is as
       the program and the bridges left it. */
    bool kept(ULONG guest, const std::vector<UBYTE> &before, bool window)
    {
        for (ULONG o = 0; o < before.size(); ++o)
        {
            bool allowed = window ? (o < 16 || (o >= 108 && o < 116)) : (o < 8 || (o >= 16 && o < 20));
            if (!allowed && ctx->mem[guest + o] != before[o]) return false;
        }
        return true;
    }
    std::vector<UBYTE> bytes(ULONG guest, ULONG size)
    { return std::vector<UBYTE>(ctx->mem + guest, ctx->mem + guest + size); }
    IPTR vp_mode(ULONG guest) { a_regs[0] = guest; return Kaffeine_IBaseVPMode(ctx, nullptr); }
};

/* ---- cases ---------------------------------------------------------------- */

/* A Personal Paint sized graph, more than the old 1024 views: it fits, and a
   snapshot that reaches the same natives again keeps every guest token. */
static void fits_beyond_1024()
{
    Fixture f;
    NativeUI ui(10, 400);
    ULONG screen, menus;

    CHECK(ui.views() == 1213);
    ui.show();
    CHECK(f.lock() == native_key);
    CHECK(!stops && native_holds == 1 && Kaffeine_IBaseLocked(f.ctx));
    CHECK(mirrors.size() == ui.views());
    screen = f.first_screen();
    menus = m68k_read32(f.ctx, f.active_window() + WINDOW_MENUSTRIP);
    CHECK(screen && menus);
    CHECK(Kaffeine_IBaseBitmap(f.ctx, screen + M68K_SCR_BITMAP) == &ui.bitmap);
    CHECK(Kaffeine_IBaseRastPort(f.ctx, screen + M68K_SCR_RASTPORT) == &ui.screen.RastPort);
    CHECK(f.vp_mode(screen + M68K_SCR_VIEWPORT) == MODE && mode_viewport == &ui.screen.ViewPort);
    CHECK(f.unlock() == 0 && native_holds == 0 && !Kaffeine_IBaseLocked(f.ctx));

    CHECK(f.lock() == native_key);
    CHECK(f.first_screen() == screen);
    CHECK(m68k_read32(f.ctx, f.active_window() + WINDOW_MENUSTRIP) == menus);
    CHECK(mirrors.size() == ui.views() && !bad_releases);
    CHECK(f.unlock() == 0 && native_holds == 0 && !stops);
}

/* Exactly 2048 views fit; 2049 are refused: the native lock goes back,
   every mirror goes back and no guest token of the snapshot stays. */
static void limit_2048()
{
    {
        Fixture f;
        NativeUI ui(20, 675);
        CHECK(ui.views() == 2048);
        ui.show();
        CHECK(f.lock() == native_key && !stops && mirrors.size() == 2048);
        CHECK(f.unlock() == 0 && native_holds == 0);
    }
    {
        Fixture f;
        NativeUI small(2, 10), ui(21, 675);
        CHECK(ui.views() == 2049);
        small.show();
        CHECK(f.lock() == native_key);
        CHECK(f.unlock() == 0);
        ui.show();
        CHECK(f.lock() == 20);
        CHECK(stops == 1 && stop_code == 20);
        CHECK(native_holds == 0 && !Kaffeine_IBaseLocked(f.ctx));
        CHECK(mirrors.empty() && !bad_releases);
        CHECK(!f.active_window() && !m68k_read32(f.ctx, IBASE + 56) && !f.first_screen());
    }
}

/* The last snapshot that fitted and a disjoint new one share the table:
   2048 old views and 2048 new ones fit before the old ones go back. */
static void old_and_new_2048()
{
    Fixture f;
    NativeUI a(20, 675), b(20, 675);
    ULONG old_screen;

    a.show();
    CHECK(f.lock() == native_key && mirrors.size() == 2048);
    old_screen = f.first_screen();
    CHECK(f.unlock() == 0);
    b.show();
    CHECK(f.lock() == native_key);
    CHECK(!stops && mirrors.size() == 2048 && !bad_releases);
    CHECK(f.first_screen() != old_screen);
    CHECK(f.unlock() == 0 && native_holds == 0);
}

/* Windows and menus that come and go: the views and the heap stay bounded
   by one snapshot, not by the history of a long running program. */
static void disjoint_epochs_bounded()
{
    Fixture f;
    NativeUI x(10, 495), y(10, 495);
    ULONG top_after_two = 0;

    CHECK(x.views() == 1498);
    for (int round = 0; round < 8; ++round)
    {
        NativeUI &ui = round & 1 ? y : x;
        ui.show();
        CHECK(f.lock() == native_key);
        CHECK(mirrors.size() == ui.views());
        CHECK(f.unlock() == 0);
        if (round == 1) top_after_two = heap_top;
    }
    CHECK(!stops && !bad_releases && native_holds == 0);
    CHECK(heap_top == top_after_two);
    CHECK(freed.size() == x.views());
}

/* A screen that the snapshot no longer reaches: its guest token resolves
   to nothing, neither under IBase nor as a later depth target. */
static void removed_tokens()
{
    Fixture f;
    NativeUI a(2, 10), b(2, 10);
    struct Screen *held = nullptr;
    ULONG old_screen, screen;

    a.show();
    CHECK(f.lock() == native_key);
    old_screen = f.first_screen();
    CHECK(f.unlock() == 0);
    b.show();
    CHECK(f.lock() == native_key);
    screen = f.first_screen();
    CHECK(screen && screen != old_screen);
    CHECK(Kaffeine_IBaseBitmap(f.ctx, old_screen + M68K_SCR_BITMAP) == nullptr);
    CHECK(Kaffeine_IBaseRastPort(f.ctx, old_screen + M68K_SCR_RASTPORT) == nullptr);
    mode_queries = 0;
    CHECK(f.vp_mode(old_screen + M68K_SCR_VIEWPORT) == ~0UL && !mode_queries);
    CHECK(Kaffeine_IBaseBitmap(f.ctx, screen + M68K_SCR_BITMAP) == &b.bitmap);
    CHECK(f.unlock() == 0);

    CHECK(!Kaffeine_IBaseScreenHold(f.ctx, old_screen, &held) && !held && !public_holds);
    CHECK(Kaffeine_IBaseScreenHold(f.ctx, screen, &held) && held == &b.screen && public_holds == 1);
    Kaffeine_IBaseScreenDrop(held);
    CHECK(public_drops == 1 && !stops);
}

/* A mirror that cannot be had refuses the snapshot like a full table. */
static void mirror_failure()
{
    Fixture f;
    NativeUI ui(10, 400);

    ui.show();
    mirror_budget = 100;
    CHECK(f.lock() == 20);
    CHECK(stops == 1 && native_holds == 0 && !Kaffeine_IBaseLocked(f.ctx));
    CHECK(mirrors.empty() && !bad_releases);
}

/* The depth limit stays: one menu of 130 items nests deeper than 128. */
static void depth_limit()
{
    Fixture f;
    NativeUI ui(1, 130);

    ui.show();
    CHECK(f.lock() == 20);
    CHECK(stops == 1 && native_holds == 0 && mirrors.empty());
}

/* Nesting by the owner takes no second native lock and no new snapshot;
   another task may neither nest nor unlock; cleanup and the end of the
   owning task give the native lock back. */
static void nesting_and_owner()
{
    {
        Fixture f;
        NativeUI ui(2, 10);
        ui.show();
        CHECK(f.lock() == native_key);
        size_t live = mirrors.size();
        CHECK(f.lock() == native_key && native_holds == 1 && mirrors.size() == live);
        CHECK(f.unlock() == 0 && native_holds == 1 && Kaffeine_IBaseLocked(f.ctx));
        CHECK(f.unlock() == 0 && native_holds == 0);
        f.ctx->guest_exec_task = 0x600;
        CHECK(f.unlock() == 20 && stops == 1);
    }
    {
        Fixture f;
        NativeUI ui(2, 10);
        ui.show();
        CHECK(f.lock() == native_key);
        f.ctx->guest_exec_task = 0x600;
        CHECK(f.lock() == 20 && native_holds == 1);
        CHECK(f.unlock() == 20 && native_holds == 1);
        Kaffeine_IBaseCleanup(f.ctx);
        CHECK(native_holds == 0 && mirrors.empty() && !f.ctx->ibase && !bad_releases);
    }
    {
        Fixture f;
        NativeUI ui(2, 10);
        ui.show();
        CHECK(f.lock() == native_key);
        Kaffeine_IBaseTaskRemoved(f.ctx, 0x500);
        CHECK(native_holds == 0 && !Kaffeine_IBaseLocked(f.ctx));
    }
}


/* ---- the program's own objects and the roots between snapshots ---------- */

/* A window of the program in a foreign screen's list: the root and the
   list name its own 68k Window; NextWindow names the next view; geometry
   and mouse follow; every other byte stays; no mirror for it. */
static void owned_window_identity()
{
    Fixture f;
    NativeUI ui(2, 10);
    struct Window mine {};
    ULONG mw, next;

    mine.WScreen = &ui.screen;
    mine.LeftEdge = 10; mine.TopEdge = 20; mine.Width = 300; mine.Height = 200;
    mine.MouseY = 6; mine.MouseX = 5; mine.GZZWidth = 290; mine.GZZHeight = 180;
    mine.NextWindow = &ui.window;
    ui.screen.FirstWindow = &mine;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    auto before = f.bytes(mw, WINDOW_SIZE);
    ui.show();
    native_ib.ActiveWindow = &mine;
    CHECK(f.lock() == native_key && !stops);
    CHECK(f.active_window() == mw);
    CHECK(f.read(f.first_screen() + 4) == mw);
    next = f.read(mw);
    CHECK(next && next != mw && mirrors.count(next));
    CHECK(f.read16(mw + 4) == 10 && f.read16(mw + 6) == 20 && f.read16(mw + 8) == 300);
    CHECK(f.read16(mw + 10) == 200 && f.read16(mw + 12) == 6 && f.read16(mw + 14) == 5);
    CHECK(f.read16(mw + 112) == 290 && f.read16(mw + 114) == 180);
    CHECK(f.kept(mw, before, true));
    CHECK(mirrors.size() == ui.views() && !mirrors.count(mw));
    CHECK(f.unlock() == 0 && native_holds == 0);
}

/* The Personal Paint check: its window's WScreen, as the bridge set it,
   is the very screen at ActiveScreen and FirstScreen, unlocked and locked,
   for a screen it opened and for one it holds as a public screen. */
static void own_screen_check()
{
    for (UWORD type : { (UWORD)KAFFEINE_SHADOW_SCREEN, (UWORD)KAFFEINE_SHADOW_PUBSCREEN })
    {
        Fixture f;
        struct Screen scr {};
        struct Window win {};
        ULONG gs, gw;

        win.WScreen = &scr;
        scr.FirstWindow = &win;
        gs = f.own(&scr, type, OWN_SCREEN, SCREEN_SIZE);
        gw = f.own(&win, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        m68k_write32(f.ctx, gw + 46, gs);
        auto screen_before = f.bytes(gs, SCREEN_SIZE);
        native_ib.FirstScreen = native_ib.ActiveScreen = &scr;
        native_ib.ActiveWindow = &win;
        f.refresh();
        CHECK(f.active_window() == gw && f.read(f.active_window() + 46) == gs);
        CHECK(f.active_screen() == gs && f.first_screen() == gs);
        CHECK(!lock_calls && !allocs && f.ctx->ibase_identity == Kaffeine_IBaseIdentity);
        CHECK(f.lock() == native_key);
        CHECK(f.active_window() == gw && f.read(gw + 46) == gs && f.first_screen() == gs);
        CHECK(f.read(gs + 4) == gw && !f.read(gs) && !f.read(gw));
        CHECK(f.kept(gs, screen_before, false) && mirrors.empty());
        CHECK(f.unlock() == 0 && native_holds == 0 && !stops);
    }
}

/* Roots of other programs stay mirrors, now also between snapshots: a
   refresh takes one snapshot under the native lock. */
static void foreign_roots_mirror()
{
    Fixture f;
    NativeUI ui(2, 10);

    ui.show();
    f.refresh();
    CHECK(lock_calls == 1 && native_holds == 0 && !stops);
    CHECK(mirrors.count(f.active_window()) && mirrors.count(f.first_screen()));
    CHECK(mirrors.size() == ui.views());
    CHECK(f.read(f.active_window() + WINDOW_MENUSTRIP));
}

/* foreign -> own -> foreign in one list, and a foreign window on a screen
   of the program: links cross both ways, own fields stay. */
static void mixed_links()
{
    {
        Fixture f;
        NativeUI ui(1, 2);
        struct Window mine {}, b {};
        ULONG mw, a, vb;

        mine.WScreen = b.WScreen = &ui.screen;
        ui.window.NextWindow = &mine;
        mine.NextWindow = &b;
        mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        auto before = f.bytes(mw, WINDOW_SIZE);
        ui.show();
        native_ib.ActiveWindow = &b;
        CHECK(f.lock() == native_key);
        a = f.read(f.first_screen() + 4);
        CHECK(mirrors.count(a) && f.read(a) == mw);
        vb = f.read(mw);
        CHECK(mirrors.count(vb) && vb == f.active_window() && !f.read(vb));
        CHECK(f.kept(mw, before, true));
        CHECK(f.unlock() == 0);
    }
    {
        Fixture f;
        struct Screen scr {};
        struct Window other {}, win {};
        ULONG gs, gw, vo;

        other.WScreen = win.WScreen = &scr;
        scr.FirstWindow = &other;
        other.NextWindow = &win;
        gs = f.own(&scr, KAFFEINE_SHADOW_PUBSCREEN, OWN_PUB, SCREEN_SIZE);
        gw = f.own(&win, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        native_ib.FirstScreen = native_ib.ActiveScreen = &scr;
        native_ib.ActiveWindow = &other;
        CHECK(f.lock() == native_key);
        vo = f.read(gs + 4);
        CHECK(mirrors.count(vo) && vo == f.active_window());
        CHECK(f.read(vo) == gw && f.read(vo + 46) == gs);
        CHECK(mirrors.size() == 1);
        CHECK(f.unlock() == 0);
    }
}

/* An own window reached twice (root and list) counts once: 2047 views and
   it fit exactly; with 2048 views it is refused whole, and no own link
   names a mirror that went back. */
static void own_objects_count_once()
{
    {
        Fixture f;
        NativeUI ui(22, 674);
        struct Window mine {};
        CHECK(ui.views() == 2047);
        mine.WScreen = &ui.screen;
        mine.NextWindow = &ui.window;
        ui.screen.FirstWindow = &mine;
        f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        ui.show();
        native_ib.ActiveWindow = &mine;
        CHECK(f.lock() == native_key && !stops && mirrors.size() == 2047);
        CHECK(f.active_window() == OWN_A);
        CHECK(f.unlock() == 0);
    }
    {
        Fixture f;
        NativeUI ui(20, 675);
        struct Window mine {};
        ULONG mw;
        CHECK(ui.views() == 2048);
        mine.WScreen = &ui.screen;
        mine.NextWindow = &ui.window;
        ui.screen.FirstWindow = &mine;
        mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        auto before = f.bytes(mw, WINDOW_SIZE);
        ui.show();
        native_ib.ActiveWindow = &mine;
        CHECK(f.lock() == 20 && stops == 1 && stop_code == 20);
        CHECK(native_holds == 0 && mirrors.empty() && !bad_releases);
        CHECK(!f.active_window() && !f.active_screen() && !f.first_screen());
        CHECK(!f.read(mw) && f.kept(mw, before, true));
    }
}

/* Between snapshots: an own root needs no lock and no mirror; nothing
   changed is the fast path; a foreign root again takes one lock and reuses
   its views; the scalars always follow. */
static void refresh_counts()
{
    Fixture f;
    NativeUI ui(2, 10);
    struct Window mine {};
    ULONG mw;
    int locks, made;

    mine.WScreen = &ui.screen;
    ui.window.NextWindow = &mine;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    ui.show();
    f.refresh();
    CHECK(lock_calls == 1 && mirrors.count(f.active_window()));
    locks = lock_calls;
    made = allocs;
    f.refresh();
    CHECK(lock_calls == locks && allocs == made);
    native_ib.ActiveWindow = &mine;
    f.refresh();
    CHECK(f.active_window() == mw && lock_calls == locks && allocs == made);
    native_ib.ActiveWindow = nullptr;
    f.refresh();
    CHECK(!f.active_window() && lock_calls == locks && allocs == made);
    native_ib.ActiveWindow = &ui.window;
    f.refresh();
    CHECK(lock_calls == locks + 1 && allocs == made && mirrors.count(f.active_window()));
    native_ib.MouseX = 123;
    native_ib.MouseY = 45;
    f.refresh();
    CHECK(f.read16(IBASE + 70) == 123 && f.read16(IBASE + 68) == 45 && lock_calls == locks + 1);
    CHECK(native_holds == 0 && !stops && !bad_releases);
}

/* ActivateWindow() and then Delay() until ActiveWindow is the window, as
   Personal Paint does: the loop ends once the input handler has moved the
   focus, at the next refresh. */
static void busy_wait_ends()
{
    Fixture f;
    NativeUI ui(1, 2);
    struct Window mine {};
    ULONG mw;
    int round;

    mine.WScreen = &ui.screen;
    ui.window.NextWindow = &mine;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    ui.show();
    f.refresh();
    for (round = 0; round < 10; ++round)
    {
        if (round == 3) native_ib.ActiveWindow = &mine;
        f.refresh();
        if (f.active_window() == mw) break;
    }
    CHECK(round == 3);
}

/* While the program holds IBase it reads its own snapshot; the roots move
   after the last UnlockIBase(). */
static void held_snapshot_stays()
{
    Fixture f;
    NativeUI ui(2, 10);
    struct Window mine {};
    ULONG view, mw;

    mine.WScreen = &ui.screen;
    ui.window.NextWindow = &mine;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    ui.show();
    CHECK(f.lock() == native_key);
    view = f.active_window();
    native_ib.ActiveWindow = &mine;
    f.refresh();
    CHECK(f.active_window() == view && lock_calls == 1);
    CHECK(f.unlock() == 0);
    f.refresh();
    CHECK(f.active_window() == mw && lock_calls == 1 && native_holds == 0);
}

/* No native lock where it is not admitted: own and NULL roots still move,
   foreign ones wait, without a lock on every call, until a call where it
   is admitted. */
static void refresh_admission()
{
    for (int blocker = 0; blocker < 5; ++blocker)
    {
        Fixture f;
        NativeUI ui(2, 10);
        struct Window mine {};
        ULONG mw;

        mine.WScreen = &ui.screen;
        mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
        native_ib.FirstScreen = native_ib.ActiveScreen = &ui.screen;
        native_ib.ActiveWindow = &mine;
        switch (blocker)
        {
        case 0: f.ctx->callback_depth = 1; break;
        case 1: f.ctx->broker_call = TRUE; break;
        case 2: f.ctx->pubscreen_list_locks = 1; break;
        case 3: f.ctx->exec_task = reinterpret_cast<struct Task *>(0x7100); break;
        case 4: f.ctx->running = FALSE; break;
        }
        f.refresh();
        f.refresh();
        CHECK(f.active_window() == mw && !f.active_screen() && !f.first_screen());
        CHECK(!lock_calls && !allocs);
        f.ctx->callback_depth = 0;
        f.ctx->broker_call = FALSE;
        f.ctx->pubscreen_list_locks = 0;
        f.ctx->exec_task = exec_task;
        f.ctx->running = TRUE;
        f.refresh();
        CHECK(lock_calls == 1 && native_holds == 0);
        CHECK(f.active_window() == mw && mirrors.count(f.first_screen()) && f.active_screen() == f.first_screen());
    }
}

/* A refresh snapshot that does not fit never stops the program: foreign
   roots are 0, own ones come back, and the same natives are not tried
   again; other natives are. */
static void refresh_refused()
{
    Fixture f;
    NativeUI ui(10, 400), small(1, 2);
    struct Window mine {};
    ULONG mw;

    mine.WScreen = &ui.screen;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    native_ib.FirstScreen = native_ib.ActiveScreen = &ui.screen;
    native_ib.ActiveWindow = &mine;
    mirror_budget = 100;
    f.refresh();
    CHECK(!stops && native_holds == 0 && lock_calls == 1);
    CHECK(mirrors.empty() && !bad_releases);
    CHECK(f.active_window() == mw && !f.active_screen() && !f.first_screen());
    f.refresh();
    CHECK(lock_calls == 1);
    mirror_budget = -1;
    native_ib.FirstScreen = native_ib.ActiveScreen = &small.screen;
    f.refresh();
    CHECK(lock_calls == 2 && mirrors.count(f.first_screen()) && f.active_window() == mw);
}

/* An own Window or Screen that goes is no root and no own link before its
   memory goes; the next refresh shows what Intuition has then. */
static void teardown()
{
    Fixture f;
    NativeUI ui(1, 2);
    struct Screen scr {};
    struct Window win {};
    ULONG gs, gw;

    win.WScreen = &scr;
    scr.FirstWindow = &win;
    scr.NextScreen = &ui.screen;
    gs = f.own(&scr, KAFFEINE_SHADOW_SCREEN, OWN_SCREEN, SCREEN_SIZE);
    gw = f.own(&win, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    native_ib.FirstScreen = native_ib.ActiveScreen = &scr;
    native_ib.ActiveWindow = &win;
    CHECK(f.lock() == native_key && f.unlock() == 0);
    CHECK(f.read(gs + 4) == gw && mirrors.count(f.read(gs)));
    Kaffeine_IBaseIdentity(f.ctx, gw, TRUE);
    CHECK(!f.active_window() && f.active_screen() == gs);
    CHECK(!f.read(gs) && !f.read(gs + 4));
    f.forget(gw);
    scr.FirstWindow = nullptr;
    native_ib.ActiveWindow = &ui.window;
    f.refresh();
    CHECK(mirrors.count(f.active_window()) && f.active_screen() == gs);
    Kaffeine_IBaseIdentity(f.ctx, gs, TRUE);
    CHECK(!f.active_screen() && !f.first_screen());
    f.forget(gs);
    native_ib.FirstScreen = native_ib.ActiveScreen = &ui.screen;
    f.refresh();
    CHECK(mirrors.count(f.first_screen()) && native_holds == 0 && !stops && !bad_releases);
}

/* Under IBase an own screen's BitMap and RastPort are not answered from
   the snapshot, so the owned rules answer them; a foreign view still is. */
static void own_lookups()
{
    Fixture f;
    NativeUI ui(1, 2);
    struct Screen scr {};
    ULONG gs, view;

    scr.NextScreen = &ui.screen;
    gs = f.own(&scr, KAFFEINE_SHADOW_SCREEN, OWN_SCREEN, SCREEN_SIZE);
    native_ib.FirstScreen = native_ib.ActiveScreen = &scr;
    native_ib.ActiveWindow = &ui.window;
    CHECK(f.lock() == native_key);
    view = f.read(gs);
    CHECK(f.first_screen() == gs && mirrors.count(view));
    CHECK(Kaffeine_IBaseBitmap(f.ctx, gs + M68K_SCR_BITMAP) == nullptr);
    CHECK(Kaffeine_IBaseRastPort(f.ctx, gs + M68K_SCR_RASTPORT) == nullptr);
    mode_queries = 0;
    CHECK(f.vp_mode(gs + M68K_SCR_VIEWPORT) == ~0UL && !mode_queries);
    CHECK(Kaffeine_IBaseBitmap(f.ctx, view + M68K_SCR_BITMAP) == &ui.bitmap);
    CHECK(f.unlock() == 0);
}

/* The same native screen, first of another program, then held by the
   program (LockPubScreen()): the roots become its own Screen although no
   native root pointer changed, the window root's WScreen with them; once
   the program lets it go, views again. */
static void same_native_acquired()
{
    Fixture f;
    NativeUI ui(2, 10);
    ULONG gp, view;

    ui.show();
    f.refresh();
    view = f.first_screen();
    CHECK(mirrors.count(view) && lock_calls == 1);
    gp = f.own(&ui.screen, KAFFEINE_SHADOW_PUBSCREEN, OWN_PUB, SCREEN_SIZE);
    Kaffeine_IBaseIdentity(f.ctx, gp, FALSE);
    f.refresh();
    CHECK(f.first_screen() == gp && f.active_screen() == gp);
    CHECK(lock_calls == 2 && f.read(f.active_window() + 46) == gp);
    CHECK(f.read(gp + 4) == f.active_window());
    Kaffeine_IBaseIdentity(f.ctx, gp, TRUE);
    f.forget(gp);
    CHECK(!f.first_screen() && !f.active_screen());
    f.refresh();
    CHECK(mirrors.count(f.first_screen()) && f.first_screen() != gp && lock_calls == 3);
    CHECK(native_holds == 0 && !stops && !bad_releases);
}


/* The focus moves from the program's window to another while the refresh
   takes the lock, and that snapshot does not fit: the fallback reads the
   roots again after the unlock and does not put back the window that was
   active before the lock. */
static void refresh_lock_handoff()
{
    Fixture f;
    NativeUI ui(2, 10);
    struct Window mine {};
    ULONG mw;

    mine.WScreen = &ui.screen;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    native_ib.FirstScreen = native_ib.ActiveScreen = &ui.screen;
    native_ib.ActiveWindow = &mine;
    focus_on_lock = &ui.window;
    mirror_budget = 0;
    f.refresh();
    CHECK(lock_calls == 1 && native_holds == 0 && !stops);
    CHECK(f.active_window() != mw && !f.active_window());
    CHECK(!f.active_screen() && !f.first_screen() && mirrors.empty());
}

/* The same roots after a snapshot that did not fit: tried again after 64
   calls, then 128 more, without a lock in between; once mirrors come back
   the next try fits. */
static void refresh_recovers()
{
    Fixture f;
    NativeUI ui(2, 10);
    struct Window mine {};
    ULONG mw;
    int calls;

    mine.WScreen = &ui.screen;
    mw = f.own(&mine, KAFFEINE_SHADOW_WINDOW, OWN_A, WINDOW_SIZE);
    native_ib.FirstScreen = native_ib.ActiveScreen = &ui.screen;
    native_ib.ActiveWindow = &mine;
    mirror_budget = 0;
    f.refresh();
    CHECK(lock_calls == 1 && f.active_window() == mw && !f.first_screen());
    for (calls = 0; lock_calls == 1 && calls < 1000; ++calls) f.refresh();
    CHECK(calls == 64 && lock_calls == 2);
    for (calls = 0; lock_calls == 2 && calls < 1000; ++calls) f.refresh();
    CHECK(calls == 128 && lock_calls == 3);
    mirror_budget = -1;
    for (calls = 0; lock_calls == 3 && calls < 1000; ++calls) f.refresh();
    CHECK(calls == 256 && lock_calls == 4);
    CHECK(mirrors.count(f.first_screen()) && f.active_window() == mw);
    f.refresh();
    CHECK(lock_calls == 4 && native_holds == 0 && !stops && !bad_releases);
}

int main()
{
    fits_beyond_1024();
    limit_2048();
    old_and_new_2048();
    disjoint_epochs_bounded();
    removed_tokens();
    mirror_failure();
    depth_limit();
    nesting_and_owner();
    owned_window_identity();
    own_screen_check();
    foreign_roots_mirror();
    mixed_links();
    own_objects_count_once();
    refresh_counts();
    busy_wait_ends();
    held_snapshot_stays();
    refresh_admission();
    refresh_refused();
    refresh_lock_handoff();
    refresh_recovers();
    teardown();
    own_lookups();
    same_native_acquired();
    std::printf("ibase snapshot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

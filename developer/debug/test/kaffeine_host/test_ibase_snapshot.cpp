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

/* ---- native Intuition --------------------------------------------------- */

static struct IntuitionBase native_ib;
struct IntuitionBase *IntuitionBase = &native_ib;

static int native_holds;                  /* LockIBase() less UnlockIBase() */
static const ULONG native_key = 0x1BA5E;
static int public_holds, public_drops, fronts, mode_queries;
static struct ViewPort *mode_viewport;

extern "C" ULONG LockIBase(ULONG what) { (void)what; ++native_holds; return native_key; }
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
static int bad_releases;

extern "C" ULONG Kaffeine_MirrorAlloc(struct KaffeineContext *ctx, ULONG size)
{
    ULONG rounded = (size + 7) & ~7u, at;
    (void)ctx;
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
        mirrors.clear();
        freed.clear();
        heap_top = HEAP;
        mirror_budget = -1;
        bad_releases = 0;
        native_holds = public_holds = public_drops = fronts = mode_queries = 0;
        stops = 0;
        stop_code = 0;
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
    ULONG first_screen() { return m68k_read32(ctx, IBASE + 60); }
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
    std::printf("ibase snapshot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Regions of 68k code and the clip Regions of private, window and
    requester layers through literal LVOs, with the real dispatcher,
    callback primitive, native hook entry, layer bridge, Region bridge and
    heap: NewRegion() (-516),
    OrRectRegion() (-510), DisposeRegion() (-534) of graphics.library and
    InstallClipRegion() (-174) of layers.library. Every other Region vector
    of graphics.conf stops the program before anything native, and every
    generated thunk is poisoned.

    The native graphics below keeps real Regions: a list of absolute
    rectangles, published as AROS keeps them, Bounds over all of them and
    RegionRectangles relative to MinX and MinY, linked Next and Prev. It
    can fail each allocation and operation, or hand out a cyclic list. The
    native layers make, delete and backfill private layers, and install
    clip Regions, as hyperlayers does, recording every call; a native
    Region is never disposed of while a native layer clips with it, and a
    native layer is never deleted with a clip Region. Window and requester
    layers are Intuition's, opened and closed through ctx->layer_ops as the
    window and requester bridges do; once Intuition has deleted one, no
    layer call may reach it.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_layers.h"
#include "kaffeine_regions.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_callback.h"
#include "kaffeine_cpu.h"

/* ── Native graphics Regions ─────────────────────────────────────── */

static KaffeineContext *live;
static std::vector<std::string> events;

struct NativeRegion
{
    Region region;
    std::vector<Rectangle> rects;          /* absolute, as the program added them */
    std::vector<RegionRectangle> nodes;    /* what graphics publishes */
};
static std::map<const Region *, NativeRegion *> live_regions;
static std::vector<const Region *> made_regions;
/* successes left for NewRegion(), -1 without a limit */
static int new_region_budget;
static bool or_rect_fails, or_region_fails, cyclic;
static Rectangle last_or_rect;
static unsigned or_rect_calls, or_region_calls, region_disposals;

static NativeRegion *native_region(const Region *r)
{
    auto it = live_regions.find(r);
    assert(it != live_regions.end());
    return it->second;
}

static void publish(NativeRegion *n)
{
    const size_t count = n->rects.size();

    n->nodes.assign(count, RegionRectangle());
    if (!count)
    {
        n->region.bounds = { 0, 0, 0, 0 };
        n->region.RegionRectangle = nullptr;
        return;
    }
    Rectangle b = n->rects[0];
    for (const Rectangle &r : n->rects)
    {
        b.MinX = std::min(b.MinX, r.MinX);
        b.MinY = std::min(b.MinY, r.MinY);
        b.MaxX = std::max(b.MaxX, r.MaxX);
        b.MaxY = std::max(b.MaxY, r.MaxY);
    }
    n->region.bounds = b;
    for (size_t i = 0; i < count; ++i)
    {
        const Rectangle &r = n->rects[i];
        n->nodes[i].bounds = { (WORD)(r.MinX - b.MinX), (WORD)(r.MinY - b.MinY),
                               (WORD)(r.MaxX - b.MinX), (WORD)(r.MaxY - b.MinY) };
        n->nodes[i].Next = i + 1 < count ? &n->nodes[i + 1] : nullptr;
        n->nodes[i].Prev = i ? &n->nodes[i - 1] : nullptr;
    }
    n->region.RegionRectangle = &n->nodes[0];
    if (cyclic)
        n->nodes.back().Next = &n->nodes[0];
}

static void add(NativeRegion *n, const Rectangle &r)
{
    if (r.MinX > r.MaxX || r.MinY > r.MaxY)
        return;
    for (const Rectangle &o : n->rects)
        if (o.MinX <= r.MinX && o.MinY <= r.MinY && o.MaxX >= r.MaxX && o.MaxY >= r.MaxY)
            return;
    n->rects.push_back(r);
}

/* Native layers whose clip Region it is */
static bool clipping(const Region *r);

extern "C" struct Region *NewRegion(void)
{
    events.push_back("NewRegion");
    if (new_region_budget == 0)
        return nullptr;
    if (new_region_budget > 0)
        --new_region_budget;
    auto *n = new NativeRegion();
    publish(n);
    live_regions[&n->region] = n;
    made_regions.push_back(&n->region);
    return &n->region;
}

extern "C" void DisposeRegion(struct Region *r)
{
    events.push_back("DisposeRegion");
    NativeRegion *n = native_region(r);
    assert(!clipping(r));
    live_regions.erase(r);
    ++region_disposals;
    delete n;
}

extern "C" BOOL OrRectRegion(struct Region *r, struct Rectangle *rect)
{
    events.push_back("OrRectRegion");
    NativeRegion *n = native_region(r);
    /* a native Rectangle: never guest memory */
    const UBYTE *p = reinterpret_cast<const UBYTE *>(rect);
    assert(p < live->mem || p >= live->mem + live->mem_size);
    ++or_rect_calls;
    last_or_rect = *rect;
    if (or_rect_fails)
        return FALSE;
    add(n, *rect);
    publish(n);
    return TRUE;
}

extern "C" BOOL OrRegionRegion(struct Region *r1, struct Region *r2)
{
    events.push_back("OrRegionRegion");
    NativeRegion *a = native_region(r1), *b = native_region(r2);
    assert(a != b);
    ++or_region_calls;
    if (or_region_fails)
        return FALSE;
    for (const Rectangle &r : a->rects)
        add(b, r);
    publish(b);
    return TRUE;
}

/* The native Regions that a call made and kept, in order */
static std::vector<const Region *> kept_since(size_t made)
{
    std::vector<const Region *> v;
    for (size_t i = made; i < made_regions.size(); ++i)
        if (live_regions.count(made_regions[i]))
            v.push_back(made_regions[i]);
    return v;
}

/* ── Native layers ───────────────────────────────────────────────── */

static std::map<const Layer *, int> layer_locks;
static int held_locks;
static std::vector<Layer_Info *> native_infos;

struct NativeLayer
{
    Layer layer;
    RastPort rp;
};
static std::vector<NativeLayer *> native_layers;
/* Layers of windows and requesters that Intuition made and has not yet
   deleted, and those it has deleted: no layer call may reach those. */
static std::set<const Layer *> window_layers, dead_layers;
static unsigned deletes, clip_calls;
/* the caller's own locks of the layer when InstallClipRegion() came */
static int clip_held;
static bool delete_fails;
/* InstallClipRegion() backfills the layer, as a smart layer may */
static bool clip_backfill;

static bool clipping(const Region *r)
{
    for (NativeLayer *n : native_layers)
        if (n->layer.ClipRegion == r)
            return true;
    for (const Layer *l : window_layers)
        if (l->ClipRegion == r)
            return true;
    return false;
}

static NativeLayer *native_of(const Layer *l)
{
    for (NativeLayer *n : native_layers)
        if (&n->layer == l)
            return n;
    return nullptr;
}

static int executor_id;
static Task *const executor = reinterpret_cast<Task *>(&executor_id);
extern "C" struct Task *FindTask(const char *name)
{
    assert(!name);
    return executor;
}

extern "C" void LockLayer(LONG dummy, struct Layer *l)
{
    assert(dummy == 0 && l && !dead_layers.count(l));
    ++layer_locks[l];
    ++held_locks;
}
extern "C" void UnlockLayer(struct Layer *l)
{
    assert(l && !dead_layers.count(l) && layer_locks[l] > 0);
    --layer_locks[l];
    --held_locks;
}
extern "C" BOOL AttemptLockLayerRom(struct Layer *l)
{
    assert(!dead_layers.count(l));
    ++layer_locks[l];
    ++held_locks;
    return TRUE;
}
extern "C" struct Hook *InstallLayerHook(struct Layer *l, struct Hook *h)
{
    assert(l && !dead_layers.count(l) && layer_locks[l] > 0);
    Hook *old = l->BackFill;
    l->BackFill = h;
    return old;
}
extern "C" void LockLayerInfo(struct Layer_Info *) {}
extern "C" void UnlockLayerInfo(struct Layer_Info *) {}
extern "C" ULONG AttemptSemaphore(struct SignalSemaphore *) { return 1; }
extern "C" void ReleaseSemaphore(struct SignalSemaphore *) {}

extern "C" struct Layer_Info *NewLayerInfo(void)
{
    Layer_Info *li = new Layer_Info();
    li->Flags = NEWLAYERINFO_CALLED;
    native_infos.push_back(li);
    return li;
}

extern "C" void DisposeLayerInfo(struct Layer_Info *li)
{
    auto it = std::find(native_infos.begin(), native_infos.end(), li);
    assert(it != native_infos.end() && !li->top_layer);
    events.push_back("DisposeLayerInfo");
    native_infos.erase(it);
    delete li;
}

extern "C" void InitBitMap(struct BitMap *bm, BYTE depth, UWORD width, UWORD height)
{
    bm->Depth = (UBYTE)depth;
    bm->Rows = height;
    bm->Flags = BMF_STANDARD;
    bm->pad = 0;
    bm->BytesPerRow = (UWORD)(((width + 15) >> 3) & ~0x1);
}

static void backfill(Layer *l)
{
    Hook *h = l->BackFill;

    if (h == LAYERS_BACKFILL || h == LAYERS_NOBACKFILL)
        return;
    BackFillMessage m = { l, l->bounds, l->bounds.MinX, l->bounds.MinY };
    reinterpret_cast<IPTR (*)(Hook *, APTR, IPTR)>(h->h_Entry)(h, l->rp, reinterpret_cast<IPTR>(&m));
}

extern "C" struct Layer *CreateUpfrontHookLayer(struct Layer_Info *li, struct BitMap *bm, LONG x0,
                                                LONG y0, LONG x1, LONG y1, LONG flags,
                                                struct Hook *hook, struct BitMap *bm2)
{
    assert(std::find(native_infos.begin(), native_infos.end(), li) != native_infos.end() && !bm2);
    auto *n = new NativeLayer();
    Layer *l = &n->layer;
    l->bounds = { (WORD)x0, (WORD)y0, (WORD)x1, (WORD)y1 };
    l->Width = (WORD)(x1 - x0 + 1);
    l->Height = (WORD)(y1 - y0 + 1);
    l->Flags = (UWORD)flags;
    l->rp = &n->rp;
    l->BackFill = hook;
    l->LayerInfo = li;
    n->rp.Layer = l;
    n->rp.BitMap = bm;
    native_layers.push_back(n);
    l->back = li->top_layer;
    if (l->back)
        l->back->front = l;
    li->top_layer = l;
    backfill(l);
    return l;
}

extern "C" LONG DeleteLayer(LONG dummy, struct Layer *l)
{
    NativeLayer *n = native_of(l);

    assert(dummy == 0 && n && layer_locks[l] == 0);
    ++deletes;
    events.push_back("DeleteLayer");
    /* the bridge took its clip Region out first */
    assert(!l->ClipRegion);
    if (delete_fails)
        return FALSE;
    for (Layer *b = l->back; b; b = b->back)
        backfill(b);
    if (l->front)
        l->front->back = l->back;
    else
        l->LayerInfo->top_layer = l->back;
    if (l->back)
        l->back->front = l->front;
    native_layers.erase(std::find(native_layers.begin(), native_layers.end(), n));
    delete n;
    return TRUE;
}

/* As layers.library does: under the layer's own lock, nested on any lock
   that the caller holds */
extern "C" struct Region *InstallClipRegion(struct Layer *l, struct Region *r)
{
    assert((native_of(l) || window_layers.count(l)) && !dead_layers.count(l));
    assert(!r || live_regions.count(r));
    ++clip_calls;
    events.push_back(r ? "InstallClipRegion+" : "InstallClipRegion0");
    clip_held = layer_locks[l];
    LockLayer(0, l);
    Region *old = l->ClipRegion;
    l->ClipRegion = r;
    if (clip_backfill)
        backfill(l);
    UnlockLayer(l);
    return old;
}

/* Intuition deletes the layer of a window or requester: CloseWindow() or
   EndRequest(). No clip Region and no lock of the bridge may be left. */
static void intuition_deletes(Layer *l, const char *what)
{
    assert(window_layers.count(l) && !l->ClipRegion && layer_locks[l] == 0);
    events.push_back(what);
    window_layers.erase(l);
    dead_layers.insert(l);
}

/* A hook that must never run: a backfill hook of a window layer that may
   be a guest address */
static IPTR trap_entry(Hook *, APTR, IPTR)
{
    assert(false);
    return 0;
}

/* Intuition and the rest of the window bridge: the window test opens its
   window through the window ops alone. */
extern "C" ULONG LockIBase(ULONG) { assert(false); return 0; }
extern "C" void UnlockIBase(ULONG) { assert(false); }
extern "C" void WindowToBack(struct Window *) { assert(false); }
extern "C" void WindowToFront(struct Window *) { assert(false); }
extern "C" struct Window *OpenWindowTagList(struct NewWindow *, struct TagItem *) { assert(false); return nullptr; }
extern "C" void CloseWindow(struct Window *) { assert(false); }
extern "C" struct Message *GetMsg(struct MsgPort *) { assert(false); return nullptr; }
extern "C" void ReplyMsg(struct Message *) { assert(false); }
extern "C" struct Message *WaitPort(struct MsgPort *) { assert(false); return nullptr; }
extern "C" void PutMsg(struct MsgPort *, struct Message *) { assert(false); }
extern "C" void RawPutChar(UBYTE) { assert(false); }
extern "C" BOOL ModifyIDCMP(struct Window *, ULONG) { assert(false); return FALSE; }
extern "C" struct IntuiMessage *GT_FilterIMsg(struct IntuiMessage *) { assert(false); return nullptr; }
extern "C" struct IntuiMessage *GT_PostFilterIMsg(struct IntuiMessage *) { assert(false); return nullptr; }
extern "C" struct Window *BuildEasyRequestArgs(struct Window *, struct EasyStruct *, ULONG, RAWARG)
{
    assert(false);
    return nullptr;
}
extern "C" LONG SysReqHandler(struct Window *, ULONG *, BOOL) { assert(false); return 0; }
extern "C" void FreeSysRequest(struct Window *) { assert(false); }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { assert(false); return 0; }
extern "C" BOOL Kaffeine_AppPortFill(KaffeineContext *, ULONG, BOOL) { return FALSE; }
extern "C" BOOL Kaffeine_AppPortMatches(KaffeineContext *, ULONG, Task *, UBYTE) { return TRUE; }
extern "C" BOOL Kaffeine_AppReplyMsg(KaffeineContext *, ULONG) { return FALSE; }
extern "C" void CloseFont(struct TextFont *) { assert(false); }

/* ── The guest side ──────────────────────────────────────────────── */

static constexpr ULONG HOOK_ENTRY = 0x0900;   /* the 68k code of every hook */
static constexpr ULONG NESTED_RETURN = 0x0A00;

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
    int id = 0;
    KaffeineCPU *self = nullptr;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }

struct HookCall
{
    int cpu;
    ULONG msg_layer;
    ULONG nested;    /* D0 of a library call it made */
};
static std::vector<HookCall> hook_calls;
enum GuestAction
{
    GUEST_NONE,
    GUEST_FAULT,        /* a CPU exception */
    GUEST_LVO           /* calls guest_lvo of the library at guest_base */
};
static GuestAction action;
static ULONG guest_base, guest_lvo;
static std::vector<ULONG> guest_lvo_a, guest_lvo_d;

static ULONG guest_call(TestCPU *c, ULONG base, ULONG lvo, const std::vector<ULONG> &a,
                        const std::vector<ULONG> &d)
{
    KaffeineDispatchResult r = {};
    size_t i;

    for (i = 0; i < a.size(); ++i) c->a[i] = a[i];
    for (i = 0; i < d.size(); ++i) c->d[i] = d[i];
    c->a[7] -= 4;
    m68k_write32(live, c->a[7], NESTED_RETURN);
    assert(Kaffeine_DispatchLibraryCall(live, c->self, base - lvo, &r));
    return c->d[0];
}

static void execute(void *s)
{
    TestCPU *c = static_cast<TestCPU *>(s);
    HookCall h = {};

    assert(c->pc == HOOK_ENTRY && live->callback_depth > 0);
    h.cpu = c->id;
    h.msg_layer = m68k_read32(live, c->a[1] + M68K_BFM_LAYER);
    switch (action)
    {
    case GUEST_NONE:
        break;
    case GUEST_FAULT:
        Kaffeine_StopGuest(live, c->self, 20);
        hook_calls.push_back(h);
        return;
    case GUEST_LVO:
        h.nested = guest_call(c, guest_base, guest_lvo, guest_lvo_a, guest_lvo_d);
        break;
    }
    hook_calls.push_back(h);
    if (!live->running)
        return;
    c->d[0] = 0x600D;
    c->pc = m68k_read32(live, c->a[7]);
    c->a[7] += 4;
}

static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr };

static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }

static const M68KThunkEntry graphics_manual[] =
    { KAFFEINE_GRAPHICS_LAYER_THUNKS, KAFFEINE_GRAPHICS_REGION_THUNKS };

/* Every vector of graphics.conf that takes a Region, written out here and
   not taken from the bridge's table */
static const ULONG region_lvos[] = { 504, 510, 516, 522, 528, 534, 558, 612, 618, 624,
                                     1086, 1092, 1098, 1104, 1110, 1116, 1122, 1128,
                                     1134, 1140, 1146, 1152, 1158 };

static std::vector<M68KThunkEntry> poisoned(const M68KThunkEntry *table, size_t count)
{
    std::vector<M68KThunkEntry> v;
    for (size_t i = 0; i < count; ++i)
        if (table[i].lvo)
            v.push_back({ table[i].lvo, poison });
    return v;
}

struct Fixture
{
    static constexpr ULONG exec_base = 0x1000, layers_base = 0x3000, graphics_base = 0x3C00;
    static constexpr ULONG stack = 0x3F00, stack2 = 0x3E00, task = 0x3800;
    static constexpr ULONG heap_start = 0x4000, heap_end = 0x1F000;
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory = std::vector<UBYTE>(0x20000, 0xA5);
    TestCPU state, state2;
    KaffeineCPU cpu = { &ops, &state }, cpu2 = { &ops, &state2 };
    std::vector<M68KThunkEntry> layers_gen, graphics_gen;

    Fixture()
    {
        events.clear();
        layer_locks.clear();
        window_layers.clear();
        dead_layers.clear();
        held_locks = clip_held = 0;
        deletes = clip_calls = or_rect_calls = or_region_calls = region_disposals = 0;
        assert(native_infos.empty() && native_layers.empty() && live_regions.empty());
        made_regions.clear();
        hook_calls.clear();
        new_region_budget = -1;
        or_rect_fails = or_region_fails = cyclic = delete_fails = clip_backfill = false;
        action = GUEST_NONE;
        state.id = 1;
        state.self = &cpu;
        state2.id = 2;
        state2.self = &cpu2;
        ctx.mem = memory.data();
        ctx.mem_size = (ULONG)memory.size();
        ctx.heap_start = heap_start;
        ctx.heap_end = heap_end;
        assert(Kaffeine_HeapInit(&ctx));
        live = &ctx;
        ctx.m68k_sysbase = exec_base;
        ctx.exec_task = executor;
        m68k_write32(&ctx, exec_base + M68K_EXECBASE_THISTASK, task);
        layers_gen = poisoned(kaffeine_thunks_layers_manual, kaffeine_thunks_layers_manual_count);
        /* a poisoned generated thunk behind every Region vector */
        graphics_gen = poisoned(graphics_manual, std::size(graphics_manual));
        for (ULONG lvo : region_lvos)
            assert(std::count_if(graphics_gen.begin(), graphics_gen.end(),
                                 [&](const M68KThunkEntry &e) { return e.lvo == lvo; }) == 1);
        lib(0, layers_base, "layers.library", kaffeine_thunks_layers_manual,
            kaffeine_thunks_layers_manual_count, layers_gen);
        lib(1, graphics_base, "graphics.library", graphics_manual, std::size(graphics_manual),
            graphics_gen);
        ctx.num_libs = 2;
        assert(Kaffeine_LayersSetup(&ctx));
    }

    ~Fixture()
    {
        Kaffeine_LayersCleanup(&ctx);
        /* windows and requesters still open keep no clip Region of the
           bridge, before Intuition ever deletes their layers */
        for (const Layer *l : window_layers)
            assert(!l->ClipRegion && layer_locks[l] == 0);
        Kaffeine_RegionsCleanup(&ctx);
        assert(!ctx.layers && !ctx.regions && held_locks == 0);
        assert(native_layers.empty() && native_infos.empty() && live_regions.empty());
        window_layers.clear();
        live = nullptr;
    }

    void lib(int i, ULONG base, const char *name, const M68KThunkEntry *manual, size_t nmanual,
             const std::vector<M68KThunkEntry> &generated)
    {
        auto &l = ctx.libs[i];
        l.m68k_addr = base;
        l.jt_start = base - 0xC00;
        std::strcpy(l.name, name);
        l.thunks = manual;
        l.num_thunks = (ULONG)nmanual;
        l.gen_thunks = generated.data();
        l.num_gen_thunks = (ULONG)generated.size();
    }

    /* A literal LVO through the dispatcher on one CPU; no generated thunk
       is reached. With preserved, only D0 may change. */
    ULONG call_on(KaffeineCPU &which, ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
                  std::initializer_list<ULONG> d = {}, bool preserved = true)
    {
        TestCPU &s = *static_cast<TestCPU *>(which.backend_state);
        const ULONG sp = &which == &cpu ? stack : stack2;
        unsigned i;

        for (i = 0; i < 8; ++i)
        {
            s.d[i] = 0xD0000000 + i;
            s.a[i] = 0xA0000000 + i;
        }
        i = 0;
        for (ULONG v : a) s.a[i++] = v;
        i = 0;
        for (ULONG v : d) s.d[i++] = v;
        s.a[7] = sp;
        s.sr = 0xA715;
        s.pc = 0x12345678;
        m68k_write32(&ctx, sp, 0x76543210);
        const TestCPU before = s;
        ctx.running = TRUE;
        ctx.exit_code = 0;
        ctx.callback_failed = FALSE;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &which, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor && generated_calls == 0);
        assert(ctx.callback_depth == 0);
        if (preserved)
        {
            assert(s.a[7] == sp + 4);
            for (i = 1; i < 8; ++i) assert(s.d[i] == before.d[i]);
            for (i = 0; i < 7; ++i) assert(s.a[i] == before.a[i]);
            assert(s.sr == before.sr && s.pc == before.pc);
        }
        return s.d[0];
    }
    ULONG call(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
               std::initializer_list<ULONG> d = {})
    {
        return call_on(cpu, base, lvo, a, d);
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    ULONG new_region() { return call(graphics_base, 516); }
    ULONG or_rect(ULONG region, ULONG rect) { return call(graphics_base, 510, { region, rect }); }
    ULONG dispose(ULONG region) { return call(graphics_base, 534, { region }); }
    ULONG clip(ULONG layer, ULONG region) { return call(layers_base, 174, { layer, region }); }
    ULONG new_info() { return call(layers_base, 144); }
    ULONG create(ULONG li, ULONG bm, LONG x0, LONG y0, LONG x1, LONG y1, ULONG hook = 0)
    {
        return call(layers_base, 186, { li, bm, 0, hook },
                    { (ULONG)x0, (ULONG)y0, (ULONG)x1, (ULONG)y1, LAYERSIMPLE });
    }
    ULONG destroy(ULONG layer) { return call(layers_base, 90, { 0, layer }); }

    /* A classic Rectangle of the program */
    ULONG rect(WORD x0, WORD y0, WORD x1, WORD y1)
    {
        const ULONG r = Kaffeine_HeapAlloc(&ctx, M68K_SIZEOF_RECTANGLE, MEMF_CLEAR);
        assert(r);
        m68k_write16(&ctx, r + 0, (UWORD)x0);
        m68k_write16(&ctx, r + 2, (UWORD)y0);
        m68k_write16(&ctx, r + 4, (UWORD)x1);
        m68k_write16(&ctx, r + 6, (UWORD)y1);
        return r;
    }
    ULONG bitmap(UWORD bpr, UWORD rows)
    {
        const ULONG bm = Kaffeine_HeapAlloc(&ctx, M68K_SIZEOF_BITMAP, MEMF_CLEAR);
        const ULONG plane = Kaffeine_HeapAlloc(&ctx, (ULONG)bpr * rows, MEMF_CLEAR);
        assert(bm && plane);
        m68k_write16(&ctx, bm + M68K_BM_BYTESPERROW, bpr);
        m68k_write16(&ctx, bm + M68K_BM_ROWS, rows);
        memory[bm + M68K_BM_DEPTH] = 1;
        m68k_write32(&ctx, bm + M68K_BM_PLANES, plane);
        return bm;
    }
    ULONG hook()
    {
        const ULONG h = Kaffeine_HeapAlloc(&ctx, M68K_SIZEOF_HOOK, MEMF_CLEAR);
        assert(h);
        m68k_write32(&ctx, h + 8, HOOK_ENTRY);
        return h;
    }
    /* Fills the heap so that no allocation of any size succeeds */
    std::vector<std::pair<ULONG, ULONG>> fill_heap()
    {
        std::vector<std::pair<ULONG, ULONG>> v;
        for (ULONG size : { 256u, 16u, 8u, 4u, 1u })
            for (ULONG b; (b = Kaffeine_HeapAlloc(&ctx, size, 0)) != 0;)
                v.push_back({ b, size });
        return v;
    }
    void unfill(const std::vector<std::pair<ULONG, ULONG>> &v)
    {
        for (const auto &b : v)
            Kaffeine_HeapFree(&ctx, b.first, b.second);
    }

    ULONG L(ULONG a) { return m68k_read32(&ctx, a); }
    UWORD W(ULONG a) { return m68k_read16(&ctx, a); }
    ULONG avail() { return Kaffeine_HeapAvail(&ctx, 0); }
    std::vector<UBYTE> heap() const
    {
        return std::vector<UBYTE>(memory.begin() + heap_start, memory.begin() + heap_end);
    }
    bool heap_is(const std::vector<UBYTE> &before) const
    {
        return std::equal(before.begin(), before.end(), memory.begin() + heap_start);
    }
    bool zero(ULONG a, ULONG size) const
    {
        for (ULONG i = 0; i < size; ++i)
            if (memory[a + i])
                return false;
        return true;
    }
    bool shadowed(ULONG a) const
    {
        for (UWORD i = 0; i < ctx.num_shadows; ++i)
            if (ctx.shadow_map[i].m68k_addr == a)
                return true;
        return false;
    }
    /* No 32-bit word of [a, a + size) is (the low half of) a native pointer */
    bool no_native(ULONG a, ULONG size)
    {
        std::vector<const void *> natives;
        for (const auto &r : live_regions)
        {
            natives.push_back(r.first);
            for (const RegionRectangle &n : r.second->nodes)
                natives.push_back(&n);
        }
        for (NativeLayer *n : native_layers)
        {
            natives.push_back(&n->layer);
            natives.push_back(&n->rp);
        }
        for (ULONG off = 0; off + 4 <= size; off += 2)
            for (const void *p : natives)
                if (L(a + off) == (ULONG)reinterpret_cast<uintptr_t>(p))
                    return false;
        return true;
    }

    /* The 68k Region at g is the snapshot of native Region n: the very
       Bounds, and a guest-linked list of the relative rectangles */
    void same(ULONG g, const Region *n)
    {
        assert((WORD)W(g + 0) == n->bounds.MinX && (WORD)W(g + 2) == n->bounds.MinY);
        assert((WORD)W(g + 4) == n->bounds.MaxX && (WORD)W(g + 6) == n->bounds.MaxY);
        ULONG rr = L(g + M68K_REGION_RECTANGLE), prev = 0;
        const RegionRectangle *nr = n->RegionRectangle;
        unsigned count = 0;
        for (; nr; nr = nr->Next, ++count)
        {
            assert(rr && rr >= heap_start && rr + M68K_SIZEOF_RR <= heap_end && !(rr & 1));
            assert(L(rr + M68K_RR_PREV) == prev);
            assert((WORD)W(rr + 8) == nr->bounds.MinX && (WORD)W(rr + 10) == nr->bounds.MinY);
            assert((WORD)W(rr + 12) == nr->bounds.MaxX && (WORD)W(rr + 14) == nr->bounds.MaxY);
            assert(!shadowed(rr));
            prev = rr;
            rr = L(rr + M68K_RR_NEXT);
            assert(count < KAFFEINE_REGION_MAX_RECTS);
        }
        assert(!rr);
        assert(!shadowed(g) && no_native(g, M68K_SIZEOF_REGION));
        if (count)
            assert(no_native(L(g + M68K_REGION_RECTANGLE), count * M68K_SIZEOF_RR));
    }
};

static size_t event_at(const char *name, size_t from = 0)
{
    for (size_t i = from; i < events.size(); ++i)
        if (events[i] == name)
            return i;
    return events.size();
}

static size_t event_count(const char *name)
{
    return (size_t)std::count(events.begin(), events.end(), std::string(name));
}

static Layer *native_layer(Fixture &f, ULONG guest_layer)
{
    RastPort *rp = static_cast<RastPort *>(
        Kaffeine_ShadowNative(&f.ctx, f.L(guest_layer + M68K_LAYER_RP), KAFFEINE_SHADOW_RASTPORT));
    return rp ? rp->Layer : nullptr;
}

/* A screen with its bar, a window of the program on it, the window of
   another program, and a requester layer that Intuition makes on the
   window, as the window and requester bridges hand them to the layer
   bridge through ctx->layer_ops. Declared before the Fixture, so that it
   outlives the end of the bridges. */
struct Desk
{
    Screen scr;
    Layer bar, foreign;
    NativeLayer wl, req;
    Window w;
    ULONG mw = 0, wlayer = 0, rlayer = 0;
    bool requested = false;

    Desk()
    {
        std::memset(static_cast<void *>(&scr), 0, sizeof(scr));
        std::memset(static_cast<void *>(&bar), 0, sizeof(bar));
        std::memset(static_cast<void *>(&foreign), 0, sizeof(foreign));
        std::memset(static_cast<void *>(&wl), 0, sizeof(wl));
        std::memset(static_cast<void *>(&req), 0, sizeof(req));
        std::memset(static_cast<void *>(&w), 0, sizeof(w));
        bar.Flags = LAYERSIMPLE | LAYERBACKDROP;
        bar.LayerInfo = &scr.LayerInfo;
        scr.BarLayer = &bar;
        foreign.Flags = LAYERSIMPLE;
        foreign.LayerInfo = &scr.LayerInfo;
        for (NativeLayer *n : { &wl, &req })
        {
            n->layer.rp = &n->rp;
            n->layer.LayerInfo = &scr.LayerInfo;
            n->layer.Window = &w;
            n->rp.Layer = &n->layer;
        }
        wl.layer.Flags = LAYERSMART;
        req.layer.Flags = LAYERSIMPLE;
        w.WLayer = &wl.layer;
        w.WScreen = &scr;
        w.RPort = &wl.rp;
        order({ &bar, &wl.layer, &foreign });
    }

    /* the native layer list of the screen, front to back */
    void order(std::initializer_list<Layer *> list)
    {
        Layer *prev = nullptr;
        scr.LayerInfo.top_layer = nullptr;
        for (Layer *l : list)
        {
            l->front = prev;
            l->back = nullptr;
            if (prev)
                prev->back = l;
            else
                scr.LayerInfo.top_layer = l;
            prev = l;
        }
    }

    /* OpenWindow(): the 68k Window and its 68k Layer */
    ULONG open(Fixture &f)
    {
        mw = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_WINDOW, MEMF_CLEAR);
        assert(mw);
        window_layers.insert(&wl.layer);
        assert(f.ctx.layer_ops->window_opened(&f.ctx, mw, &w));
        wlayer = f.L(mw + M68K_WIN_WLAYER);
        assert(wlayer);
        return wlayer;
    }

    /* Request(): Intuition makes the requester layer in front of the window */
    ULONG request(Fixture &f)
    {
        /* a new layer, maybe where an earlier one was */
        std::memset(static_cast<void *>(&req.layer), 0, sizeof(req.layer));
        req.layer.Flags = LAYERSIMPLE;
        req.layer.rp = &req.rp;
        req.layer.LayerInfo = &scr.LayerInfo;
        req.layer.Window = &w;
        order({ &bar, &req.layer, &wl.layer, &foreign });
        dead_layers.erase(&req.layer);
        window_layers.insert(&req.layer);
        rlayer = f.ctx.layer_ops->request_opened(&f.ctx, &w, &req.layer);
        assert(rlayer && rlayer != wlayer);
        requested = true;
        return rlayer;
    }

    /* EndRequest(): the bridge first, then Intuition deletes the layer */
    void end_request(Fixture &f)
    {
        f.ctx.layer_ops->request_closing(&f.ctx, &req.layer);
        intuition_deletes(&req.layer, "EndRequest");
        order({ &bar, &wl.layer, &foreign });
        requested = false;
    }

    /* CloseWindow(), in the order of the window bridge: the layer bridge
       lets go of the window, its requesters end, the window closes */
    void close(Fixture &f)
    {
        f.ctx.layer_ops->window_closing(&f.ctx, mw, &w);
        if (requested)
            end_request(f);
        intuition_deletes(&wl.layer, "CloseWindow");
        order({ &bar, &foreign });
        Kaffeine_HeapFree(&f.ctx, mw, M68K_SIZEOF_WINDOW);
        mw = 0;
    }
};

/* The native Region that OrRectRegion() of a Rectangle has just made */
static const Region *region_with(Fixture &f, ULONG region, WORD x0, WORD y0, WORD x1, WORD y1)
{
    const size_t made = made_regions.size();
    assert(f.or_rect(region, f.rect(x0, y0, x1, y1)) == TRUE && f.ctx.running);
    return kept_since(made).back();
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_registration_and_guards()
{
    Fixture f;
    const ULONG region = f.new_region(), r = f.rect(0, 0, 3, 3);
    assert(region && f.or_rect(region, r) == TRUE);

    /* the three bridged vectors, and every other Region vector refused */
    for (ULONG lvo : region_lvos)
    {
        const M68KThunkEntry *e = nullptr;
        for (const M68KThunkEntry &m : graphics_manual)
            if (m.lvo == lvo)
                e = &m;
        assert(e);
        if (lvo == 510) assert(e->thunk == Kaffeine_OrRectRegion);
        else if (lvo == 516) assert(e->thunk == Kaffeine_NewRegion);
        else if (lvo == 534) assert(e->thunk == Kaffeine_DisposeRegion);
        else assert(e->thunk == kaffeine_regions_Unsupported);
    }
    for (ULONG i = 0; i < kaffeine_thunks_layers_manual_count; ++i)
        if (kaffeine_thunks_layers_manual[i].lvo == 174)
            assert(kaffeine_thunks_layers_manual[i].thunk == Kaffeine_InstallClipRegion);

    /* the refused ones stop before anything native, with a live Region of
       the program and a Rectangle in every register */
    const Region *native = kept_since(0).front();
    const auto heap = f.heap();
    const size_t before = events.size();
    for (ULONG lvo : region_lvos)
    {
        if (lvo == 510 || lvo == 516 || lvo == 534)
            continue;
        assert(f.call(Fixture::graphics_base, lvo, { region, region, r, r }, { 1, 1, 1, 1 }) == 20 &&
               f.stopped());
        assert(events.size() == before && f.heap_is(heap));
    }
    assert(live_regions.size() == 1 && live_regions.count(native));
    f.same(region, native);
}

static void test_identity_and_snapshots()
{
    Fixture f;
    const ULONG base = f.avail();
    const UWORD shadows = f.ctx.num_shadows;
    const ULONG a = f.new_region(), b = f.new_region();

    /* an empty classic Region of the bridge, never in the shadow map */
    assert(a && b && a != b && f.ctx.running && events == std::vector<std::string>({ "NewRegion", "NewRegion" }));
    assert(a >= Fixture::heap_start && a < Fixture::heap_end && f.zero(a, M68K_SIZEOF_REGION));
    assert(f.ctx.num_shadows == shadows && !f.shadowed(a) && m68k_to_host_or_shadow(&f.ctx, a) == f.ctx.mem + a);
    const Region *na = made_regions[0];
    f.same(a, na);

    /* the first Rectangle, signed: the native Rectangle has its exact
       WORDs, and the Region its Bounds, big-endian */
    const ULONG r1 = f.rect(-5, -3, 4, 6);
    size_t made = made_regions.size();
    assert(f.or_rect(a, r1) == TRUE && f.ctx.running);
    assert(last_or_rect.MinX == -5 && last_or_rect.MinY == -3 && last_or_rect.MaxX == 4 && last_or_rect.MaxY == 6);
    std::vector<const Region *> now = kept_since(made);
    assert(now.size() == 1 && !live_regions.count(na));
    na = now[0];
    assert(f.memory[a + 0] == 0xFF && f.memory[a + 1] == 0xFB && f.memory[a + 2] == 0xFF && f.memory[a + 3] == 0xFD);
    assert(f.memory[a + 4] == 0x00 && f.memory[a + 5] == 0x04 && f.memory[a + 6] == 0x00 && f.memory[a + 7] == 0x06);
    const ULONG list1 = f.L(a + M68K_REGION_RECTANGLE);
    assert(list1 && !f.L(list1 + M68K_RR_NEXT) && !f.L(list1 + M68K_RR_PREV));
    assert(f.W(list1 + 8) == 0 && f.W(list1 + 10) == 0 && f.W(list1 + 12) == 9 && f.W(list1 + 14) == 9);
    f.same(a, na);
    /* the native copy took the old Region, then the Rectangle */
    assert(or_region_calls == 1 && or_rect_calls == 1);

    /* a second one, apart: Bounds over both, two RegionRectangles linked
       by guest addresses only, relative to the new MinX and MinY */
    made = made_regions.size();
    assert(f.or_rect(a, f.rect(20, 10, 29, 12)) == TRUE);
    now = kept_since(made);
    assert(now.size() == 1 && !live_regions.count(na));
    na = now[0];
    assert((WORD)f.W(a) == -5 && (WORD)f.W(a + 2) == -3 && f.W(a + 4) == 29 && f.W(a + 6) == 12);
    const ULONG list2 = f.L(a + M68K_REGION_RECTANGLE);
    const ULONG second = f.L(list2 + M68K_RR_NEXT);
    assert(second == list2 + M68K_SIZEOF_RR && f.L(second + M68K_RR_PREV) == list2 && !f.L(second + M68K_RR_NEXT));
    assert(f.W(second + 8) == 25 && f.W(second + 10) == 13 && f.W(second + 12) == 34 && f.W(second + 14) == 15);
    f.same(a, na);

    /* nothing added: the same native Region is still published */
    for (ULONG rect : { f.rect(5, 5, 4, 5), f.rect(0, 1, 0, 0) })
    {
        const size_t calls = events.size();
        const auto heap = f.heap();
        assert(f.or_rect(a, rect) == TRUE && f.ctx.running && events.size() == calls && f.heap_is(heap));
    }
    made = made_regions.size();
    assert(f.or_rect(a, f.rect(0, 0, 1, 1)) == TRUE && kept_since(made).size() == 1);
    na = kept_since(made)[0];
    f.same(a, na);

    /* b is untouched all along, and both stay the same guest objects */
    f.same(b, made_regions[1]);
    assert(live_regions.size() == 2);

    /* DisposeRegion(): the native Region goes, the 68k one is cleared */
    const ULONG list = f.L(a + M68K_REGION_RECTANGLE);
    assert(f.dispose(a) == 0 && f.ctx.running && !live_regions.count(na));
    assert(f.zero(a, M68K_SIZEOF_REGION) && f.zero(list, 2 * M68K_SIZEOF_RR));
    /* stale: refused before anything native */
    const size_t calls = events.size();
    assert(f.dispose(a) == 20 && f.stopped());
    assert(f.or_rect(a, f.rect(0, 0, 1, 1)) == 20 && f.stopped() && events.size() == calls);
    assert(f.dispose(b) == 0 && live_regions.empty());
    /* only the Rectangles of the test are left in the heap */
    assert(f.avail() < base && f.ctx.num_shadows == shadows);
}

static void test_new_region_failures()
{
    Fixture f;
    const ULONG base = f.avail();

    /* no native Region: 0, and the guest one went */
    new_region_budget = 0;
    assert(f.new_region() == 0 && f.ctx.running && f.avail() == base && live_regions.empty());
    assert(event_count("NewRegion") == 1);
    new_region_budget = -1;
    std::vector<UBYTE> heap;

    /* no guest memory: 0 before anything native */
    {
        const auto fillers = f.fill_heap();
        heap = f.heap();
        const size_t calls = events.size();
        assert(f.new_region() == 0 && f.ctx.running && events.size() == calls && f.heap_is(heap));
        f.unfill(fillers);
    }
    assert(f.avail() == base);

    /* a full table: 0 before anything native */
    std::vector<ULONG> regions;
    for (int i = 0; i < KAFFEINE_REGION_MAX; ++i)
    {
        const ULONG r = f.new_region();
        assert(r && std::find(regions.begin(), regions.end(), r) == regions.end());
        regions.push_back(r);
    }
    heap = f.heap();
    const size_t calls = events.size();
    assert(f.new_region() == 0 && f.ctx.running && events.size() == calls && f.heap_is(heap));
    assert(f.dispose(regions[3]) == 0);
    regions[3] = f.new_region();
    assert(regions[3]);
    for (ULONG r : regions)
        assert(f.dispose(r) == 0);
    assert(live_regions.empty() && f.avail() == base);
}

static void test_atomic_or_rect()
{
    Fixture f;
    const ULONG region = f.new_region();
    assert(f.or_rect(region, f.rect(0, 0, 3, 3)) == TRUE && f.or_rect(region, f.rect(10, 0, 13, 3)) == TRUE);
    const ULONG extra = f.rect(20, 20, 21, 21);
    const Region *const native = kept_since(0).back();
    const auto rects = native_region(native)->rects;
    auto unchanged = [&] {
        const auto &now = native_region(native)->rects;
        return now.size() == rects.size() &&
               std::memcmp(now.data(), rects.data(), rects.size() * sizeof(Rectangle)) == 0;
    };

    /* Each failure leaves the native Region, the 68k one, its list and the
       whole heap as they were; the copy goes. */
    auto failed = [&](auto arrange, auto restore) {
        const size_t live_before = live_regions.size();
        const unsigned disposals = region_disposals;
        arrange();
        const auto heap = f.heap();
        assert(f.or_rect(region, extra) == FALSE && f.ctx.running);
        restore();
        assert(f.heap_is(heap) && live_regions.size() == live_before && live_regions.count(native));
        assert(unchanged());
        f.same(region, native);
        return region_disposals - disposals;
    };
    /* no native copy */
    assert(failed([] { new_region_budget = 0; }, [] { new_region_budget = -1; }) == 0);
    /* the copy of the current Region fails */
    assert(failed([] { or_region_fails = true; }, [] { or_region_fails = false; }) == 1);
    /* the Rectangle fails */
    assert(failed([] { or_rect_fails = true; }, [] { or_rect_fails = false; }) == 1);
    /* the result is a cycle */
    assert(failed([] { cyclic = true; }, [] { cyclic = false; }) == 1);
    /* no guest memory for the new list */
    {
        std::vector<std::pair<ULONG, ULONG>> fillers;
        assert(failed([&] { fillers = f.fill_heap(); }, [] {}) == 1);
        f.unfill(fillers);
    }
    assert(f.or_rect(region, extra) == TRUE && !live_regions.count(native));

    /* the snapshot bound: KAFFEINE_REGION_MAX_RECTS rectangles, not one more */
    const ULONG other = f.new_region();
    for (int i = 0; i < KAFFEINE_REGION_MAX_RECTS; ++i)
        assert(f.or_rect(other, f.rect((WORD)(2 * i), 30, (WORD)(2 * i), 30)) == TRUE);
    const size_t made = made_regions.size();
    const Region *full = kept_since(0).back();
    assert(native_region(full)->rects.size() == KAFFEINE_REGION_MAX_RECTS);
    f.same(other, full);
    const ULONG overflow = f.rect(200, 30, 200, 30);
    const auto heap = f.heap();
    assert(f.or_rect(other, overflow) == FALSE && f.ctx.running);
    assert(f.heap_is(heap) && live_regions.count(full) && kept_since(made).empty());
    f.same(other, full);
}

static void test_malformed()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8), layer = f.create(li, bm, 0, 0, 7, 7);
    const ULONG region = f.new_region(), r = f.rect(0, 0, 1, 1), gone = f.new_region();
    const ULONG foreign = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_REGION, MEMF_CLEAR);
    assert(layer && region && gone && foreign && f.dispose(gone) == 0);
    assert(f.or_rect(region, r) == TRUE);
    const Region *native = kept_since(0).back();

    const auto heap = f.heap();
    const size_t calls = events.size();
    auto refused = [&](ULONG lvo, std::initializer_list<ULONG> a) {
        assert(f.call(Fixture::graphics_base, lvo, a) == 20 && f.stopped());
        assert(events.size() == calls && f.heap_is(heap));
    };
    /* no Region of NewRegion(): none, foreign, inner, stale, other objects */
    for (ULONG bad : { 0u, foreign, region + 2, region + 8, gone, layer, li, bm, 0x5555u, 0xFFFFFFF0u })
    {
        refused(510, { bad, r });
        refused(534, { bad });
    }
    /* no Rectangle in guest memory: none, odd, past its end, across it */
    for (ULONG bad : { 0u, r + 1, f.ctx.mem_size, f.ctx.mem_size - 4, 0xFFFFFFFEu })
        refused(510, { region, bad });
    f.same(region, native);
}

static void test_clip_install_clear_replace_share()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8);
    const ULONG l1 = f.create(li, bm, 0, 0, 15, 7), l2 = f.create(li, bm, 8, 0, 31, 7);
    const ULONG a = f.new_region(), b = f.new_region();
    assert(l1 && l2 && a && b);
    size_t made = made_regions.size();
    assert(f.or_rect(a, f.rect(0, 0, 3, 3)) == TRUE);
    const Region *na = kept_since(made).back();
    made = made_regions.size();
    assert(f.or_rect(b, f.rect(4, 4, 7, 7)) == TRUE);
    const Region *nb = kept_since(made).back();
    Layer *n1 = native_layer(f, l1), *n2 = native_layer(f, l2);
    f.same(a, na);
    f.same(b, nb);

    /* install: the real native Region, the 68k one in Layer.ClipRegion */
    assert(f.clip(l1, a) == 0 && f.ctx.running && n1->ClipRegion == na);
    assert(f.L(l1 + M68K_LAYER_CLIPREGION) == a && held_locks == 0);
    /* shared with another layer */
    assert(f.clip(l2, a) == 0 && n2->ClipRegion == na && f.L(l2 + M68K_LAYER_CLIPREGION) == a);
    assert(f.no_native(l1, M68K_SIZEOF_LAYER) && f.no_native(l2, M68K_SIZEOF_LAYER));

    /* a leased Region neither changes nor goes */
    const ULONG extra = f.rect(9, 9, 9, 9);
    const size_t calls = events.size();
    const auto heap = f.heap();
    assert(f.or_rect(a, extra) == 20 && f.stopped());
    assert(f.dispose(a) == 20 && f.stopped());
    assert(events.size() == calls && f.heap_is(heap) && live_regions.count(na));

    /* replace: the previous 68k Region comes back, never a native one */
    assert(f.clip(l1, b) == a && n1->ClipRegion == nb && f.L(l1 + M68K_LAYER_CLIPREGION) == b);
    /* still leased by l2 */
    assert(f.dispose(a) == 20 && f.stopped() && live_regions.count(na));
    /* the 68k layers follow a new layer in front; ClipRegion stays */
    const ULONG l3 = f.create(li, bm, 0, 0, 3, 3);
    assert(l3 && f.L(li + M68K_LI_TOP_LAYER) == l3 && f.L(l3 + M68K_LAYER_BACK) == l2);
    assert(f.L(l1 + M68K_LAYER_CLIPREGION) == b && f.L(l2 + M68K_LAYER_CLIPREGION) == a &&
           !f.L(l3 + M68K_LAYER_CLIPREGION));

    /* clear: the old one comes back, and is the program's again */
    assert(f.clip(l2, 0) == a && !n2->ClipRegion && !f.L(l2 + M68K_LAYER_CLIPREGION));
    assert(f.or_rect(a, extra) == TRUE && f.ctx.running);
    assert(f.dispose(a) == 0 && f.ctx.running);
    /* the same Region again: one lease still */
    assert(f.clip(l1, b) == b && n1->ClipRegion == nb);
    assert(f.clip(l1, 0) == b && !n1->ClipRegion);
    assert(f.clip(l1, 0) == 0 && f.ctx.running);
    assert(f.dispose(b) == 0 && f.ctx.running && live_regions.empty());
    /* a disposed one is no Region */
    const unsigned clips = clip_calls;
    assert(f.clip(l1, b) == 20 && f.stopped() && clip_calls == clips);
}

static void test_clip_refusals()
{
    Desk d;
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8), l = f.create(li, bm, 0, 0, 7, 7);
    const ULONG region = f.new_region(), gone = f.new_region();
    const ULONG foreign = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_REGION, MEMF_CLEAR);
    assert(l && region && gone && f.dispose(gone) == 0);
    const ULONG deleted = f.create(li, bm, 0, 0, 3, 3);
    assert(f.destroy(deleted) == TRUE);

    /* a window of the program, the bar of its screen and the window of
       another program, both borrowed */
    const ULONG wlayer = d.open(f), screen_info = f.L(wlayer + M68K_LAYER_LAYERINFO);
    const ULONG gbar = f.L(screen_info + M68K_LI_TOP_LAYER), gforeign = f.L(wlayer + M68K_LAYER_BACK);
    assert(gbar && gforeign && gbar != wlayer && gforeign != wlayer && gforeign != gbar);

    const unsigned clips = clip_calls;
    auto refused = [&](ULONG layer, ULONG r) {
        assert(f.clip(layer, r) == 20 && f.stopped() && clip_calls == clips);
    };
    /* no layer of the program: borrowed, stale, others */
    for (ULONG bad : { 0u, gbar, gforeign, deleted, li, screen_info, l + 2, wlayer + 2, region, 0x5555u })
    {
        refused(bad, region);
        refused(bad, 0);
    }
    assert(held_locks == 0);
    /* no Region of the program: foreign, inner, stale, other objects */
    for (ULONG bad : { foreign, region + 2, gone, l, li, bm, 0x5555u })
        refused(l, bad);
    /* a native clip Region that the bridge did not install */
    Layer *n = native_layer(f, l);
    Region stray = {};
    n->ClipRegion = &stray;
    refused(l, 0);
    refused(l, region);
    n->ClipRegion = nullptr;
    /* with a layer lock of the program */
    assert(f.call(Fixture::layers_base, 96, { 0, l }) == 0 && f.ctx.running);
    refused(l, region);
    assert(f.call(Fixture::layers_base, 102, { l }) == 0);
    assert(!d.wl.layer.ClipRegion && !d.bar.ClipRegion && !d.foreign.ClipRegion);
    /* none of those leased the Region */
    assert(f.dispose(region) == 0 && f.ctx.running);
    d.close(f);
}

static void test_clip_callbacks()
{
    /* A backfill while InstallClipRegion() runs natively: on this thunk's
       CPU, and it can neither dispose of nor change a Region, nor change
       a clip Region: the program stops, the native layer keeps the new
       clip Region, and the counts follow it. */
    for (ULONG nested : { 534u, 510u, 174u, 0u })
    {
        Fixture f;
        const ULONG li = f.new_info(), bm = f.bitmap(4, 8), h = f.hook();
        const ULONG l = f.create(li, bm, 0, 0, 7, 7, h), other = f.create(li, bm, 0, 0, 3, 3);
        const ULONG a = f.new_region(), b = f.new_region(), r = f.rect(1, 1, 2, 2);
        size_t made = made_regions.size();
        assert(l && other && a && b && f.or_rect(a, r) == TRUE);
        const Region *na = kept_since(made).back();
        made = made_regions.size();
        assert(f.or_rect(b, r) == TRUE);
        const Region *nb = kept_since(made).back();
        Layer *n = native_layer(f, l);
        const size_t hooks = hook_calls.size();

        clip_backfill = true;
        if (nested)
        {
            action = GUEST_LVO;
            guest_base = nested == 174 ? Fixture::layers_base : Fixture::graphics_base;
            guest_lvo = nested;
            guest_lvo_a = nested == 534 ? std::vector<ULONG>{ a } :
                          nested == 510 ? std::vector<ULONG>{ b, r } : std::vector<ULONG>{ other, b };
        }
        else
            action = GUEST_FAULT;
        const size_t calls = events.size();
        assert(f.call_on(f.cpu2, Fixture::layers_base, 174, { l, a }, {}, false) == 20);
        assert(!f.ctx.running && f.ctx.exit_code == 20 && f.ctx.callback_depth == 0);
        assert(hook_calls.size() == hooks + 1 && hook_calls.back().cpu == 2 && hook_calls.back().msg_layer == l);
        if (nested)
            assert(hook_calls.back().nested == 20);
        /* only the outer clip change reached native code */
        assert(events.size() == calls + 1 && events.back() == "InstallClipRegion+");
        assert(n->ClipRegion == na && f.L(l + M68K_LAYER_CLIPREGION) == a);
        assert(!native_layer(f, other)->ClipRegion && live_regions.size() == 2);
        f.same(a, na);
        f.same(b, nb);
        clip_backfill = false;
        action = GUEST_NONE;
        /* a is leased, b is not */
        assert(f.dispose(a) == 20 && f.stopped());
        assert(f.dispose(b) == 0 && f.ctx.running);
        /* the end detaches it before the layer goes, then frees it */
    }
}

static void test_delete_and_cleanup_order()
{
    {
        Fixture f;
        const ULONG li = f.new_info(), bm = f.bitmap(4, 8);
        const ULONG l1 = f.create(li, bm, 0, 0, 7, 7), l2 = f.create(li, bm, 0, 0, 15, 7);
        const ULONG a = f.new_region(), b = f.new_region();
        size_t made = made_regions.size();
        assert(f.or_rect(a, f.rect(0, 0, 1, 1)) == TRUE);
        const Region *na = kept_since(made).back();
        made = made_regions.size();
        assert(f.or_rect(b, f.rect(2, 2, 3, 3)) == TRUE);
        const Region *nb = kept_since(made).back();
        assert(f.clip(l1, a) == 0 && f.clip(l2, b) == 0);

        /* DeleteLayer(): the clip Region leaves first, then the lease */
        events.clear();
        assert(f.destroy(l1) == TRUE && f.ctx.running);
        assert(event_at("InstallClipRegion0") < event_at("DeleteLayer") && event_count("DisposeRegion") == 0);
        assert(f.dispose(a) == 0 && f.ctx.running && !live_regions.count(na));

        /* layers keeps the layer: the clip Region comes back, still leased */
        Layer *n2 = native_layer(f, l2);
        delete_fails = true;
        events.clear();
        assert(f.destroy(l2) == FALSE && f.ctx.running);
        delete_fails = false;
        assert(events == std::vector<std::string>({ "InstallClipRegion0", "DeleteLayer", "InstallClipRegion+" }));
        assert(n2->ClipRegion == nb && f.L(l2 + M68K_LAYER_CLIPREGION) == b);
        assert(f.dispose(b) == 20 && f.stopped() && live_regions.count(nb));
    }
    {
        /* the end: a Region shared by two layers, one by a third; every
           clip Region leaves before any layer goes, the Regions go only
           after the layers and their LayerInfo, and both cleanups are
           idempotent */
        Fixture f;
        const ULONG li = f.new_info(), bm = f.bitmap(4, 8);
        const ULONG l1 = f.create(li, bm, 0, 0, 7, 7), l2 = f.create(li, bm, 0, 0, 15, 7);
        const ULONG l3 = f.create(li, bm, 0, 0, 3, 3);
        const ULONG shared = f.new_region(), own = f.new_region(), unused = f.new_region();
        assert(f.or_rect(shared, f.rect(0, 0, 1, 1)) == TRUE && f.or_rect(own, f.rect(2, 2, 3, 3)) == TRUE);
        assert(f.clip(l1, shared) == 0 && f.clip(l2, shared) == 0 && f.clip(l3, own) == 0);
        const auto list = f.L(shared + M68K_REGION_RECTANGLE);
        events.clear();
        Kaffeine_LayersCleanup(&f.ctx);
        assert(native_layers.empty() && native_infos.empty() && live_regions.size() == 3);
        assert(event_count("InstallClipRegion0") == 3 && event_count("DeleteLayer") == 3);
        assert(event_count("DisposeRegion") == 0 && event_at("DeleteLayer") > event_at("InstallClipRegion0"));
        Kaffeine_LayersCleanup(&f.ctx);
        Kaffeine_RegionsCleanup(&f.ctx);
        assert(live_regions.empty() && event_count("DisposeRegion") == 3 && !f.ctx.regions);
        assert(event_at("DisposeRegion") > event_at("DisposeLayerInfo"));
        for (ULONG g : { shared, own, unused })
            assert(f.zero(g, M68K_SIZEOF_REGION));
        assert(f.zero(list, M68K_SIZEOF_RR));
        Kaffeine_RegionsCleanup(&f.ctx);
        assert(event_count("DisposeRegion") == 3);
        /* without the bridges, the calls stop before anything native */
        assert(f.dispose(shared) == 20 && f.stopped());
        assert(f.clip(l1, 0) == 20 && f.stopped());
    }
    {
        /* layers keeps a layer at the end: its clip Region leaves it all
           the same, and the Region goes with the others */
        Fixture f;
        const ULONG li = f.new_info(), bm = f.bitmap(4, 8), l = f.create(li, bm, 0, 0, 7, 7);
        const ULONG region = f.new_region();
        assert(f.or_rect(region, f.rect(0, 0, 1, 1)) == TRUE && f.clip(l, region) == 0);
        Layer *n = native_layer(f, l);
        delete_fails = true;
        Kaffeine_LayersCleanup(&f.ctx);
        delete_fails = false;
        assert(native_layers.size() == 1 && !n->ClipRegion && native_infos.size() == 1);
        Kaffeine_RegionsCleanup(&f.ctx);
        assert(live_regions.empty());
        /* the test's own leftovers */
        NativeLayer *kept = native_of(n);
        native_layers.clear();
        delete kept;
        native_infos[0]->top_layer = nullptr;
        delete native_infos[0];
        native_infos.clear();
    }
}

static void test_window_clip_install_replace_remove()
{
    Desk d;
    Fixture f;
    const ULONG wlayer = d.open(f);
    const ULONG a = f.new_region(), b = f.new_region(), empty = f.rect(5, 5, 4, 5);
    const ULONG extra = f.rect(9, 9, 9, 9);
    const Region *na = region_with(f, a, 0, 0, 3, 3), *nb = region_with(f, b, 4, 4, 7, 7);
    Layer *n = &d.wl.layer;
    /* An empty Rectangle changes nothing: TRUE for a Region of the
       program, 20 for a leased one, either way without native calls or
       guest memory. */
    auto leased = [&](ULONG region) {
        const size_t calls = events.size();
        const ULONG result = f.or_rect(region, empty);
        assert(events.size() == calls && (result == TRUE || (result == 20 && f.stopped())));
        return result == 20;
    };
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG base = f.avail();
    const auto heap = f.heap();
    const UWORD shadows = f.ctx.num_shadows;

    /* install: the real native Region under the bridge's lock of the
       layer, which the native call nests on; the 68k one in ClipRegion */
    assert(f.clip(wlayer, a) == 0 && f.ctx.running && n->ClipRegion == na);
    assert(f.L(wlayer + M68K_LAYER_CLIPREGION) == a && clip_held == 1 && held_locks == 0);
    assert(f.no_native(wlayer, M68K_SIZEOF_LAYER) && f.avail() == base && f.ctx.num_shadows == shadows);
    assert(f.L(d.mw + M68K_WIN_WLAYER) == wlayer);

    /* leased: it neither changes nor goes, with nothing native */
    const size_t calls = events.size();
    assert(leased(a) && !leased(b));
    assert(f.or_rect(a, extra) == 20 && f.stopped());
    assert(f.dispose(a) == 20 && f.stopped());
    assert(events.size() == calls && live_regions.count(na) && n->ClipRegion == na);
    f.same(a, na);
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(wlayer + M68K_LAYER_CLIPREGION) == a);

    /* replace: the old 68k Region comes back as the program gave it, and
       is the program's again */
    assert(f.clip(wlayer, b) == a && f.ctx.running && n->ClipRegion == nb);
    assert(f.L(wlayer + M68K_LAYER_CLIPREGION) == b && clip_held == 1 && held_locks == 0);
    assert(!leased(a) && leased(b));
    /* the same Region again is still one lease */
    assert(f.clip(wlayer, b) == b && n->ClipRegion == nb && leased(b));

    /* remove */
    assert(f.clip(wlayer, 0) == b && f.ctx.running && !n->ClipRegion);
    assert(!f.L(wlayer + M68K_LAYER_CLIPREGION) && !leased(a) && !leased(b));
    assert(f.clip(wlayer, 0) == 0 && f.ctx.running && !n->ClipRegion);
    Kaffeine_LayersRefresh(&f.ctx);
    assert(!f.L(wlayer + M68K_LAYER_CLIPREGION));
    /* none of that took or left guest memory */
    assert(f.avail() == base && f.ctx.num_shadows == shadows && f.heap_is(heap));

    /* the same 68k Region after a change of the program stands for its
       new native Region */
    na = region_with(f, a, 9, 9, 9, 9);
    f.same(a, na);
    assert(f.clip(wlayer, a) == 0 && n->ClipRegion == na && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);

    /* the snapshot of the screen goes and comes back: the 68k Layer and
       its ClipRegion stay */
    f.ctx.layer_ops->screen_closing(&f.ctx, &d.scr);
    assert(f.L(d.mw + M68K_WIN_WLAYER) == wlayer && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);
    assert(!f.L(wlayer + M68K_LAYER_LAYERINFO));
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(d.mw + M68K_WIN_WLAYER) == wlayer && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);
    assert(f.L(wlayer + M68K_LAYER_LAYERINFO) && f.no_native(wlayer, M68K_SIZEOF_LAYER));
    assert(n->ClipRegion == na && leased(a));

    /* the window closes: the clip Region leaves the layer first */
    events.clear();
    d.close(f);
    assert(events == std::vector<std::string>({ "InstallClipRegion0", "CloseWindow" }));
    assert(f.dispose(a) == 0 && f.dispose(b) == 0 && f.ctx.running && live_regions.empty());
    /* the 68k Layer went with the window */
    const unsigned clips = clip_calls;
    assert(f.clip(wlayer, 0) == 20 && f.stopped() && clip_calls == clips);
}

static void test_requester_clip()
{
    Desk d;
    Fixture f;
    const ULONG wlayer = d.open(f), rlayer = d.request(f);
    const ULONG a = f.new_region(), b = f.new_region();
    const Region *na = region_with(f, a, 0, 0, 3, 3), *nb = region_with(f, b, 4, 4, 7, 7);
    Layer *r = &d.req.layer, *n = &d.wl.layer;
    const ULONG base = f.avail();

    /* the requester layer takes its own clip Region, the window another */
    assert(f.clip(rlayer, a) == 0 && f.ctx.running && r->ClipRegion == na && clip_held == 1);
    assert(f.L(rlayer + M68K_LAYER_CLIPREGION) == a && !n->ClipRegion);
    assert(f.clip(wlayer, a) == 0 && n->ClipRegion == na && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);
    assert(f.no_native(rlayer, M68K_SIZEOF_LAYER) && f.avail() == base && held_locks == 0);

    /* both lease it */
    assert(f.dispose(a) == 20 && f.stopped());
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(rlayer + M68K_LAYER_CLIPREGION) == a && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);
    assert(f.L(wlayer + M68K_LAYER_FRONT) == rlayer && f.L(rlayer + M68K_LAYER_BACK) == wlayer);

    /* replace on the requester; the window keeps a */
    assert(f.clip(rlayer, b) == a && r->ClipRegion == nb && f.L(rlayer + M68K_LAYER_CLIPREGION) == b);
    assert(f.dispose(a) == 20 && f.stopped());

    /* EndRequest(): the clip Region leaves the layer before Intuition
       deletes it, and b is the program's again */
    events.clear();
    d.end_request(f);
    assert(event_at("InstallClipRegion0") < event_at("EndRequest") && event_count("InstallClipRegion0") == 1);
    assert(n->ClipRegion == na && f.L(wlayer + M68K_LAYER_CLIPREGION) == a);
    assert(f.dispose(b) == 0 && f.ctx.running && !live_regions.count(nb));
    /* the stale requester layer is no layer of the program */
    const unsigned clips = clip_calls;
    assert(f.clip(rlayer, a) == 20 && f.stopped() && clip_calls == clips);

    /* CloseWindow(): the same for the window, with a requester on it */
    d.request(f);
    assert(f.clip(d.rlayer, a) == 0 && d.req.layer.ClipRegion == na);
    events.clear();
    d.close(f);
    /* the window's clip Region, then the requester's, then Intuition */
    assert(event_count("InstallClipRegion0") == 2 && event_count("DisposeRegion") == 0);
    assert(event_at("InstallClipRegion0", event_at("InstallClipRegion0") + 1) < event_at("EndRequest"));
    assert(events.back() == "CloseWindow" && !d.wl.layer.ClipRegion);
    assert(f.dispose(a) == 0 && f.ctx.running && live_regions.empty());
}

static void test_window_clip_refusals()
{
    Desk d;
    Fixture f;
    Hook native_hook = {};
    native_hook.h_Entry = reinterpret_cast<decltype(native_hook.h_Entry)>(&trap_entry);
    const ULONG wlayer = d.open(f);
    const ULONG a = f.new_region(), b = f.new_region(), empty = f.rect(5, 5, 4, 5);
    const Region *na = region_with(f, a, 0, 0, 3, 3);
    region_with(f, b, 4, 4, 7, 7);
    Layer *n = &d.wl.layer;
    unsigned clips = clip_calls;
    auto refused = [&](ULONG layer, ULONG r) {
        const auto heap = f.heap();
        assert(f.clip(layer, r) == 20 && f.stopped() && clip_calls == clips);
        assert(held_locks == 0 && f.heap_is(heap));
    };

    /* a native clip Region that the bridge did not install */
    Region stray = {};
    n->ClipRegion = &stray;
    refused(wlayer, a);
    refused(wlayer, 0);
    assert(n->ClipRegion == &stray);
    n->ClipRegion = nullptr;

    /* a BackFill that may be a guest address, from WA_BackFill, and any
       other hook that is no sentinel */
    n->BackFill = reinterpret_cast<Hook *>(static_cast<uintptr_t>(Fixture::heap_start + 0x100));
    refused(wlayer, a);
    n->BackFill = &native_hook;
    refused(wlayer, a);
    /* none of those leased a: an empty Rectangle is still taken */
    assert(f.or_rect(a, empty) == TRUE && f.ctx.running);

    /* with a lock of the program: of the window layer, or of a private
       layer, and a private layer with a window lock */
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8), l = f.create(li, bm, 0, 0, 7, 7);
    n->BackFill = LAYERS_BACKFILL;
    assert(f.call(Fixture::layers_base, 96, { 0, wlayer }) == 0 && f.ctx.running);
    const auto heap = f.heap();
    assert(f.clip(wlayer, a) == 20 && f.stopped() && clip_calls == clips && f.heap_is(heap));
    assert(f.clip(l, a) == 20 && f.stopped() && clip_calls == clips);
    assert(f.call(Fixture::layers_base, 102, { wlayer }) == 0 && f.ctx.running);
    assert(f.call(Fixture::layers_base, 96, { 0, l }) == 0 && f.ctx.running);
    assert(f.clip(wlayer, a) == 20 && f.stopped() && clip_calls == clips);
    assert(f.call(Fixture::layers_base, 102, { l }) == 0 && held_locks == 0);

    /* both sentinels are taken */
    n->BackFill = LAYERS_NOBACKFILL;
    assert(f.clip(wlayer, a) == 0 && f.ctx.running && n->ClipRegion == na && clip_calls == ++clips);

    /* installed, Intuition or anyone else replaced it natively: refused,
       and the bridge's lease stays until the layer goes */
    n->ClipRegion = &stray;
    refused(wlayer, b);
    refused(wlayer, 0);
    n->ClipRegion = const_cast<Region *>(na);

    /* while it is installed, the layer keeps a sentinel BackFill: a hook
       handle of the bridge is refused, sentinels are taken */
    assert(f.call(Fixture::layers_base, 198, { wlayer, 0 }) == 1 && n->BackFill == LAYERS_BACKFILL);
    assert(f.clip(wlayer, 0) == a && !n->ClipRegion && clip_calls == ++clips);
    n->BackFill = &native_hook;
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG handle = f.L(wlayer + M68K_LAYER_BACKFILL);
    assert(handle > 1 && f.call(Fixture::layers_base, 198, { wlayer, 0 }) == handle);
    assert(n->BackFill == LAYERS_BACKFILL);
    assert(f.clip(wlayer, a) == 0 && n->ClipRegion == na && clip_calls == ++clips);
    assert(f.call(Fixture::layers_base, 198, { wlayer, handle }) == 20 && f.stopped());
    assert(n->BackFill == LAYERS_BACKFILL && held_locks == 0);
    assert(f.call(Fixture::layers_base, 198, { wlayer, 1 }) == 0 && n->BackFill == LAYERS_NOBACKFILL);

    /* not from a 68k backfill hook of a private layer */
    {
        const ULONG h = f.hook();
        action = GUEST_LVO;
        guest_base = Fixture::layers_base;
        guest_lvo = 174;
        guest_lvo_a = { wlayer, b };
        const size_t hooks = hook_calls.size();
        f.call_on(f.cpu, Fixture::layers_base, 186, { li, bm, 0, h }, { 0, 0, 3, 3, LAYERSIMPLE }, false);
        action = GUEST_NONE;
        assert(hook_calls.size() == hooks + 1 && hook_calls.back().nested == 20);
        assert(n->ClipRegion == na && f.L(wlayer + M68K_LAYER_CLIPREGION) == a && clip_calls == clips);
    }
    /* b was never leased; a is until the window goes */
    assert(f.dispose(b) == 0 && f.ctx.running);
    assert(f.dispose(a) == 20 && f.stopped());
    d.close(f);
    assert(f.dispose(a) == 0 && f.ctx.running && live_regions.empty());
}

static void test_window_clip_cleanup()
{
    /* the end, with a window and a requester still open, each with a clip
       Region and the window with a lock of the program: every clip Region
       leaves its native layer, under the layer lock, before Intuition ever
       deletes the layer, and no backfill runs a hook that is no sentinel */
    Desk d;
    Fixture f;
    Hook trap = {};
    trap.h_Entry = reinterpret_cast<decltype(trap.h_Entry)>(&trap_entry);
    const ULONG wlayer = d.open(f), rlayer = d.request(f);
    const ULONG a = f.new_region(), b = f.new_region(), unused = f.new_region();
    region_with(f, a, 0, 0, 3, 3);
    region_with(f, b, 4, 4, 7, 7);
    assert(unused && f.clip(wlayer, a) == 0 && f.clip(rlayer, b) == 0);
    assert(f.call(Fixture::layers_base, 96, { 0, wlayer }) == 0 && f.ctx.running);
    /* a hook that the bridge could not refuse, set natively */
    d.wl.layer.BackFill = &trap;
    clip_backfill = true;
    events.clear();
    Kaffeine_LayersCleanup(&f.ctx);
    clip_backfill = false;
    assert(!f.ctx.layers && held_locks == 0);
    assert(!d.wl.layer.ClipRegion && !d.req.layer.ClipRegion);
    assert(d.wl.layer.BackFill == LAYERS_NOBACKFILL);
    assert(event_count("InstallClipRegion0") == 2 && event_count("DisposeRegion") == 0);
    assert(live_regions.size() == 3);
    /* the Regions are free: they all go, after the layers */
    Kaffeine_RegionsCleanup(&f.ctx);
    assert(live_regions.empty() && event_count("DisposeRegion") == 3);
    /* Intuition deletes the layers only now, with nothing of the bridge */
    const size_t calls = events.size();
    intuition_deletes(&d.req.layer, "EndRequest");
    intuition_deletes(&d.wl.layer, "CloseWindow");
    assert(events.size() == calls + 2);
    /* and the calls stop before anything native */
    assert(f.clip(wlayer, 0) == 20 && f.stopped());
}

extern "C" BOOL Request(struct Requester *,struct Window *) { assert(false); return FALSE; }
extern "C" void EndRequest(struct Requester *,struct Window *) { assert(false); }
extern "C" BOOL SetDMRequest(struct Window *,struct Requester *) { assert(false); return FALSE; }

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "LVO 516, 510 and 534 are bridged; every other Region vector stops first", test_registration_and_guards },
        { "Stable 68k Regions with big-endian snapshots of the native Bounds and rectangles", test_identity_and_snapshots },
        { "NewRegion() without native memory, guest memory or room leaves nothing behind", test_new_region_failures },
        { "OrRectRegion() publishes a complete native copy or leaves everything whole", test_atomic_or_rect },
        { "Foreign, stale and malformed Regions and Rectangles stop before anything native", test_malformed },
        { "Clip Regions: install, share, replace, clear, with 68k identities", test_clip_install_clear_replace_share },
        { "Clip Regions only of layers and Regions of the program, never borrowed ones", test_clip_refusals },
        { "Backfills during InstallClipRegion() cannot touch Regions or clips", test_clip_callbacks },
        { "Deletion and the end detach clip Regions before layers, free Regions after", test_delete_and_cleanup_order },
        { "Window clip Regions: install, lease, replace, remove, stable identities and heap", test_window_clip_install_replace_remove },
        { "Requester clip Regions leave before EndRequest() and CloseWindow()", test_requester_clip },
        { "Window clips refuse foreign clips, backfill hooks, locks and callbacks", test_window_clip_refusals },
        { "The end takes window and requester clips out before Intuition deletes layers", test_window_clip_cleanup },
    };
    for (const Test &t : tests)
    {
        std::printf("%s ... ", t.name);
        std::fflush(stdout);
        t.run();
        std::printf("ok\n");
    }
    return 0;
}

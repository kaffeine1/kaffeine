/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    The layers of 68k code through literal LVOs, the real dispatcher, run
    loop, window bridge and heap: classic 68k Layers at the NDK offsets of
    kaffeine_shadow_layouts.h, with stable identities and links that follow
    the native list; asynchronous depth calls whose effect shows only after
    Intuition moved the window, through the poll of the outermost run loop
    without guest tasks or through a message; backdrop bits that the program
    changes and that only a depth call takes; backfill sentinels and opaque
    handles; refusals of every other pointer and call before anything
    native; several windows, borrowed layers, locks, missing memory, full
    tables, screens and teardown; LayerInfos of NewLayerInfo() and
    DisposeLayerInfo(), owned apart from the borrowed ones of screens. The native Intuition and layers below act
    as AROS does and record every call; their depth calls are asynchronous.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <exec/memory.h>
#include <graphics/rastport.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
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
#include "kaffeine_shadow.h"
#include "kaffeine_shadow_layouts.h"
#include "kaffeine_layers.h"
#include "kaffeine_screens.h"
#include "kaffeine_window.h"
#include "kaffeine_gadgets.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_cpu.h"
#include "kaffeine_callback.h"
#include "kaffeine_exec_compat.h"

/* ── The native side ─────────────────────────────────────────────── */

static KaffeineContext *live;
static std::vector<std::string> events;
static std::map<const Layer *, int> layer_locks;
static int held_locks, info_depth, ibase_depth;
static std::map<Layer_Info *, int> info_locks;
static Task *native_task = reinterpret_cast<Task *>(static_cast<IPTR>(0x11110000));
extern "C" Task *FindTask(const char *name) { assert(!name); return native_task; }
static bool info_busy;
static unsigned info_calls, ibase_calls, native_installs, attempts;
static bool attempt_fails;
static Layer *attempt_busy_layer;
static bool update_fails;
static unsigned refresh_begins, refresh_ends, update_begins, update_ends;
static std::vector<BOOL> refresh_complete;
static std::function<void()> refresh_entry;
struct DepthCall
{
    Window *window;
    bool front;
    ULONG wflags;
    UWORD lflags;
};
static std::vector<DepthCall> depth_calls;
static Window *open_result;
static std::vector<Window *> closed;
static ULONG zero_at_close;   /* a 68k Layer that must be gone as the window closes */
static std::map<MsgPort *, std::deque<IntuiMessage *>> queues;
static std::vector<IntuiMessage *> replied;
static Hook intuition_hook, other_hook;

static Screen scr;
static Layer bar, foreign;
static Window foreign_window;

/* The native list of a screen, front to back */
static void order(Screen &s, const std::vector<Layer *> &layers)
{
    Layer *prev = nullptr;

    s.LayerInfo.top_layer = layers.empty() ? nullptr : layers.front();
    for (Layer *l : layers)
    {
        l->front = prev;
        l->back = nullptr;
        l->LayerInfo = &s.LayerInfo;
        if (prev)
            prev->back = l;
        prev = l;
    }
}

/* As DeleteLayer(): the layer leaves its list */
static void unlink_native(Layer *l)
{
    if (!l || !l->LayerInfo)
        return;
    if (l->front)
        l->front->back = l->back;
    else if (l->LayerInfo->top_layer == l)
        l->LayerInfo->top_layer = l->back;
    if (l->back)
        l->back->front = l->front;
    l->front = l->back = nullptr;
}

/* The bridge takes a LayerInfo lock alone, never after a layer lock. */
extern "C" void LockLayerInfo(struct Layer_Info *li)
{
    assert(li && (!held_locks || info_locks[li]));
    ++info_locks[li];
    ++info_depth;
    ++info_calls;
    events.push_back("LockLayerInfo");
}
extern "C" void UnlockLayerInfo(struct Layer_Info *li)
{
    assert(li && info_locks[li] > 0 && info_depth > 0);
    --info_locks[li];
    --info_depth;
    events.push_back("UnlockLayerInfo");
}
extern "C" ULONG AttemptSemaphore(struct SignalSemaphore *lock)
{
    auto *li=reinterpret_cast<Layer_Info *>(reinterpret_cast<char *>(lock)-offsetof(Layer_Info,Lock));
    if(info_busy && !info_locks[li]) return 0;
    LockLayerInfo(li);
    return 1;
}
extern "C" void ReleaseSemaphore(struct SignalSemaphore *lock)
{
    auto *li=reinterpret_cast<Layer_Info *>(reinterpret_cast<char *>(lock)-offsetof(Layer_Info,Lock));
    UnlockLayerInfo(li);
}
extern "C" void LockLayer(LONG dummy, struct Layer *l)
{
    assert(dummy == 0 && l && (!info_depth || info_locks[l->LayerInfo]));
    ++layer_locks[l];
    ++held_locks;
    events.push_back("LockLayer");
}
extern "C" void UnlockLayer(struct Layer *l)
{
    assert(l && layer_locks[l] > 0);
    --layer_locks[l];
    --held_locks;
    events.push_back("UnlockLayer");
}
extern "C" BOOL AttemptLockLayerRom(struct Layer *l)
{
    ++attempts;
    if (attempt_fails || l == attempt_busy_layer)
        return FALSE;
    ++layer_locks[l];
    ++held_locks;
    return TRUE;
}
/* BeginUpdate adds a layer ref; failure releases it without entering update
   state. The extracted native Intuition body decides whether to EndUpdate. */
static BOOL BeginUpdate(Layer *l)
{
    assert(!(l->Flags & LAYERUPDATING));
    ++update_begins;events.push_back("BeginUpdate");LockLayer(0,l);
    if(update_fails) { UnlockLayer(l);return FALSE; }
    l->Flags |= LAYERUPDATING;return TRUE;
}
static void EndUpdate(Layer *l,BOOL complete)
{
    assert(l->Flags & LAYERUPDATING);
    ++update_ends;events.push_back("EndUpdate");
    l->Flags &= ~LAYERUPDATING;if(complete)l->Flags &= ~LAYERREFRESH;
    UnlockLayer(l);
}
struct LayersBase;
static struct { struct LayersBase *LayersBase; } source_ibase;
#define GetPrivIBase(unused) (&source_ibase)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define DEBUG_REFRESH(unused) ((void)0)
#define SANITY_CHECK(w) assert(w);
#define WLAYER(w) ((w)->WLayer)
#define BLAYER(w) (((w)->Flags & WFLG_GIMMEZEROZERO) ? (w)->BorderRPort->Layer : nullptr)
#define AROS_ATOMIC_OR(a,b) ((a) |= (b))
#define AROS_ATOMIC_AND(a,b) ((a) &= (b))
#define LockLayerInfo(li) ((void)LayersBase, ::LockLayerInfo(li))
#define UnlockLayerInfo(li) ((void)LayersBase, ::UnlockLayerInfo(li))
#include "refresh_native_generated.h"
#undef UnlockLayerInfo
#undef LockLayerInfo
#undef AROS_ATOMIC_AND
#undef AROS_ATOMIC_OR
#undef BLAYER
#undef WLAYER
#undef SANITY_CHECK
#undef DEBUG_REFRESH
#undef AROS_LIBFUNC_EXIT
#undef AROS_LIBFUNC_INIT
#undef GetPrivIBase
extern "C" void BeginRefresh(Window *w)
{
    assert(Kaffeine_LayerInfoLocked(live) && info_locks[&w->WScreen->LayerInfo]>0 &&
           layer_locks[w->WLayer]>0);
    if(w->Flags&WFLG_GIMMEZEROZERO)assert(layer_locks[w->BorderRPort->Layer]>0);
    ++refresh_begins;events.push_back("BeginRefresh");
    if(refresh_entry)refresh_entry();
    source_BeginRefresh(w);
}
extern "C" void EndRefresh(Window *w,BOOL complete)
{
    assert(Kaffeine_LayerInfoLocked(live));
    ++refresh_ends;refresh_complete.push_back(complete);events.push_back("EndRefresh");
    source_EndRefresh(w,complete);
}
/* As hyperlayers: under the layer lock, the old hook comes back. */
extern "C" struct Hook *InstallLayerHook(struct Layer *l, struct Hook *h)
{
    assert(l && layer_locks[l] > 0);
    ++native_installs;
    Hook *old = l->BackFill;
    l->BackFill = h;
    events.push_back("InstallLayerHook");
    return old;
}
/* As hyperlayers: a new Layer_Info without layers, or NULL without memory.
   DisposeLayerInfo() takes only one of these, alive and without layers;
   a screen's LayerInfo or one disposed of already fails here. */
static std::vector<Layer_Info *> native_infos;
static unsigned new_infos, disposed_infos;
static bool new_info_fails;
extern "C" struct Layer_Info *NewLayerInfo(void)
{
    ++new_infos;
    events.push_back("NewLayerInfo");
    if (new_info_fails)
        return nullptr;
    Layer_Info *li = new Layer_Info();
    li->Flags = NEWLAYERINFO_CALLED;
    native_infos.push_back(li);
    return li;
}
extern "C" void DisposeLayerInfo(struct Layer_Info *li)
{
    auto it = std::find(native_infos.begin(), native_infos.end(), li);
    assert(it != native_infos.end() && !li->top_layer);
    native_infos.erase(it);
    ++disposed_infos;
    events.push_back("DisposeLayerInfo");
    delete li;
}
extern "C" ULONG LockIBase(ULONG what)
{
    assert(what == 0 && ibase_depth == 0 && held_locks == 0);
    ++ibase_depth;
    ++ibase_calls;
    return 0x1BA5E;
}
extern "C" void UnlockIBase(ULONG lock)
{
    assert(lock == 0x1BA5E && ibase_depth == 1);
    --ibase_depth;
}
/* Asynchronous, as Intuition's: nothing moves until the input handler runs,
   which the tests do with order(). */
static void depth(Window *w, bool front)
{
    assert(ibase_depth == 0 && info_depth == 0);
    depth_calls.push_back({ w, front, w->Flags, w->WLayer ? w->WLayer->Flags : (UWORD)0 });
    events.push_back(front ? "WindowToFront" : "WindowToBack");
}
extern "C" void WindowToBack(struct Window *w) { depth(w, false); }
extern "C" void WindowToFront(struct Window *w) { depth(w, true); }

extern "C" struct Window *OpenWindowTagList(struct NewWindow *nw, struct TagItem *tags)
{
    assert(!nw && tags);
    events.push_back("OpenWindowTagList");
    return open_result;
}
extern "C" void CloseWindow(struct Window *w)
{
    events.push_back("CloseWindow");
    closed.push_back(w);
    // the program's locks and the 68k Layer went first
    if (w->WLayer)
        assert(layer_locks[w->WLayer] == 0);
    if (zero_at_close)
        for (ULONG i = 0; i < M68K_SIZEOF_LAYER; ++i)
            assert(live->mem[zero_at_close + i] == 0);
    unlink_native(w->WLayer);
}
extern "C" struct Message *GetMsg(struct MsgPort *port)
{
    auto &q = queues[port];
    if (q.empty())
        return nullptr;
    IntuiMessage *im = q.front();
    q.pop_front();
    return &im->ExecMessage;
}
extern "C" void ReplyMsg(struct Message *message)
{
    replied.push_back(reinterpret_cast<IntuiMessage *>(message));
}
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
extern "C" void FreeSysRequest(struct Window *w)
{
    events.push_back("FreeSysRequest");
    CloseWindow(w);
}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { assert(false); return 0; }
extern "C" BOOL Kaffeine_AppPortFill(KaffeineContext *, ULONG, BOOL) { return FALSE; }
extern "C" BOOL Kaffeine_AppPortMatches(KaffeineContext *, ULONG, Task *, UBYTE) { return TRUE; }
extern "C" BOOL Kaffeine_AppReplyMsg(KaffeineContext *, ULONG) { return FALSE; }
/* metadata-only fonts: no native opens */
extern "C" void CloseFont(struct TextFont *) { assert(false); }

/* A window of the program with its RastPort, IDCMP port and layer */
struct NativeWindow
{
    Window window;
    RastPort rp;
    MsgPort port;
    Layer layer;
};

static void make_layer(Layer &l, WORD x, WORD y, WORD w, WORD h, UWORD flags)
{
    std::memset(static_cast<void *>(&l), 0, sizeof(l));
    l.bounds.MinX = x;
    l.bounds.MinY = y;
    l.bounds.MaxX = (WORD)(x + w - 1);
    l.bounds.MaxY = (WORD)(y + h - 1);
    l.Width = w;
    l.Height = h;
    l.Flags = flags;
}

static void make_window(NativeWindow &n, Screen &s, WORD x, WORD y, WORD w, WORD h)
{
    std::memset(static_cast<void *>(&n), 0, sizeof(n));
    make_layer(n.layer, x, y, w, h, LAYERSMART);
    n.layer.Window = &n.window;
    n.layer.rp = &n.rp;
    n.layer.BackFill = &intuition_hook;
    n.layer.Scroll_X = 3;
    n.layer.Scroll_Y = -4;
    n.rp.Layer = &n.layer;
    n.window.LeftEdge = x;
    n.window.TopEdge = y;
    n.window.Width = w;
    n.window.Height = h;
    n.window.Flags = 0x1002;    /* WFLG_ACTIVATE | WFLG_DRAGBAR */
    n.window.WScreen = &s;
    n.window.RPort = &n.rp;
    n.window.WLayer = &n.layer;
    n.window.IDCMPFlags = IDCMP_INTUITICKS;
    n.window.UserPort = &n.port;
    n.port.mp_SigBit = 21;
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
/* A slice of a program that only polls guest memory; it stops itself. */
static unsigned slices, slice_limit;
static std::function<void()> before_completion;
static void execute(void *)
{
    if (++slices >= slice_limit)
    {
        if (before_completion) before_completion();
        live->running = FALSE;
    }
}
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr };

static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }

static const M68KThunkEntry exec_manual[] = { KAFFEINE_EXEC_IDCMP_THUNKS };
static const M68KThunkEntry intuition_manual[] =
    { KAFFEINE_INTUITION_WINDOW_THUNKS, KAFFEINE_INTUITION_LAYER_THUNKS,
      KAFFEINE_INTUITION_REQUEST_THUNKS };
static const M68KThunkEntry graphics_manual[] = { KAFFEINE_GRAPHICS_LAYER_THUNKS };

/* A poisoned generated thunk behind every manual slot */
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
    static constexpr ULONG exec_base = 0x1000, intuition_base = 0x2000, layers_base = 0x3000,
                           graphics_base = 0x3C00;
    static constexpr ULONG stack = 0x3F00, task = 0x3800, tags = 0x3A00;
    static constexpr ULONG heap_start = 0x4000, heap_end = 0x1F000;
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory = std::vector<UBYTE>(0x20000, 0xA5);
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };
    std::vector<M68KThunkEntry> exec_gen, intuition_gen, layers_gen, graphics_gen;
    ULONG heap_initial = 0;

    explicit Fixture(bool bridge = true)
    {
        events.clear();
        layer_locks.clear();
        info_locks.clear();
        native_task = reinterpret_cast<Task *>(static_cast<IPTR>(0x11110000));
        held_locks = info_depth = ibase_depth = 0;
        info_busy = false;
        info_calls = ibase_calls = native_installs = attempts = 0;
        attempt_fails = false;
        attempt_busy_layer=nullptr;update_fails=false;
        refresh_begins=refresh_ends=update_begins=update_ends=0;
        refresh_complete.clear();refresh_entry=nullptr;
        assert(native_infos.empty());
        new_infos = disposed_infos = 0;
        new_info_fails = false;
        depth_calls.clear();
        open_result = nullptr;
        closed.clear();
        zero_at_close = 0;
        queues.clear();
        replied.clear();
        ctx.exec_task = native_task;
        ctx.guest_exec_task = task;
        ctx.mem = memory.data();
        ctx.mem_size = (ULONG)memory.size();
        ctx.heap_start = heap_start;
        ctx.heap_end = heap_end;
        assert(Kaffeine_HeapInit(&ctx));
        live = &ctx;
        ctx.m68k_sysbase = exec_base;
        m68k_write32(&ctx, exec_base + M68K_EXECBASE_THISTASK, task);
        exec_gen = poisoned(exec_manual, std::size(exec_manual));
        intuition_gen = poisoned(intuition_manual, std::size(intuition_manual));
        layers_gen = poisoned(kaffeine_thunks_layers_manual, kaffeine_thunks_layers_manual_count);
        graphics_gen = poisoned(graphics_manual, std::size(graphics_manual));
        lib(0, exec_base, "exec.library", exec_manual, std::size(exec_manual), exec_gen);
        lib(1, intuition_base, "intuition.library", intuition_manual, std::size(intuition_manual),
            intuition_gen);
        lib(2, layers_base, "layers.library", kaffeine_thunks_layers_manual,
            kaffeine_thunks_layers_manual_count, layers_gen);
        lib(3, graphics_base, "graphics.library", graphics_manual, std::size(graphics_manual),
            graphics_gen);
        ctx.num_libs = 4;
        m68k_write32(&ctx, stack, 0x76543210);
        m68k_write32(&ctx, tags, TAG_DONE);
        m68k_write32(&ctx, tags + 4, 0);

        /* the screen: its bar on top, behind it the window of another program */
        std::memset(static_cast<void *>(&scr), 0, sizeof(scr));
        std::memset(static_cast<void *>(&foreign_window), 0, sizeof(foreign_window));
        make_layer(bar, 0, 0, 640, 11, LAYERSIMPLE | LAYERBACKDROP);
        make_layer(foreign, 300, 200, 100, 80, LAYERSMART);
        foreign.Window = &foreign_window;
        foreign.BackFill = LAYERS_NOBACKFILL;
        scr.BarLayer = &bar;
        order(scr, { &bar, &foreign });
        heap_initial = avail();
        if (bridge)
            assert(Kaffeine_LayersSetup(&ctx) && ctx.layers && ctx.layer_ops &&
                   ctx.gui_poll == Kaffeine_LayersRefresh);
    }

    ~Fixture()
    {
        Kaffeine_LayersCleanup(&ctx);
        assert(!ctx.layers && !ctx.layer_ops && !ctx.gui_poll);
        assert(held_locks == 0 && info_depth == 0 && ibase_depth == 0);
        assert(native_infos.empty());
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

    /* A literal LVO through the dispatcher; only D0 may change, and no
       generated thunk is ever reached. */
    ULONG call(ULONG base, ULONG lvo, std::initializer_list<ULONG> a = {},
               std::initializer_list<ULONG> d = {})
    {
        unsigned i;

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
        const TestCPU before = state;
        ctx.running = TRUE;
        ctx.exit_code = 0;
        ctx.callback_failed = FALSE;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        auto *previous = ctx.cpu;
        ctx.cpu = &cpu;
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        ctx.cpu = previous;
        assert(result.return_pc == 0x76543210 && !result.supervisor && state.a[7] == stack + 4);
        for (i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc && generated_calls == 0);
        return state.d[0];
    }
    bool stopped() const { return !ctx.running && ctx.exit_code == 20 && state.d[0] == 20; }

    /* The outermost run loop, without guest tasks, for n slices */
    void run(unsigned n, std::function<void()> while_live = {})
    {
        assert(!ctx.sched && !ctx.cpu);
        assert(!before_completion);
        slices = 0;
        slice_limit = n;
        before_completion = std::move(while_live);
        state.d[0] = 0;
        Kaffeine_RunCPU(&ctx, &cpu);
        before_completion = {};
        assert(slices == n && !ctx.cpu);
    }

    ULONG open(NativeWindow &n)
    {
        open_result = &n.window;
        return call(intuition_base, 606, { 0, tags });
    }
    void close(ULONG window)
    {
        assert(call(intuition_base, 72, { window }) == 0 && ctx.running);
    }
    ULONG L(ULONG a) { return m68k_read32(&ctx, a); }
    UWORD W(ULONG a) { return m68k_read16(&ctx, a); }
    ULONG avail() { return Kaffeine_HeapAvail(&ctx, 0); }
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
    std::vector<UBYTE> heap() const
    {
        return std::vector<UBYTE>(memory.begin() + heap_start, memory.begin() + heap_end);
    }
    bool heap_is(const std::vector<UBYTE> &before) const
    {
        return std::equal(before.begin(), before.end(), memory.begin() + heap_start);
    }
};

/* Everything native the bridge could have touched */
struct Counters
{
    unsigned info, ibase, installs, attempts, new_infos, disposed_infos;
    int held;
    size_t depths;

    bool operator==(const Counters &o) const
    {
        return info == o.info && ibase == o.ibase && installs == o.installs &&
               attempts == o.attempts && new_infos == o.new_infos &&
               disposed_infos == o.disposed_infos && held == o.held && depths == o.depths;
    }
};
static Counters counters()
{
    return { info_calls, ibase_calls, native_installs, attempts, new_infos, disposed_infos,
             held_locks, depth_calls.size() };
}

static size_t event_at(const char *name, size_t from = 0)
{
    for (size_t i = from; i < events.size(); ++i)
        if (events[i] == name)
            return i;
    return events.size();
}

/* ── Tests ───────────────────────────────────────────────────────── */

static const M68KStructLayout *layout(const char *name)
{
    for (const M68KStructLayout *l = shadow_gen_layouts; l->name; ++l)
        if (!std::strcmp(l->name, name))
            return l;
    return nullptr;
}

static bool field(const char *name, UWORD offset, UBYTE type)
{
    const M68KStructLayout *l = layout(name);

    assert(l);
    for (const M68KFieldMap *f = l->fields; f->type != SF_END; ++f)
        if (f->m68k_off == offset && f->type == type)
            return true;
    return false;
}

static void test_layout()
{
    /* sizes of the generated classic layouts */
    assert(layout("Layer")->m68k_size == M68K_SIZEOF_LAYER);
    assert(layout("Layer_Info")->m68k_size == M68K_SIZEOF_LAYER_INFO);
    assert(layout("Hook")->m68k_size == M68K_SIZEOF_HOOK);
    assert(layout("Window")->m68k_size == M68K_SIZEOF_WINDOW);
    assert(layout("Screen")->m68k_size == M68K_SIZEOF_SCREEN);
    /* pointer fields of the Layer, its Lock and the fields around it */
    for (UWORD offset : { M68K_LAYER_CLIPRECT, M68K_LAYER_RP, M68K_LAYER_SUPERBITMAP,
                          M68K_LAYER_SUPERCLIPRECT, M68K_LAYER_WINDOW, M68K_LAYER_LAYERINFO,
                          M68K_LAYER_LOCK, M68K_LAYER_LOCK + M68K_SS_WAITQUEUE,
                          M68K_LAYER_LOCK + M68K_SS_OWNER, M68K_LAYER_BACKFILL,
                          M68K_LAYER_CLIPREGION, M68K_LAYER_DAMAGELIST })
        assert(field("Layer", offset, SF_PTR));
    assert(field("Layer", M68K_LAYER_LOCK + M68K_SS_LN_TYPE, SF_BYTE));
    assert(field("Layer", M68K_LAYER_LOCK + M68K_SS_NESTCOUNT, SF_WORD));
    assert(field("Layer", M68K_LAYER_LOCK + M68K_SS_QUEUECOUNT, SF_WORD));
    /* the scalars between them, as graphics/clip.h packs them */
    static_assert(M68K_LAYER_MINX == M68K_LAYER_RP + 4 && M68K_LAYER_RESERVED == M68K_LAYER_MINX + 8 &&
                  M68K_LAYER_PRIORITY == M68K_LAYER_RESERVED + 4 &&
                  M68K_LAYER_FLAGS == M68K_LAYER_PRIORITY + 2 &&
                  M68K_LAYER_SUPERBITMAP == M68K_LAYER_FLAGS + 2 &&
                  M68K_LAYER_SCROLL_X == M68K_LAYER_WINDOW + 4 &&
                  M68K_LAYER_CR == M68K_LAYER_SCROLL_Y + 2 &&
                  M68K_LAYER_LAYERINFO == M68K_LAYER_CR + 20 &&
                  M68K_LAYER_BACKFILL == M68K_LAYER_LOCK + 46 &&
                  M68K_LAYER_WIDTH == M68K_LAYER_SAVECLIPRECTS + 4 &&
                  M68K_LAYER_DAMAGELIST == M68K_LAYER_HEIGHT + 2 + 18 &&
                  M68K_SIZEOF_LAYER == M68K_LAYER_DAMAGELIST + 4, "classic Layer");
    assert(field("Layer_Info", M68K_LI_TOP_LAYER, SF_PTR) && field("Layer_Info", M68K_LI_LOCK, SF_PTR));
    /* the semaphore of a Layer_Info, as NewLayerInfo() mirrors it */
    assert(field("Layer_Info", M68K_LI_LOCK + M68K_SS_LN_TYPE, SF_BYTE) &&
           field("Layer_Info", M68K_LI_LOCK + M68K_SS_NESTCOUNT, SF_WORD) &&
           field("Layer_Info", M68K_LI_LOCK + M68K_SS_WAITQUEUE, SF_PTR) &&
           field("Layer_Info", M68K_LI_LOCK + M68K_SS_OWNER, SF_PTR) &&
           field("Layer_Info", M68K_LI_LOCK + M68K_SS_QUEUECOUNT, SF_WORD));
    static_assert(M68K_LI_LOCK + 46 <= M68K_SIZEOF_LAYER_INFO, "classic Layer_Info");
    assert(field("Window", M68K_WIN_WLAYER, SF_PTR) && field("Window", M68K_WIN_RPORT, SF_PTR));
    assert(field("RastPort", M68K_RP_LAYER, SF_PTR));
    assert(field("Screen", M68K_SCR_RASTPORT + M68K_RP_LAYER, SF_PTR));
    assert(field("Screen", M68K_SCR_LAYERINFO + M68K_LI_TOP_LAYER, SF_PTR));
    assert(field("Screen", M68K_SCR_LAYERINFO + M68K_LI_LOCK, SF_PTR));
    assert(field("Screen", M68K_SCR_BARLAYER, SF_PTR));
}

static void test_registration()
{
    static const ULONG lvos[] =
        { 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96, 102, 108, 114, 120, 126, 132, 138, 144,
          150, 156, 162, 168, 174, 180, 186, 192, 198, 204, 210, 216, 222, 228, 234, 240, 246,
          258, 264, 270 };
    assert(kaffeine_thunks_layers_manual_count == std::size(lvos));
    for (size_t i = 0; i < std::size(lvos); ++i)
    {
        const M68KThunkEntry &e = kaffeine_thunks_layers_manual[i];
        assert(e.lvo == lvos[i] && e.thunk);
        if (e.lvo == 90) assert(e.thunk == Kaffeine_DeleteLayer);
        else if (e.lvo == 186) assert(e.thunk == Kaffeine_CreateUpfrontHookLayer);
        else if (e.lvo == 96) assert(e.thunk == Kaffeine_LockLayer);
        else if (e.lvo == 102) assert(e.thunk == Kaffeine_UnlockLayer);
        else if (e.lvo == 120) assert(e.thunk == Kaffeine_LockLayerInfo);
        else if (e.lvo == 138) assert(e.thunk == Kaffeine_UnlockLayerInfo);
        else if (e.lvo == 144) assert(e.thunk == Kaffeine_NewLayerInfo);
        else if (e.lvo == 150) assert(e.thunk == Kaffeine_DisposeLayerInfo);
        else if (e.lvo == 174) assert(e.thunk == Kaffeine_InstallClipRegion);
        else if (e.lvo == 198) assert(e.thunk == Kaffeine_InstallLayerHook);
        else if (e.lvo == 204) assert(e.thunk == Kaffeine_InstallLayerInfoHook);
        else assert(e.thunk == kaffeine_layers_Unsupported);
    }
    assert(!kaffeine_thunks_layers_manual[std::size(lvos)].lvo &&
           !kaffeine_thunks_layers_manual[std::size(lvos)].thunk);
    assert(std::size(graphics_manual) == 16 && graphics_manual[5].lvo == 30 &&
           graphics_manual[5].thunk == Kaffeine_BltBitMap && graphics_manual[0].lvo == 432 &&
           graphics_manual[0].thunk == Kaffeine_LockLayerRom && graphics_manual[1].lvo == 438 &&
           graphics_manual[1].thunk == Kaffeine_UnlockLayerRom && graphics_manual[2].lvo == 444 &&
           graphics_manual[3].lvo == 450 && graphics_manual[2].thunk == kaffeine_layers_Unsupported &&
           graphics_manual[3].thunk == kaffeine_layers_Unsupported && graphics_manual[4].lvo == 654 &&
           graphics_manual[4].thunk == Kaffeine_AttemptLockLayerRom);
    const M68KThunkEntry depth_table[] = { KAFFEINE_INTUITION_LAYER_THUNKS };
    assert(depth_table[0].lvo == 306 && depth_table[0].thunk == Kaffeine_WindowToBack &&
           depth_table[1].lvo == 312 && depth_table[1].thunk == Kaffeine_WindowToFront);
}

static void test_identity()
{
    NativeWindow a;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    assert(mw && f.ctx.running);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER), rp = f.L(mw + M68K_WIN_RPORT);
    assert(la && rp && f.L(rp + M68K_RP_LAYER) == la);
    const ULONG info = f.L(la + M68K_LAYER_LAYERINFO);
    assert(info);
    const ULONG gbar = f.L(info + M68K_LI_TOP_LAYER), gfor = f.L(la + M68K_LAYER_BACK);
    assert(gbar && gfor && gbar != la && gfor != la && gbar != gfor);
    /* the links of the native list, front to back */
    assert(f.L(gbar + M68K_LAYER_FRONT) == 0 && f.L(gbar + M68K_LAYER_BACK) == la);
    assert(f.L(la + M68K_LAYER_FRONT) == gbar && f.L(gfor + M68K_LAYER_FRONT) == la);
    assert(f.L(gfor + M68K_LAYER_BACK) == 0);
    /* scalars at their classic offsets */
    assert(f.W(la + M68K_LAYER_MINX) == 10 && f.W(la + M68K_LAYER_MINY) == 20);
    assert(f.W(la + M68K_LAYER_MAXX) == 209 && f.W(la + M68K_LAYER_MAXY) == 119);
    assert(f.W(la + M68K_LAYER_WIDTH) == 200 && f.W(la + M68K_LAYER_HEIGHT) == 100);
    assert(f.W(la + M68K_LAYER_FLAGS) == LAYERSMART && f.W(la + M68K_LAYER_PRIORITY) == 0);
    assert(f.W(la + M68K_LAYER_SCROLL_X) == 3 && f.W(la + M68K_LAYER_SCROLL_Y) == 0xFFFC);
    assert(f.W(gbar + M68K_LAYER_FLAGS) == (LAYERSIMPLE | LAYERBACKDROP) &&
           f.W(gbar + M68K_LAYER_MAXX) == 639);
    /* the program's own objects, nothing of the other program */
    assert(f.L(la + M68K_LAYER_WINDOW) == mw && f.L(la + M68K_LAYER_RP) == rp);
    assert(f.L(gfor + M68K_LAYER_WINDOW) == 0 && f.L(gfor + M68K_LAYER_RP) == 0);
    for (ULONG g : { gbar, la, gfor })
    {
        assert(f.L(g + M68K_LAYER_LAYERINFO) == info);
        /* no native pointer, clip list, region or bitmap */
        assert(!f.L(g + M68K_LAYER_CLIPRECT) && !f.L(g + M68K_LAYER_RESERVED) &&
               !f.L(g + M68K_LAYER_SUPERBITMAP) && !f.L(g + M68K_LAYER_SUPERCLIPRECT));
        assert(f.zero(g + M68K_LAYER_CR, 20) && !f.L(g + M68K_LAYER_RESERVED1) &&
               !f.L(g + M68K_LAYER_CLIPREGION) && !f.L(g + M68K_LAYER_SAVECLIPRECTS) &&
               !f.L(g + M68K_LAYER_DAMAGELIST) && f.zero(g + M68K_LAYER_HEIGHT + 2, 18));
        /* a guest semaphore, as InitSemaphore() leaves it */
        const ULONG ss = g + M68K_LAYER_LOCK, q = ss + M68K_SS_WAITQUEUE;
        assert(f.memory[ss + M68K_SS_LN_TYPE] == 15 && f.W(ss + M68K_SS_NESTCOUNT) == 0);
        assert(f.L(q) == q + 4 && f.L(q + 4) == 0 && f.L(q + 8) == q);
        assert(f.L(ss + M68K_SS_OWNER) == 0 && f.W(ss + M68K_SS_QUEUECOUNT) == 0xFFFF);
        /* never a shadow: no generic thunk resolves a 68k Layer to a native one */
        assert(!f.shadowed(g) && m68k_to_host_or_shadow(&f.ctx, g) == f.ctx.mem + g);
    }
    /* BackFill: a handle of Intuition's hook, the sentinels as they are */
    const ULONG handle = f.L(la + M68K_LAYER_BACKFILL);
    assert(handle > 1 && handle >= Fixture::heap_start && handle < Fixture::heap_end);
    assert(!f.shadowed(handle));
    assert(f.L(gbar + M68K_LAYER_BACKFILL) == 0 && f.L(gfor + M68K_LAYER_BACKFILL) == 1);
    /* the LayerInfo of a screen without a 68k Screen is the bridge's own */
    assert(!f.shadowed(info) && f.memory[info + M68K_LI_LOCK + M68K_SS_LN_TYPE] == 15);
    /* identities stay: a new snapshot of the same list changes nothing */
    const auto before = f.heap();
    const unsigned infos = info_calls;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls == infos + 1 && f.heap_is(before));
    /* the window closes: its 68k Layer goes before the native layer, the
       rest of the snapshot with its last holder */
    zero_at_close = la;
    f.close(mw);
    assert(closed.size() == 1 && closed[0] == &a.window);
    for (ULONG g : { gbar, la, gfor, info })
        assert(f.zero(g, M68K_SIZEOF_LAYER_INFO));
    assert(f.zero(handle, M68K_SIZEOF_HOOK) && f.avail() == f.heap_initial);
}

static void test_async_depth_and_poll()
{
    NativeWindow a;
    IntuiMessage im;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER);
    const ULONG info = f.L(la + M68K_LAYER_LAYERINFO);
    const ULONG gbar = f.L(la + M68K_LAYER_FRONT), gfor = f.L(la + M68K_LAYER_BACK);
    assert(gbar && gfor && f.L(info + M68K_LI_TOP_LAYER) == gbar);

    /* WindowToBack(): Intuition gets the window; the snapshot taken right
       after still shows the list as it is, since nothing has moved yet */
    unsigned infos = info_calls;
    assert(f.call(Fixture::intuition_base, 306, { mw }) == 0 && f.ctx.running);
    assert(depth_calls.size() == 1 && depth_calls[0].window == &a.window && !depth_calls[0].front);
    assert(info_calls == infos + 1);
    assert(f.L(la + M68K_LAYER_BACK) == gfor && f.L(gfor + M68K_LAYER_BACK) == 0);
    /* Intuition's input handler moves it; no snapshot sees it yet */
    order(scr, { &bar, &foreign, &a.layer });
    assert(f.L(la + M68K_LAYER_BACK) == gfor);

    /* The program polls in a loop: no guest tasks, no thunk. The poll of
       the outermost loop comes with the 4096th complete slice. */
    assert(!f.ctx.sched && !f.ctx.task_ops && f.ctx.gui_poll_slices == 0);
    infos = info_calls;
    f.run(KAFFEINE_GUI_POLL_SLICES);
    assert(info_calls == infos && f.ctx.gui_poll_slices == KAFFEINE_GUI_POLL_SLICES - 1);
    assert(f.L(la + M68K_LAYER_BACK) == gfor);
    f.run(2);
    assert(info_calls == infos + 1 && f.ctx.gui_poll_slices == 0);
    assert(f.L(gbar + M68K_LAYER_BACK) == gfor && f.L(gfor + M68K_LAYER_FRONT) == gbar);
    assert(f.L(gfor + M68K_LAYER_BACK) == la && f.L(la + M68K_LAYER_FRONT) == gfor);
    assert(f.L(la + M68K_LAYER_BACK) == 0 && f.L(info + M68K_LI_TOP_LAYER) == gbar);
    /* the same identities */
    assert(f.L(mw + M68K_WIN_WLAYER) == la && f.L(la + M68K_LAYER_WINDOW) == mw);

    /* WindowToFront(), then a message of the window: WindowSync() takes the
       snapshot again once Intuition has moved it */
    assert(f.call(Fixture::intuition_base, 312, { mw }) == 0 && depth_calls.back().front);
    assert(f.L(la + M68K_LAYER_BACK) == 0);
    order(scr, { &bar, &a.layer, &foreign });
    std::memset(static_cast<void *>(&im), 0, sizeof(im));
    im.Class = IDCMP_INTUITICKS;
    im.IDCMPWindow = &a.window;
    queues[&a.port].push_back(&im);
    const ULONG msg = f.call(Fixture::exec_base, 372, { f.L(mw + M68K_WIN_USERPORT) });
    assert(msg && f.ctx.running);
    assert(f.L(gbar + M68K_LAYER_BACK) == la && f.L(la + M68K_LAYER_BACK) == gfor);
    assert(f.L(gfor + M68K_LAYER_FRONT) == la && f.L(gfor + M68K_LAYER_BACK) == 0);
    f.call(Fixture::exec_base, 378, { 0, msg });
    assert(replied.size() == 1 && replied[0] == &im);

    /* NULL does nothing, as in Intuition */
    assert(f.call(Fixture::intuition_base, 306, { 0 }) == 0 && f.ctx.running && depth_calls.size() == 2);
    f.close(mw);
    assert(f.avail() == f.heap_initial);
}

static void test_busy_snapshot()
{
    NativeWindow a;
    Fixture f;
    make_window(a,scr,10,20,200,100);
    order(scr,{ &bar,&a.layer,&foreign });
    const ULONG mw=f.open(a);
    const ULONG la=f.L(mw+M68K_WIN_WLAYER), previous=f.L(la+M68K_LAYER_BACK);
    auto before=f.heap();
    unsigned calls=info_calls;
    order(scr,{ &bar,&foreign,&a.layer });
    info_busy=true;
    f.run(KAFFEINE_GUI_POLL_SLICES+1);
    assert(info_calls==calls && f.heap_is(before) && f.L(la+M68K_LAYER_BACK)==previous);
    info_busy=false;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls==calls+1 && f.L(la+M68K_LAYER_BACK)==0);
    f.close(mw);
    assert(f.avail()==f.heap_initial);
}

static void test_backdrop()
{
    NativeWindow a;
    IntuiMessage im;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER);
    assert(f.L(mw + M68K_WIN_FLAGS) == 0x1002 && f.W(la + M68K_LAYER_FLAGS) == LAYERSMART);

    /* The program sets both backdrop bits, and changes bits it may not. */
    m68k_write32(&f.ctx, mw + M68K_WIN_FLAGS, 0x1002 | WFLG_BACKDROP | 0x4);
    m68k_write16(&f.ctx, la + M68K_LAYER_FLAGS, LAYERSMART | LAYERBACKDROP | LAYERSUPER);
    /* A message and the poll keep the pending backdrop bits, and only them;
       nothing native changes before a depth call. */
    std::memset(static_cast<void *>(&im), 0, sizeof(im));
    im.Class = IDCMP_INTUITICKS;
    im.IDCMPWindow = &a.window;
    queues[&a.port].push_back(&im);
    const ULONG msg = f.call(Fixture::exec_base, 372, { f.L(mw + M68K_WIN_USERPORT) });
    assert(msg);
    assert(f.L(mw + M68K_WIN_FLAGS) == (0x1002 | WFLG_BACKDROP));
    assert(f.W(la + M68K_LAYER_FLAGS) == (LAYERSMART | LAYERBACKDROP));
    f.call(Fixture::exec_base, 378, { 0, msg });
    Kaffeine_LayersRefresh(&f.ctx);
    f.run(KAFFEINE_GUI_POLL_SLICES + 1);
    assert(f.L(mw + M68K_WIN_FLAGS) == (0x1002 | WFLG_BACKDROP));
    assert(f.W(la + M68K_LAYER_FLAGS) == (LAYERSMART | LAYERBACKDROP));
    assert(a.window.Flags == 0x1002 && a.layer.Flags == LAYERSMART && ibase_calls == 0);

    /* WindowToBack(): just those bits, under LockIBase() and LockLayer(),
       before Intuition gets the window; every other native flag stays */
    events.clear();
    assert(f.call(Fixture::intuition_base, 306, { mw }) == 0 && f.ctx.running);
    assert(ibase_calls == 1 && ibase_depth == 0 && held_locks == 0);
    assert(depth_calls.size() == 1 && depth_calls[0].wflags == (0x1002 | WFLG_BACKDROP) &&
           depth_calls[0].lflags == (LAYERSMART | LAYERBACKDROP));
    assert(a.window.Flags == (0x1002 | WFLG_BACKDROP) && a.layer.Flags == (LAYERSMART | LAYERBACKDROP));
    assert(event_at("LockLayer") < event_at("UnlockLayer") &&
           event_at("UnlockLayer") < event_at("WindowToBack"));
    /* now native, they follow Intuition again */
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.W(la + M68K_LAYER_FLAGS) == (LAYERSMART | LAYERBACKDROP));

    /* The program clears them: the next depth call clears them natively. */
    m68k_write32(&f.ctx, mw + M68K_WIN_FLAGS, f.L(mw + M68K_WIN_FLAGS) & ~(ULONG)WFLG_BACKDROP);
    m68k_write16(&f.ctx, la + M68K_LAYER_FLAGS, (UWORD)(f.W(la + M68K_LAYER_FLAGS) & ~LAYERBACKDROP));
    assert(f.call(Fixture::intuition_base, 312, { mw }) == 0);
    assert(a.window.Flags == 0x1002 && a.layer.Flags == LAYERSMART && ibase_calls == 2);
    assert(depth_calls.back().front && depth_calls.back().wflags == 0x1002 &&
           depth_calls.back().lflags == LAYERSMART);

    /* Without a change of the program, a depth call changes no native flag. */
    events.clear();
    assert(f.call(Fixture::intuition_base, 306, { mw }) == 0);
    assert(ibase_calls == 2 && event_at("LockLayer") == events.size());
    /* Native changes show when the program changed nothing. */
    a.layer.Flags |= LAYERBACKDROP;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.W(la + M68K_LAYER_FLAGS) == (LAYERSMART | LAYERBACKDROP));
    a.layer.Flags = LAYERSMART;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.W(la + M68K_LAYER_FLAGS) == LAYERSMART);
    f.close(mw);
}

static void test_hooks()
{
    NativeWindow a, c;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    make_window(c, scr, 50, 60, 100, 100);
    c.layer.BackFill = &other_hook;
    order(scr, { &bar, &a.layer, &c.layer, &foreign });
    const ULONG ma = f.open(a), mc = f.open(c);
    const ULONG la = f.L(ma + M68K_WIN_WLAYER), lc = f.L(mc + M68K_WIN_WLAYER);
    const ULONG gbar = f.L(la + M68K_LAYER_FRONT), gfor = f.L(lc + M68K_LAYER_BACK);
    const ULONG info = f.L(la + M68K_LAYER_LAYERINFO);
    const ULONG ha = f.L(la + M68K_LAYER_BACKFILL), hc = f.L(lc + M68K_LAYER_BACKFILL);
    assert(ha > 1 && hc > 1 && ha != hc && f.L(gfor + M68K_LAYER_BACKFILL) == 1);
    unsigned n = native_installs;

    /* LAYERS_NOBACKFILL: the previous hook comes back as its handle */
    assert(f.call(Fixture::layers_base, 198, { la, 1 }) == ha && f.ctx.running);
    assert(a.layer.BackFill == LAYERS_NOBACKFILL && f.L(la + M68K_LAYER_BACKFILL) == 1);
    /* LAYERS_BACKFILL */
    assert(f.call(Fixture::layers_base, 198, { la, 0 }) == 1);
    assert(a.layer.BackFill == nullptr && f.L(la + M68K_LAYER_BACKFILL) == 0);
    /* the handle installs the very native hook again, at full host width */
    assert(f.call(Fixture::layers_base, 198, { la, ha }) == 0);
    assert(a.layer.BackFill == &intuition_hook && f.L(la + M68K_LAYER_BACKFILL) == ha);
    assert(native_installs == n + 3 && held_locks == 0);
    /* and again: the same handle for the same hook */
    assert(f.call(Fixture::layers_base, 198, { la, ha }) == ha && a.layer.BackFill == &intuition_hook);

    /* refusals, before anything native */
    const ULONG guest_hook = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_HOOK, MEMF_CLEAR);
    assert(guest_hook);
    m68k_write32(&f.ctx, guest_hook + 8, 0x5000);   /* h_Entry: 68k code */
    const Counters before = counters();
    struct { ULONG layer, hook; } bad[] =
    {
        { la, guest_hook },     /* a 68k Hook would run guest code in layers */
        { la, hc },             /* the handle of another layer's hook */
        { la, 0x5000 },
        { gfor, 0 },            /* the layer of another program */
        { gbar, 1 },            /* the screen bar */
        { info, 0 },            /* a LayerInfo is no Layer */
        { ma, 0 },              /* a Window is no Layer */
        { 0x1234, 0 }
    };
    for (const auto &b : bad)
    {
        assert(f.call(Fixture::layers_base, 198, { b.layer, b.hook }) == 20 && f.stopped());
        assert(counters() == before);
    }
    assert(a.layer.BackFill == &intuition_hook && foreign.BackFill == LAYERS_NOBACKFILL &&
           bar.BackFill == nullptr && c.layer.BackFill == &other_hook);
    /* NULL: nothing, as in layers.library */
    assert(f.call(Fixture::layers_base, 198, { 0, 1 }) == 0 && f.ctx.running && counters() == before);
    /* InstallLayerInfoHook() stops until it is bridged */
    assert(f.call(Fixture::layers_base, 204, { info, 0 }) == 20 && f.stopped() && counters() == before);
    Kaffeine_HeapFree(&f.ctx, guest_hook, M68K_SIZEOF_HOOK);

    /* handles go with their layer */
    f.close(ma);
    assert(f.zero(ha, M68K_SIZEOF_HOOK));
    assert(f.call(Fixture::layers_base, 198, { lc, ha }) == 20 && f.stopped() &&
           c.layer.BackFill == &other_hook);
    assert(f.call(Fixture::layers_base, 198, { lc, 0 }) == hc && c.layer.BackFill == nullptr);
    assert(f.call(Fixture::layers_base, 198, { lc, hc }) == 0 && c.layer.BackFill == &other_hook);
    f.close(mc);
    assert(f.avail() == f.heap_initial);
}

static void test_refusals()
{
    NativeWindow a;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER), info = f.L(la + M68K_LAYER_LAYERINFO);
    const ULONG gfor = f.L(la + M68K_LAYER_BACK);
    const Counters before = counters();
    const auto heap = f.heap();

    /* every other layers.library slot stops before any native call, with
       68k Layers, LayerInfos and Hooks in every register */
    for (ULONG i = 0; i < kaffeine_thunks_layers_manual_count; ++i)
    {
        const M68KThunkEntry &e = kaffeine_thunks_layers_manual[i];
        if (e.thunk != kaffeine_layers_Unsupported)
            continue;
        assert(f.call(Fixture::layers_base, e.lvo, { la, la, info, la, la, la, la },
                      { 0, 0, 10, 10, 0, 0, 0, 0 }) == 20 && f.stopped());
        assert(counters() == before && f.heap_is(heap));
    }
    /* SyncSBitMap() and CopySBitMap() */
    for (ULONG lvo : { 444u, 450u })
    {
        assert(f.call(Fixture::graphics_base, lvo, { la }) == 20 && f.stopped());
        assert(counters() == before && f.heap_is(heap));
    }
    /* depth calls of anything but a window of the program */
    for (ULONG w : { la, gfor, info, 0x5555u, 0x3A00u })
    {
        assert(f.call(Fixture::intuition_base, 306, { w }) == 20 && f.stopped());
        assert(f.call(Fixture::intuition_base, 312, { w }) == 20 && f.stopped());
    }
    assert(counters() == before && f.heap_is(heap));
    /* layer locks of anything but the layer of a window of the program */
    for (ULONG l : { gfor, f.L(info + M68K_LI_TOP_LAYER), info, mw, 0x5555u, 0u })
    {
        assert(f.call(Fixture::layers_base, 96, { 0, l }) == 20 && f.stopped());
        assert(f.call(Fixture::layers_base, 102, { l }) == 20 && f.stopped());
        assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, l }) == 20 && f.stopped());
        assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, l }) == 20 && f.stopped());
        assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, l }) == 20 && f.stopped());
    }
    /* an unlock without a lock of the program */
    assert(f.call(Fixture::layers_base, 102, { la }) == 20 && f.stopped());
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, la }) == 20 && f.stopped());
    assert(counters() == before && f.heap_is(heap));
    f.close(mw);
}

static void test_windows_and_locks()
{
    NativeWindow a, c;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    make_window(c, scr, 50, 60, 100, 100);
    order(scr, { &bar, &a.layer, &c.layer, &foreign });
    const ULONG ma = f.open(a), mc = f.open(c);
    const ULONG la = f.L(ma + M68K_WIN_WLAYER), lc = f.L(mc + M68K_WIN_WLAYER);
    const ULONG info = f.L(la + M68K_LAYER_LAYERINFO), gbar = f.L(info + M68K_LI_TOP_LAYER);
    const ULONG gfor = f.L(lc + M68K_LAYER_BACK);
    /* two owned 68k Layers in one coherent list, one LayerInfo */
    assert(la && lc && la != lc && f.L(lc + M68K_LAYER_LAYERINFO) == info);
    assert(f.L(gbar + M68K_LAYER_BACK) == la && f.L(la + M68K_LAYER_BACK) == lc);
    assert(f.L(lc + M68K_LAYER_FRONT) == la && f.L(gfor + M68K_LAYER_FRONT) == lc);
    assert(f.L(la + M68K_LAYER_WINDOW) == ma && f.L(lc + M68K_LAYER_WINDOW) == mc);

    /* nested program locks of its own layers */
    assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, la }) == 0 && f.ctx.running);
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && layer_locks[&a.layer] == 2);
    /* no LayerInfo lock after a layer lock: the poll and the snapshot of a
       depth call wait until the program lets go */
    unsigned infos = info_calls;
    order(scr, { &bar, &c.layer, &a.layer, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    f.run(KAFFEINE_GUI_POLL_SLICES + 1, [&] {
        /* The periodic poll has run while the root still owns both locks.
           A depth call in that same live scope must not take LayerInfo. */
        assert(f.ctx.running && f.ctx.cpu == &f.cpu && held_locks == 2);
        assert(info_calls == infos && f.L(la + M68K_LAYER_BACK) == lc);
        assert(f.call(Fixture::intuition_base, 306, { mc }) == 0 && depth_calls.size() == 1);
        assert(info_calls == infos && f.L(la + M68K_LAYER_BACK) == lc);
    });
    /* A completed root retires its scope before its CPU returns. The
       remaining independent adapter checks need a freshly acquired scope. */
    assert(!f.ctx.running && held_locks == 0 && layer_locks[&a.layer] == 0);
    assert(info_calls == infos && f.L(la + M68K_LAYER_BACK) == lc);
    assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, la }) == 0);
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && layer_locks[&a.layer] == 2);
    /* AttemptLockLayerRom(), as the native one answers */
    attempt_fails = true;
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, lc }) == FALSE && layer_locks[&c.layer] == 0);
    attempt_fails = false;
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, lc }) == TRUE && layer_locks[&c.layer] == 1);
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, lc }) == 0 && layer_locks[&c.layer] == 0);
    assert(f.call(Fixture::layers_base, 102, { la }) == 0 && layer_locks[&a.layer] == 1);
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, la }) == 0 && held_locks == 0);
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls == infos + 1 && f.L(gbar + M68K_LAYER_BACK) == lc && f.L(lc + M68K_LAYER_BACK) == la);

    /* A window closes with a lock that the program left: the lock and its
       68k Layer go before the native window; its neighbours close up as
       DeleteLayer() takes it out, and the next snapshot agrees. */
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && held_locks == 1);
    events.clear();
    zero_at_close = la;
    f.close(ma);
    zero_at_close = 0;
    assert(event_at("UnlockLayer") < event_at("CloseWindow") && event_at("CloseWindow") < events.size());
    assert(f.L(lc + M68K_LAYER_BACK) == gfor && f.L(gfor + M68K_LAYER_FRONT) == lc);
    std::vector<UBYTE> snapshot(f.memory.begin() + lc, f.memory.begin() + lc + M68K_SIZEOF_LAYER);
    Kaffeine_LayersRefresh(&f.ctx);
    assert(std::equal(snapshot.begin(), snapshot.end(), f.memory.begin() + lc));
    /* the top window closes: the LayerInfo's top follows */
    order(scr, { &c.layer, &bar, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(info + M68K_LI_TOP_LAYER) == lc);
    f.close(mc);
    assert(f.avail() == f.heap_initial);
}

static void closing_unlocked(KaffeineContext *, Window *)
{
    assert(held_locks == 0);
    events.push_back("closing hook");
}

static void test_review_lifetimes()
{
    NativeWindow a, b;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    make_window(b, scr, 40, 50, 200, 100);
    order(scr, { &bar, &a.layer, &b.layer, &foreign });
    const ULONG ma = f.open(a), mb = f.open(b);
    const ULONG la = f.L(ma + M68K_WIN_WLAYER), lb = f.L(mb + M68K_WIN_WLAYER);
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0);
    m68k_write32(&f.ctx, mb + M68K_WIN_FLAGS, f.L(mb + M68K_WIN_FLAGS) ^ WFLG_BACKDROP);
    m68k_write16(&f.ctx, lb + M68K_LAYER_FLAGS, f.W(lb + M68K_LAYER_FLAGS) ^ LAYERBACKDROP);
    const Counters before = counters();
    assert(f.call(Fixture::intuition_base, 312, { mb }) == 20 && f.stopped());
    assert(counters() == before); /* no IBase or second native layer lock */
    assert(f.call(Fixture::layers_base, 102, { la }) == 0);
    assert(f.call(Fixture::intuition_base, 312, { mb }) == 0 && f.ctx.running);
    assert((b.window.Flags & WFLG_BACKDROP) && (b.layer.Flags & LAYERBACKDROP));

    /* A close must drop layer locks before menu and gadget hooks that may
       wait for Intuition. The ordinary public delete path still does so. */
    static KaffeineWindowGadgets gadgets = {};
    gadgets.closing = closing_unlocked;
    f.ctx.menu_closing = closing_unlocked;
    f.ctx.window_gadgets = &gadgets;
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0);
    events.clear();
    f.close(ma);
    assert(event_at("UnlockLayer") < event_at("closing hook"));
    assert(event_at("closing hook") < event_at("CloseWindow"));
    f.close(mb);
    /* The explicit requester API must use the same prepare-before-hooks
       order as CloseWindow and context teardown. */
    f.ctx.window_gadgets = nullptr;
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG request = f.open(a);
    assert(request && Kaffeine_ShadowNative(&f.ctx, request, KAFFEINE_SHADOW_WINDOW) == &a.window);
    assert(Kaffeine_ShadowAdd(&f.ctx, request, &a.window, KAFFEINE_SHADOW_REQUESTER_WINDOW));
    const ULONG request_layer = f.L(request + M68K_WIN_WLAYER);
    f.ctx.window_gadgets = &gadgets;
    assert(f.call(Fixture::layers_base, 96, { 0, request_layer }) == 0);
    assert(Kaffeine_ShadowNative(&f.ctx, request, KAFFEINE_SHADOW_REQUESTER_WINDOW) == &a.window);
    events.clear();
    assert(f.call(Fixture::intuition_base, 372, { request }) == 0 && f.ctx.running);
    assert(event_at("UnlockLayer") < event_at("closing hook"));
    assert(event_at("closing hook") < event_at("FreeSysRequest"));
    assert(f.avail() == f.heap_initial);
}

static void test_retired_screen_recovery()
{
    NativeWindow a;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_SCREEN));
    const ULONG li = sg + M68K_SCR_LAYERINFO, ss = li + M68K_LI_LOCK;
    Kaffeine_LayerSemaphoreInit(&f.ctx, ss);
    f.ctx.pubscreens[0].native = &scr;
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].owned = TRUE;
    f.ctx.layer_ops->screen_mirrored(&f.ctx, &scr);
    const ULONG mw = f.open(a), layer = f.L(mw + M68K_WIN_WLAYER);
    m68k_write32(&f.ctx, ss + M68K_SS_OWNER, 0x12348);
    m68k_write16(&f.ctx, ss + M68K_SS_NESTCOUNT, 1);
    m68k_write16(&f.ctx, ss + M68K_SS_QUEUECOUNT, 0);
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    assert(!f.L(layer + M68K_LAYER_LAYERINFO));
    assert(f.call(Fixture::intuition_base, 312, { mw }) == 0);
    assert(f.L(layer + M68K_LAYER_LAYERINFO) == li && f.L(li + M68K_LI_TOP_LAYER));
    assert(f.L(ss + M68K_SS_OWNER) == 0x12348 && f.W(ss + M68K_SS_NESTCOUNT) == 1);
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    f.ctx.layer_ops->window_synced(&f.ctx, mw, &a.window);
    assert(f.L(layer + M68K_LAYER_LAYERINFO) == li && f.L(li + M68K_LI_TOP_LAYER));
    assert(f.L(ss + M68K_SS_OWNER) == 0x12348 && f.W(ss + M68K_SS_QUEUECOUNT) == 0);
    f.close(mw);
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    f.ctx.pubscreens[0] = {};
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_SCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    assert(f.avail() == f.heap_initial);
}

static void test_borrowed_address_reuse()
{
    NativeWindow anchor, fresh;
    Fixture f;
    make_window(anchor, scr, 10, 20, 200, 100);
    order(scr, { &bar, &anchor.layer, &foreign });
    const ULONG ma = f.open(anchor), la = f.L(ma + M68K_WIN_WLAYER);
    const ULONG old = f.L(la + M68K_LAYER_BACK);
    assert(old);
    m68k_write16(&f.ctx, old + M68K_LAYER_FLAGS, LAYERSIMPLE | LAYERBACKDROP);
    const ULONG old_front = f.L(old + M68K_LAYER_FRONT);
    const ULONG old_info = f.L(old + M68K_LAYER_LAYERINFO);
    const ULONG ss = old + M68K_LAYER_LOCK;
    m68k_write32(&f.ctx, ss + M68K_SS_OWNER, 0x12348);
    m68k_write16(&f.ctx, ss + M68K_SS_NESTCOUNT, 1);
    make_window(fresh, scr, 30, 40, 120, 80);
    /* Native allocator reuses the address of a vanished foreign layer. */
    fresh.window.WLayer = &foreign;
    fresh.rp.Layer = &foreign;
    foreign.rp = &fresh.rp;
    foreign.Window = &fresh.window;
    foreign.Flags = LAYERSIMPLE;
    order(scr, { &bar, &foreign, &anchor.layer });
    info_busy = true;
    const ULONG mw = f.open(fresh), layer = f.L(mw + M68K_WIN_WLAYER);
    assert(layer == old && f.L(old + M68K_LAYER_FRONT) == old_front);
    assert(f.L(old + M68K_LAYER_LAYERINFO) == old_info);
    assert(f.L(ss + M68K_SS_OWNER) == 0x12348 && f.W(ss + M68K_SS_NESTCOUNT) == 1);
    assert(f.call(Fixture::intuition_base, 312, { mw }) == 0 && f.ctx.running);
    assert(!(foreign.Flags & LAYERBACKDROP));
    info_busy = false;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(!(f.W(layer + M68K_LAYER_FLAGS) & LAYERBACKDROP));
    assert(f.L(ss + M68K_SS_OWNER) == 0x12348 && f.W(ss + M68K_SS_NESTCOUNT) == 1);
    f.close(mw);
    f.close(ma);
    assert(f.avail() == f.heap_initial);
}

static void test_guest_lock_owner()
{
    NativeWindow a;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a), layer = f.L(mw + M68K_WIN_WLAYER);
    const ULONG task_addr = Fixture::exec_base + M68K_EXECBASE_THISTASK;
    const ULONG owner = f.L(task_addr);
    assert(owner && f.call(Fixture::layers_base, 96, { 0, layer }) == 0);
    const Counters before = counters();
    m68k_write32(&f.ctx, task_addr, owner + 0x100);
    /* the blocking locks cannot wait for the other guest task */
    assert(f.call(Fixture::layers_base, 96, { 0, layer }) == 20 && f.stopped());
    assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, layer }) == 20 && f.stopped());
    assert(f.call(Fixture::layers_base, 102, { layer }) == 20 && f.stopped());
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, layer }) == 20 && f.stopped());
    /* AttemptLockLayerRom() never blocks: the lock is busy, FALSE, the
       program runs on, and the native one is never asked */
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, layer }) == FALSE && f.ctx.running);
    assert(counters() == before && layer_locks[&a.layer] == 1);
    assert(f.call(Fixture::layers_base, 198, { layer, 0 }) == 20 && f.stopped());
    assert(f.call(Fixture::intuition_base, 312, { mw }) == 20 && f.stopped());
    assert(counters() == before && held_locks == 1);
    m68k_write32(&f.ctx, task_addr, owner);
    /* the owner itself still nests, also through the attempt */
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, layer }) == TRUE && f.ctx.running);
    assert(attempts == before.attempts + 1 && layer_locks[&a.layer] == 2);
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, layer }) == 0 && held_locks == 1);
    assert(f.call(Fixture::layers_base, 102, { layer }) == 0 && held_locks == 0);
    f.close(mw);
    assert(f.avail() == f.heap_initial);
}

static void test_borrowed()
{
    NativeWindow a;
    Window other = {};
    Layer other_layer, border;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    make_layer(other_layer, 400, 300, 50, 50, LAYERSIMPLE);
    other_layer.Window = &other;
    order(scr, { &bar, &other_layer, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER), info = f.L(la + M68K_LAYER_LAYERINFO);
    const ULONG gbar = f.L(info + M68K_LI_TOP_LAYER), go = f.L(la + M68K_LAYER_FRONT);
    assert(go && go != gbar && f.L(go + M68K_LAYER_WINDOW) == 0 && f.W(go + M68K_LAYER_MINX) == 400);

    /* the other program closes its window: the next snapshot drops its
       borrowed 68k Layer, and the list closes up */
    order(scr, { &bar, &a.layer, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(la + M68K_LAYER_FRONT) == gbar && f.L(gbar + M68K_LAYER_BACK) == la);
    assert(f.zero(go, M68K_SIZEOF_LAYER));
    /* a new layer appears in front: a 68k Layer of its own */
    order(scr, { &other_layer, &bar, &a.layer, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG top = f.L(info + M68K_LI_TOP_LAYER);
    assert(top && top != gbar && f.L(top + M68K_LAYER_BACK) == gbar && f.L(gbar + M68K_LAYER_FRONT) == top);

    /* the border layer of the program's own GZZ window: it names the 68k
       Window, but the program can neither lock nor change it */
    make_layer(border, 8, 18, 204, 104, LAYERSIMPLE);
    border.Window = &a.window;
    order(scr, { &bar, &border, &a.layer, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG gb = f.L(la + M68K_LAYER_FRONT);
    assert(gb && gb != gbar && f.L(gb + M68K_LAYER_WINDOW) == mw && f.L(gb + M68K_LAYER_RP) == 0);
    const Counters before = counters();
    assert(f.call(Fixture::layers_base, 198, { gb, 0 }) == 20 && f.stopped());
    assert(f.call(Fixture::layers_base, 96, { 0, gb }) == 20 && f.stopped());
    m68k_write16(&f.ctx, gb + M68K_LAYER_FLAGS, LAYERSIMPLE | LAYERBACKDROP);
    assert(f.call(Fixture::intuition_base, 306, { mw }) == 0);
    assert(border.Flags == LAYERSIMPLE && ibase_calls == before.ibase && held_locks == 0);
    f.close(mw);
    assert(f.zero(gb, M68K_SIZEOF_LAYER) && f.avail() == f.heap_initial);
}

static void test_screen_fields()
{
    NativeWindow a;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    /* a 68k Screen of the program, as LockPubScreen() makes it */
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg);
    Kaffeine_LayerSemaphoreInit(&f.ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_PUBSCREEN));
    f.ctx.pubscreens[0].native = &scr;
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].locks = 1;
    f.ctx.layer_ops->screen_mirrored(&f.ctx, &scr);
    const ULONG li = sg + M68K_SCR_LAYERINFO, gbar = f.L(sg + M68K_SCR_BARLAYER);
    assert(gbar && f.L(li + M68K_LI_TOP_LAYER) == gbar && f.L(gbar + M68K_LAYER_LAYERINFO) == li);
    assert(f.memory[li + M68K_LI_LOCK + M68K_SS_LN_TYPE] == 15);
    /* the RastPort of a screen has no layer */
    assert(f.L(sg + M68K_SCR_RASTPORT + M68K_RP_LAYER) == 0);
    /* a window there: its 68k Layer names the LayerInfo of the 68k Screen */
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER);
    assert(f.L(mw + M68K_WIN_WSCREEN) == sg && f.L(la + M68K_LAYER_LAYERINFO) == li);
    assert(f.L(gbar + M68K_LAYER_BACK) == la && f.L(sg + M68K_SCR_BARLAYER) == gbar);

    /* the snapshot of the screen goes, as before CloseScreen(): the 68k
       Screen names no layer, borrowed 68k Layers go, the window keeps its
       68k Layer without links */
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    assert(!f.L(sg + M68K_SCR_BARLAYER) && !f.L(li + M68K_LI_TOP_LAYER) && f.zero(gbar, M68K_SIZEOF_LAYER));
    assert(f.L(mw + M68K_WIN_WLAYER) == la && !f.L(la + M68K_LAYER_FRONT) && !f.L(la + M68K_LAYER_BACK) &&
           !f.L(la + M68K_LAYER_LAYERINFO));
    /* Intuition refused: the next poll takes the screen again; the owned
       68k Layer is the same, the borrowed ones are new */
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG gbar2 = f.L(sg + M68K_SCR_BARLAYER);
    assert(gbar2 && f.L(gbar2 + M68K_LAYER_BACK) == la && f.L(la + M68K_LAYER_LAYERINFO) == li);
    /* the 68k Screen goes while the window stays: the window's screen gets
       a LayerInfo of the bridge */
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    f.ctx.pubscreens[0].native = nullptr;
    f.ctx.pubscreens[0].guest = 0;
    f.ctx.pubscreens[0].locks = 0;
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG own = f.L(la + M68K_LAYER_LAYERINFO);
    assert(own && own != li && f.L(f.L(own + M68K_LI_TOP_LAYER) + M68K_LAYER_BACK) == la);
    /* every snapshot at once, as at the end of the program */
    f.ctx.layer_ops->screen_closing(&f.ctx, nullptr);
    assert(!f.L(la + M68K_LAYER_LAYERINFO) && f.L(mw + M68K_WIN_WLAYER) == la);
    f.close(mw);
    assert(f.avail() == f.heap_initial);
}

static void test_bar_rastport()
{
    RastPort first = {}, replacement = {}, foreign_rp = {};
    Fixture f;
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg);
    Kaffeine_LayerSemaphoreInit(&f.ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_SCREEN));
    f.ctx.pubscreens[0].native = &scr;
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].locks = 1;
    first.Layer = replacement.Layer = &bar;
    foreign_rp.Layer = &foreign;
    bar.rp = &first;
    foreign.rp = &foreign_rp;

    /* A public screen visitor cannot draw through its bar or foreign
       windows, even when those layers have native RastPorts. */
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG gb = f.L(sg + M68K_SCR_BARLAYER);
    const ULONG gf = f.L(gb + M68K_LAYER_BACK);
    assert(gb && gf && !f.L(gb + M68K_LAYER_RP) && !f.L(gf + M68K_LAYER_RP));

    /* The screen owner gets a typed, stable RastPort for its internal bar,
       and the reverse link names the same classic Layer. */
    f.ctx.pubscreens[0].owned = TRUE;
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG rp = f.L(gb + M68K_LAYER_RP);
    assert(rp && f.L(rp + M68K_RP_LAYER) == gb && !f.L(gf + M68K_LAYER_RP));
    assert(Kaffeine_ShadowNative(&f.ctx, rp, KAFFEINE_SHADOW_RASTPORT) == &first);
    /* Internal drawing contexts remain owned by the bridge even if guest
       code incorrectly passes one to FreeMem. */
    const ULONG before_free = f.avail();
    Kaffeine_HeapFree(&f.ctx, rp, M68K_SIZEOF_RASTPORT);
    assert(f.avail() == before_free);
    m68k_write16(&f.ctx, rp + 36, 7); /* classic RastPort.cp_x */
    assert(Kaffeine_RastPortIn(&f.ctx, rp) == &first && first.cp_x == 7);
    const ULONG available = f.avail();
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(gb + M68K_LAYER_RP) == rp && f.avail() == available);

    /* No memory for a replacement: the old snapshot stays whole. Once
       memory returns, publication retires the former typed mapping. */
    std::vector<ULONG> fillers;
    for (ULONG b; (b = Kaffeine_HeapAlloc(&f.ctx, 16, 0)) != 0;)
        fillers.push_back(b);
    bar.rp = &replacement;
    const auto before = f.heap();
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.heap_is(before) && f.L(gb + M68K_LAYER_RP) == rp);
    for (ULONG b : fillers)
        Kaffeine_HeapFree(&f.ctx, b, 16);
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG rp2 = f.L(gb + M68K_LAYER_RP);
    assert(rp2 && rp2 != rp && f.L(rp2 + M68K_RP_LAYER) == gb);
    assert(!Kaffeine_ShadowNative(&f.ctx, rp, KAFFEINE_SHADOW_RASTPORT));
    assert(Kaffeine_ShadowNative(&f.ctx, rp2, KAFFEINE_SHADOW_RASTPORT) == &replacement);
    /* Replacement leaves two free intervals instead of one. Their extra
       allocator header is excluded from AvailMem; teardown must still
       recover the exact initial amount below. */
    assert(f.avail() + KAFFEINE_HEAP_HDR == available);

    /* A mapping supplied by another owner is reused and never freed by
       graph retirement; the graph frees only the one it allocated. */
    const ULONG existing = Kaffeine_RastPortCreate(&f.ctx, &first);
    assert(existing);
    bar.rp = &first;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(gb + M68K_LAYER_RP) == existing);
    assert(!Kaffeine_ShadowNative(&f.ctx, rp2, KAFFEINE_SHADOW_RASTPORT));
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    assert(Kaffeine_ShadowNative(&f.ctx, existing, KAFFEINE_SHADOW_RASTPORT) == &first);
    Kaffeine_RastPortDelete(&f.ctx, existing);

    /* A newly attached internal RastPort leaves before CloseScreen can
       destroy its native layer; a refused close can recreate the mirror. */
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG last_bar = f.L(sg + M68K_SCR_BARLAYER);
    const ULONG last_rp = f.L(last_bar + M68K_LAYER_RP);
    assert(last_rp);
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    assert(!Kaffeine_ShadowNative(&f.ctx, last_rp, KAFFEINE_SHADOW_RASTPORT));
    assert(!f.L(sg + M68K_SCR_BARLAYER));
    f.ctx.pubscreens[0] = {};
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_SCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    assert(f.avail() == f.heap_initial);
}

static void test_bounds_and_memory()
{
    static Screen s1, s2, s3;
    static Layer pool[70];
    NativeWindow a;
    Fixture f;
    std::memset(static_cast<void *>(&s1), 0, sizeof(s1));
    std::memset(static_cast<void *>(&s2), 0, sizeof(s2));
    std::memset(static_cast<void *>(&s3), 0, sizeof(s3));
    for (int i = 0; i < 70; ++i)
        make_layer(pool[i], (WORD)i, (WORD)i, 10, 10, LAYERSIMPLE);
    std::vector<Layer *> one, two;
    for (int i = 0; i < 48; ++i) one.push_back(&pool[i]);
    for (int i = 48; i < 64; ++i) two.push_back(&pool[i]);
    order(s1, one);
    order(s2, two);
    /* two screens that the program holds fill the table of 68k Layers */
    f.ctx.pubscreens[0].native = &s1;
    f.ctx.pubscreens[0].guest = 0x100;
    f.ctx.pubscreens[1].native = &s2;
    f.ctx.pubscreens[1].guest = 0x200;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls == 2);

    /* no room for the 68k Layer of a new window: it closes again, and
       nothing of it stays */
    make_window(a, s3, 0, 0, 50, 50);
    order(s3, { &a.layer });
    const ULONG avail = f.avail();
    const UWORD shadows = f.ctx.num_shadows;
    assert(f.open(a) == 0 && f.ctx.running && closed.size() == 1 && closed[0] == &a.window);
    assert(f.avail() == avail && f.ctx.num_shadows == shadows);

    /* room for the window's own 68k Layer, not for the rest of its screen:
       the window opens, and its snapshot holds nothing partial */
    f.ctx.pubscreens[1].native = nullptr;
    f.ctx.pubscreens[1].guest = 0;
    Kaffeine_LayersRefresh(&f.ctx);
    std::vector<Layer *> three = { &a.layer };
    for (int i = 48; i < 68; ++i) three.push_back(&pool[i]);
    order(s3, three);
    closed.clear();
    const ULONG mw = f.open(a);
    assert(mw && closed.empty());
    const ULONG la = f.L(mw + M68K_WIN_WLAYER);
    assert(la && !f.L(la + M68K_LAYER_FRONT) && !f.L(la + M68K_LAYER_BACK) &&
           !f.L(la + M68K_LAYER_LAYERINFO) && !f.W(la + M68K_LAYER_WIDTH));
    /* with room, the next poll writes the whole snapshot */
    f.ctx.pubscreens[0].native = nullptr;
    f.ctx.pubscreens[0].guest = 0;
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG info = f.L(la + M68K_LAYER_LAYERINFO);
    assert(info && f.L(info + M68K_LI_TOP_LAYER) == la && f.L(la + M68K_LAYER_BACK) &&
           f.W(la + M68K_LAYER_WIDTH) == 50);
    ULONG g = la, count = 0;
    while (g) { ++count; g = f.L(g + M68K_LAYER_BACK); }
    assert(count == 21);

    /* more layers than a snapshot takes: the previous one stays whole */
    auto before = f.heap();
    std::vector<Layer *> many = { &a.layer };
    for (int i = 0; i < 48; ++i) many.push_back(&pool[i]);
    order(s3, many);
    unsigned infos = info_calls;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls == infos + 1 && f.heap_is(before));

    /* no guest memory for the 68k Layer of a new layer: the snapshot stays
       until there is */
    order(s3, { &a.layer, &pool[48] });
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG g48 = f.L(la + M68K_LAYER_BACK);
    assert(g48 && !f.L(g48 + M68K_LAYER_BACK));
    std::vector<ULONG> fillers;
    for (ULONG b; (b = Kaffeine_HeapAlloc(&f.ctx, 16, 0)) != 0;)
        fillers.push_back(b);
    order(s3, { &pool[49], &a.layer, &pool[48] });
    before = f.heap();
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.heap_is(before) && f.L(info + M68K_LI_TOP_LAYER) == la);
    for (ULONG b : fillers)
        Kaffeine_HeapFree(&f.ctx, b, 16);
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG top = f.L(info + M68K_LI_TOP_LAYER);
    assert(top && top != la && f.L(top + M68K_LAYER_BACK) == la && f.L(la + M68K_LAYER_BACK) == g48);
    f.close(mw);
    assert(f.avail() == f.heap_initial);
}

/* A 68k Layer_Info as NewLayerInfo() gives it: no layer, a guest
   semaphore as InitSemaphore() leaves it, the creation flag, and every other byte zero, so
   that no native pointer is in it. */
static bool fresh_info(Fixture &f, ULONG li)
{
    const ULONG ss = li + M68K_LI_LOCK, q = ss + M68K_SS_WAITQUEUE;

    return f.zero(li, M68K_LI_LOCK) && f.zero(ss, M68K_SS_LN_TYPE) &&
           f.memory[ss + M68K_SS_LN_TYPE] == 15 &&
           f.zero(ss + M68K_SS_LN_TYPE + 1, M68K_SS_NESTCOUNT - M68K_SS_LN_TYPE - 1) &&
           f.W(ss + M68K_SS_NESTCOUNT) == 0 &&
           f.L(q) == q + 4 && f.L(q + 4) == 0 && f.L(q + 8) == q &&
           f.zero(q + 12, M68K_SS_OWNER - M68K_SS_WAITQUEUE - 12) &&
           f.L(ss + M68K_SS_OWNER) == 0 && f.W(ss + M68K_SS_QUEUECOUNT) == 0xFFFF &&
           f.zero(ss + 46, M68K_LI_FLAGS - M68K_LI_LOCK - 46) &&
           f.W(li + M68K_LI_FLAGS) == NEWLAYERINFO_CALLED &&
           f.zero(li + M68K_LI_FLAGS + 2, M68K_SIZEOF_LAYER_INFO - M68K_LI_FLAGS - 2);
}

static void test_layer_info()
{
    Fixture f;
    const UWORD shadows = f.ctx.num_shadows;

    /* NewLayerInfo(): one native Layer_Info, one classic 68k one */
    const ULONG li = f.call(Fixture::layers_base, 144);
    assert(li && f.ctx.running && new_infos == 1 && native_infos.size() == 1);
    assert(li >= Fixture::heap_start && li + M68K_SIZEOF_LAYER_INFO <= Fixture::heap_end);
    assert(fresh_info(f, li));
    /* never a shadow: no generated thunk resolves it to the native one */
    assert(!f.shadowed(li) && f.ctx.num_shadows == shadows &&
           m68k_to_host_or_shadow(&f.ctx, li) == f.ctx.mem + li);
    /* stable: no snapshot or poll writes it */
    const std::vector<UBYTE> mirror(f.memory.begin() + li,
                                    f.memory.begin() + li + M68K_SIZEOF_LAYER_INFO);
    Kaffeine_LayersRefresh(&f.ctx);
    f.run(KAFFEINE_GUI_POLL_SLICES + 1);
    assert(std::equal(mirror.begin(), mirror.end(), f.memory.begin() + li));

    /* a second one is a pair of its own */
    const ULONG li2 = f.call(Fixture::layers_base, 144);
    assert(li2 && li2 != li && fresh_info(f, li2) && native_infos.size() == 2 &&
           native_infos[0] != native_infos[1]);
    Layer_Info *const n1 = native_infos[0], *const n2 = native_infos[1];

    /* DisposeLayerInfo(): just its native one, once; the 68k one is cleared */
    events.clear();
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running);
    assert(disposed_infos == 1 && native_infos.size() == 1 && native_infos[0] == n2);
    assert(events.size() == 1 && events[0] == "DisposeLayerInfo" && n1 != n2);
    assert(f.zero(li, M68K_SIZEOF_LAYER_INFO) && fresh_info(f, li2));
    /* disposed of: a stale handle stops before the native call */
    const Counters before = counters();
    assert(f.call(Fixture::layers_base, 150, { li }) == 20 && f.stopped() && counters() == before);
    assert(f.call(Fixture::layers_base, 150, { li2 }) == 0 && f.ctx.running && native_infos.empty());
    assert(f.call(Fixture::layers_base, 150, { li2 }) == 20 && f.stopped());
    assert(disposed_infos == 2 && f.avail() == f.heap_initial && f.ctx.num_shadows == shadows);
}

static void test_layer_info_memory()
{
    Fixture f;

    /* no native Layer_Info: 0, and the 68k one made first goes again */
    const ULONG avail = f.avail();
    const UWORD shadows = f.ctx.num_shadows;
    new_info_fails = true;
    assert(f.call(Fixture::layers_base, 144) == 0 && f.ctx.running);
    assert(new_infos == 1 && native_infos.empty() && f.avail() == avail &&
           f.ctx.num_shadows == shadows);
    new_info_fails = false;

    /* no guest memory: 0 before any native call, and nothing changes */
    std::vector<ULONG> fillers;
    for (ULONG b; (b = Kaffeine_HeapAlloc(&f.ctx, 16, 0)) != 0;)
        fillers.push_back(b);
    auto before = f.heap();
    assert(f.call(Fixture::layers_base, 144) == 0 && f.ctx.running);
    assert(new_infos == 1 && native_infos.empty() && f.heap_is(before));
    for (ULONG b : fillers)
        Kaffeine_HeapFree(&f.ctx, b, 16);

    /* a full table: 0 before any native call or allocation */
    std::vector<ULONG> infos;
    for (int i = 0; i < KAFFEINE_LAYER_MAX_INFOS; ++i)
    {
        const ULONG li = f.call(Fixture::layers_base, 144);
        assert(li && f.ctx.running && fresh_info(f, li));
        assert(std::find(infos.begin(), infos.end(), li) == infos.end());
        infos.push_back(li);
    }
    assert(native_infos.size() == KAFFEINE_LAYER_MAX_INFOS);
    const unsigned made = new_infos;
    before = f.heap();
    assert(f.call(Fixture::layers_base, 144) == 0 && f.ctx.running);
    assert(new_infos == made && f.heap_is(before));
    /* one goes, and there is room again */
    assert(f.call(Fixture::layers_base, 150, { infos.back() }) == 0 && f.ctx.running);
    infos.back() = f.call(Fixture::layers_base, 144);
    assert(infos.back() && new_infos == made + 1 && native_infos.size() == KAFFEINE_LAYER_MAX_INFOS);
    for (ULONG li : infos)
        assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running);
    assert(native_infos.empty() && f.avail() == f.heap_initial);
}

static void test_layer_info_refusals()
{
    NativeWindow a;
    Layer stray;
    Fixture f;
    make_window(a, scr, 10, 20, 200, 100);
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a);
    const ULONG la = f.L(mw + M68K_WIN_WLAYER), own = f.L(la + M68K_LAYER_LAYERINFO);
    assert(la && own);
    /* a 68k Screen of the program, with the LayerInfo of the screen */
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg);
    Kaffeine_LayerSemaphoreInit(&f.ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_PUBSCREEN));
    f.ctx.pubscreens[0].native = &scr;
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].locks = 1;
    f.ctx.layer_ops->screen_mirrored(&f.ctx, &scr);
    assert(f.L(la + M68K_LAYER_LAYERINFO) == sg + M68K_SCR_LAYERINFO);
    /* a Layer_Info that the program made itself */
    const ULONG guest_info = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_LAYER_INFO, MEMF_CLEAR);
    assert(guest_info);
    const ULONG li = f.call(Fixture::layers_base, 144);
    assert(li && native_infos.size() == 1);
    Layer_Info *const native = native_infos[0];

    /* borrowed, foreign and inner pointers stop before the native call */
    Counters before = counters();
    const auto heap = f.heap();
    for (ULONG bad : { sg + M68K_SCR_LAYERINFO, own, sg, la, mw, guest_info, li + 2,
                       li + M68K_LI_LOCK, 0x5555u, 0u })
    {
        assert(f.call(Fixture::layers_base, 150, { bad }) == 20 && f.stopped());
        assert(counters() == before && f.heap_is(heap));
    }
    /* the other LayerInfo calls still stop, with it in every register */
    for (ULONG lvo : { 156u, 162u, 204u })
    {
        assert(f.call(Fixture::layers_base, lvo, { li, li }) == 20 && f.stopped());
        assert(counters() == before && f.heap_is(heap));
    }
    /* with layers in its native list it stays, untouched */
    make_layer(stray, 0, 0, 10, 10, LAYERSIMPLE);
    native->top_layer = &stray;
    assert(f.call(Fixture::layers_base, 150, { li }) == 20 && f.stopped());
    assert(counters() == before && f.heap_is(heap) && native_infos.size() == 1);
    native->top_layer = nullptr;
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running && native_infos.empty());
    assert(f.call(Fixture::layers_base, 150, { li }) == 20 && f.stopped());
    assert(disposed_infos == 1);

    /* the snapshots go as before, the screen's LayerInfo never disposed of */
    f.ctx.layer_ops->screen_closing(&f.ctx, &scr);
    f.ctx.pubscreens[0] = {};
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    Kaffeine_HeapFree(&f.ctx, guest_info, M68K_SIZEOF_LAYER_INFO);
    f.close(mw);
    assert(disposed_infos == 1 && f.avail() == f.heap_initial);
}

static void test_layer_info_teardown()
{
    NativeWindow a;
    Layer stray;
    {
        /* as at the end of the program: windows and screens go first, and
           leave LayerInfos of the program alone; the bridge disposes of
           each that is left once, after a held layer lock went back */
        Fixture f;
        make_window(a, scr, 10, 20, 200, 100);
        order(scr, { &bar, &a.layer, &foreign });
        const ULONG mw = f.open(a);
        const ULONG la = f.L(mw + M68K_WIN_WLAYER);
        const ULONG l1 = f.call(Fixture::layers_base, 144), l2 = f.call(Fixture::layers_base, 144);
        const ULONG l3 = f.call(Fixture::layers_base, 144);
        assert(l1 && l2 && l3 && native_infos.size() == 3);
        assert(f.call(Fixture::layers_base, 150, { l2 }) == 0 && f.ctx.running);
        Layer_Info *const n3 = native_infos[1];
        /* one with layers, which no bridged call makes: never freed under them */
        make_layer(stray, 0, 0, 10, 10, LAYERSIMPLE);
        n3->top_layer = &stray;
        assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && held_locks == 1);
        events.clear();
        Kaffeine_LayersCleanup(&f.ctx);
        assert(held_locks == 0 && !f.ctx.layers);
        assert(disposed_infos == 2 && native_infos.size() == 1 && native_infos[0] == n3);
        assert(event_at("UnlockLayer") < event_at("DisposeLayerInfo") &&
               event_at("DisposeLayerInfo") < events.size());
        for (ULONG li : { l1, l2, l3 })
            assert(f.zero(li, M68K_SIZEOF_LAYER_INFO));
        /* again: nothing */
        const Counters after = counters();
        Kaffeine_LayersCleanup(&f.ctx);
        assert(counters() == after);
        /* without the bridge: no LayerInfo, no native call, no disposal */
        assert(f.call(Fixture::layers_base, 144) == 0 && f.ctx.running && counters() == after);
        assert(f.call(Fixture::layers_base, 150, { l1 }) == 20 && f.stopped() && counters() == after);
        f.close(mw);
        assert(f.avail() == f.heap_initial);
        /* the native Layer_Info that kept its layers is the test's */
        native_infos.clear();
        n3->top_layer = nullptr;
        delete n3;
    }
    {
        /* the window closes and every snapshot goes first; the LayerInfo
           of the program stays until the bridge's own end */
        Fixture f;
        make_window(a, scr, 10, 20, 200, 100);
        order(scr, { &bar, &a.layer, &foreign });
        const ULONG mw = f.open(a);
        const ULONG li = f.call(Fixture::layers_base, 144);
        assert(li && native_infos.size() == 1);
        f.close(mw);
        f.ctx.layer_ops->screen_closing(&f.ctx, nullptr);
        assert(native_infos.size() == 1 && disposed_infos == 0 && fresh_info(f, li));
        Kaffeine_LayersCleanup(&f.ctx);
        assert(native_infos.empty() && disposed_infos == 1 && f.zero(li, M68K_SIZEOF_LAYER_INFO));
        assert(f.avail() == f.heap_initial);
    }
}

static void test_cleanup_and_without_bridge()
{
    NativeWindow a;
    {
        Fixture f;
        make_window(a, scr, 10, 20, 200, 100);
        order(scr, { &bar, &a.layer, &foreign });
        const ULONG mw = f.open(a);
        const ULONG la = f.L(mw + M68K_WIN_WLAYER), rp = f.L(mw + M68K_WIN_RPORT);
        assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && held_locks == 1);
        /* the end of the program: the lock goes back, no 68k Layer stays */
        Kaffeine_LayersCleanup(&f.ctx);
        assert(held_locks == 0 && !f.ctx.layers && !f.ctx.layer_ops && !f.ctx.gui_poll);
        assert(!f.L(mw + M68K_WIN_WLAYER) && !f.L(rp + M68K_RP_LAYER) && f.zero(la, M68K_SIZEOF_LAYER));
        Kaffeine_LayersCleanup(&f.ctx);
        /* the window closes later, without the bridge */
        events.clear();
        f.close(mw);
        assert(event_at("UnlockLayer") == events.size() && event_at("CloseWindow") < events.size());
        assert(f.avail() == f.heap_initial);
        /* a layer call without the bridge stops, a depth call still works */
        assert(f.call(Fixture::layers_base, 198, { la, 0 }) == 20 && f.stopped());
        f.run(2 * KAFFEINE_GUI_POLL_SLICES);
    }
    {
        Fixture f(false);
        make_window(a, scr, 10, 20, 200, 100);
        order(scr, { &bar, &a.layer, &foreign });
        const ULONG mw = f.open(a);
        assert(mw && !f.L(mw + M68K_WIN_WLAYER) && !f.L(f.L(mw + M68K_WIN_RPORT) + M68K_RP_LAYER));
        assert(f.call(Fixture::intuition_base, 306, { mw }) == 0 && depth_calls.size() == 1);
        assert(f.call(Fixture::intuition_base, 306, { 0x5555 }) == 20 && f.stopped());
        f.run(2 * KAFFEINE_GUI_POLL_SLICES);
        assert(info_calls == 0);
        f.close(mw);
        assert(f.avail() == f.heap_initial);
    }
}

extern "C" BOOL Request(struct Requester *,struct Window *) { assert(false); return FALSE; }
extern "C" void EndRequest(struct Requester *,struct Window *) { assert(false); }
extern "C" BOOL SetDMRequest(struct Window *,struct Requester *) { assert(false); return FALSE; }

static void test_requester_layer_rastport_binding()
{
    Fixture f;NativeWindow a;Layer req;RastPort rp;
    make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
    ULONG mw=f.open(a),base=f.L(mw+M68K_WIN_WLAYER);
    make_layer(req,12,24,60,30,LAYERSIMPLE);
    rp=a.rp;rp.Layer=&req;req.rp=&rp;req.Window=&a.window;
    order(scr,{&bar,&req,&a.layer,&foreign});
    const auto *ops=f.ctx.layer_ops;
    assert(ops->request_opened && ops->request_closing);
    const ULONG avail=f.avail();const auto shadows=f.ctx.num_shadows;
    info_busy=true;
    assert(!ops->request_opened(&f.ctx,&a.window,&req));
    assert(f.avail()==avail && f.ctx.num_shadows==shadows);
    info_busy=false;
    Kaffeine_LayersRefresh(&f.ctx);
    ULONG borrowed=f.L(base+M68K_LAYER_FRONT);assert(borrowed && borrowed!=base);
    ULONG layer=ops->request_opened(&f.ctx,&a.window,&req);
    assert(layer && layer==borrowed && layer!=base);
    ULONG guest_rp=f.L(layer+M68K_LAYER_RP);
    assert(guest_rp && Kaffeine_ShadowNative(&f.ctx,guest_rp,KAFFEINE_SHADOW_RASTPORT)==&rp);
    assert(f.L(guest_rp+M68K_RP_LAYER)==layer && f.L(layer+M68K_LAYER_WINDOW)==mw);
    assert(f.L(mw+M68K_WIN_WLAYER)==base);
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(base+M68K_LAYER_FRONT)==layer && f.L(layer+M68K_LAYER_BACK)==base);
    assert(f.L(layer+M68K_LAYER_RP)==guest_rp);
    const auto before=f.heap();Kaffeine_LayersRefresh(&f.ctx);assert(f.heap_is(before));
    assert(f.call(Fixture::layers_base,96,{0,layer})==20 && f.stopped()); // metadata-only layer
    ops->request_closing(&f.ctx,&req);
    assert(!Kaffeine_ShadowNative(&f.ctx,guest_rp,KAFFEINE_SHADOW_RASTPORT));
    order(scr,{&bar,&a.layer,&foreign});Kaffeine_LayersRefresh(&f.ctx);
    assert(f.L(base+M68K_LAYER_FRONT)!=layer);
    ops->request_closing(&f.ctx,&req); // idempotent
    f.close(mw);
}

/* layer_ops->draw_target for a RastPort that the program copied from its
   window: the real native layer and BitMap of an owned window or of a
   requester on it, only with the BitMap that the bridges publish for it,
   by their tables; borrowed layers, the bar, a closed window, a layer held
   by another guest task, an unknown address and a screen's BitMap without
   a Layer stop the program. No native layer call, lock or allocation. */
static void test_draw_targets()
{
    static BitMap screen_bm, other_bm;
    NativeWindow a;
    Fixture f;
    const auto *ops = f.ctx.layer_ops;
    auto target = [&](ULONG layer, ULONG bitmap, RastPort &rp) {
        BitMap adapter;
        std::memset(static_cast<void *>(&rp), 0, sizeof(rp));
        std::memset(static_cast<void *>(&adapter), 0x5A, sizeof(adapter));
        f.ctx.running = TRUE;
        f.ctx.exit_code = 0;
        const BOOL ok = ops->draw_target(&f.ctx, layer, bitmap, &rp, &adapter);
        assert(ok ? f.ctx.running && !f.ctx.exit_code : !f.ctx.running && f.ctx.exit_code == 20);
        if (!ok)
            assert(!rp.Layer && !rp.BitMap);
        return ok;
    };
    RastPort rp;
    assert(ops->draw_target);

    /* on a public screen: the window's RastPort shows BitMap 0 */
    make_window(a, scr, 10, 20, 200, 100);
    a.rp.BitMap = &screen_bm;
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mw = f.open(a), la = f.L(mw + M68K_WIN_WLAYER), wrp = f.L(mw + M68K_WIN_RPORT);
    const ULONG gbar = f.L(f.L(la + M68K_LAYER_LAYERINFO) + M68K_LI_TOP_LAYER), gfor = f.L(la + M68K_LAYER_BACK);
    assert(la && gbar && gfor && f.L(wrp + M68K_RP_LAYER) == la && f.L(wrp + M68K_RP_BITMAP) == 0);
    const Counters before = counters();
    const ULONG avail = f.avail();
    assert(target(la, 0, rp) && rp.Layer == &a.layer && rp.BitMap == &screen_bm);
    assert(counters() == before && f.avail() == avail);
    /* another BitMap, a borrowed layer, the bar, raw guest bytes, the
       window and its RastPort instead of the Layer */
    const ULONG bad[][2] = { { la, 0x5000 }, { la, wrp }, { gfor, 0 }, { gbar, 0 },
                             { 0x5000, 0 }, { mw, 0 }, { wrp, 0 }, { la + 2, 0 } };
    for (const auto &b : bad)
        assert(!target(b[0], b[1], rp));
    assert(counters() == before && f.avail() == avail);
    /* the BitMap of the window, if it changed natively: still exactly that one */
    a.rp.BitMap = &other_bm;
    assert(target(la, 0, rp) && rp.BitMap == &other_bm);
    a.rp.BitMap = nullptr;
    assert(!target(la, 0, rp));
    a.rp.BitMap = &screen_bm;

    /* held by another guest task: refused; by this one: taken */
    const ULONG task_addr = Fixture::exec_base + M68K_EXECBASE_THISTASK, owner = f.L(task_addr);
    assert(f.call(Fixture::layers_base, 96, { 0, la }) == 0 && held_locks == 1);
    assert(target(la, 0, rp));
    m68k_write32(&f.ctx, task_addr, owner + 0x100);
    assert(!target(la, 0, rp));
    m68k_write32(&f.ctx, task_addr, owner);
    assert(f.call(Fixture::layers_base, 102, { la }) == 0 && held_locks == 0);

    /* a requester layer on the window, with its own native RastPort */
    Layer req;
    RastPort req_rp;
    make_layer(req, 12, 24, 60, 30, LAYERSIMPLE);
    req_rp = a.rp;
    req_rp.Layer = &req;
    req.rp = &req_rp;
    req.Window = &a.window;
    order(scr, { &bar, &req, &a.layer, &foreign });
    const ULONG lr = ops->request_opened(&f.ctx, &a.window, &req);
    assert(lr && lr != la);
    assert(target(lr, 0, rp) && rp.Layer == &req && rp.BitMap == &screen_bm);
    assert(!target(lr, 0x5000, rp));
    ops->request_closing(&f.ctx, &req);
    order(scr, { &bar, &a.layer, &foreign });
    Kaffeine_LayersRefresh(&f.ctx);
    /* the requester has ended: its 68k Layer names nothing */
    assert(!target(lr, 0, rp));

    /* without a Layer only an ordinary planar BitMap is a target */
    assert(!target(0, 0, rp) && !target(0, 0x5000, rp));
    f.close(mw);
    /* the window is gone: its copied RastPort draws nothing */
    assert(!target(la, 0, rp));
    assert(f.avail() == f.heap_initial);

    /* on a screen of the program: exactly its typed BitMap */
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_SCREEN));
    Kaffeine_LayerSemaphoreInit(&f.ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
    f.ctx.pubscreens[0].native = &scr;
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].owned = TRUE;
    scr.RastPort.BitMap = &screen_bm;
    ops->screen_mirrored(&f.ctx, &scr);
    make_window(a, scr, 10, 20, 200, 100);
    a.rp.BitMap = &screen_bm;
    order(scr, { &bar, &a.layer, &foreign });
    const ULONG mo = f.open(a), lo = f.L(mo + M68K_WIN_WLAYER), handle = sg + M68K_SCR_BITMAP;
    assert(f.L(f.L(mo + M68K_WIN_RPORT) + M68K_RP_BITMAP) == handle);
    assert(target(lo, handle, rp) && rp.Layer == &a.layer && rp.BitMap == &screen_bm);
    assert(!target(lo, 0, rp) && !target(lo, handle + 2, rp));
    /* a screen's BitMap without a Layer would draw unclipped over its windows */
    assert(!target(0, handle, rp));
    /* the window draws elsewhere than the screen: then BitMap 0 */
    a.rp.BitMap = &other_bm;
    assert(target(lo, 0, rp) && rp.BitMap == &other_bm && !target(lo, handle, rp));
    a.rp.BitMap = &screen_bm;
    /* the same screen only locked, as a public one: BitMap 0 */
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_SCREEN);
    assert(Kaffeine_ShadowAdd(&f.ctx, sg, &scr, KAFFEINE_SHADOW_PUBSCREEN));
    f.ctx.pubscreens[0].owned = FALSE;
    assert(target(lo, 0, rp) && !target(lo, handle, rp));
    f.close(mo);
    ops->screen_closing(&f.ctx, &scr);
    f.ctx.pubscreens[0] = {};
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    scr.RastPort.BitMap = nullptr;
    assert(held_locks == 0 && native_installs == before.installs && f.avail() == f.heap_initial);
}

static void test_teardown_releases_all_window_locks()
{
    Fixture f;NativeWindow a,b;
    make_window(a,scr,10,20,200,100);make_window(b,scr,30,40,220,120);
    order(scr,{&bar,&a.layer,&b.layer,&foreign});
    ULONG ma=f.open(a),mb=f.open(b);
    ULONG la=f.L(ma+M68K_WIN_WLAYER),lb=f.L(mb+M68K_WIN_WLAYER);
    f.call(Fixture::layers_base,96,{0,la});f.call(Fixture::layers_base,96,{0,la});
    f.call(Fixture::layers_base,96,{0,lb});
    assert(held_locks==3 && f.ctx.layer_ops->has_locks(&f.ctx));
    events.clear();
    Kaffeine_WindowsCleanup(&f.ctx);
    assert(held_locks==0 && !f.ctx.layer_ops->has_locks(&f.ctx));
    assert(closed.size()==2 && event_at("UnlockLayer")<event_at("CloseWindow"));
}

static void test_info_lock_smoke()
{
    Fixture f; NativeWindow a;
    make_window(a,scr,10,20,200,100);
    order(scr,{&bar,&a.layer,&foreign});
    ULONG mw=f.open(a), layer=f.L(mw+M68K_WIN_WLAYER), info=f.L(layer+M68K_LAYER_LAYERINFO);
    assert(info && !info_depth);
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==1);
    assert(Kaffeine_LayerInfoLocked(&f.ctx) && f.ctx.layer_ops->has_locks(&f.ctx));
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==2);
    assert(f.call(Fixture::layers_base,96,{0,layer})==0 && held_locks==1);
    assert(f.call(Fixture::layers_base,102,{layer})==0 && held_locks==0);
    assert(f.call(Fixture::layers_base,138,{info})==0 && info_depth==1);
    assert(f.call(Fixture::layers_base,138,{info})==0 && info_depth==0 && !Kaffeine_LayerInfoLocked(&f.ctx));
    ULONG own=f.call(Fixture::layers_base,144);
    assert(own && f.call(Fixture::layers_base,120,{own})==0 && info_depth==1);
    assert(f.call(Fixture::layers_base,138,{own})==0 && !info_depth);
    assert(f.call(Fixture::layers_base,150,{own})==0);
    f.close(mw);
}

static void test_info_lock_snapshot_and_nesting()
{
    Fixture f; NativeWindow a,b;
    make_window(a,scr,10,20,200,100); make_window(b,scr,20,30,210,110);
    order(scr,{&bar,&a.layer,&b.layer,&foreign});
    ULONG ma=f.open(a),mb=f.open(b),la=f.L(ma+M68K_WIN_WLAYER),lb=f.L(mb+M68K_WIN_WLAYER);
    ULONG info=f.L(la+M68K_LAYER_LAYERINFO);
    order(scr,{&b.layer,&bar,&a.layer,&foreign});
    m68k_write32(&f.ctx,info+M68K_LI_TOP_LAYER,0xdeadbeef);
    std::memset(f.memory.data()+info+M68K_LI_LOCK,0xfe,46);
    std::vector<UBYTE> semaphore(f.memory.begin()+info+M68K_LI_LOCK,
                                 f.memory.begin()+info+M68K_LI_LOCK+46);
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==1);
    assert(f.L(info+M68K_LI_TOP_LAYER)==lb && f.L(lb+M68K_LAYER_BACK)==f.L(la+M68K_LAYER_FRONT));
    assert(!std::memcmp(semaphore.data(),f.memory.data()+info+M68K_LI_LOCK,semaphore.size()));
    unsigned calls=info_calls;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(info_calls==calls && f.L(info+M68K_LI_TOP_LAYER)==lb);
    for(unsigned n=1;n<255;++n)
        assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==static_cast<int>(n+1));
    calls=info_calls;
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && info_depth==255 && info_calls==calls);
    for(unsigned n=255;n;--n)
        assert(f.call(Fixture::layers_base,138,{info})==0 && info_depth==static_cast<int>(n-1));
    assert(!Kaffeine_LayerInfoLocked(&f.ctx));
    calls=info_calls;
    assert(f.call(Fixture::layers_base,138,{info})==20 && f.stopped() && info_calls==calls && !info_depth);
    f.close(ma);f.close(mb);
}

static void test_info_lock_refusals()
{
    Fixture f; NativeWindow a;
    make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
    ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
    for(ULONG bad:{0U,info+1,info+M68K_LI_LOCK,layer,mw,0x5555U,0xffffffffU})
    {
        unsigned calls=info_calls;
        assert(f.call(Fixture::layers_base,120,{bad})==20 && f.stopped());
        assert(f.call(Fixture::layers_base,138,{bad})==20 && f.stopped() && info_calls==calls && !info_depth);
    }
    unsigned calls=info_calls;
    m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task+4);
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && !info_depth && info_calls==calls);
    m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task);
    info_busy=true;
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && info_calls==calls && !info_depth);
    info_busy=false;
    for(unsigned mode=0;mode<4;++mode)
    {
        if(mode==0) f.ctx.callback_depth=1;
        if(mode==1) f.ctx.broker_call=TRUE;
        if(mode==2) f.ctx.bitmap_blits=1;
        if(mode==3) native_task=reinterpret_cast<Task*>(static_cast<IPTR>(0x22220000));
        assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && info_calls==calls && !info_depth);
        f.ctx.callback_depth=0;f.ctx.broker_call=FALSE;f.ctx.bitmap_blits=0;native_task=f.ctx.exec_task;
    }
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==1);
    calls=info_calls;
    m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task+4);
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped());
    assert(f.call(Fixture::layers_base,138,{info})==20 && f.stopped() && info_depth==1 && info_calls==calls);
    m68k_write32(&f.ctx,Fixture::exec_base+M68K_EXECBASE_THISTASK,Fixture::task);
    native_task=reinterpret_cast<Task*>(static_cast<IPTR>(0x22220000));
    assert(f.call(Fixture::layers_base,138,{info})==20 && f.stopped() && info_depth==1);
    f.ctx.layer_ops->release_locks(&f.ctx);
    assert(info_depth==1 && Kaffeine_LayerInfoLocked(&f.ctx));
    native_task=f.ctx.exec_task;
    assert(f.call(Fixture::layers_base,138,{info})==0 && !info_depth);
    ULONG owned=f.call(Fixture::layers_base,144);
    assert(f.call(Fixture::layers_base,150,{owned})==0);
    calls=info_calls;
    assert(f.call(Fixture::layers_base,120,{owned})==20 && f.stopped() && info_calls==calls);
    f.close(mw);
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && !info_depth);
}

static void test_info_lock_order()
{
    Fixture f; NativeWindow a,b; Screen other={};
    make_window(a,scr,10,20,200,100);make_window(b,other,20,30,210,110);
    order(scr,{&bar,&a.layer,&foreign});order(other,{&b.layer});
    ULONG ma=f.open(a),mb=f.open(b),la=f.L(ma+M68K_WIN_WLAYER),lb=f.L(mb+M68K_WIN_WLAYER);
    ULONG info=f.L(la+M68K_LAYER_LAYERINFO);
    assert(f.call(Fixture::layers_base,96,{0,la})==0 && held_locks==1);
    unsigned calls=info_calls;
    assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped() && info_calls==calls && !info_depth);
    assert(f.call(Fixture::layers_base,102,{la})==0 && !held_locks);
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==1);
    assert(f.call(Fixture::layers_base,96,{0,la})==0 && held_locks==1);
    assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==2);
    assert(f.call(Fixture::layers_base,96,{0,lb})==20 && f.stopped() && held_locks==1);
    assert(f.call(Fixture::graphics_base,654,{0,0,0,0,0,lb})==FALSE && f.ctx.running && held_locks==1);
    assert(f.call(Fixture::layers_base,102,{la})==0 && !held_locks);
    assert(f.call(Fixture::layers_base,138,{info})==0 && info_depth==1);
    assert(f.call(Fixture::layers_base,138,{info})==0 && !info_depth);
    f.close(ma);f.close(mb);
}

static void test_info_lock_rollback()
{
    for(unsigned mode=0;mode<3;++mode)
    {
        Fixture f;NativeWindow a;Layer extra[49]={};
        make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
        std::vector<ULONG> fillers;
        if(mode==0)
        {
            for(unsigned n=0;n<49;++n) make_layer(extra[n],n,n,10,10,LAYERSIMPLE);
            std::vector<Layer*> chain;
            for(auto &l:extra) chain.push_back(&l);
            order(scr,chain);
        }
        if(mode==1)
        {
            make_layer(extra[0],0,0,10,10,LAYERSIMPLE);
            order(scr,{&bar,&extra[0],&a.layer,&foreign});
            for(ULONG b;(b=Kaffeine_HeapAlloc(&f.ctx,16,0))!=0;) fillers.push_back(b);
        }
        if(mode==2) foreign.back=&bar; // bounded native list cycle
        const auto before=f.heap();
        unsigned calls=info_calls;
        assert(f.call(Fixture::layers_base,120,{info})==20 && f.stopped());
        assert(!info_depth && !Kaffeine_LayerInfoLocked(&f.ctx) && info_calls==calls+2 && f.heap_is(before));
        for(ULONG b:fillers) Kaffeine_HeapFree(&f.ctx,b,16);
        order(scr,{&bar,&a.layer,&foreign});
        assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==1);
        assert(f.call(Fixture::layers_base,138,{info})==0 && !info_depth);
        f.close(mw);
    }
}

static void test_info_lock_lifetime()
{
    for(unsigned mode=0;mode<3;++mode)
    {
        Fixture f;NativeWindow a;
        make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
        ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
        assert(f.call(Fixture::layers_base,120,{info})==0);
        assert(f.call(Fixture::layers_base,120,{info})==0 && info_depth==2);
        if(mode==0)
        {
            assert(f.call(Fixture::layers_base,96,{0,layer})==0);
            f.ctx.layer_ops->task_removed(&f.ctx,Fixture::task+4);
            assert(info_depth==2 && held_locks==1);
            f.ctx.layer_ops->task_removed(&f.ctx,Fixture::task);
            assert(!info_depth && !held_locks);
        }
        if(mode==1) f.ctx.layer_ops->screen_closing(&f.ctx,&scr);
        if(mode==2) f.ctx.layer_ops->release_locks(&f.ctx);
        assert(!info_depth && !Kaffeine_LayerInfoLocked(&f.ctx));
        f.ctx.layer_ops->release_locks(&f.ctx);assert(!info_depth);
        f.close(mw);
    }
    Fixture f;
    ULONG info=f.call(Fixture::layers_base,144);
    assert(f.call(Fixture::layers_base,120,{info})==0);
    assert(f.call(Fixture::layers_base,150,{info})==20 && f.stopped() && info_depth==1 && native_infos.size()==1);
    Kaffeine_LayersCleanup(&f.ctx);
    assert(!info_depth && native_infos.empty() && !f.ctx.layers);
}

static unsigned info_packet_calls;
static IPTR info_native_packet(KaffeineContext *,ULONG,ULONG) { ++info_packet_calls; return 7; }
static void test_info_native_packet_guard()
{
    Fixture f; NativeWindow a;
    make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
    ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
    ULONG msg=Kaffeine_HeapAlloc(&f.ctx,68,MEMF_CLEAR);
    assert(msg);
    info_packet_calls=0;
    for(bool handler:{false,true})
    {
        ULONG port=0x3520;
        if(handler) { f.ctx.handlers[0].guest=port;f.ctx.handler_packet=info_native_packet; }
        else { f.ctx.files[0].guest=0x3500;f.ctx.files[0].handler=port;f.ctx.file_packet=info_native_packet; }
        assert(f.call(Fixture::layers_base,120,{info})==0);
        const auto before=f.heap();
        f.ctx.cpu=&f.cpu;
        assert(Kaffeine_RoutePutMsg(&f.ctx,port,msg,0x55)==20 && !f.ctx.running && f.ctx.callback_failed);
        f.ctx.cpu=nullptr;
        assert(info_packet_calls==0 && f.heap_is(before) && info_depth==1);
        assert(f.call(Fixture::layers_base,138,{info})==0);
        f.ctx.running=TRUE;
        assert(Kaffeine_RoutePutMsg(&f.ctx,port,msg,0x55)==0x55 && info_packet_calls==1);
        info_packet_calls=0;
        f.ctx.handlers[0]={};f.ctx.files[0]={};f.ctx.file_packet=nullptr;f.ctx.handler_packet=nullptr;
    }
    // Opaque guest IPC remains an ordinary in-arena operation in this scope.
    ULONG port=Kaffeine_HeapAlloc(&f.ctx,34,MEMF_CLEAR),ordinary=Kaffeine_HeapAlloc(&f.ctx,20,MEMF_CLEAR);
    assert(port && ordinary);
    f.memory[port+8]=NT_MSGPORT;f.memory[port+14]=2;
    m68k_write32(&f.ctx,port+20,port+24);m68k_write32(&f.ctx,port+28,port+20);
    m68k_write16(&f.ctx,ordinary+18,20);
    assert(f.call(Fixture::layers_base,120,{info})==0);
    assert(Kaffeine_RoutePutMsg(&f.ctx,port,ordinary,0x66)==0x66 && f.ctx.running && f.L(port+20)==ordinary);
    assert(f.call(Fixture::layers_base,138,{info})==0);
    // Drain before fixture teardown; this is the public classic list unlink.
    m68k_write32(&f.ctx,port+20,port+24);m68k_write32(&f.ctx,port+28,port+20);
    Kaffeine_HeapFree(&f.ctx,ordinary,20);Kaffeine_HeapFree(&f.ctx,port,34);Kaffeine_HeapFree(&f.ctx,msg,68);
    f.close(mw);
}

static unsigned info_probe_slices;
static ULONG info_probe_info;
static void test_info_lock_boundaries()
{
    Fixture f;NativeWindow a;
    make_window(a,scr,10,20,200,100);order(scr,{&bar,&a.layer,&foreign});
    ULONG mw=f.open(a),layer=f.L(mw+M68K_WIN_WLAYER),info=f.L(layer+M68K_LAYER_LAYERINFO);
    for(ULONG lvo:{318U,336U,384U,408U,444U,450U,456U,462U,474U,552U,564U,582U,660U,672U,678U})
    {
        assert(f.call(Fixture::layers_base,120,{info})==0);
        assert(f.call(Fixture::exec_base,lvo)==20 && f.stopped() && f.ctx.callback_failed);
        assert(f.call(Fixture::layers_base,138,{info})==0);
    }
    assert(f.call(Fixture::layers_base,120,{info})==0);
    assert(f.call(Fixture::intuition_base,306,{mw})==20 && f.stopped() && depth_calls.empty());
    assert(f.call(Fixture::layers_base,138,{info})==0);
    assert(f.call(Fixture::layers_base,120,{info})==0);
    assert(f.call(Fixture::graphics_base,270)==20 && f.stopped());
    assert(f.call(Fixture::layers_base,138,{info})==0);
    M68KFakeLibBase saved=f.ctx.libs[3];
    for(const char *library:{"workbench.library","gadtools.library","asl.library","icon.library","diskfont.library","datatypes.library","locale.library","unknown.library"})
    {
        std::strcpy(f.ctx.libs[3].name,library);
        for(ULONG lvo:{42U,48U,54U,90U,96U})
        {
            if(!std::strcmp(library,"gadtools.library") && lvo!=42 && lvo!=90 && lvo!=96) continue;
            assert(f.call(Fixture::layers_base,120,{info})==0);
            assert(f.call(Fixture::graphics_base,lvo)==20 && f.stopped() && generated_calls==0);
            assert(f.call(Fixture::layers_base,138,{info})==0);
        }
    }
    f.ctx.libs[3].dos=TRUE;std::strcpy(f.ctx.libs[3].name,"dos.library");
    assert(f.call(Fixture::layers_base,120,{info})==0);
    assert(f.call(Fixture::graphics_base,150)==20 && f.stopped());
    assert(f.call(Fixture::layers_base,138,{info})==0);f.ctx.libs[3]=saved;
    assert(f.call(Fixture::layers_base,120,{info})==0);
    KaffeineGuestCall call={};
    f.ctx.cpu=&f.cpu;f.ctx.running=TRUE;
    assert(!Kaffeine_CallGuest(&f.ctx,&f.cpu,0x5000,&call) && f.stopped() && f.ctx.callback_failed);
    f.ctx.cpu=nullptr;
    assert(f.call(Fixture::layers_base,138,{info})==0);
    // A fault/stop retires the native scope before its CPU can be destroyed.
    KaffeineCPUOps run_ops=ops;
    info_probe_slices=0;info_probe_info=info;
    run_ops.execute=[](void*) {
        assert(++info_probe_slices==1);
        Kaffeine_SetA(live->cpu,0,info_probe_info);Kaffeine_SetA(live->cpu,7,Fixture::stack);
        KaffeineDispatchResult result={};
        assert(Kaffeine_DispatchLibraryCall(live,live->cpu,Fixture::layers_base-120,&result));
        assert(Kaffeine_LayerInfoLocked(live) && info_depth==1);
        Kaffeine_StopGuest(live,live->cpu,20);
        Kaffeine_SetD(live->cpu,0,0); // enclosing void thunk cannot replace the stop code
    };
    unsigned drained=0;
    f.ctx.root_completion_data=&drained;
    f.ctx.domain_drain=[](KaffeineContext *ctx)->BOOL {
        ++*static_cast<unsigned*>(ctx->root_completion_data);
        assert(!Kaffeine_LayerInfoLocked(ctx) && !info_depth && !ctx->running);
        return TRUE;
    };
    f.cpu.ops=&run_ops;
    assert(Kaffeine_RunCPU(&f.ctx,&f.cpu)==20 && info_probe_slices==1 && !info_depth && !Kaffeine_LayerInfoLocked(&f.ctx));
    assert(drained && !info_depth);
    f.ctx.domain_drain=nullptr;f.ctx.root_completion_data=nullptr;
    f.cpu.ops=&ops;
    f.close(mw);
}

#include "refresh_fixture_tests.h"

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "Typed native refresh balances successful/OOM and GZZ locks and preserves guest fields", test_refresh_scopes },
        { "GadTools refresh aliases use tracked manual scopes and retain Info safety gates", test_refresh_aliases_and_gates },
        { "Refresh rejects nested, busy, native-update, wrong-window and wrong-owner scopes", test_refresh_refusals },
        { "Refresh abort releases inner layers before EndRefresh and outer Info before retirement", test_refresh_lifetime },
        { "Info locks refresh real topology, ignore guest semaphore bytes and bound nesting", test_info_lock_snapshot_and_nesting },
        { "Info locks reject foreign, stale, busy, callback and cross-task owners", test_info_lock_refusals },
        { "Info before layer order and same-info nested access are enforced", test_info_lock_order },
        { "Snapshot OOM, cycles and depth limits roll back the acquired native lock", test_info_lock_rollback },
        { "Info locks balance task, screen, explicit disposal and final cleanup", test_info_lock_lifetime },
        { "Native packet routes refuse before DOS in an Info scope; opaque guest IPC stays ordinary", test_info_native_packet_guard },
        { "Info scopes reject DOS, waits, Intuition and callbacks and retire before CPU teardown", test_info_lock_boundaries },
        { "Typed screen and owned LayerInfo locks nest on the actual native owner", test_info_lock_smoke },
        { "Classic offsets agree with the generated layouts and the NDK packing", test_layout },
        { "Every layers.library slot is manual; depth and graphics layer slots", test_registration },
        { "One coherent snapshot: links, scalars, own objects, no native pointers, stable identities", test_identity },
        { "Depth calls are asynchronous; the outer-loop poll and messages show the move", test_async_depth_and_poll },
        { "A busy native LayerInfo skips the poll without blocking guest replies", test_busy_snapshot },
        { "Backdrop bits of the program stay pending until a depth call applies just them", test_backdrop },
        { "Backfill sentinels and handles; 68k Hooks and foreign layers refused before native calls", test_hooks },
        { "Unsupported layer calls and foreign pointers stop before anything native", test_refusals },
        { "Several windows, nested locks, no LayerInfo lock under a layer lock, close with a lock", test_windows_and_locks },
        { "Program teardown releases every window lock before closing the first window", test_teardown_releases_all_window_locks },
        { "Requester Layer exposes one stable typed RastPort and retires both before native deletion", test_requester_layer_rastport_binding },
        { "Copied window RastPorts draw only into the real layer with its published BitMap", test_draw_targets },
        { "Layer locks distinguish guest task owners before native recursion", test_guest_lock_owner },
        { "Backdrop updates respect lock order; close unlocks before menu and gadget teardown", test_review_lifetimes },
        { "Messages and depth calls restore retired graphs without resetting a held semaphore", test_retired_screen_recovery },
        { "A recycled borrowed native address does not inherit pending guest backdrop edits", test_borrowed_address_reuse },
        { "Borrowed layers come and go; a GZZ border is readable but never changed", test_borrowed },
        { "BarLayer and LayerInfo of a 68k Screen; snapshots end before the screen may close", test_screen_fields },
        { "Only an owned screen exposes its bar RastPort; stable identity, failure rollback and retirement", test_bar_rastport },
        { "Full tables, missing memory and deep lists keep whole snapshots", test_bounds_and_memory },
        { "NewLayerInfo() pairs: classic stable mirror, no shadow, exact ownership, stale refused", test_layer_info },
        { "NewLayerInfo() without native or guest memory or room gives 0 with full rollback", test_layer_info_memory },
        { "DisposeLayerInfo() refuses screen, bridge, foreign, inner, stale and nonempty infos", test_layer_info_refusals },
        { "Teardown disposes of each remaining LayerInfo once, after windows and screens", test_layer_info_teardown },
        { "Teardown with a held lock, and windows without the bridge", test_cleanup_and_without_bridge }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Layers of 68k programs: %zu groups passed\n", std::size(tests));
    return 0;
}

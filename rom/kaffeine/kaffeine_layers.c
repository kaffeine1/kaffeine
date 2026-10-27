/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Layers of 68k code. A classic program reads Window.WLayer and
    RastPort.Layer, follows the front and back links of the layers of its
    screen, reads Screen.BarLayer and LayerInfo.top_layer, and may poll
    those links until an asynchronous WindowToBack() or WindowToFront()
    has happened. The bridge gives it bounded snapshots of the native
    topology: one 68k Layer of the classic layout (graphics/clip.h) for
    each native layer of a screen that the program uses, which stays the
    same guest object as long as the native layer is in the snapshots, and
    one 68k Layer_Info for each such screen, the one inside the 68k Screen
    when the program has one. Each snapshot is taken in one pass under
    LockLayerInfo() of that screen into native storage, and only then
    written to guest memory, so that the links that the program sees always
    describe one real state of the native list; links are never made up. A
    screen with more than KAFFEINE_LAYER_MAX_DEPTH layers, or without guest
    memory for its 68k Layers, keeps its previous snapshot whole.

    Only scalars go to guest memory: bounds, priority, Flags, scroll
    offsets, Width and Height, and guest pointers to other 68k Layers, to
    the 68k Layer_Info, to the 68k Window and RastPort of the program that
    a layer belongs to, and BackFill as LAYERS_BACKFILL (0),
    LAYERS_NOBACKFILL (1) or an opaque 68k handle of a native hook. No
    native pointer, native semaphore, native hook or ClipRect reaches guest
    memory; the Lock of a 68k Layer is a guest SignalSemaphore that
    protects nothing native. The 68k Layers are never entries of the
    generic shadow map, so that no generated thunk can resolve one to a
    native layer, before or after it is gone.

    A 68k Layer is owned when it is the WLayer of a window of the program;
    every other layer of the topology, such as the screen bar, the border
    layer of a GZZ window, requester layers and the windows of other
    programs, is borrowed: the program can read it, but no call of the
    bridge changes or locks it. The bar of a screen the program opened
    additionally exposes its typed RastPort, so drawing through BarLayer.rp
    works; this mirror retires before the screen closes. A borrowed public
    screen never acquires this drawing access. A screen has a snapshot while the program
    has a window there, or a 68k Screen of it (kaffeine_screens.c), so
    that the native screen is open whenever the bridge reads its layers.
    The window and screen bridges tell this one, through ctx->layer_ops,
    when a window opens or is about to close and before a screen may close,
    so that the 68k Layers and hook handles of a native layer go before the
    layer does. Snapshots are taken again when a window of the program gets
    a message other than a VERIFY one, whose reply Intuition may be waiting
    for, after the depth calls and in the poll that the outermost
    run loop calls every KAFFEINE_GUI_POLL_SLICES slices (kaffeine_cpu.c),
    also without guest tasks; never while the program holds the lock of a
    layer, since LockLayerInfo() after LockLayer() would invert the order
    of the layer locks.

    A program may change WFLG_BACKDROP in Window.Flags and LAYERBACKDROP in
    Layer.Flags of its own window before WindowToBack() or WindowToFront().
    Until such a call, the snapshots leave each bit as the program wrote it;
    the call sets just that bit in the native Flags, under LockIBase() and
    LockLayer() respectively, keeps every other native flag, and calls
    Intuition. Intuition moves the window later, in its input handler; the
    snapshots show the move once it has happened.

    InstallLayerHook() takes the layer of a window of the program and
    LAYERS_BACKFILL, LAYERS_NOBACKFILL or a handle that the bridge handed
    out for a native hook of that same layer; it returns the previous hook
    the same way. On a window layer a 68k Hook would run guest code from
    native layers code and stops the program, as does every other call of
    layers.library and the graphics calls on a layer that the bridge cannot
    take; see kaffeine_layers.h.

    NewLayerInfo() makes a real native Layer_Info and gives the program a
    68k Layer_Info of the classic 102 bytes for it: cleared, with no layer
    and a guest SignalSemaphore as InitSemaphore() leaves it, which protects
    nothing native, plus the documented NEWLAYERINFO_CALLED flag. The pair lives in a table of
    its own, at most KAFFEINE_LAYER_MAX_INFOS at once, and never in the
    generic shadow map, so that no generated thunk resolves it, and no
    LayerInfo of a screen, which is borrowed, can ever reach
    DisposeLayerInfo(). Without room, guest memory or a native Layer_Info
    the call gives 0 and nothing of it stays. DisposeLayerInfo() takes only
    a LayerInfo of NewLayerInfo() that is still the program's, and only
    while its native list holds no layer but the internal root layer of
    layers.library (below); anything else stops the program before the
    native call. No other task knows such a LayerInfo, so the bridge reads
    its list without the lock. Those that the program keeps go at the end,
    once.

    CreateUpfrontHookLayer() makes offscreen layers in such a LayerInfo,
    and nowhere else: never in the LayerInfo of a screen, so the snapshots
    above never see them. The program gives a classic planar BitMap of its
    own (40 bytes, depth 1 to 8, Flags 0 or BMF_STANDARD, at most 4094
    bytes per row and 32767 rows) whose word aligned planes lie whole in
    the guest heap. Its native counterpart, set up by InitBitMap() with the
    converted plane pointers, so the planes stay guest memory and nothing
    native is read from guest bytes, belongs to the LayerInfo, not to a
    layer: with the first layer, layers.library makes an internal root
    layer over the whole BitMap, behind every other layer, which stays
    after the program deleted all of its layers, reads the BitMap whenever
    a deletion uncovers it, and goes only inside DisposeLayerInfo(). So the
    first creation in a LayerInfo fixes its common BitMap (unless layers
    failed it with neither root nor list left, when nothing native reads
    the BitMap and it goes again), every later one
    must give the same 68k BitMap, unchanged, as the classic API has all
    layers of a LayerInfo share one, and the native BitMap lives until
    DisposeLayerInfo() has returned, or for good with a LayerInfo that is
    never disposed of. Simple and smart layers, optionally backdrop,
    without a super BitMap, inside the BitMap, are taken; anything else
    stops the program before any native call. Each such layer is private:
    a stable 68k Layer of the classic layout and a typed 68k RastPort for
    its native one, both bridge-owned, never a Layer in the shadow map,
    with no Window. LayerInfo.top_layer and the links of these 68k Layers
    follow the native list after every creation and deletion; the root
    layer has no 68k Layer and is left out, so the program sees just its
    own layers in their native order, and top_layer 0 without them.

    The backfill hook may be LAYERS_BACKFILL, LAYERS_NOBACKFILL or a 68k
    Hook. For a 68k Hook the layer gets a native hook of its own that runs
    it synchronously with Kaffeine_CallGuest(): A0 is the 68k Hook, A2 the
    68k RastPort, A1 a classic BackFillMessage of the bridge whose Layer is
    the 68k Layer; no native pointer reaches the program. The CPU is that of
    the thunk that called layers, else the current guest CPU, and only on
    the native task that runs the guest; called from any other task, or
    while the bridge deletes layers on its own, the hook does nothing.
    layers.library may run the hook before CreateUpfrontHookLayer() returns:
    every guest and native piece of the layer is made before the native
    call, and the first backfill binds the 68k Layer and RastPort to the
    native layer, so the program already sees them there. A backfill hook
    cannot create or delete layers, nor dispose of a LayerInfo; it stops
    the program. A fault in the hook stops the program; a layer that the
    native call made anyway is deleted without running 68k code again.

    DeleteLayer() takes only a current layer of CreateUpfrontHookLayer().
    The program's locks of it go back and its 68k RastPort leaves before
    the native call, whose result the program gets; a layer that layers
    keeps gets both back. A LayerInfo with such layers cannot be disposed
    of. All guest tasks run on one native executor, whose native layer
    semaphores would take a lock of another guest task for a recursive
    one: while another guest task holds the lock of any layer of a
    LayerInfo, CreateUpfrontHookLayer() and DeleteLayer() there stop the
    program before anything native, LockLayer() and LockLayerRom() of that
    layer stop it too, and AttemptLockLayerRom(), which never blocks, gives
    FALSE. At the end, those that the program kept are deleted before their
    LayerInfos are disposed of; a LayerInfo that still has a layer then is
    never disposed of, and keeps its common BitMap.

    InstallClipRegion() takes such a layer and NULL or a Region of
    NewRegion() of the program (kaffeine_regions.c), whose real native
    Region it installs; Layer.ClipRegion shows the 68k Region, and the
    previous one comes back to the program as it gave it. A Region may
    clip several private layers; each of them leases it, so that it can
    neither change nor be disposed of while a native layer uses it. The
    clip Region leaves the native layer before DeleteLayer(), also before
    a deletion of the bridge, and comes back when layers keeps the layer;
    the Regions themselves go only after every layer, at the end.

    BltBitMap() of graphics takes three kinds of 68k BitMap, as source and
    as destination. A common BitMap: the 68k BitMap that the first layer
    of a LayerInfo of NewLayerInfo() that is still the program's fixed,
    with the same scalars and planes as then, found through the LayerInfo
    table alone, for which the native call gets the bridge's common
    BitMap; such a 68k BitMap with other scalars or planes is refused, and
    never read as a new BitMap. The BitMap of a screen that the program
    opened: the 68k BitMap inside its 68k Screen, which its RastPort names
    (kaffeine_screens.c), known by that address alone, for which the call
    gets the BitMap of the native screen's RastPort at that time, whose
    size graphics gives (GetBitMapAttr()); the BitMap of a screen that the
    program only uses is refused, also when its bytes look planar. Any
    other classic planar BitMap of the program, checked as for
    CreateUpfrontHookLayer(), whose storage is the program's: also one
    whose LayerInfo has been disposed of, or without the layer bridge. The
    call gets a native BitMap of its own for it, from InitBitMap() and the
    checked plane addresses, which lives for this call alone; the same 68k
    BitMap as source and destination gets one for both. No native pointer
    reaches guest memory and no BitMap enters the shadow map. The classic
    WORD coordinates and sizes must be nonnegative and give rectangles
    inside both BitMaps, TempA must be NULL, and anything else stops the
    program before the blit; an empty size gives 0 without a native call.
    The program gets the native result in D0. WaitBlit() follows the
    native call, and until then ctx->bitmap_blits keeps every LayerInfo of
    the program and every screen that it opened, so every native BitMap of
    the call outlives the blit.
*/

#include "kaffeine_intern.h"
#include "kaffeine_hook.h"
#include "kaffeine_layers.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"
#include "kaffeine_regions.h"
#include "kaffeine_screens.h"

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <string.h>

enum { LR_FREE = 0, LR_OWNED = 1, LR_BORROWED = 2, LR_PRIVATE = 3 };

struct LayerPrivate;

/* One 68k Layer. graph is the screen whose snapshot holds it, -1 for an
   owned one between snapshots and for a private one. lflags_pub and
   wflags_pub are the LAYERBACKDROP and WFLG_BACKDROP bits that the bridge
   wrote last: a different bit in guest memory is the program's, not yet
   used. A private one, of CreateUpfrontHookLayer(), has no native layer
   until it is bound. */
struct LayerRecord
{
    ULONG guest;
    struct Layer *native;
    struct Window *window;      /* owned: the window of the program */
    ULONG window_guest;         /* owned: its 68k Window */
    WORD graph;
    UBYTE kind;
    UBYTE seen;
    UWORD lflags_pub;
    UWORD locks;                /* owned: the program's nested locks */
    ULONG lock_task;            /* guest owner, distinct from the native task */
    ULONG wflags_pub;
    struct LayerPrivate *priv;  /* private: what it keeps natively */
};

/* The common BitMap of a LayerInfo of NewLayerInfo(), in native memory of
   its own. layers.library makes the internal root layer of a LayerInfo
   (check_lp) over the BitMap of its first layer and keeps that root, and
   its RastPort's BitMap, after every layer of the program is gone, until
   DisposeLayerInfo() deletes it; deleting a layer backfills the root there
   too. So this lives from the first native creation in the LayerInfo
   that leaves a root or a list until DisposeLayerInfo() has returned, or for good when the LayerInfo
   is never disposed of, and never changes: graphics' own planar BitMap
   from InitBitMap(), with the checked scalars and plane addresses of the
   68k BitMap that it stands for, which every layer of the LayerInfo must
   use unchanged. */
struct LayerCommon
{
    struct BitMap bitmap;
    ULONG guest;                       /* the 68k BitMap */
    ULONG planes[8];                   /* its plane addresses, 0 past Depth */
    UWORD bpr, rows;
    UBYTE depth, flags;
};

/* What a layer of CreateUpfrontHookLayer() keeps natively for its whole
   native life, in native memory of its own: the native backfill hook for a
   68k Hook and what runs that, and the guest objects of the bridge that
   the hook hands out. Its native BitMap is the common one of its
   LayerInfo, which outlives it. */
struct LayerPrivate
{
    struct Hook hook;                  /* the layer's BackFill for a 68k Hook */
    struct KaffeineNativeHook call;    /* runs the 68k Hook */
    struct LayerCommon *common;        /* its LayerInfo's, not this layer's */
    struct KaffeineContext *ctx;
    struct RastPort *rp;               /* the native layer's, once bound */
    ULONG guest_info, guest_bitmap, guest_hook, guest_rp, guest_msg;
    ULONG clip_guest;                  /* its clip Region of the program, leased */
    struct Region *clip_native;        /* the native one, installed in the layer */
    WORD record, info;
    UBYTE dying;                       /* its native layer is going */
    UBYTE active;                      /* its 68k Hook runs */
};

/* The snapshot of one screen. info is the 68k Layer_Info that the 68k
   Layers name: inside the 68k Screen (screen_guest) when the program has
   one, else own_info. */
struct LayerGraph
{
    struct Screen *screen;
    ULONG own_info;
    ULONG screen_guest;
    ULONG info;
    ULONG bar_rp;              /* only a RastPort that this graph created */
};

/* A native backfill hook that the program got from InstallLayerHook() or
   reads in BackFill, valid only for the owned layer where it was seen. */
struct LayerHook
{
    ULONG guest;
    struct Hook *native;
    WORD owner;
};

/* What a snapshot takes of one native layer, under LockLayerInfo() */
struct LayerSnap
{
    struct Layer *native;
    struct RastPort *rp;
    struct Layer *rp_layer;
    APTR window;
    struct Hook *backfill;
    struct Rectangle bounds;
    UWORD priority, flags;
    WORD scroll_x, scroll_y, width, height;
    WORD record;
};

/* A Layer_Info of NewLayerInfo() of the program and its 68k Layer_Info;
   a free slot has neither. common: its common BitMap, from the first
   native creation in it that leaves a root or a list on. stuck: it holds a layer that layers refused
   to delete, and is never disposed of. */
struct LayerInfoRecord
{
    ULONG guest;
    struct Layer_Info *native;
    struct LayerCommon *common;
    BOOL stuck;
};

struct KaffeineLayers
{
    struct LayerRecord records[KAFFEINE_LAYER_MAX_RECORDS];
    struct LayerGraph graphs[KAFFEINE_LAYER_MAX_GRAPHS];
    struct LayerHook hooks[KAFFEINE_LAYER_MAX_HOOKS];
    struct LayerInfoRecord infos[KAFFEINE_LAYER_MAX_INFOS];
    struct LayerSnap snap[KAFFEINE_LAYER_MAX_DEPTH];
    UWORD snap_count;
    struct Layer *snap_bar, *snap_screen_layer;
    ULONG snap_bar_rp;
    BOOL snap_bar_rp_new;
    /* the program's native layer locks, of all layers */
    ULONG locks;               /* only window/screen layer locks */
    ULONG private_locks;       /* private infos cannot block Intuition */
    /* Private layers: how many there are, the record that
       CreateUpfrontHookLayer() is making (-1), the CPU and native task of
       the creation or deletion in progress (gate), the 68k backfill hooks
       running, and quiet while the bridge deletes layers on its own, when
       no 68k hook may run. */
    UWORD privates;
    WORD pending;
    UWORD gate, callbacks, quiet;
    void *gate_cpu;
    struct Task *gate_task;
};

/* Stop the program with return code 20, which the thunk returns to D0. */
static IPTR layers_refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

static ULONG layers_task(struct KaffeineContext *ctx)
{
    ULONG base = ctx->m68k_sysbase;

    if (!ctx->mem || !base || base >= ctx->mem_size ||
        ctx->mem_size - base < M68K_EXECBASE_THISTASK + 4)
        return 0;
    return m68k_read32(ctx, base + M68K_EXECBASE_THISTASK);
}

/* Native semaphores see the single native executor for every guest task.
   They must not mistake another guest task for a recursive lock owner.
   Cross-task layer contention is bounded unsupported until a guest wait
   queue exists; refuse it before a native layer call can recurse. */
static BOOL layers_task_access(struct KaffeineContext *ctx, struct LayerRecord *r)
{
    if (r->locks && r->lock_task != layers_task(ctx))
    {
        layers_refuse(ctx, "a layer held by another guest task");
        return FALSE;
    }
    return TRUE;
}

/* A guest SignalSemaphore as InitSemaphore() leaves it */
static void layers_semaphore(struct KaffeineContext *ctx, ULONG ss)
{
    Kaffeine_LayerSemaphoreInit(ctx, ss);
}

/* ── Records ─────────────────────────────────────────────────────── */

static int layers_record_guest(struct KaffeineLayers *st, ULONG guest)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind != LR_FREE && st->records[i].guest == guest)
            return i;
    return -1;
}

/* The record of a native layer in snapshot g, or an owned one anywhere */
static int layers_record_native(struct KaffeineLayers *st, const struct Layer *l, int g)
{
    int i;

    for (i = 0; l && i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind != LR_FREE && st->records[i].native == l &&
            (st->records[i].kind == LR_OWNED || st->records[i].graph == g))
            return i;
    return -1;
}

static int layers_record_window(struct KaffeineLayers *st, ULONG window)
{
    int i;

    for (i = 0; window && i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind == LR_OWNED && st->records[i].window_guest == window)
            return i;
    return -1;
}

static int layers_record_slot(struct KaffeineLayers *st)
{
    int i;

    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind == LR_FREE)
            return i;
    return -1;
}

/* A cleared 68k Layer with its guest semaphore in slot ri, or FALSE */
static BOOL layers_record_make(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                               struct Layer *native, int g, UBYTE kind)
{
    struct LayerRecord *r = &st->records[ri];
    ULONG guest = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_LAYER);

    if (!guest)
        return FALSE;
    memset(r, 0, sizeof(*r));
    r->guest = guest;
    r->native = native;
    r->graph = (WORD)g;
    r->kind = kind;
    layers_semaphore(ctx, guest + M68K_LAYER_LOCK);
    return TRUE;
}

static void layers_hook_free(struct KaffeineContext *ctx, struct LayerHook *h)
{
    if (ctx->mem && h->guest)
        memset(ctx->mem + h->guest, 0, M68K_SIZEOF_HOOK);
    Kaffeine_MirrorRelease(ctx, h->guest);
    h->guest = 0;
    h->native = NULL;
    h->owner = -1;
}

/* The 68k Layer goes, with the hook handles of its layer */
static void layers_record_free(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri)
{
    struct LayerRecord *r = &st->records[ri];
    int i;

    for (i = 0; i < KAFFEINE_LAYER_MAX_HOOKS; i++)
        if (st->hooks[i].guest && st->hooks[i].owner == ri)
            layers_hook_free(ctx, &st->hooks[i]);
    if (ctx->mem && r->guest)
        memset(ctx->mem + r->guest, 0, M68K_SIZEOF_LAYER);
    Kaffeine_MirrorRelease(ctx, r->guest);
    memset(r, 0, sizeof(*r));
    r->graph = -1;
}

/* BackFill as the program sees it: 0, 1, the 68k Hook of a private layer
   for its own native hook, or the handle of a native hook of an owned
   layer, made here with create; 0 for any other hook, and when the handle
   table or the guest heap is full. */
static ULONG layers_hook_guest(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                               struct Hook *native, BOOL create)
{
    struct LayerPrivate *p = st->records[ri].priv;
    int i, slot = -1;
    ULONG guest;

    if (!native)
        return 0;
    if (native == LAYERS_NOBACKFILL)
        return 1;
    if (st->records[ri].kind == LR_PRIVATE)
        return p && p->guest_hook && native == &p->hook ? p->guest_hook : 0;
    if (st->records[ri].kind != LR_OWNED)
        return 0;
    for (i = 0; i < KAFFEINE_LAYER_MAX_HOOKS; i++)
    {
        if (st->hooks[i].guest && st->hooks[i].owner == ri && st->hooks[i].native == native)
            return st->hooks[i].guest;
        if (!st->hooks[i].guest && slot < 0)
            slot = i;
    }
    if (!create || slot < 0 || !(guest = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_HOOK)))
        return 0;
    st->hooks[slot].guest = guest;
    st->hooks[slot].native = native;
    st->hooks[slot].owner = (WORD)ri;
    return guest;
}

static int layers_hook_handle(struct KaffeineLayers *st, ULONG guest)
{
    int i;

    for (i = 0; guest && i < KAFFEINE_LAYER_MAX_HOOKS; i++)
        if (st->hooks[i].guest == guest)
            return i;
    return -1;
}

/* ── Snapshots of screens ────────────────────────────────────────── */

static int layers_graph_of(struct KaffeineLayers *st, const struct Screen *scr)
{
    int g;

    for (g = 0; scr && g < KAFFEINE_LAYER_MAX_GRAPHS; g++)
        if (st->graphs[g].screen == scr)
            return g;
    return -1;
}

static int layers_graph_ensure(struct KaffeineLayers *st, struct Screen *scr)
{
    int g = layers_graph_of(st, scr);

    if (g >= 0 || !scr)
        return g;
    for (g = 0; g < KAFFEINE_LAYER_MAX_GRAPHS; g++)
        if (!st->graphs[g].screen)
        {
            memset(&st->graphs[g], 0, sizeof(st->graphs[g]));
            st->graphs[g].screen = scr;
            return g;
        }
    bug("[kaffeine] layer snapshot table full, no layers for screen %p\n", scr);
    return -1;
}

/* Whether the screen of snapshot g stays open for the bridge: a window of
   the program has its layer there (but the record exclude), or the
   program has a 68k Screen of it. Reads nothing native. */
static BOOL layers_graph_held(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g,
                              int exclude)
{
    struct Screen *scr = st->graphs[g].screen;
    int i;

    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (i != exclude && st->records[i].kind == LR_OWNED && st->records[i].graph == g)
            return TRUE;
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].guest && ctx->pubscreens[i].native == scr)
            return TRUE;
    return FALSE;
}

/* An owned 68k Layer leaves its snapshot: its links and LayerInfo are no
   longer known. */
static void layers_record_detach(struct KaffeineContext *ctx, struct LayerRecord *r)
{
    m68k_write32(ctx, r->guest + M68K_LAYER_FRONT, 0);
    m68k_write32(ctx, r->guest + M68K_LAYER_BACK, 0);
    m68k_write32(ctx, r->guest + M68K_LAYER_LAYERINFO, 0);
    r->graph = -1;
}

/* The snapshot of a screen goes, before the screen may close or its 68k
   Screen goes: borrowed 68k Layers go, owned ones stay with their windows.
   Touches nothing native. */
static void layers_bar_rp_free(struct KaffeineContext *ctx, ULONG guest)
{
    Kaffeine_RastPortDetach(ctx, guest);
    Kaffeine_MirrorRelease(ctx, guest);
}

static void layers_graph_retire(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g)
{
    struct LayerGraph *gr = &st->graphs[g];
    ULONG sg = gr->screen_guest;
    int i;

    /* Before the native screen can delete the bar's internal RastPort. */
    layers_bar_rp_free(ctx, gr->bar_rp);
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
    {
        if (st->records[i].kind == LR_FREE || st->records[i].graph != g)
            continue;
        if (st->records[i].kind == LR_OWNED)
            layers_record_detach(ctx, &st->records[i]);
        else
            layers_record_free(ctx, st, i);
    }
    /* the 68k Screen, if it is still the one that the bridge wrote */
    if (sg && Kaffeine_ScreenGuest(ctx, gr->screen) == sg)
    {
        m68k_write32(ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_TOP_LAYER, 0);
        m68k_write32(ctx, sg + M68K_SCR_BARLAYER, 0);
        m68k_write32(ctx, sg + M68K_SCR_RASTPORT + M68K_RP_LAYER, 0);
    }
    if (gr->own_info)
    {
        if (ctx->mem)
            memset(ctx->mem + gr->own_info, 0, M68K_SIZEOF_LAYER_INFO);
        Kaffeine_MirrorRelease(ctx, gr->own_info);
    }
    memset(gr, 0, sizeof(*gr));
}

/* The 68k Layer_Info of snapshot g: in the 68k Screen while there is one,
   else one of the bridge; 0 without memory. */
static ULONG layers_graph_info(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g)
{
    struct LayerGraph *gr = &st->graphs[g];
    ULONG sg = Kaffeine_ScreenGuest(ctx, gr->screen);

    if (sg)
    {
        gr->screen_guest = sg;
        return sg + M68K_SCR_LAYERINFO;
    }
    gr->screen_guest = 0;
    if (!gr->own_info)
    {
        if (!(gr->own_info = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_LAYER_INFO)))
            return 0;
        layers_semaphore(ctx, gr->own_info + M68K_LI_LOCK);
    }
    return gr->own_info;
}

/* Take the native layer list of a screen, front to back, under its
   LayerInfo lock; FALSE when it has more layers than a snapshot takes. */
static BOOL layers_snapshot(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g)
{
    struct LayerGraph *gr = &st->graphs[g];
    struct Screen *scr = gr->screen;
    struct Layer_Info *li = &scr->LayerInfo;
    struct Layer *l;
    UWORD n = 0;
    int i;
    BOOL whole = TRUE;

    st->snap_bar_rp = 0;
    st->snap_bar_rp_new = FALSE;
    /* A periodic snapshot must never wait for Intuition while it may be
       waiting for this program to reply to a VERIFY message. The native
       LockLayerInfo is an ObtainSemaphore of this same public lock. */
    if (!AttemptSemaphore(&li->Lock))
        return FALSE;
    for (l = li->top_layer; l; l = l->back)
    {
        struct LayerSnap *s;

        if (n == KAFFEINE_LAYER_MAX_DEPTH)
        {
            whole = FALSE;
            break;
        }
        s = &st->snap[n++];
        s->native = l;
        s->rp = l->rp;
        s->rp_layer = l->rp ? l->rp->Layer : NULL;
        s->window = l->Window;
        s->backfill = l->BackFill;
        s->bounds = l->bounds;
        s->priority = l->priority;
        s->flags = l->Flags;
        s->scroll_x = l->Scroll_X;
        s->scroll_y = l->Scroll_Y;
        s->width = l->Width;
        s->height = l->Height;
        s->record = -1;
    }
    st->snap_bar = scr->BarLayer;
    st->snap_screen_layer = scr->RastPort.Layer;
    /* A custom screen's bar has its own internal RastPort, distinct from
       Screen.RastPort. Only the program that opened the screen may draw
       through it. Read and attach it under LayerInfo, while the layer and
       its RastPort are alive; borrowed public-screen bars stay read-only.
       Keep a new allocation provisional until the whole graph publishes. */
    for (i = 0; whole && i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].native == scr && ctx->pubscreens[i].guest &&
            ctx->pubscreens[i].owned)
        {
            UWORD j;

            for (j = 0; j < n; j++)
                if (st->snap[j].native == st->snap_bar && st->snap[j].rp)
                {
                    ULONG rp = Kaffeine_ShadowGuest(ctx, st->snap[j].rp,
                                                   KAFFEINE_SHADOW_RASTPORT);
                    if (rp)
                        st->snap_bar_rp = rp == gr->bar_rp ? rp : 0;
                    else
                    {
                        st->snap_bar_rp = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_RASTPORT);
                        if (st->snap_bar_rp &&
                            !Kaffeine_RastPortAttach(ctx, st->snap_bar_rp, st->snap[j].rp))
                        {
                            Kaffeine_MirrorRelease(ctx, st->snap_bar_rp);
                            st->snap_bar_rp = 0;
                        }
                        st->snap_bar_rp_new = st->snap_bar_rp != 0;
                        if (!st->snap_bar_rp)
                            whole = FALSE;
                    }
                    break;
                }
            break;
        }
    ReleaseSemaphore(&li->Lock);
    st->snap_count = whole ? n : 0;
    return whole;
}

/* The 68k Layer of a native layer of the current snapshot, or 0 */
static ULONG layers_snap_guest(struct KaffeineLayers *st, const struct Layer *l)
{
    UWORD i;

    for (i = 0; l && i < st->snap_count; i++)
        if (st->snap[i].native == l && st->snap[i].record >= 0)
            return st->records[st->snap[i].record].guest;
    return 0;
}

/* Write one 68k Layer from its snapshot. A LAYERBACKDROP that the program
   changed stays until a depth call uses it. */
static void layers_publish(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                           const struct LayerSnap *s, ULONG front, ULONG back, ULONG info)
{
    struct LayerRecord *r = &st->records[ri];
    ULONG a = r->guest;
    UWORD flags = s->flags;
    UWORD wrote = m68k_read16(ctx, a + M68K_LAYER_FLAGS) & LAYERBACKDROP;
    ULONG i;
    ULONG rp = Kaffeine_ShadowGuest(ctx, s->rp, KAFFEINE_SHADOW_RASTPORT);

    if (wrote != r->lflags_pub)
        flags = (UWORD)((flags & ~LAYERBACKDROP) | wrote);
    else
        r->lflags_pub = flags & LAYERBACKDROP;
    m68k_write32(ctx, a + M68K_LAYER_FRONT, front);
    m68k_write32(ctx, a + M68K_LAYER_BACK, back);
    m68k_write32(ctx, a + M68K_LAYER_CLIPRECT, 0);
    m68k_write32(ctx, a + M68K_LAYER_RP, rp);
    if (rp && s->rp_layer == s->native)
        m68k_write32(ctx, rp + M68K_RP_LAYER, a);
    m68k_write16(ctx, a + M68K_LAYER_MINX, (UWORD)s->bounds.MinX);
    m68k_write16(ctx, a + M68K_LAYER_MINY, (UWORD)s->bounds.MinY);
    m68k_write16(ctx, a + M68K_LAYER_MAXX, (UWORD)s->bounds.MaxX);
    m68k_write16(ctx, a + M68K_LAYER_MAXY, (UWORD)s->bounds.MaxY);
    m68k_write32(ctx, a + M68K_LAYER_RESERVED, 0);
    m68k_write16(ctx, a + M68K_LAYER_PRIORITY, s->priority);
    m68k_write16(ctx, a + M68K_LAYER_FLAGS, flags);
    m68k_write32(ctx, a + M68K_LAYER_SUPERBITMAP, 0);
    m68k_write32(ctx, a + M68K_LAYER_SUPERCLIPRECT, 0);
    m68k_write32(ctx, a + M68K_LAYER_WINDOW,
                 Kaffeine_ShadowGuest(ctx, s->window, KAFFEINE_SHADOW_WINDOW));
    m68k_write16(ctx, a + M68K_LAYER_SCROLL_X, (UWORD)s->scroll_x);
    m68k_write16(ctx, a + M68K_LAYER_SCROLL_Y, (UWORD)s->scroll_y);
    for (i = 0; i < 5; i++)
        m68k_write32(ctx, a + M68K_LAYER_CR + 4 * i, 0);
    m68k_write32(ctx, a + M68K_LAYER_LAYERINFO, info);
    m68k_write32(ctx, a + M68K_LAYER_BACKFILL, layers_hook_guest(ctx, st, ri, s->backfill, TRUE));
    m68k_write32(ctx, a + M68K_LAYER_RESERVED1, 0);
    m68k_write32(ctx, a + M68K_LAYER_CLIPREGION,
                 r->kind == LR_PRIVATE && r->priv ? r->priv->clip_guest : 0);
    m68k_write32(ctx, a + M68K_LAYER_SAVECLIPRECTS, 0);
    m68k_write16(ctx, a + M68K_LAYER_WIDTH, (UWORD)s->width);
    m68k_write16(ctx, a + M68K_LAYER_HEIGHT, (UWORD)s->height);
    m68k_write32(ctx, a + M68K_LAYER_DAMAGELIST, 0);
}

/* Write the snapshot just taken for g, all or nothing: a 68k Layer for
   each native layer that has none, the links in native order, the top
   layer, the bar and the screen fields; borrowed 68k Layers whose layer
   has left the list go. FALSE, with nothing written, without memory or
   room for the new 68k Layers or the 68k Layer_Info. */
static BOOL layers_graph_publish(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g)
{
    struct LayerGraph *gr = &st->graphs[g];
    UWORD n = st->snap_count, i;
    int made[KAFFEINE_LAYER_MAX_DEPTH], fresh = 0, ri, k;
    ULONG info = layers_graph_info(ctx, st, g), top;

    if (!info)
        return FALSE;
    for (i = 0; i < n; i++)
        st->snap[i].record = (WORD)layers_record_native(st, st->snap[i].native, g);
    for (i = 0; i < n; i++)
    {
        if (st->snap[i].record >= 0)
            continue;
        ri = layers_record_slot(st);
        if (ri < 0 || !layers_record_make(ctx, st, ri, st->snap[i].native, g, LR_BORROWED))
        {
            bug("[kaffeine] no room for the 68k Layers of screen %p, snapshot kept\n",
                gr->screen);
            for (k = 0; k < fresh; k++)
                layers_record_free(ctx, st, made[k]);
            for (i = 0; i < n; i++)
                st->snap[i].record = -1;
            return FALSE;
        }
        st->snap[i].record = (WORD)ri;
        made[fresh++] = ri;
    }
    for (ri = 0; ri < KAFFEINE_LAYER_MAX_RECORDS; ri++)
        st->records[ri].seen = 0;
    if (gr->bar_rp != st->snap_bar_rp)
        layers_bar_rp_free(ctx, gr->bar_rp);
    gr->bar_rp = st->snap_bar_rp;
    for (i = 0; i < n; i++)
    {
        st->records[st->snap[i].record].seen = 1;
        st->records[st->snap[i].record].graph = (WORD)g;
    }
    for (i = 0; i < n; i++)
        layers_publish(ctx, st, st->snap[i].record, &st->snap[i],
                       i ? st->records[st->snap[i - 1].record].guest : 0,
                       i + 1 < n ? st->records[st->snap[i + 1].record].guest : 0, info);
    for (ri = 0; ri < KAFFEINE_LAYER_MAX_RECORDS; ri++)
    {
        struct LayerRecord *r = &st->records[ri];

        if (r->kind == LR_FREE || r->graph != g || r->seen)
            continue;
        if (r->kind == LR_OWNED)
        {
            /* a window of the program whose layer is not in the list */
            m68k_write32(ctx, r->guest + M68K_LAYER_FRONT, 0);
            m68k_write32(ctx, r->guest + M68K_LAYER_BACK, 0);
            m68k_write32(ctx, r->guest + M68K_LAYER_LAYERINFO, info);
        }
        else
            layers_record_free(ctx, st, ri);
    }
    top = n ? st->records[st->snap[0].record].guest : 0;
    m68k_write32(ctx, info + M68K_LI_TOP_LAYER, top);
    if (gr->screen_guest)
    {
        m68k_write32(ctx, gr->screen_guest + M68K_SCR_BARLAYER, layers_snap_guest(st, st->snap_bar));
        m68k_write32(ctx, gr->screen_guest + M68K_SCR_RASTPORT + M68K_RP_LAYER,
                     layers_snap_guest(st, st->snap_screen_layer));
    }
    gr->info = info;
    return TRUE;
}

/* A new snapshot of g, unless the program holds a layer lock */
static BOOL layers_graph_refresh(struct KaffeineContext *ctx, struct KaffeineLayers *st, int g)
{
    if (g < 0 || st->locks || !st->graphs[g].screen)
        return FALSE;
    if (!layers_snapshot(ctx, st, g))
    {
        return FALSE;
    }
    if (!layers_graph_publish(ctx, st, g))
    {
        if (st->snap_bar_rp_new)
            layers_bar_rp_free(ctx, st->snap_bar_rp);
        return FALSE;
    }
    return TRUE;
}

void Kaffeine_LayersRefresh(struct KaffeineContext *ctx)
{
    struct KaffeineLayers *st = ctx->layers;
    int i;

    if (!st || st->locks)
        return;
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].guest && ctx->pubscreens[i].native)
            layers_graph_ensure(st, ctx->pubscreens[i].native);
    /* owned 68k Layers whose snapshot went while their window stayed */
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind == LR_OWNED && st->records[i].graph < 0)
            st->records[i].graph = (WORD)layers_graph_ensure(st, st->records[i].window->WScreen);
    for (i = 0; i < KAFFEINE_LAYER_MAX_GRAPHS; i++)
    {
        if (!st->graphs[i].screen)
            continue;
        if (!layers_graph_held(ctx, st, i, -1))
            layers_graph_retire(ctx, st, i);
        else
            layers_graph_refresh(ctx, st, i);
    }
}

/* ── What windows and screens tell the bridge ────────────────────── */

static BOOL layers_window_opened(struct KaffeineContext *ctx, ULONG mw, struct Window *w)
{
    struct KaffeineLayers *st = ctx->layers;
    struct Layer *l = w->WLayer;
    struct LayerRecord *r;
    ULONG rp;
    int g, ri;

    if (!st || !l || layers_record_window(st, mw) >= 0)
        return TRUE;
    if ((g = layers_graph_ensure(st, w->WScreen)) < 0)
        return FALSE;
    ri = layers_record_native(st, l, g);
    if (ri < 0)
    {
        ri = layers_record_slot(st);
        if (ri < 0 || !layers_record_make(ctx, st, ri, l, g, LR_OWNED))
        {
            bug("[kaffeine] no room for the 68k Layer of window %p\n", w);
            if (!layers_graph_held(ctx, st, g, -1))
                layers_graph_retire(ctx, st, g);
            return FALSE;
        }
    }
    /* a layer that a snapshot had seen keeps its 68k Layer */
    r = &st->records[ri];
    if (r->kind == LR_BORROWED)
    {
        /* A new window reused a vanished borrowed native address. Treat
           its old guest flag as already published, never pending. Keep
           the prior whole graph and guest semaphore until a fresh native
           snapshot succeeds. */
        r->lflags_pub = m68k_read16(ctx, r->guest + M68K_LAYER_FLAGS) & LAYERBACKDROP;
    }
    r->kind = LR_OWNED;
    r->native = l;
    r->window = w;
    r->window_guest = mw;
    r->graph = (WORD)g;
    r->wflags_pub = m68k_read32(ctx, mw + M68K_WIN_FLAGS) & WFLG_BACKDROP;
    m68k_write32(ctx, mw + M68K_WIN_WLAYER, r->guest);
    rp = m68k_read32(ctx, mw + M68K_WIN_RPORT);
    if (rp && w->RPort && w->RPort->Layer == l &&
        Kaffeine_ShadowNative(ctx, rp, KAFFEINE_SHADOW_RASTPORT) == w->RPort)
        m68k_write32(ctx, rp + M68K_RP_LAYER, r->guest);
    layers_graph_refresh(ctx, st, g);
    return TRUE;
}

static void layers_window_closing(struct KaffeineContext *ctx, ULONG mw, struct Window *w)
{
    struct KaffeineLayers *st = ctx->layers;
    struct LayerRecord *r;
    ULONG guest, front, back, rp, info;
    int ri, g, i;

    (void)w;
    if (!st || (ri = layers_record_window(st, mw)) < 0)
        return;
    r = &st->records[ri];
    g = r->graph;
    guest = r->guest;
    /* the program's locks go back before Intuition deletes the layer */
    for (; r->locks; r->locks--, st->locks--)
        UnlockLayer(r->native);
    /* the layer leaves the list as DeleteLayer() takes it out */
    front = m68k_read32(ctx, guest + M68K_LAYER_FRONT);
    back = m68k_read32(ctx, guest + M68K_LAYER_BACK);
    if (layers_record_guest(st, front) >= 0 && m68k_read32(ctx, front + M68K_LAYER_BACK) == guest)
        m68k_write32(ctx, front + M68K_LAYER_BACK, back);
    if (layers_record_guest(st, back) >= 0 && m68k_read32(ctx, back + M68K_LAYER_FRONT) == guest)
        m68k_write32(ctx, back + M68K_LAYER_FRONT, front);
    if (g >= 0 && (info = st->graphs[g].info) &&
        m68k_read32(ctx, info + M68K_LI_TOP_LAYER) == guest)
        m68k_write32(ctx, info + M68K_LI_TOP_LAYER, back);
    /* borrowed layers that belong to the window, such as its GZZ border */
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind == LR_BORROWED &&
            m68k_read32(ctx, st->records[i].guest + M68K_LAYER_WINDOW) == mw)
            m68k_write32(ctx, st->records[i].guest + M68K_LAYER_WINDOW, 0);
    m68k_write32(ctx, mw + M68K_WIN_WLAYER, 0);
    rp = m68k_read32(ctx, mw + M68K_WIN_RPORT);
    if (rp && m68k_read32(ctx, rp + M68K_RP_LAYER) == guest)
        m68k_write32(ctx, rp + M68K_RP_LAYER, 0);
    layers_record_free(ctx, st, ri);
    if (g >= 0 && !layers_graph_held(ctx, st, g, -1))
        layers_graph_retire(ctx, st, g);
}

static ULONG layers_window_flags(struct KaffeineContext *ctx, ULONG mw, ULONG native_flags)
{
    struct KaffeineLayers *st = ctx->layers;
    ULONG wrote;
    int ri;

    if (!st || (ri = layers_record_window(st, mw)) < 0)
        return native_flags;
    wrote = m68k_read32(ctx, mw + M68K_WIN_FLAGS) & WFLG_BACKDROP;
    if (wrote != st->records[ri].wflags_pub)
        return (native_flags & ~(ULONG)WFLG_BACKDROP) | wrote;
    st->records[ri].wflags_pub = native_flags & WFLG_BACKDROP;
    return native_flags;
}

static void layers_window_synced(struct KaffeineContext *ctx, ULONG mw, struct Window *w)
{
    struct KaffeineLayers *st = ctx->layers;
    int ri;

    (void)w;
    if (st && (ri = layers_record_window(st, mw)) >= 0)
    {
        if (st->records[ri].graph < 0)
            st->records[ri].graph = (WORD)layers_graph_ensure(st, w->WScreen);
        layers_graph_refresh(ctx, st, st->records[ri].graph);
    }
}

static void layers_screen_mirrored(struct KaffeineContext *ctx, struct Screen *scr)
{
    struct KaffeineLayers *st = ctx->layers;

    if (st)
        layers_graph_refresh(ctx, st, layers_graph_ensure(st, scr));
}

static void layers_screen_closing(struct KaffeineContext *ctx, struct Screen *scr)
{
    struct KaffeineLayers *st = ctx->layers;
    int g;

    if (!st)
        return;
    for (g = 0; g < KAFFEINE_LAYER_MAX_GRAPHS; g++)
        if (st->graphs[g].screen && (!scr || st->graphs[g].screen == scr))
            layers_graph_retire(ctx, st, g);
}

static const struct KaffeineLayerOps layer_ops =
{
    layers_window_opened,
    layers_window_closing,
    layers_window_flags,
    layers_window_synced,
    layers_screen_mirrored,
    layers_screen_closing
};

/* ── Depth calls ─────────────────────────────────────────────────── */

/* The backdrop bits that the program changed go to the native window and
   layer, each alone, under the lock that Intuition and layers take. */
static void layers_backdrop(struct KaffeineContext *ctx, struct LayerRecord *r)
{
    ULONG window = m68k_read32(ctx, r->window_guest + M68K_WIN_FLAGS) & WFLG_BACKDROP;
    UWORD layer = m68k_read16(ctx, r->guest + M68K_LAYER_FLAGS) & LAYERBACKDROP;

    if (window != r->wflags_pub)
    {
        ULONG lock = LockIBase(0);

        r->window->Flags = (r->window->Flags & ~(ULONG)WFLG_BACKDROP) | window;
        UnlockIBase(lock);
        r->wflags_pub = window;
    }
    if (layer != r->lflags_pub)
    {
        LockLayer(0, r->native);
        r->native->Flags = (UWORD)((r->native->Flags & ~LAYERBACKDROP) | layer);
        UnlockLayer(r->native);
        r->lflags_pub = layer;
    }
}

/* WindowToBack() and WindowToFront() of a window of the program. Intuition
   moves the window later; the snapshot is taken again now, and shows the
   move once it has happened. NULL does nothing, as in Intuition. */
static IPTR layers_depth(struct KaffeineContext *ctx, ULONG window, BOOL front)
{
    struct KaffeineLayers *st = ctx->layers;
    struct Window *w;
    int ri = -1;

    if (!window)
        return 0;
    if (!(w = Kaffeine_ShadowNative(ctx, window, KAFFEINE_SHADOW_WINDOW)))
        return layers_refuse(ctx, front ? "WindowToFront(): not a window of the program"
                                        : "WindowToBack(): not a window of the program");
    if (st && (ri = layers_record_window(st, window)) >= 0)
    {
        if (!layers_task_access(ctx, &st->records[ri]))
            return ctx->exit_code;
        if (st->locks &&
            (((m68k_read32(ctx, window + M68K_WIN_FLAGS) & WFLG_BACKDROP) !=
                st->records[ri].wflags_pub) ||
             ((m68k_read16(ctx, st->records[ri].guest + M68K_LAYER_FLAGS) & LAYERBACKDROP) !=
                st->records[ri].lflags_pub)))
            return layers_refuse(ctx, "a backdrop change while holding a layer lock");
        layers_backdrop(ctx, &st->records[ri]);
    }
    if (front)
        WindowToFront(w);
    else
        WindowToBack(w);
    if (ri >= 0)
    {
        if (st->records[ri].graph < 0)
            st->records[ri].graph = (WORD)layers_graph_ensure(st, w->WScreen);
        layers_graph_refresh(ctx, st, st->records[ri].graph);
    }
    return 0;
}

/* intuition -306: WindowToBack(A0=window) */
IPTR Kaffeine_WindowToBack(struct KaffeineContext *ctx, void *cpu)
{
    return layers_depth(ctx, THUNK_A(0), FALSE);
}

/* intuition -312: WindowToFront(A0=window) */
IPTR Kaffeine_WindowToFront(struct KaffeineContext *ctx, void *cpu)
{
    return layers_depth(ctx, THUNK_A(0), TRUE);
}

/* ── Layer locks ─────────────────────────────────────────────────── */

/* Whether record ri is a private layer with a native layer that is not
   going */
static BOOL layers_private_live(struct KaffeineLayers *st, int ri)
{
    const struct LayerRecord *r = &st->records[ri];

    return r->kind == LR_PRIVATE && r->native && r->priv && !r->priv->dying;
}

/* The owned or live private record of a 68k Layer that the program
   passes, whichever guest task holds it, or -1 after stopping the
   program */
static int layers_known(struct KaffeineContext *ctx, ULONG layer, const char *why)
{
    struct KaffeineLayers *st = ctx->layers;
    int ri = st ? layers_record_guest(st, layer) : -1;

    if (ri < 0 || (st->records[ri].kind != LR_OWNED && !layers_private_live(st, ri)))
    {
        layers_refuse(ctx, why);
        return -1;
    }
    return ri;
}

/* The same, and not held by another guest task */
static int layers_owned(struct KaffeineContext *ctx, ULONG layer, const char *why)
{
    int ri = layers_known(ctx, layer, why);

    if (ri >= 0 && !layers_task_access(ctx, &ctx->layers->records[ri]))
        return -1;
    return ri;
}

/* The program locks the layer of one of its windows, nested; the lock
   goes back at the latest before the window closes. */
static IPTR layers_lock(struct KaffeineContext *ctx, ULONG layer, BOOL attempt)
{
    int ri = layers_known(ctx, layer, "a layer lock of a layer that is not one of the program's");
    struct LayerRecord *r;

    if (ri < 0)
        return ctx->exit_code;
    r = &ctx->layers->records[ri];
    /* AttemptLockLayerRom() never blocks: a lock that another guest task
       holds is busy, so the attempt gives FALSE and the program runs on,
       without a native call, which would succeed by recursion on the one
       native executor of all guest tasks. LockLayer() and LockLayerRom()
       would have to wait for that task, which they cannot until a guest
       wait queue exists: they stop the program. */
    if (r->locks && r->lock_task != layers_task(ctx))
    {
        if (attempt)
            return FALSE;
        layers_task_access(ctx, r);
        return ctx->exit_code;
    }
    if (r->locks == KAFFEINE_LAYER_MAX_LOCKS)
        return layers_refuse(ctx, "too many nested layer locks");
    if (attempt)
    {
        if (!AttemptLockLayerRom(r->native))
            return FALSE;
    }
    else
        LockLayer(0, r->native);
    if (!r->locks)
        r->lock_task = layers_task(ctx);
    r->locks++;
    if (r->kind == LR_PRIVATE)
        ctx->layers->private_locks++;
    else
        ctx->layers->locks++;
    return TRUE;
}

static IPTR layers_unlock(struct KaffeineContext *ctx, ULONG layer)
{
    int ri = layers_owned(ctx, layer, "a layer unlock of a layer that is not one of the program's");
    struct LayerRecord *r;

    if (ri < 0)
        return ctx->exit_code;
    r = &ctx->layers->records[ri];
    if (!r->locks)
        return layers_refuse(ctx, "a layer unlock without a lock of the program");
    UnlockLayer(r->native);
    r->locks--;
    if (!r->locks)
        r->lock_task = 0;
    if (r->kind == LR_PRIVATE)
        ctx->layers->private_locks--;
    else
        ctx->layers->locks--;
    return 0;
}

/* layers -96: LockLayer(A0=dummy, A1=layer) */
IPTR Kaffeine_LockLayer(struct KaffeineContext *ctx, void *cpu)
{
    IPTR result = layers_lock(ctx, THUNK_A(1), FALSE);

    return ctx->running ? 0 : result;
}

/* layers -102: UnlockLayer(A0=layer) */
IPTR Kaffeine_UnlockLayer(struct KaffeineContext *ctx, void *cpu)
{
    return layers_unlock(ctx, THUNK_A(0));
}

/* graphics -432: LockLayerRom(A5=layer) */
IPTR Kaffeine_LockLayerRom(struct KaffeineContext *ctx, void *cpu)
{
    IPTR result = layers_lock(ctx, THUNK_A(5), FALSE);

    return ctx->running ? 0 : result;
}

/* graphics -438: UnlockLayerRom(A5=layer) */
IPTR Kaffeine_UnlockLayerRom(struct KaffeineContext *ctx, void *cpu)
{
    return layers_unlock(ctx, THUNK_A(5));
}

/* graphics -654: AttemptLockLayerRom(A5=layer) -> D0=success. Never
   blocks: FALSE, with the program running on and no native call, while
   another guest task holds the layer. */
IPTR Kaffeine_AttemptLockLayerRom(struct KaffeineContext *ctx, void *cpu)
{
    return layers_lock(ctx, THUNK_A(5), TRUE);
}

/* ── Backfill hooks ──────────────────────────────────────────────── */

/* layers -198: InstallLayerHook(A0=layer, A1=hook) -> D0=old hook. The
   layer of a window of the program, with LAYERS_BACKFILL (0),
   LAYERS_NOBACKFILL (1) or a handle that the bridge gave for a native hook
   of that layer; the previous hook comes back the same way. A private
   layer takes 0, 1 and the 68k Hook that it was made with, whose native
   hook stays with the layer. Anything else stops the program before the
   native call. NULL does nothing and gives NULL, as in layers.library. */
IPTR Kaffeine_InstallLayerHook(struct KaffeineContext *ctx, void *cpu)
{
    ULONG layer = THUNK_A(0), hook = THUNK_A(1), result;
    struct KaffeineLayers *st = ctx->layers;
    struct LayerRecord *r;
    struct Hook *native, *old;
    int ri, h;

    if (!layer)
        return 0;
    if ((ri = layers_owned(ctx, layer, "InstallLayerHook(): not the layer of a window of the program")) < 0)
        return ctx->exit_code;
    r = &st->records[ri];
    if (hook == 0 || hook == 1)
        native = hook ? LAYERS_NOBACKFILL : NULL;
    else if (r->kind == LR_PRIVATE && r->priv->guest_hook && hook == r->priv->guest_hook)
        native = &r->priv->hook;
    else if ((h = layers_hook_handle(st, hook)) >= 0 && st->hooks[h].owner == ri)
        native = st->hooks[h].native;
    else
        return layers_refuse(ctx, "InstallLayerHook(): a 68k Hook cannot be a native backfill hook");
    /* the handle of the previous hook before anything native changes */
    LockLayer(0, r->native);
    old = r->native->BackFill;
    result = layers_hook_guest(ctx, st, ri, old, TRUE);
    if (old && old != LAYERS_NOBACKFILL && !result)
    {
        UnlockLayer(r->native);
        return layers_refuse(ctx, "InstallLayerHook(): no room for the handle of the previous hook");
    }
    InstallLayerHook(r->native, native);
    UnlockLayer(r->native);
    m68k_write32(ctx, r->guest + M68K_LAYER_BACKFILL, hook);
    return result;
}

/* ── LayerInfos of the program ───────────────────────────────────── */

/* The slot of a 68k Layer_Info of NewLayerInfo() that is still the
   program's, or -1 */
static int layers_info_owned(struct KaffeineLayers *st, ULONG guest)
{
    int i;

    for (i = 0; st && guest && i < KAFFEINE_LAYER_MAX_INFOS; i++)
        if (st->infos[i].native && st->infos[i].guest == guest)
            return i;
    return -1;
}

/* The 68k Layer_Info of a slot goes, cleared, and the slot is free. With
   disposed, DisposeLayerInfo() has returned, so its root layer is gone and
   the common BitMap goes too; else layers keeps the native one, and the
   common BitMap stays with it for good. */
static void layers_info_release(struct KaffeineContext *ctx, struct LayerInfoRecord *r,
                                BOOL disposed)
{
    if (ctx->mem && r->guest)
        memset(ctx->mem + r->guest, 0, M68K_SIZEOF_LAYER_INFO);
    Kaffeine_MirrorRelease(ctx, r->guest);
    if (disposed)
        FreeVec(r->common);
    r->guest = 0;
    r->native = NULL;
    r->common = NULL;
    r->stuck = FALSE;
}

/* Whether a private layer, alive or being made, is in LayerInfo slot i */
static BOOL layers_info_private(struct KaffeineLayers *st, int i)
{
    int ri;

    for (ri = 0; ri < KAFFEINE_LAYER_MAX_RECORDS; ri++)
        if (st->records[ri].kind == LR_PRIVATE && st->records[ri].priv->info == i)
            return TRUE;
    return FALSE;
}

/* Whether slot i has layers that its disposal would free under them. Its
   native list may hold nothing but the internal root layer, check_lp,
   which layers.library made with the first layer, over the common BitMap,
   and which DisposeLayerInfo() deletes itself: that deletion fails, and
   the root outlives the freed LayerInfo, while any layer is in front of
   it. A private layer alive or being made, one that layers refused to
   delete, any other native layer, a root outside the list or over another
   BitMap, and a list longer than the bound all keep it. No other task
   knows the LayerInfo, so its list is read without the lock. */
static BOOL layers_info_kept(struct KaffeineLayers *st, int i)
{
    const struct Layer_Info *li = st->infos[i].native;
    const struct LayerCommon *c = st->infos[i].common;
    const struct Layer *l;
    BOOL root = FALSE;
    UWORD steps = 0;

    if (layers_info_private(st, i) || st->infos[i].stuck)
        return TRUE;
    for (l = li->top_layer; l; l = l->back)
    {
        if (++steps > KAFFEINE_LAYER_MAX_DEPTH || l != li->check_lp || l->LayerInfo != li ||
            !c || !l->rp || l->rp->BitMap != &c->bitmap)
            return TRUE;
        root = TRUE;
    }
    return li->check_lp && !root;
}

/* layers -144: NewLayerInfo() -> D0=li. A native Layer_Info of the
   program and its 68k Layer_Info, or 0 without room, guest memory or
   native memory, and then nothing of it stays. */
IPTR Kaffeine_NewLayerInfo(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    struct Layer_Info *native;
    ULONG guest;
    int i, slot = -1;

    (void)cpu;
    for (i = 0; st && i < KAFFEINE_LAYER_MAX_INFOS && slot < 0; i++)
        if (!st->infos[i].native)
            slot = i;
    if (slot < 0)
    {
        if (st)
            bug("[kaffeine] NewLayerInfo(): no room for another LayerInfo\n");
        return 0;
    }
    /* the guest one first: without it, nothing native is made */
    if (!(guest = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_LAYER_INFO)))
        return 0;
    if (!(native = NewLayerInfo()))
    {
        Kaffeine_MirrorRelease(ctx, guest);
        return 0;
    }
    layers_semaphore(ctx, guest + M68K_LI_LOCK);
    m68k_write16(ctx, guest + M68K_LI_FLAGS, native->Flags & NEWLAYERINFO_CALLED);
    st->infos[slot].guest = guest;
    st->infos[slot].native = native;
    st->infos[slot].common = NULL;
    st->infos[slot].stuck = FALSE;
    return guest;
}

/* layers -150: DisposeLayerInfo(A0=li). Only a LayerInfo of
   NewLayerInfo() that is still the program's, without layers; a LayerInfo
   of a screen, of the bridge, one already disposed of, any other pointer
   and NULL stop the program before the native call, as does a call from
   a backfill hook. */
IPTR Kaffeine_DisposeLayerInfo(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    int i = layers_info_owned(st, THUNK_A(0));

    if (i < 0)
        return layers_refuse(ctx, "DisposeLayerInfo(): not a LayerInfo of NewLayerInfo() of the program");
    if (st->gate || st->callbacks)
        return layers_refuse(ctx, "DisposeLayerInfo(): from a backfill hook");
    if (ctx->bitmap_blits)
        return layers_refuse(ctx, "DisposeLayerInfo(): during a blit");
    if (layers_info_kept(st, i))
        return layers_refuse(ctx, "DisposeLayerInfo(): the LayerInfo still has layers");
    /* deletes the internal root, which reads the common BitMap */
    DisposeLayerInfo(st->infos[i].native);
    layers_info_release(ctx, &st->infos[i], TRUE);
    return 0;
}

/* ── Private offscreen layers ────────────────────────────────────── */

/* The private record of a native layer, or -1 */
static int layers_private_of(struct KaffeineLayers *st, const struct Layer *l)
{
    int i;

    for (i = 0; l && i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind == LR_PRIVATE && st->records[i].native == l)
            return i;
    return -1;
}

/* The 68k Layers of LayerInfo slot info follow its native list, front to
   back: links, scalars, RastPort and top_layer. The list that the program
   sees is that of its own layers alone, in their real native order: the
   internal root layer, which layers.library puts behind them with the
   first one and keeps after the last one is deleted, and any other native
   layer without a private record of this LayerInfo, is left out and gets
   no 68k Layer, so top_layer is 0 once the program has no layer there,
   and the backmost layer of the program has no back link. */
static void layers_private_relink(struct KaffeineContext *ctx, struct KaffeineLayers *st, int info)
{
    ULONG guest_info = st->infos[info].guest;
    int order[KAFFEINE_LAYER_MAX_PRIVATE], n = 0, i, ri;
    struct LayerSnap s;
    struct Layer *l;
    UWORD steps = 0;

    for (l = st->infos[info].native->top_layer;
         l && steps < KAFFEINE_LAYER_MAX_DEPTH && n < KAFFEINE_LAYER_MAX_PRIVATE;
         l = l->back, steps++)
        if ((ri = layers_private_of(st, l)) >= 0 && st->records[ri].priv->info == info)
            order[n++] = ri;
    for (i = 0; i < n; i++)
    {
        l = st->records[order[i]].native;
        s.native = l;
        s.rp = l->rp;
        s.rp_layer = l->rp ? l->rp->Layer : NULL;
        s.window = NULL;
        s.backfill = l->BackFill;
        s.bounds = l->bounds;
        s.priority = l->priority;
        s.flags = l->Flags;
        s.scroll_x = l->Scroll_X;
        s.scroll_y = l->Scroll_Y;
        s.width = l->Width;
        s.height = l->Height;
        s.record = (WORD)order[i];
        layers_publish(ctx, st, order[i], &s, i ? st->records[order[i - 1]].guest : 0,
                       i + 1 < n ? st->records[order[i + 1]].guest : 0, guest_info);
    }
    m68k_write32(ctx, guest_info + M68K_LI_TOP_LAYER, n ? st->records[order[0]].guest : 0);
}

/* The 68k Layer and RastPort of private record ri stand for native layer
   l from now on; FALSE when l is no new layer of its LayerInfo or the
   RastPort has no room in the shadow map. */
static BOOL layers_private_bind(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                                struct Layer *l)
{
    struct LayerRecord *r = &st->records[ri];
    struct LayerPrivate *p = r->priv;

    if (!l || r->native || l->LayerInfo != st->infos[p->info].native || !l->rp ||
        layers_private_of(st, l) >= 0 || !Kaffeine_RastPortAttach(ctx, p->guest_rp, l->rp))
        return FALSE;
    r->native = l;
    p->rp = l->rp;
    m68k_write32(ctx, p->guest_rp + M68K_RP_LAYER, r->guest);
    m68k_write32(ctx, p->guest_rp + M68K_RP_BITMAP, p->guest_bitmap);
    layers_private_relink(ctx, st, p->info);
    return TRUE;
}

/* The native arguments of a backfill of a private layer as guest ones,
   just before its 68k Hook runs: the 68k RastPort, and the BackFillMessage
   of the bridge with the 68k Layer. The first backfill of the layer that
   CreateUpfrontHookLayer() is making binds it. FALSE, which stops the
   program, for anything that is not a backfill of this layer, and for a
   backfill of it from inside its own 68k Hook. */
static BOOL layers_backfill_translate(void *data, APTR object, IPTR message,
                                      ULONG *guest_object, ULONG *guest_message)
{
    struct LayerPrivate *p = data;
    struct KaffeineContext *ctx = p->ctx;
    struct KaffeineLayers *st = ctx->layers;
    const struct BackFillMessage *m = (const struct BackFillMessage *)message;
    struct LayerRecord *r = &st->records[p->record];
    ULONG g = p->guest_msg;

    if (!m || p->active)
        return FALSE;
    if (!r->native &&
        (st->pending != p->record || !layers_private_bind(ctx, st, p->record, m->Layer)))
        return FALSE;
    if (m->Layer != r->native || object != (APTR)p->rp)
        return FALSE;
    m68k_write32(ctx, g + M68K_BFM_LAYER, r->guest);
    m68k_write16(ctx, g + M68K_BFM_BOUNDS, (UWORD)m->Bounds.MinX);
    m68k_write16(ctx, g + M68K_BFM_BOUNDS + 2, (UWORD)m->Bounds.MinY);
    m68k_write16(ctx, g + M68K_BFM_BOUNDS + 4, (UWORD)m->Bounds.MaxX);
    m68k_write16(ctx, g + M68K_BFM_BOUNDS + 6, (UWORD)m->Bounds.MaxY);
    m68k_write32(ctx, g + M68K_BFM_OFFSETX, (ULONG)m->OffsetX);
    m68k_write32(ctx, g + M68K_BFM_OFFSETY, (ULONG)m->OffsetY);
    Kaffeine_RastPortOut(ctx, p->guest_rp, p->rp);
    m68k_write32(ctx, p->guest_rp + M68K_RP_LAYER, r->guest);
    /* The hidden part of a smart layer is drawn in a ClipRect BitMap of
       layers, which has no 68k BitMap. */
    m68k_write32(ctx, p->guest_rp + M68K_RP_BITMAP,
                 p->rp->BitMap == &p->common->bitmap ? p->guest_bitmap : 0);
    p->active = 1;
    *guest_object = p->guest_rp;
    *guest_message = g;
    return TRUE;
}

/* The native BackFill of a private layer with a 68k Hook. Only the native
   task that runs the guest runs the 68k Hook: on the CPU of the creation
   or deletion in progress, else on the current guest CPU, through
   Kaffeine_InitNativeHook()'s entry, which keeps the CPU state. Called
   from any other task, without a guest CPU, after a stop, or while the
   bridge deletes layers on its own, it does nothing. */
AROS_UFH3S(IPTR, layers_backfill_entry,
    AROS_UFHA(struct Hook *, native_hook, A0),
    AROS_UFHA(APTR, object, A2),
    AROS_UFHA(IPTR, message, A1))
{
    AROS_USERFUNC_INIT

    struct LayerPrivate *p = native_hook->h_Data;
    struct KaffeineContext *ctx = p->ctx;
    struct KaffeineLayers *st = ctx->layers;
    struct Task *task = FindTask(NULL);
    UBYTE active = p->active;
    void *cpu;
    IPTR result;

    if (!st || st->quiet || p->dying || !ctx->running || ctx->callback_failed)
        return 0;
    if (st->gate ? task != st->gate_task : (!ctx->exec_task || task != ctx->exec_task))
        return 0;
    cpu = st->gate ? st->gate_cpu : (void *)ctx->cpu;
    if (!cpu)
        return 0;
    p->call.cpu = cpu;
    st->callbacks++;
    result = AROS_UFC3(IPTR, p->call.hook.h_Entry,
        AROS_UFCA(struct Hook *, &p->call.hook, A0),
        AROS_UFCA(APTR, object, A2),
        AROS_UFCA(IPTR, message, A1));
    st->callbacks--;
    p->active = active;
    if (!active && p->rp)
        m68k_write32(ctx, p->guest_rp + M68K_RP_BITMAP, p->guest_bitmap);
    return result;

    AROS_USERFUNC_EXIT
}

/* A classic planar BitMap of the program at bm, as d: 40 bytes in guest
   memory, BytesPerRow even and at most KAFFEINE_LAYER_MAX_BPR, Rows 1 to
   KAFFEINE_LAYER_MAX_ROWS, so that the root layer over all of it has WORD
   bounds, Depth 1 to 8, Flags 0 or BMF_STANDARD, pad 0, and each plane of
   its depth word aligned and whole in the guest heap. Only d's scalars
   and guest addresses are set; its native BitMap is not. */
static BOOL layers_private_bitmap(struct KaffeineContext *ctx, ULONG bm, struct LayerCommon *d)
{
    ULONG lo = 0, hi = ctx->mem_size, size, i;

    if (ctx->heap_start < ctx->heap_end && ctx->heap_end <= ctx->mem_size)
    {
        lo = ctx->heap_start;
        hi = ctx->heap_end;
    }
    if (!bm || (bm & 1) || bm >= ctx->mem_size || ctx->mem_size - bm < M68K_SIZEOF_BITMAP)
        return FALSE;
    d->guest = bm;
    d->bpr = m68k_read16(ctx, bm + M68K_BM_BYTESPERROW);
    d->rows = m68k_read16(ctx, bm + M68K_BM_ROWS);
    d->flags = ctx->mem[bm + M68K_BM_FLAGS];
    d->depth = ctx->mem[bm + M68K_BM_DEPTH];
    if (!d->bpr || (d->bpr & 1) || d->bpr > KAFFEINE_LAYER_MAX_BPR || !d->rows ||
        d->rows > KAFFEINE_LAYER_MAX_ROWS || (d->flags && d->flags != BMF_STANDARD) ||
        !d->depth || d->depth > 8 || m68k_read16(ctx, bm + M68K_BM_PAD))
        return FALSE;
    /* At most 4094 * 32767: no overflow of ULONG */
    size = (ULONG)d->bpr * d->rows;
    for (i = 0; i < 8; i++)
    {
        d->planes[i] = i < d->depth ? m68k_read32(ctx, bm + M68K_BM_PLANES + 4 * i) : 0;
        if (i < d->depth && (!d->planes[i] || (d->planes[i] & 1) || d->planes[i] < lo ||
                             d->planes[i] >= hi || hi - d->planes[i] < size))
            return FALSE;
    }
    return TRUE;
}

/* Whether the 68k BitMap that d describes is the common one, unchanged */
static BOOL layers_private_common(const struct LayerCommon *c, const struct LayerCommon *d)
{
    ULONG i;

    if (c->guest != d->guest || c->bpr != d->bpr || c->rows != d->rows ||
        c->depth != d->depth || c->flags != d->flags)
        return FALSE;
    for (i = 0; i < 8; i++)
        if (c->planes[i] != d->planes[i])
            return FALSE;
    return TRUE;
}

/* A new common BitMap for d, from InitBitMap() and d's plane addresses
   alone, or NULL without memory or when graphics disagrees */
static struct LayerCommon *layers_private_common_make(struct KaffeineContext *ctx,
                                                      const struct LayerCommon *d)
{
    struct LayerCommon *c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR);
    ULONG i;

    if (!c)
        return NULL;
    *c = *d;
    memset(&c->bitmap, 0, sizeof(c->bitmap));
    /* at most 4094 * 8 = 32752 pixels: a UWORD */
    InitBitMap(&c->bitmap, d->depth, (UWORD)(d->bpr * 8), d->rows);
    if (c->bitmap.BytesPerRow != d->bpr || c->bitmap.Rows != d->rows ||
        c->bitmap.Depth != d->depth)
    {
        bug("[kaffeine] CreateUpfrontHookLayer(): InitBitMap() disagrees with the 68k BitMap\n");
        FreeVec(c);
        return NULL;
    }
    for (i = 0; i < 8; i++)
        c->bitmap.Planes[i] = i < d->depth ? ctx->mem + d->planes[i] : NULL;
    return c;
}

/* The bridge's side of private record ri goes: its 68k Layer, RastPort
   and message, cleared, and its native storage, unless keep, when layers
   still has a layer that uses it. */
static void layers_private_free(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                                BOOL keep)
{
    struct LayerPrivate *p = st->records[ri].priv;
    ULONG guest[2], size[2];
    int i;

    /* A native constructor may have freed a bound layer itself. Its
       native locks are gone, but the guest accounting must retire too. */
    st->private_locks -= st->records[ri].locks;
    st->records[ri].locks = 0;
    st->records[ri].lock_task = 0;
    guest[0] = p->guest_rp;
    size[0] = M68K_SIZEOF_RASTPORT;
    guest[1] = p->guest_msg;
    size[1] = M68K_SIZEOF_BFM;
    Kaffeine_RastPortDetach(ctx, p->guest_rp);
    for (i = 0; i < 2; i++)
    {
        if (!guest[i])
            continue;
        if (ctx->mem)
            memset(ctx->mem + guest[i], 0, size[i]);
        Kaffeine_MirrorRelease(ctx, guest[i]);
    }
    if (!keep)
        FreeVec(p);
    st->records[ri].priv = NULL;
    st->privates--;
    layers_record_free(ctx, st, ri);
}

/* Native layer l of private record ri goes, with no 68k hook running, and
   then the record: TRUE, or FALSE when layers keeps l, which then gets no
   hook of the bridge and keeps the native storage, and whose LayerInfo,
   with its common BitMap, is never disposed of. hyperlayers' DeleteLayer()
   refuses only a layer with children in front of it, before it changes
   anything, which no layer of the program has; the refusal is handled all
   the same, as layers.library documents one. */
static BOOL layers_private_unmake(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                                  struct Layer *l)
{
    struct LayerRecord *r = &st->records[ri];
    struct LayerPrivate *p = r->priv;
    int info = p->info;
    LONG ok;

    for (; r->locks; r->locks--, st->private_locks--)
        UnlockLayer(r->native);
    Kaffeine_RastPortDetach(ctx, p->guest_rp);
    p->dying = 1;
    st->quiet++;
    /* the clip Region leaves the native layer before the layer goes, and
       stays out of one that layers keeps */
    if (p->clip_native)
        InstallClipRegion(l, NULL);
    ok = DeleteLayer(0, l);
    if (!ok)
    {
        bug("[kaffeine] layers kept a layer of the program, its LayerInfo stays\n");
        LockLayer(0, l);
        InstallLayerHook(l, LAYERS_NOBACKFILL);
        UnlockLayer(l);
        st->infos[info].stuck = TRUE;
    }
    st->quiet--;
    Kaffeine_RegionUnlease(ctx, p->clip_guest);
    p->clip_guest = 0;
    p->clip_native = NULL;
    layers_private_free(ctx, st, ri, !ok);
    layers_private_relink(ctx, st, info);
    return ok != 0;
}

/* Whether another guest task holds a lock of a live private layer of
   LayerInfo slot info. All guest tasks share one native executor, so the
   native semaphores would let a creation or deletion in there by
   recursion, which rebuilds the ClipRects of the locked layer and may run
   its backfill hook on the wrong guest CPU. Locks of the current guest
   task, and those in other LayerInfos, keep nothing out. */
static BOOL layers_info_contended(struct KaffeineContext *ctx, struct KaffeineLayers *st, int info)
{
    ULONG task = layers_task(ctx);
    int ri;

    for (ri = 0; ri < KAFFEINE_LAYER_MAX_RECORDS; ri++)
        if (layers_private_live(st, ri) && st->records[ri].priv->info == info &&
            st->records[ri].locks && st->records[ri].lock_task != task)
            return TRUE;
    return FALSE;
}

/* A creation or deletion runs 68k backfill hooks on the CPU of its thunk,
   and only on this native task. */
static void layers_gate_open(struct KaffeineLayers *st, void *cpu)
{
    st->gate_cpu = cpu;
    st->gate_task = FindTask(NULL);
    st->gate = 1;
}

static void layers_gate_close(struct KaffeineLayers *st)
{
    st->gate = 0;
    st->gate_cpu = NULL;
    st->gate_task = NULL;
}

/* layers -186: CreateUpfrontHookLayer(A0=li, A1=bm, D0=x0, D1=y0, D2=x1,
   D3=y1, D4=flags, A3=hook, A2=bm2) -> D0=layer. A LayerInfo of
   NewLayerInfo() of the program, a classic planar BitMap of guest memory,
   bounds inside it, LAYERSIMPLE or LAYERSMART with LAYERBACKDROP or not,
   LAYERS_BACKFILL, LAYERS_NOBACKFILL or a 68k Hook, and no super BitMap.
   All layers of a LayerInfo share its BitMap, as the classic API
   documents: from the first native creation in it on, only the same 68k
   BitMap, unchanged, is taken. Anything else, a call from a backfill
   hook, and a LayerInfo where another guest task holds a layer lock stop
   the program before any native call. 0 without room or memory, or when
   layers makes no layer, and then nothing of the layer stays; the root
   layer that layers may have made in the LayerInfo anyway stays with the
   common BitMap until its disposal. A first creation that leaves neither
   root nor list fixes no common BitMap. */
IPTR Kaffeine_CreateUpfrontHookLayer(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    ULONG bm = THUNK_A(1), hook = THUNK_A(3), flags = THUNK_D(4);
    ULONG kind = flags & ~(ULONG)LAYERBACKDROP;
    LONG x0 = (LONG)THUNK_D(0), y0 = (LONG)THUNK_D(1), x1 = (LONG)THUNK_D(2), y1 = (LONG)THUNK_D(3);
    int info = layers_info_owned(st, THUNK_A(0)), ri = -1;
    struct KaffeineNativeHook probe;
    struct LayerCommon d, *common;
    struct LayerPrivate *p;
    struct LayerRecord *r;
    struct Hook *backfill;
    struct Layer *l;
    BOOL fresh;

    if (info < 0)
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): not a LayerInfo of NewLayerInfo() of the program");
    if (st->gate || st->callbacks)
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): from a backfill hook");
    if (layers_info_contended(ctx, st, info))
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): a layer of the LayerInfo held by another guest task");
    if (THUNK_A(2))
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): super BitMap layers are not supported");
    if (kind != LAYERSIMPLE && kind != LAYERSMART)
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): unsupported layer flags");
    memset(&d, 0, sizeof(d));
    if (!layers_private_bitmap(ctx, bm, &d))
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): not a classic planar BitMap of the program");
    common = st->infos[info].common;
    fresh = !common;
    if (common && !layers_private_common(common, &d))
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): not the common BitMap of the LayerInfo");
    if (x0 < 0 || y0 < 0 || x0 > x1 || y0 > y1 || x1 >= (LONG)d.bpr * 8 || y1 >= (LONG)d.rows ||
        x1 > 0x7FFF || y1 > 0x7FFF)
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): bounds outside the BitMap");
    if (hook > 1 && !Kaffeine_InitNativeHook(&probe, ctx, cpu, hook, layers_backfill_translate, NULL))
        return layers_refuse(ctx, "CreateUpfrontHookLayer(): not a valid 68k Hook");

    /* Every piece of the layer before the native call, which may run the
       hook before it returns the layer */
    if (st->privates >= KAFFEINE_LAYER_MAX_PRIVATE || (ri = layers_record_slot(st)) < 0 ||
        ctx->num_shadows >= KAFFEINE_MAX_SHADOWS)
    {
        bug("[kaffeine] CreateUpfrontHookLayer(): no room for another layer\n");
        return 0;
    }
    if (!(p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    if (!layers_record_make(ctx, st, ri, NULL, -1, LR_PRIVATE))
    {
        FreeVec(p);
        return 0;
    }
    r = &st->records[ri];
    r->priv = p;
    st->privates++;
    p->ctx = ctx;
    p->record = (WORD)ri;
    p->info = (WORD)info;
    p->guest_info = st->infos[info].guest;
    p->guest_bitmap = bm;
    p->guest_rp = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_RASTPORT);
    p->guest_msg = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_BFM);
    /* The first creation in the LayerInfo makes its common BitMap, which
       is graphics' own planar one: nothing but the checked scalars and
       plane addresses comes from the guest. */
    if (p->guest_rp && p->guest_msg && !common)
        common = layers_private_common_make(ctx, &d);
    if (!p->guest_rp || !p->guest_msg || !common)
    {
        layers_private_free(ctx, st, ri, FALSE);
        return 0;
    }
    p->common = common;
    backfill = hook ? LAYERS_NOBACKFILL : LAYERS_BACKFILL;
    if (hook > 1)
    {
        Kaffeine_InitNativeHook(&p->call, ctx, cpu, hook, layers_backfill_translate, p);
        p->hook.h_Entry = (APTR)layers_backfill_entry;
        p->hook.h_Data = p;
        p->guest_hook = hook;
        backfill = &p->hook;
    }
    /* From here on layers may make the root layer over the common BitMap,
       even when it then makes no layer: the LayerInfo holds the BitMap
       until its disposal once a root or a list survives. */
    st->infos[info].common = common;

    st->pending = (WORD)ri;
    layers_gate_open(st, cpu);
    l = CreateUpfrontHookLayer(st->infos[info].native, &common->bitmap, x0, y0, x1, y1,
                               (LONG)flags, backfill, NULL);
    layers_gate_close(st);
    st->pending = -1;

    if (l && ctx->running && (r->native == l || (!r->native && layers_private_bind(ctx, st, ri, l))))
    {
        layers_private_relink(ctx, st, info);
        return r->guest;
    }
    /* Nothing of a partial creation stays, and no 68k hook runs again. A
       root layer that layers made stays, over the common BitMap, and is
       left out of the 68k list. */
    if (l)
        layers_private_unmake(ctx, st, ri, l);
    else
    {
        layers_private_free(ctx, st, ri, FALSE);
        layers_private_relink(ctx, st, info);
    }
    /* A first attempt that left neither the internal root (check_lp, NULL
       from NewLayerInfo() on) nor any layer in the native list: nothing
       native reads the common BitMap, which goes, and a later creation may
       give another one. */
    if (fresh && !st->infos[info].native->top_layer && !st->infos[info].native->check_lp)
    {
        st->infos[info].common = NULL;
        FreeVec(common);
    }
    return ctx->running ? 0 : (IPTR)ctx->exit_code;
}

/* layers -90: DeleteLayer(A0=dummy, A1=layer) -> D0=success. Only a
   current layer of CreateUpfrontHookLayer() of the program; the layer of a
   window, a borrowed one, one already deleted, any other pointer, a call
   from a backfill hook and a LayerInfo where another guest task holds the
   lock of this or any other layer stop the program before the native
   call. The program's locks of the layer go back and its 68k RastPort
   leaves first; when layers keeps the layer, both come back. */
IPTR Kaffeine_DeleteLayer(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    int ri = st ? layers_record_guest(st, THUNK_A(1)) : -1, info;
    struct LayerPrivate *p;
    struct LayerRecord *r;
    struct Layer *l;
    UWORD locks;
    ULONG task;
    LONG result;

    if (ri < 0 || !layers_private_live(st, ri))
        return layers_refuse(ctx, "DeleteLayer(): not a layer of CreateUpfrontHookLayer() of the program");
    if (st->gate || st->callbacks)
        return layers_refuse(ctx, "DeleteLayer(): from a backfill hook");
    r = &st->records[ri];
    p = r->priv;
    l = r->native;
    info = p->info;
    if (layers_info_contended(ctx, st, info))
        return layers_refuse(ctx, "DeleteLayer(): a layer of the LayerInfo held by another guest task");
    locks = r->locks;
    task = r->lock_task;
    for (; r->locks; r->locks--, st->private_locks--)
        UnlockLayer(l);
    r->lock_task = 0;
    Kaffeine_RastPortDetach(ctx, p->guest_rp);
    p->dying = 1;
    layers_gate_open(st, cpu);
    /* the clip Region leaves the native layer first; it stays leased, and
       comes back when layers keeps the layer */
    if (p->clip_native)
        InstallClipRegion(l, NULL);
    result = DeleteLayer(0, l);
    if (!result && p->clip_native)
        InstallClipRegion(l, p->clip_native);
    layers_gate_close(st);
    if (!result)
    {
        p->dying = 0;
        if (Kaffeine_RastPortAttach(ctx, p->guest_rp, p->rp))
            m68k_write32(ctx, p->guest_rp + M68K_RP_BITMAP, p->guest_bitmap);
        for (; r->locks < locks; r->locks++, st->private_locks++)
            LockLayer(0, l);
        r->lock_task = task;
        layers_private_relink(ctx, st, info);
        return ctx->running ? 0 : (IPTR)ctx->exit_code;
    }
    Kaffeine_RegionUnlease(ctx, p->clip_guest);
    layers_private_free(ctx, st, ri, FALSE);
    layers_private_relink(ctx, st, info);
    return ctx->running ? (IPTR)result : (IPTR)ctx->exit_code;
}

/* layers -174: InstallClipRegion(A0=layer, A1=region) -> D0=old region. A
   current layer of CreateUpfrontHookLayer() of the program, and NULL or a
   live Region of NewRegion() of the program (kaffeine_regions.c), which may
   be the clip Region of other private layers too; the previous one comes
   back as the program gave it, NULL included. Each private layer leases
   its clip Region, which then can neither change nor go until the layer
   lets go of it: by another call, or before the layer itself goes. The
   native call runs with the gate open, so a backfill that it causes runs
   on this thunk's CPU and cannot change layers or Regions. Window and
   borrowed layers, any other Region, a call from a backfill hook or with
   a layer lock of the program, and a native clip Region that the bridge
   did not install stop the program before the native call. */
IPTR Kaffeine_InstallClipRegion(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    ULONG region = THUNK_A(1), old_guest;
    int ri = st ? layers_record_guest(st, THUNK_A(0)) : -1;
    struct Region *native = NULL, *old, *old_native;
    struct LayerPrivate *p;
    struct LayerRecord *r;

    if (ri < 0 || !layers_private_live(st, ri))
        return layers_refuse(ctx, "InstallClipRegion(): not a private layer of the program");
    if (st->gate || st->callbacks || st->locks || st->private_locks)
        return layers_refuse(ctx, "InstallClipRegion(): during a backfill or with a layer lock");
    r = &st->records[ri];
    p = r->priv;
    if (r->native->ClipRegion != p->clip_native)
        return layers_refuse(ctx, "InstallClipRegion(): a native clip Region cannot reach the guest");
    if (region && !(native = Kaffeine_RegionLease(ctx, region)))
        return ctx->exit_code;
    old_guest = p->clip_guest;
    old_native = p->clip_native;
    layers_gate_open(st, cpu);
    old = InstallClipRegion(r->native, native);
    layers_gate_close(st);
    /* The native layer clips with the new one now, whatever happened: the
       counts follow it, and the old one is free to change or go. */
    p->clip_guest = region;
    p->clip_native = native;
    m68k_write32(ctx, r->guest + M68K_LAYER_CLIPREGION, region);
    Kaffeine_RegionUnlease(ctx, old_guest);
    if (old != old_native)
        return layers_refuse(ctx, "InstallClipRegion(): unexpected previous native Region");
    return ctx->running ? old_guest : (IPTR)ctx->exit_code;
}

/* Whether native layers code runs on behalf of the program: a creation,
   deletion or clip change of a private layer, or a 68k backfill hook. No
   Region of the program may change then. */
BOOL Kaffeine_LayersNativeBusy(struct KaffeineContext *ctx)
{
    struct KaffeineLayers *st = ctx->layers;

    return st && (st->gate || st->callbacks);
}

/* At the end of the program: every private layer goes, with no 68k hook
   running, before any LayerInfo is disposed of. */
static void layers_private_cleanup(struct KaffeineContext *ctx, struct KaffeineLayers *st)
{
    int i;

    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
    {
        if (st->records[i].kind != LR_PRIVATE)
            continue;
        if (st->records[i].native)
            layers_private_unmake(ctx, st, i, st->records[i].native);
        else
            layers_private_free(ctx, st, i, FALSE);
    }
}

/* ── Blits ───────────────────────────────────────────────────────── */

enum { BLIT_REFUSED, BLIT_COMMON, BLIT_SCREEN, BLIT_PLANAR };

/* One BitMap of a blit: the native BitMap that graphics gets and the size
   of it in pixels. For an ordinary planar 68k BitMap, desc is that BitMap
   as checked, and desc.bitmap becomes the native BitMap of this call
   alone, once the whole call is checked. */
struct BlitSide
{
    struct BitMap *native;
    struct LayerCommon desc;
    ULONG width, height;
    int kind;
};

/* What the 68k BitMap bm is to BltBitMap(), with no native call: the
   BitMap of a screen that the program opened, by its address alone; a
   common BitMap of a LayerInfo of NewLayerInfo() that is still the
   program's, while bm has the scalars and planes it had when the first
   layer fixed it; else an ordinary classic planar BitMap of the program,
   checked as CreateUpfrontHookLayer() checks it. The BitMap of a screen
   that the program only uses, and a common one whose 68k BitMap has
   changed, are refused and never read as an ordinary BitMap. Only the
   tables of the bridges name native BitMaps; guest bytes never do. */
static int layers_blit_side(struct KaffeineContext *ctx, ULONG bm, struct BlitSide *s)
{
    struct KaffeineLayers *st = ctx->layers;
    struct Screen *scr;
    BOOL known = FALSE;
    int i;

    memset(s, 0, sizeof(*s));
    switch (Kaffeine_ScreenBitMapOf(ctx, bm, &scr))
    {
    case KAFFEINE_SCREEN_BITMAP_OWNED:
        s->native = scr->RastPort.BitMap;
        return s->kind = s->native ? BLIT_SCREEN : BLIT_REFUSED;
    case KAFFEINE_SCREEN_BITMAP_BORROWED:
        return s->kind = BLIT_REFUSED;
    }
    for (i = 0; st && bm && i < KAFFEINE_LAYER_MAX_INFOS; i++)
    {
        struct LayerCommon *c = st->infos[i].common;

        if (!st->infos[i].native || !c || c->guest != bm)
            continue;
        known = TRUE;
        memset(&s->desc, 0, sizeof(s->desc));
        if (layers_private_bitmap(ctx, bm, &s->desc) && layers_private_common(c, &s->desc))
        {
            s->native = &c->bitmap;
            s->width = (ULONG)c->bpr * 8;
            s->height = c->rows;
            return s->kind = BLIT_COMMON;
        }
    }
    memset(&s->desc, 0, sizeof(s->desc));
    if (known || !layers_private_bitmap(ctx, bm, &s->desc))
        return s->kind = BLIT_REFUSED;
    s->width = (ULONG)s->desc.bpr * 8;
    s->height = s->desc.rows;
    return s->kind = BLIT_PLANAR;
}

/* The native BitMap of this call for a checked ordinary planar 68k
   BitMap: graphics' own, from InitBitMap() and the checked plane
   addresses alone; FALSE when graphics disagrees with the 68k BitMap. */
static BOOL layers_blit_adapter(struct KaffeineContext *ctx, struct BlitSide *s)
{
    struct LayerCommon *d = &s->desc;
    ULONG i;

    memset(&d->bitmap, 0, sizeof(d->bitmap));
    /* at most 4094 * 8 = 32752 pixels: a UWORD */
    InitBitMap(&d->bitmap, d->depth, (UWORD)(d->bpr * 8), d->rows);
    if (d->bitmap.BytesPerRow != d->bpr || d->bitmap.Rows != d->rows ||
        d->bitmap.Depth != d->depth)
        return FALSE;
    for (i = 0; i < 8; i++)
        d->bitmap.Planes[i] = i < d->depth ? ctx->mem + d->planes[i] : NULL;
    s->native = &d->bitmap;
    return TRUE;
}

/* graphics -30: BltBitMap(A0=src, D0=xSrc, D1=ySrc, A1=dest, D2=xDest,
   D3=yDest, D4=xSize, D5=ySize, D6=minterm, D7=mask, A2=tempA) -> D0=planes.
   Between common BitMaps of LayerInfos of the program, BitMaps of screens
   that it opened and its ordinary planar BitMaps, with nonnegative WORD
   coordinates and sizes inside both and no TempA; anything else stops the
   program before the blit. An empty size gives 0 without a native call. */
IPTR Kaffeine_BltBitMap(struct KaffeineContext *ctx, void *cpu)
{
    ULONG sbm = THUNK_A(0), dbm = THUNK_A(1);
    WORD xs = (WORD)THUNK_D(0), ys = (WORD)THUNK_D(1);
    WORD xd = (WORD)THUNK_D(2), yd = (WORD)THUNK_D(3);
    WORD w = (WORD)THUNK_D(4), h = (WORD)THUNK_D(5);
    UBYTE minterm = (UBYTE)THUNK_D(6), mask = (UBYTE)THUNK_D(7);
    struct BlitSide src, dst;
    struct BlitSide *side[2];
    LONG planes;
    int i;

    side[0] = &src;
    side[1] = &dst;
    if (layers_blit_side(ctx, sbm, &src) == BLIT_REFUSED ||
        layers_blit_side(ctx, dbm, &dst) == BLIT_REFUSED)
        return layers_refuse(ctx, "BltBitMap(): not a BitMap that the program may blit");
    if (THUNK_A(2))
        return layers_refuse(ctx, "BltBitMap(): a TempA buffer is not supported");
    if (xs < 0 || ys < 0 || xd < 0 || yd < 0 || w < 0 || h < 0)
        return layers_refuse(ctx, "BltBitMap(): negative coordinates or size");
    if (!w || !h)
        return 0;
    /* The size of a screen's BitMap is graphics', never the 68k snapshot's
       nor a guess from BytesPerRow of a HIDD BitMap. */
    for (i = 0; i < 2; i++)
        if (side[i]->kind == BLIT_SCREEN)
        {
            side[i]->width = (ULONG)GetBitMapAttr(side[i]->native, BMA_WIDTH);
            side[i]->height = (ULONG)GetBitMapAttr(side[i]->native, BMA_HEIGHT);
        }
    /* each at most 32767 + 32767: no overflow */
    if ((ULONG)(xs + w) > src.width || (ULONG)(ys + h) > src.height ||
        (ULONG)(xd + w) > dst.width || (ULONG)(yd + h) > dst.height)
        return layers_refuse(ctx, "BltBitMap(): a rectangle outside its BitMap");
    /* one native BitMap for the same ordinary 68k BitMap on both sides, so
       that graphics sees a blit inside one BitMap */
    if (src.kind == BLIT_PLANAR && !layers_blit_adapter(ctx, &src))
        return layers_refuse(ctx, "BltBitMap(): InitBitMap() disagrees with the 68k BitMap");
    if (dst.kind == BLIT_PLANAR && dbm == sbm)
        dst.native = src.native;
    else if (dst.kind == BLIT_PLANAR && !layers_blit_adapter(ctx, &dst))
        return layers_refuse(ctx, "BltBitMap(): InitBitMap() disagrees with the 68k BitMap");
    /* Every native BitMap stays until the blit is done: no LayerInfo of the
       program is disposed of and no screen of it closes until then, and the
       blitter has finished before the call's own BitMaps go. */
    ctx->bitmap_blits++;
    planes = BltBitMap(src.native, xs, ys, dst.native, xd, yd, w, h, minterm, mask, NULL);
    WaitBlit();
    ctx->bitmap_blits--;
    return ctx->running ? (IPTR)(ULONG)planes : (IPTR)ctx->exit_code;
}

/* Other graphics vectors that consume or initialize a BitMap cannot
   interpret the classic object or an opaque owned-screen handle. */
IPTR kaffeine_bitmaps_Unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return layers_refuse(ctx, "unsupported graphics BitMap call");
}

/* Every layer call that the bridge does not take */
IPTR kaffeine_layers_Unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return layers_refuse(ctx, "unsupported layers.library call");
}

const struct M68KThunkEntry kaffeine_thunks_layers_manual[] = {
    KAFFEINE_LAYERS_THUNKS,
    { 0, NULL }
};
const ULONG kaffeine_thunks_layers_manual_count =
    sizeof(kaffeine_thunks_layers_manual) / sizeof(kaffeine_thunks_layers_manual[0]) - 1;

/* ── Setup and the end ───────────────────────────────────────────── */

BOOL Kaffeine_LayersSetup(struct KaffeineContext *ctx)
{
    struct KaffeineLayers *st;
    int i;

    if (ctx->layers)
        return TRUE;
    if (!(st = AllocVec(sizeof(*st), MEMF_ANY | MEMF_CLEAR)))
    {
        bug("[kaffeine] no memory for the layer bridge, programs see no layers\n");
        return FALSE;
    }
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        st->records[i].graph = -1;
    for (i = 0; i < KAFFEINE_LAYER_MAX_HOOKS; i++)
        st->hooks[i].owner = -1;
    st->pending = -1;
    ctx->layers = st;
    ctx->layer_ops = &layer_ops;
    ctx->gui_poll = Kaffeine_LayersRefresh;
    ctx->gui_poll_slices = 0;
    return TRUE;
}

void Kaffeine_LayersCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineLayers *st = ctx->layers;
    struct LayerRecord *r;
    ULONG rp;
    int i;

    if (!st)
        return;
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
    {
        r = &st->records[i];
        if (r->kind != LR_OWNED)
            continue;
        /* a window still open: its locks go back, and it names no 68k Layer */
        for (; r->locks; r->locks--)
            UnlockLayer(r->native);
        m68k_write32(ctx, r->window_guest + M68K_WIN_WLAYER, 0);
        rp = m68k_read32(ctx, r->window_guest + M68K_WIN_RPORT);
        if (rp && m68k_read32(ctx, rp + M68K_RP_LAYER) == r->guest)
            m68k_write32(ctx, rp + M68K_RP_LAYER, 0);
    }
    /* the layers of CreateUpfrontHookLayer() go before their LayerInfos */
    layers_private_cleanup(ctx, st);
    st->locks = 0;
    st->private_locks = 0;
    for (i = 0; i < KAFFEINE_LAYER_MAX_GRAPHS; i++)
        if (st->graphs[i].screen)
            layers_graph_retire(ctx, st, i);
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind != LR_FREE)
            layers_record_free(ctx, st, i);
    for (i = 0; i < KAFFEINE_LAYER_MAX_HOOKS; i++)
        if (st->hooks[i].guest)
            layers_hook_free(ctx, &st->hooks[i]);
    /* LayerInfos of NewLayerInfo() that the program kept, each once, with
       their internal root layers and then their common BitMaps. One with
       any other layer, such as one that layers refused to delete, is left
       to native memory with its common BitMap rather than freed under it. */
    for (i = 0; i < KAFFEINE_LAYER_MAX_INFOS; i++)
    {
        BOOL kept;

        if (!st->infos[i].native)
            continue;
        if ((kept = layers_info_kept(st, i)))
            bug("[kaffeine] a LayerInfo of the program still has layers, not disposed of\n");
        else
            DisposeLayerInfo(st->infos[i].native);
        layers_info_release(ctx, &st->infos[i], !kept);
    }
    if (ctx->gui_poll == Kaffeine_LayersRefresh)
        ctx->gui_poll = NULL;
    if (ctx->layer_ops == &layer_ops)
        ctx->layer_ops = NULL;
    ctx->layers = NULL;
    FreeVec(st);
}

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
    the same way. A 68k Hook would run guest code from native layers code
    and stops the program, as does every other call of layers.library and
    the graphics calls on a layer that the bridge cannot take; see
    kaffeine_layers.h.

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
    while its native list is empty; anything else stops the program before
    the native call. Since no bridged call puts layers into such a
    LayerInfo, no other task knows it, and the bridge reads its list without
    the lock. Those that the program keeps go at the end, once.
*/

#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_offsets.h"
#include "kaffeine_rastport.h"

#include <aros/debug.h>
#include <exec/memory.h>
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

enum { LR_FREE = 0, LR_OWNED = 1, LR_BORROWED = 2 };

/* One 68k Layer. graph is the screen whose snapshot holds it, -1 for an
   owned one between snapshots. lflags_pub and wflags_pub are the
   LAYERBACKDROP and WFLG_BACKDROP bits that the bridge wrote last: a
   different bit in guest memory is the program's, not yet used. */
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
   a free slot has neither. */
struct LayerInfoRecord
{
    ULONG guest;
    struct Layer_Info *native;
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
    ULONG locks;
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

/* BackFill as the program sees it: 0, 1, or the handle of a native hook of
   an owned layer, made here with create; 0 for any other hook, and when
   the handle table or the guest heap is full. */
static ULONG layers_hook_guest(struct KaffeineContext *ctx, struct KaffeineLayers *st, int ri,
                               struct Hook *native, BOOL create)
{
    int i, slot = -1;
    ULONG guest;

    if (!native)
        return 0;
    if (native == LAYERS_NOBACKFILL)
        return 1;
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
    m68k_write32(ctx, a + M68K_LAYER_CLIPREGION, 0);
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

/* The owned record of a 68k Layer that the program passes, or -1 after
   stopping the program */
static int layers_owned(struct KaffeineContext *ctx, ULONG layer, const char *why)
{
    struct KaffeineLayers *st = ctx->layers;
    int ri = st ? layers_record_guest(st, layer) : -1;

    if (ri < 0 || st->records[ri].kind != LR_OWNED)
    {
        layers_refuse(ctx, why);
        return -1;
    }
    if (!layers_task_access(ctx, &st->records[ri]))
        return -1;
    return ri;
}

/* The program locks the layer of one of its windows, nested; the lock
   goes back at the latest before the window closes. */
static IPTR layers_lock(struct KaffeineContext *ctx, ULONG layer, BOOL attempt)
{
    int ri = layers_owned(ctx, layer, "a layer lock of a layer that is not one of the program's");
    struct LayerRecord *r;

    if (ri < 0)
        return ctx->exit_code;
    r = &ctx->layers->records[ri];
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

/* graphics -654: AttemptLockLayerRom(A5=layer) -> D0=success */
IPTR Kaffeine_AttemptLockLayerRom(struct KaffeineContext *ctx, void *cpu)
{
    return layers_lock(ctx, THUNK_A(5), TRUE);
}

/* ── Backfill hooks ──────────────────────────────────────────────── */

/* layers -198: InstallLayerHook(A0=layer, A1=hook) -> D0=old hook. The
   layer of a window of the program, with LAYERS_BACKFILL (0),
   LAYERS_NOBACKFILL (1) or a handle that the bridge gave for a native hook
   of that layer; the previous hook comes back the same way. Anything else
   stops the program before the native call. NULL does nothing and gives
   NULL, as in layers.library. */
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

/* The 68k Layer_Info of a slot goes, cleared, and the slot is free; the
   caller has disposed of the native one, or keeps it. */
static void layers_info_release(struct KaffeineContext *ctx, struct LayerInfoRecord *r)
{
    if (ctx->mem && r->guest)
        memset(ctx->mem + r->guest, 0, M68K_SIZEOF_LAYER_INFO);
    Kaffeine_MirrorRelease(ctx, r->guest);
    r->guest = 0;
    r->native = NULL;
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
    return guest;
}

/* layers -150: DisposeLayerInfo(A0=li). Only a LayerInfo of
   NewLayerInfo() that is still the program's, without layers; a LayerInfo
   of a screen, of the bridge, one already disposed of, any other pointer
   and NULL stop the program before the native call. */
IPTR Kaffeine_DisposeLayerInfo(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineLayers *st = ctx->layers;
    int i = layers_info_owned(st, THUNK_A(0));

    if (i < 0)
        return layers_refuse(ctx, "DisposeLayerInfo(): not a LayerInfo of NewLayerInfo() of the program");
    if (st->infos[i].native->top_layer)
        return layers_refuse(ctx, "DisposeLayerInfo(): the LayerInfo still has layers");
    DisposeLayerInfo(st->infos[i].native);
    layers_info_release(ctx, &st->infos[i]);
    return 0;
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
    st->locks = 0;
    for (i = 0; i < KAFFEINE_LAYER_MAX_GRAPHS; i++)
        if (st->graphs[i].screen)
            layers_graph_retire(ctx, st, i);
    for (i = 0; i < KAFFEINE_LAYER_MAX_RECORDS; i++)
        if (st->records[i].kind != LR_FREE)
            layers_record_free(ctx, st, i);
    for (i = 0; i < KAFFEINE_LAYER_MAX_HOOKS; i++)
        if (st->hooks[i].guest)
            layers_hook_free(ctx, &st->hooks[i]);
    /* LayerInfos of NewLayerInfo() that the program kept, each once. One
       with layers, which no bridged call makes, is left to native memory
       rather than freed under them. */
    for (i = 0; i < KAFFEINE_LAYER_MAX_INFOS; i++)
    {
        if (!st->infos[i].native)
            continue;
        if (st->infos[i].native->top_layer)
            bug("[kaffeine] a LayerInfo of the program still has layers, not disposed of\n");
        else
            DisposeLayerInfo(st->infos[i].native);
        layers_info_release(ctx, &st->infos[i]);
    }
    if (ctx->gui_poll == Kaffeine_LayersRefresh)
        ctx->gui_poll = NULL;
    if (ctx->layer_ops == &layer_ops)
        ctx->layer_ops = NULL;
    ctx->layers = NULL;
    FreeVec(st);
}

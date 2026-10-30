/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Private offscreen layers of 68k code through literal LVOs, with the real
    dispatcher, callback primitive, native hook entry, RastPort bridge, layer
    bridge and heap: CreateUpfrontHookLayer() (-186) and DeleteLayer() (-90)
    in LayerInfos of NewLayerInfo(). Classic planar BitMaps become native
    ones of the bridge over the same guest planes; 68k backfill hooks run
    synchronously with a 68k RastPort and a classic BackFillMessage, also
    before the constructor returns, on the current guest CPU and only on the
    native task of the guest; identities, links and top_layer follow the
    native list; malformed arguments, foreign handles, recursion, missing
    memory, native and callback failures, deletion and teardown. The native
    layers.library below acts as hyperlayers does: the first creation in a
    LayerInfo makes a hidden internal root layer (check_lp) over the whole
    BitMap it is given, behind every other layer, even when it then makes
    no layer; the root stays after every other layer is deleted, reads its
    BitMap whenever a deletion uncovers it and when DisposeLayerInfo()
    deletes it, which it can only do with no other layer in front. A new
    layer goes in front (behind the non-backdrop ones if it is a backdrop
    layer), its backfill hook runs before it is returned, a deletion runs
    the hooks of the layers behind, and every call is recorded. BltBitMap()
    (-30 of graphics) between the common BitMaps of such LayerInfos, the
    BitMaps of screens that the program opened and its ordinary planar
    BitMaps reaches a reference planar blit and WaitBlit(); the screens are
    set up in the table of screens as the screen bridge leaves them.
    InitBitMap() (-390 of graphics) writes the classic scalars of ordinary
    68k BitMaps as graphics' initbitmap.c does, with no native call, and
    stops before any write for screen BitMaps and kept common ones. Every
    generated thunk is poisoned.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <exec/memory.h>
#include <graphics/gfx.h>
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
#include "kaffeine_layers.h"
#include "kaffeine_screens.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_callback.h"
#include "kaffeine_cpu.h"

/* ── The native side ─────────────────────────────────────────────── */

static KaffeineContext *live;
static std::vector<std::string> events;
static std::map<const Layer *, int> layer_locks;
static int held_locks;
static unsigned native_installs, init_bitmaps, default_fills, new_infos, disposed_infos;
static unsigned clip_calls;

extern "C" struct Region *InstallClipRegion(struct Layer *layer, struct Region *region)
{
    assert(layer && !region);
    ++clip_calls;
    struct Region *old = layer->ClipRegion;
    layer->ClipRegion = region;
    return old;
}
static std::vector<Layer_Info *> native_infos;

/* Native tasks: the one that runs the guest, and another one */
static int executor_id, foreign_id;
static Task *const executor = reinterpret_cast<Task *>(&executor_id);
static Task *const foreign_task = reinterpret_cast<Task *>(&foreign_id);
static Task *current_task = executor;
extern "C" struct Task *FindTask(const char *name)
{
    assert(!name);
    return current_task;
}

extern "C" void LockLayer(LONG dummy, struct Layer *l)
{
    assert(dummy == 0 && l);
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
    ++layer_locks[l];
    ++held_locks;
    return TRUE;
}
extern "C" struct Hook *InstallLayerHook(struct Layer *l, struct Hook *h)
{
    assert(l && layer_locks[l] > 0);
    ++native_installs;
    Hook *old = l->BackFill;
    l->BackFill = h;
    events.push_back("InstallLayerHook");
    return old;
}
extern "C" struct Hook *InstallLayerInfoHook(struct Layer_Info *li, struct Hook *h)
{
    Hook *old = static_cast<Hook *>(li->BlankHook);
    li->BlankHook = h;
    if (li->check_lp) li->check_lp->BackFill = h;
    events.push_back("InstallLayerInfoHook");
    return old;
}
/* The screen of the window test: its LayerInfo is free at once. */
extern "C" void LockLayerInfo(struct Layer_Info *) { events.push_back("LockLayerInfo"); }
extern "C" void UnlockLayerInfo(struct Layer_Info *) { events.push_back("UnlockLayerInfo"); }
extern "C" ULONG AttemptSemaphore(struct SignalSemaphore *) { return 1; }
extern "C" void ReleaseSemaphore(struct SignalSemaphore *) {}

extern "C" struct Layer_Info *NewLayerInfo(void)
{
    ++new_infos;
    events.push_back("NewLayerInfo");
    Layer_Info *li = new Layer_Info();
    li->Flags = NEWLAYERINFO_CALLED;
    native_infos.push_back(li);
    return li;
}
/* The internal root layer of a LayerInfo, as hyperlayers keeps it: what
   its BitMap was when it was made, which it reads later */
struct NativeRoot
{
    Layer layer;
    RastPort rp;
    BitMap *bm;
    BitMap copy;
};
static std::map<const Layer_Info *, NativeRoot *> roots;
static unsigned root_makes, root_reads, root_deletes;

/* The root reads its BitMap: the very one it was made with, unchanged,
   over planes that are still guest memory */
static void root_read(Layer *l)
{
    NativeRoot *r = roots.at(l->LayerInfo);
    const BitMap *bm = l->rp->BitMap;

    assert(l == &r->layer && l == l->LayerInfo->check_lp && bm == r->bm);
    assert(std::memcmp(bm, &r->copy, sizeof(BitMap)) == 0 && bm->Depth >= 1 && bm->Depth <= 8);
    const size_t span = (size_t)bm->BytesPerRow * bm->Rows;
    for (int i = 0; i < bm->Depth; ++i)
    {
        const UBYTE *p = bm->Planes[i];
        assert(p >= live->mem && span <= (size_t)(live->mem + live->mem_size - p));
        volatile UBYTE first = p[0], last = p[span - 1];
        (void)first;
        (void)last;
    }
    ++root_reads;
}

static void forget_root(Layer_Info *li)
{
    NativeRoot *r = roots.at(li);
    roots.erase(li);
    li->check_lp = nullptr;
    delete r;
}

/* As _FreeExtLayerInfo(): the root goes with the LayerInfo. hyperlayers'
   DeleteLayer() of the root fails while a layer is in front of it, and the
   root would outlive the LayerInfo: the bridge never lets that happen. */
extern "C" void DisposeLayerInfo(struct Layer_Info *li)
{
    auto it = std::find(native_infos.begin(), native_infos.end(), li);
    assert(it != native_infos.end());
    if (Layer *root = li->check_lp)
    {
        assert(li->top_layer == root && !root->front && !root->back);
        root_read(root);
        li->top_layer = nullptr;
        forget_root(li);
        ++root_deletes;
    }
    assert(!li->top_layer);
    native_infos.erase(it);
    ++disposed_infos;
    events.push_back("DisposeLayerInfo");
    delete li;
}

/* The BitMaps that InitBitMap() set up since the current thunk began */
static std::vector<const BitMap *> call_inits;

/* As graphics: the scalars of a planar BitMap; the planes are the caller's. */
extern "C" void InitBitMap(struct BitMap *bm, BYTE depth, UWORD width, UWORD height)
{
    ++init_bitmaps;
    call_inits.push_back(bm);
    bm->Depth = (UBYTE)depth;
    bm->Rows = height;
    bm->Flags = BMF_STANDARD;
    bm->pad = 0;
    bm->BytesPerRow = (UWORD)(((width + 15) >> 3) & ~0x1);
}

/* Native layers with their RastPort, as layers makes them */
struct NativeLayer
{
    Layer layer;
    RastPort rp;
};
static std::vector<NativeLayer *> native_layers;
struct CreateCall
{
    Layer_Info *li;
    BitMap *bm;
    BitMap copy;
    LONG x0, y0, x1, y1, flags;
    Hook *hook;
    BitMap *bm2;
};
static std::vector<CreateCall> creates;
static unsigned deletes;
/* EARLY fails after the root (as when its regions find no memory), ROOT
   before it, LATE after it ran the hook of the new layer */
enum CreateMode { CREATE_OK, CREATE_FAILS_EARLY, CREATE_FAILS_LATE, CREATE_ROOT_FAILS };
static CreateMode create_mode;
static bool delete_fails;
/* the hook gets a ClipRect BitMap of layers, as for a hidden part */
static bool swap_bitmap;
static BitMap cliprect_bitmap;
/* the native task that layers runs the hooks on; NULL: the caller's */
static Task *hook_task;

static NativeLayer *native_of(const Layer *l)
{
    for (NativeLayer *n : native_layers)
        if (&n->layer == l)
            return n;
    return nullptr;
}

static void link_front(Layer_Info *li, Layer *l)
{
    Layer *behind = li->top_layer, *prev = nullptr;

    if (l->Flags & LAYERBACKDROP)
        while (behind && behind != li->check_lp && !(behind->Flags & LAYERBACKDROP))
        {
            prev = behind;
            behind = behind->back;
        }
    l->front = prev;
    l->back = behind;
    if (prev)
        prev->back = l;
    else
        li->top_layer = l;
    if (behind)
        behind->front = l;
}

static void unlink_native(Layer *l)
{
    if (l->front)
        l->front->back = l->back;
    else
        l->LayerInfo->top_layer = l->back;
    if (l->back)
        l->back->front = l->front;
    l->front = l->back = nullptr;
}

static void forget(NativeLayer *n)
{
    native_layers.erase(std::find(native_layers.begin(), native_layers.end(), n));
    delete n;
}

/* As _CallLayerHook() of hyperlayers */
static void backfill(Layer *l, const Rectangle &r, LONG ox, LONG oy)
{
    Hook *h = l->BackFill;

    /* the root has the LayerInfo's BlankHook, none here: it clears its
       BitMap */
    if (l == l->LayerInfo->check_lp)
    {
        root_read(l);
        return;
    }
    if (h == LAYERS_BACKFILL)
    {
        ++default_fills;
        return;
    }
    if (h == LAYERS_NOBACKFILL)
        return;
    BackFillMessage m = { l, r, ox, oy };
    BitMap *saved = l->rp->BitMap;
    Task *task = current_task;
    if (swap_bitmap)
        l->rp->BitMap = &cliprect_bitmap;
    if (hook_task)
        current_task = hook_task;
    reinterpret_cast<IPTR (*)(Hook *, APTR, IPTR)>(h->h_Entry)(h, l->rp, reinterpret_cast<IPTR>(&m));
    current_task = task;
    l->rp->BitMap = saved;
}

extern "C" struct Layer *CreateUpfrontHookLayer(struct Layer_Info *li, struct BitMap *bm, LONG x0,
                                                LONG y0, LONG x1, LONG y1, LONG flags,
                                                struct Hook *hook, struct BitMap *bm2)
{
    assert(std::find(native_infos.begin(), native_infos.end(), li) != native_infos.end());
    /* nothing of the guest is handed over as native storage */
    const UBYTE *b = reinterpret_cast<const UBYTE *>(bm);
    assert(b < live->mem || b >= live->mem + live->mem_size);
    creates.push_back({ li, bm, *bm, x0, y0, x1, y1, flags, hook, bm2 });
    events.push_back("CreateUpfrontHookLayer");
    if (create_mode == CREATE_ROOT_FAILS && !li->check_lp)
        return nullptr;
    if (!li->check_lp)
    {
        /* As CreateLayerTagList(): the root over all of the BitMap, with
           the WORD bounds of every layer */
        const LONG max_x = (LONG)bm->BytesPerRow * 8 - 1, max_y = (LONG)bm->Rows - 1;
        assert(max_x >= 0 && max_x <= 0x7FFF && max_y >= 0 && max_y <= 0x7FFF);
        auto *r = new NativeRoot();
        r->bm = bm;
        r->copy = *bm;
        r->layer.bounds = { 0, 0, (WORD)max_x, (WORD)max_y };
        r->layer.Width = (WORD)(max_x + 1);
        r->layer.Height = (WORD)(max_y + 1);
        r->layer.rp = &r->rp;
        r->layer.LayerInfo = li;
        r->rp.Layer = &r->layer;
        r->rp.BitMap = bm;
        roots[li] = r;
        li->check_lp = &r->layer;
        link_front(li, &r->layer);
        ++root_makes;
    }
    else
        /* every layer of the LayerInfo is given the root's BitMap */
        assert(bm == li->check_lp->rp->BitMap);
    if (create_mode == CREATE_FAILS_EARLY)
        return nullptr;
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
    link_front(li, l);
    /* the new layer is filled before it is returned */
    backfill(l, l->bounds, x0, y0);
    if (create_mode == CREATE_FAILS_LATE)
    {
        /* A native object that vanishes owns no semaphore any longer. */
        held_locks -= layer_locks[l];
        layer_locks.erase(l);
        unlink_native(l);
        forget(n);
        return nullptr;
    }
    return l;
}

extern "C" LONG DeleteLayer(LONG dummy, struct Layer *l)
{
    NativeLayer *n = native_of(l);

    assert(dummy == 0 && n);
    ++deletes;
    events.push_back("DeleteLayer");
    /* the bridge let go of it first */
    assert(layer_locks[l] == 0);
    assert(!Kaffeine_ShadowGuest(live, l->rp, KAFFEINE_SHADOW_RASTPORT));
    if (delete_fails)
        return FALSE;
    /* the layers behind show what it hid */
    for (Layer *b = l->back; b; b = b->back)
        backfill(b, b->bounds, b->bounds.MinX, b->bounds.MinY);
    unlink_native(l);
    forget(n);
    return TRUE;
}

/* The BitMap that a screen of the program draws into, as graphics has it:
   planes in native memory, which no guest address reaches, and a width
   that GetBitMapAttr() gives, narrower than BytesPerRow says, as for a
   HIDD BitMap. Only an open one may reach graphics. */
struct NativeScreenBitMap
{
    Screen screen = {};
    BitMap bm = {};
    std::vector<UBYTE> planes;
    ULONG width = 0;
    bool open = false;
};
static std::vector<NativeScreenBitMap *> screen_bitmaps;
static unsigned bitmap_attrs;

static NativeScreenBitMap *screen_of(const BitMap *bm)
{
    for (NativeScreenBitMap *s : screen_bitmaps)
        if (&s->bm == bm)
            return s;
    return nullptr;
}

extern "C" IPTR GetBitMapAttr(struct BitMap *bm, ULONG attribute)
{
    NativeScreenBitMap *s = screen_of(bm);

    assert(s && s->open && !live->bitmap_blits);
    ++bitmap_attrs;
    switch (attribute)
    {
    case BMA_WIDTH: return s->width;
    case BMA_HEIGHT: return s->bm.Rows;
    case BMA_DEPTH: return s->bm.Depth;
    case BMA_FLAGS: return s->bm.Flags & (BMF_DISPLAYABLE | BMF_INTERLEAVED | BMF_STANDARD);
    }
    assert(false);
    return 0;
}

/* graphics' BltBitMap() as a reference over planar BitMaps: copy (0xC0)
   or clear (0x00) of the planes that mask selects, of both depths, giving
   the number of planes written, or blit_forced. Only common BitMaps of
   live LayerInfos, unchanged since their creation, BitMaps of open screens
   and planar BitMaps that InitBitMap() set up during this very call over
   planes in the guest heap may arrive, inside both extents, and WaitBlit()
   follows each before anything else. blit_reenter runs in the middle, as
   if something else ran during the blit. */
enum Origin { ORIGIN_COMMON, ORIGIN_SCREEN, ORIGIN_ADAPTER };
struct BlitCall
{
    BitMap *src, *dst;
    WORD xs, ys, xd, yd, w, h;
    UBYTE minterm, mask;
    UBYTE *temp;
    LONG result;
    Origin src_origin, dst_origin;
    BitMap src_copy, dst_copy;   /* as graphics got them; adapters go with the call */
};
static std::vector<BlitCall> blits;
static unsigned wait_blits;
static bool blit_pending, blit_force;
static LONG blit_forced;
static std::function<void()> blit_reenter;

/* Whether bm is the BitMap of the latest creation that got it, in a
   LayerInfo that is still alive, as that creation got it */
static bool live_common(const BitMap *bm)
{
    for (auto c = creates.rbegin(); c != creates.rend(); ++c)
        if (c->bm == bm)
            return std::find(native_infos.begin(), native_infos.end(), c->li) != native_infos.end() &&
                   std::memcmp(bm, &c->copy, sizeof(BitMap)) == 0;
    return false;
}

/* Where a BitMap that reaches graphics comes from; anything else fails */
static Origin origin(const BitMap *bm)
{
    const UBYTE *b = reinterpret_cast<const UBYTE *>(bm);

    assert(b < live->mem || b >= live->mem + live->mem_size);
    if (live_common(bm))
        return ORIGIN_COMMON;
    if (NativeScreenBitMap *s = screen_of(bm))
    {
        assert(s->open && s->screen.RastPort.BitMap == bm);
        return ORIGIN_SCREEN;
    }
    /* a native BitMap of this call alone: InitBitMap()'s, never one of a
       LayerInfo, over whole word aligned planes of the guest heap */
    assert(std::find(call_inits.begin(), call_inits.end(), bm) != call_inits.end());
    for (const CreateCall &c : creates)
        assert(c.bm != bm);
    assert(bm->Flags == BMF_STANDARD && !bm->pad && bm->Depth >= 1 && bm->Depth <= 8);
    const size_t span = (size_t)bm->BytesPerRow * bm->Rows;
    for (int i = 0; i < 8; ++i)
    {
        const UBYTE *p = bm->Planes[i];
        if (i >= bm->Depth)
        {
            assert(!p);
            continue;
        }
        assert(p >= live->mem + live->heap_start && p < live->mem + live->heap_end);
        assert(!((p - live->mem) & 1) && span <= (size_t)(live->mem + live->heap_end - p));
    }
    return ORIGIN_ADAPTER;
}

static ULONG width_of(const BitMap *bm, Origin o)
{
    return o == ORIGIN_SCREEN ? screen_of(bm)->width : (ULONG)bm->BytesPerRow * 8;
}

static bool plane_bit(const BitMap *bm, int p, int x, int y)
{
    return bm->Planes[p][(size_t)y * bm->BytesPerRow + (x >> 3)] & (0x80 >> (x & 7));
}

extern "C" LONG BltBitMap(struct BitMap *src, WORD xs, WORD ys, struct BitMap *dst, WORD xd,
                          WORD yd, WORD w, WORD h, UBYTE minterm, UBYTE mask, UBYTE *temp)
{
    assert(!blit_pending && !temp && live->bitmap_blits == 1);
    const Origin so = origin(src), dor = origin(dst);
    assert(xs >= 0 && ys >= 0 && xd >= 0 && yd >= 0 && w > 0 && h > 0);
    assert((ULONG)(xs + w) <= width_of(src, so) && ys + h <= src->Rows);
    assert((ULONG)(xd + w) <= width_of(dst, dor) && yd + h <= dst->Rows);
    assert(minterm == 0xC0 || minterm == 0x00);
    events.push_back("BltBitMap");
    if (blit_reenter)
    {
        blit_reenter();
        assert(origin(src) == so && origin(dst) == dor);
    }
    const int depth = std::min(src->Depth, dst->Depth);
    LONG planes = 0;
    for (int p = 0; p < depth; ++p)
    {
        if (!(mask & (1 << p)))
            continue;
        /* through a copy, for overlap inside one BitMap */
        std::vector<bool> bits((size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                bits[(size_t)y * w + x] = minterm == 0xC0 && plane_bit(src, p, xs + x, ys + y);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                UBYTE &byte = dst->Planes[p][(size_t)(yd + y) * dst->BytesPerRow + ((xd + x) >> 3)];
                const UBYTE bit = (UBYTE)(0x80 >> ((xd + x) & 7));
                byte = bits[(size_t)y * w + x] ? (UBYTE)(byte | bit) : (UBYTE)(byte & ~bit);
            }
        ++planes;
    }
    if (blit_force)
        planes = blit_forced;
    blits.push_back({ src, dst, xs, ys, xd, yd, w, h, minterm, mask, temp, planes, so, dor, *src, *dst });
    blit_pending = true;
    return planes;
}

extern "C" void WaitBlit(void)
{
    assert(blit_pending && live->bitmap_blits == 1);
    blit_pending = false;
    ++wait_blits;
    events.push_back("WaitBlit");
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

/* What the 68k hook saw, read from guest memory as 68k code reads it */
struct HookCall
{
    int cpu;
    ULONG a0, a1, a2;
    ULONG msg_layer;
    WORD bounds[4];
    LONG ox, oy;
    ULONG rp_layer, rp_bitmap, layer_rp, layer_info, info_top, layer_backfill;
    void *rp_native;
    ULONG nested;    /* D0 of a library call it made */
};
static std::vector<HookCall> hook_calls;
enum GuestAction
{
    GUEST_DRAW,         /* writes into plane 0 of the 68k BitMap of its RastPort */
    GUEST_FAULT,        /* a CPU exception */
    GUEST_LVO,          /* calls guest_lvo of layers.library through its jump table */
    GUEST_LOCK,         /* locks and unlocks its own layer */
    GUEST_LOCK_KEEP,    /* leaves a lock to test a failed native constructor */
    GUEST_NESTED        /* native code runs the same backfill again from inside */
};
static GuestAction action;
static ULONG guest_lvo;
static std::vector<ULONG> guest_lvo_a, guest_lvo_d;
static ULONG layers_lib_base;

static ULONG rd32(ULONG a) { return m68k_read32(live, a); }
static UWORD rd16(ULONG a) { return m68k_read16(live, a); }

static ULONG guest_call(TestCPU *c, ULONG lvo, const std::vector<ULONG> &a, const std::vector<ULONG> &d)
{
    KaffeineDispatchResult r = {};
    size_t i;

    for (i = 0; i < a.size(); ++i) c->a[i] = a[i];
    for (i = 0; i < d.size(); ++i) c->d[i] = d[i];
    c->a[7] -= 4;
    m68k_write32(live, c->a[7], NESTED_RETURN);
    assert(Kaffeine_DispatchLibraryCall(live, c->self, layers_lib_base - lvo, &r));
    return c->d[0];
}

static void execute(void *s)
{
    TestCPU *c = static_cast<TestCPU *>(s);
    HookCall h = {};

    /* only the 68k hooks run in this test */
    assert(c->pc == HOOK_ENTRY && live->callback_depth > 0);
    h.cpu = c->id;
    h.a0 = c->a[0];
    h.a1 = c->a[1];
    h.a2 = c->a[2];
    h.msg_layer = rd32(h.a1 + M68K_BFM_LAYER);
    for (int i = 0; i < 4; ++i)
        h.bounds[i] = (WORD)rd16(h.a1 + M68K_BFM_BOUNDS + 2 * i);
    h.ox = (LONG)rd32(h.a1 + M68K_BFM_OFFSETX);
    h.oy = (LONG)rd32(h.a1 + M68K_BFM_OFFSETY);
    h.rp_layer = rd32(h.a2 + M68K_RP_LAYER);
    h.rp_bitmap = rd32(h.a2 + M68K_RP_BITMAP);
    h.layer_rp = rd32(h.msg_layer + M68K_LAYER_RP);
    h.layer_info = rd32(h.msg_layer + M68K_LAYER_LAYERINFO);
    h.info_top = rd32(h.layer_info + M68K_LI_TOP_LAYER);
    h.layer_backfill = rd32(h.msg_layer + M68K_LAYER_BACKFILL);
    h.rp_native = Kaffeine_ShadowNative(live, h.a2, KAFFEINE_SHADOW_RASTPORT);
    /* a hook may change every register but A7 */
    for (int i = 0; i < 8; ++i)
        c->d[i] = 0xC0DE0000u + (ULONG)i;
    for (int i = 0; i < 7; ++i)
        c->a[i] = 0xC0DEA000u + (ULONG)i;
    switch (action)
    {
    case GUEST_DRAW:
        if (h.rp_bitmap)
        {
            const ULONG plane = rd32(h.rp_bitmap + M68K_BM_PLANES);
            const ULONG row = rd16(h.rp_bitmap + M68K_BM_BYTESPERROW);
            live->mem[plane + (ULONG)h.bounds[1] * row + ((ULONG)h.bounds[0] >> 3)] = 0x5A;
        }
        break;
    case GUEST_FAULT:
        Kaffeine_StopGuest(live, c->self, 20);
        hook_calls.push_back(h);
        return;
    case GUEST_LVO:
        h.nested = guest_call(c, guest_lvo, guest_lvo_a, guest_lvo_d);
        break;
    case GUEST_LOCK:
        h.nested = guest_call(c, 96, { 0, h.msg_layer }, {});
        assert(live->running && h.nested == 0);
        guest_call(c, 102, { h.msg_layer }, {});
        assert(live->running);
        break;
    case GUEST_LOCK_KEEP:
        h.nested = guest_call(c, 96, { 0, h.msg_layer }, {});
        assert(live->running && h.nested == 0);
        break;
    case GUEST_NESTED:
        {
            Layer *l = static_cast<RastPort *>(h.rp_native)->Layer;
            backfill(l, l->bounds, 0, 0);
        }
        break;
    }
    hook_calls.push_back(h);
    if (!live->running)
        return;
    c->d[0] = 0x600D;
    c->pc = rd32(c->a[7]);
    c->a[7] += 4;
}

static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr };

static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }

static const M68KThunkEntry graphics_manual[] = { KAFFEINE_GRAPHICS_LAYER_THUNKS };
static const M68KThunkEntry cybergraphics_manual[] = { KAFFEINE_CYBERGRAPHICS_BITMAP_THUNKS };

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
    static constexpr ULONG exec_base = 0x1000, layers_base = 0x3000, graphics_base = 0x3C00, cybergraphics_base = 0x2000;
    static constexpr ULONG stack = 0x3F00, stack2 = 0x3E00, task = 0x3800;
    static constexpr ULONG heap_start = 0x4000, heap_end = 0x1F000;
    KaffeineContext ctx = {};
    std::vector<UBYTE> memory = std::vector<UBYTE>(0x20000, 0xA5);
    TestCPU state, state2;
    KaffeineCPU cpu = { &ops, &state }, cpu2 = { &ops, &state2 };
    std::vector<M68KThunkEntry> layers_gen, graphics_gen, cybergraphics_gen;
    ULONG heap_initial = 0;

    Fixture()
    {
        events.clear();
        layer_locks.clear();
        held_locks = 0;
        native_installs = init_bitmaps = default_fills = new_infos = disposed_infos = deletes = 0;
        root_makes = root_reads = root_deletes = 0;
        assert(native_infos.empty() && native_layers.empty() && roots.empty());
        creates.clear();
        hook_calls.clear();
        blits.clear();
        wait_blits = 0;
        blit_pending = blit_force = false;
        blit_forced = 0;
        blit_reenter = nullptr;
        call_inits.clear();
        screen_bitmaps.clear();
        bitmap_attrs = 0;
        create_mode = CREATE_OK;
        delete_fails = swap_bitmap = false;
        hook_task = nullptr;
        current_task = executor;
        action = GUEST_DRAW;
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
        layers_lib_base = layers_base;
        ctx.m68k_sysbase = exec_base;
        ctx.exec_task = executor;
        m68k_write32(&ctx, exec_base + M68K_EXECBASE_THISTASK, task);
        layers_gen = poisoned(kaffeine_thunks_layers_manual, kaffeine_thunks_layers_manual_count);
        graphics_gen = poisoned(graphics_manual, std::size(graphics_manual));
        lib(0, layers_base, "layers.library", kaffeine_thunks_layers_manual,
            kaffeine_thunks_layers_manual_count, layers_gen);
        lib(1, graphics_base, "graphics.library", graphics_manual, std::size(graphics_manual),
            graphics_gen);
        cybergraphics_gen = poisoned(cybergraphics_manual, std::size(cybergraphics_manual));
        lib(2, cybergraphics_base, "cybergraphics.library", cybergraphics_manual,
            std::size(cybergraphics_manual), cybergraphics_gen);
        ctx.num_libs = 3;
        heap_initial = avail();
        assert(Kaffeine_LayersSetup(&ctx));
    }

    ~Fixture()
    {
        Kaffeine_LayersCleanup(&ctx);
        assert(!ctx.layers && held_locks == 0);
        assert(native_layers.empty() && native_infos.empty() && roots.empty());
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
       is reached. With preserved, only D0 may change, also across 68k
       hooks; a call that a hook stopped keeps the hook's registers. */
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
        call_inits.clear();
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

    ULONG new_info() { return call(layers_base, 144); }
    ULONG create(ULONG li, ULONG bm, LONG x0, LONG y0, LONG x1, LONG y1, ULONG flags,
                 ULONG hook = 0, ULONG bm2 = 0)
    {
        return call(layers_base, 186, { li, bm, bm2, hook },
                    { (ULONG)x0, (ULONG)y0, (ULONG)x1, (ULONG)y1, flags });
    }
    ULONG destroy(ULONG layer) { return call(layers_base, 90, { 0, layer }); }

    /* A classic BitMap of the program with planes in the heap */
    ULONG bitmap(UWORD bpr, UWORD rows, UBYTE depth, std::vector<ULONG> *planes = nullptr)
    {
        const ULONG bm = Kaffeine_HeapAlloc(&ctx, M68K_SIZEOF_BITMAP, MEMF_CLEAR);
        assert(bm);
        m68k_write16(&ctx, bm + M68K_BM_BYTESPERROW, bpr);
        m68k_write16(&ctx, bm + M68K_BM_ROWS, rows);
        memory[bm + M68K_BM_FLAGS] = 0;
        memory[bm + M68K_BM_DEPTH] = depth;
        for (UBYTE i = 0; i < depth; ++i)
        {
            const ULONG p = Kaffeine_HeapAlloc(&ctx, (ULONG)bpr * rows, MEMF_CLEAR);
            assert(p);
            m68k_write32(&ctx, bm + M68K_BM_PLANES + 4 * i, p);
            if (planes)
                planes->push_back(p);
        }
        return bm;
    }
    /* A screen as the screen bridge leaves it (kaffeine_screens.c, not
       linked here): its 68k Screen in the table of screens, of the shadow
       type of a screen that the program opened (owned) or of a public
       screen that it locked, RastPort.BitMap naming the embedded 68k
       BitMap, whose snapshot has the native scalars and no planes. The
       native BitMap is bpr * 8 pixels wide by BytesPerRow, width by
       GetBitMapAttr(). */
    ULONG open_screen(NativeScreenBitMap &n, UWORD bpr, UWORD rows, UBYTE depth, ULONG width,
                      bool owned = true)
    {
        int slot = 0;

        n.bm.BytesPerRow = bpr;
        n.bm.Rows = rows;
        n.bm.Depth = depth;
        n.bm.Flags = 0x80;
        n.planes.assign((size_t)bpr * rows * depth, 0);
        for (UBYTE i = 0; i < depth; ++i)
            n.bm.Planes[i] = n.planes.data() + (size_t)i * bpr * rows;
        n.width = width;
        n.open = true;
        n.screen.RastPort.BitMap = &n.bm;
        screen_bitmaps.push_back(&n);
        const ULONG sg = Kaffeine_MirrorAlloc(&ctx, M68K_SIZEOF_SCREEN);
        assert(sg && Kaffeine_ShadowAdd(&ctx, sg, &n.screen,
                                        owned ? KAFFEINE_SHADOW_SCREEN : KAFFEINE_SHADOW_PUBSCREEN));
        Kaffeine_LayerSemaphoreInit(&ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
        while (ctx.pubscreens[slot].guest)
            ++slot;
        ctx.pubscreens[slot].native = &n.screen;
        ctx.pubscreens[slot].guest = sg;
        ctx.pubscreens[slot].locks = owned ? 0 : 1;
        ctx.pubscreens[slot].owned = owned;
        const ULONG h = sg + M68K_SCR_BITMAP;
        m68k_write32(&ctx, sg + M68K_SCR_RASTPORT + M68K_RP_BITMAP, h);
        m68k_write16(&ctx, h + M68K_BM_BYTESPERROW, bpr);
        m68k_write16(&ctx, h + M68K_BM_ROWS, rows);
        memory[h + M68K_BM_FLAGS] = 0x80;
        memory[h + M68K_BM_DEPTH] = depth;
        return sg;
    }
    /* The screen closes, as screen_forget() lets it go */
    void close_screen(NativeScreenBitMap &n, ULONG sg)
    {
        int slot = 0;

        while (ctx.pubscreens[slot].guest != sg)
            ++slot;
        ctx.layer_ops->screen_closing(&ctx, &n.screen);
        std::memset(memory.data() + sg + M68K_SCR_BITMAP, 0, M68K_SIZEOF_BITMAP);
        m68k_write32(&ctx, sg + M68K_SCR_RASTPORT + M68K_RP_BITMAP, 0);
        Kaffeine_ShadowForget(&ctx, sg, ctx.pubscreens[slot].owned ? KAFFEINE_SHADOW_SCREEN
                                                                     : KAFFEINE_SHADOW_PUBSCREEN);
        Kaffeine_MirrorRelease(&ctx, sg);
        ctx.pubscreens[slot] = {};
        n.open = false;
        screen_bitmaps.erase(std::find(screen_bitmaps.begin(), screen_bitmaps.end(), &n));
    }
    /* A classic 68k Hook whose h_Entry is HOOK_ENTRY */
    ULONG hook(ULONG entry = HOOK_ENTRY)
    {
        const ULONG h = Kaffeine_HeapAlloc(&ctx, M68K_SIZEOF_HOOK, MEMF_CLEAR);
        assert(h);
        m68k_write32(&ctx, h + 8, entry);
        m68k_write32(&ctx, h + 16, 0xDA7A);   /* h_Data stays the program's */
        return h;
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
    /* No 32-bit word of [a, a + size) is (the low half of) a native pointer */
    bool no_native(ULONG a, ULONG size, const std::vector<const void *> &natives)
    {
        for (ULONG off = 0; off + 4 <= size; off += 2)
            for (const void *p : natives)
                if (p && L(a + off) == (ULONG)reinterpret_cast<uintptr_t>(p))
                    return false;
        return true;
    }
};

static std::vector<const void *> natives()
{
    std::vector<const void *> v(native_infos.begin(), native_infos.end());
    for (NativeLayer *n : native_layers)
    {
        v.push_back(&n->layer);
        v.push_back(&n->rp);
    }
    for (const auto &r : roots)
    {
        v.push_back(&r.second->layer);
        v.push_back(&r.second->rp);
    }
    for (const CreateCall &c : creates)
    {
        v.push_back(c.bm);
        if (c.hook != LAYERS_BACKFILL && c.hook != LAYERS_NOBACKFILL)
            v.push_back(c.hook);
    }
    return v;
}

static Layer *native_layer(Fixture &f, ULONG guest_layer)
{
    RastPort *rp = static_cast<RastPort *>(
        Kaffeine_ShadowNative(&f.ctx, f.L(guest_layer + M68K_LAYER_RP), KAFFEINE_SHADOW_RASTPORT));
    return rp ? rp->Layer : nullptr;
}

static size_t event_at(const char *name, size_t from = 0)
{
    for (size_t i = from; i < events.size(); ++i)
        if (events[i] == name)
            return i;
    return events.size();
}

static size_t event_last(const char *name)
{
    for (size_t i = events.size(); i-- > 0;)
        if (events[i] == name)
            return i;
    return events.size();
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_registration()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 2), h = f.hook();

    for (ULONG i = 0; i < kaffeine_thunks_layers_manual_count; ++i)
    {
        const M68KThunkEntry &e = kaffeine_thunks_layers_manual[i];
        if (e.lvo == 90)
            assert(e.thunk == Kaffeine_DeleteLayer);
        else if (e.lvo == 186)
            assert(e.thunk == Kaffeine_CreateUpfrontHookLayer);
    }
    /* the other constructors stay refused before anything native, with
       valid arguments in every register */
    for (ULONG lvo : { 36u, 42u, 192u, 234u, 240u })
    {
        const M68KThunkEntry *e = nullptr;
        for (ULONG i = 0; i < kaffeine_thunks_layers_manual_count; ++i)
            if (kaffeine_thunks_layers_manual[i].lvo == lvo)
                e = &kaffeine_thunks_layers_manual[i];
        assert(e && e->thunk == kaffeine_layers_Unsupported);
        assert(f.call(Fixture::layers_base, lvo, { li, bm, 0, h }, { 0, 0, 7, 7, LAYERSIMPLE }) == 20 &&
               f.stopped());
    }
    assert(creates.empty() && init_bitmaps == 0 && deletes == 0);
}

static void test_conversion_and_identity()
{
    Fixture f;
    std::vector<ULONG> planes;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 16, 3, &planes);
    const ULONG base = f.avail();
    const UWORD shadows = f.ctx.num_shadows;

    const ULONG l1 = f.create(li, bm, 2, 3, 20, 10, LAYERSMART);
    assert(l1 && f.ctx.running && creates.size() == 1 && init_bitmaps == 1);
    /* the native BitMap is the bridge's, as InitBitMap() makes it, over the
       very guest planes */
    const CreateCall c = creates[0];
    assert(c.li == native_infos[0] && c.bm2 == nullptr && c.hook == LAYERS_BACKFILL);
    assert(c.x0 == 2 && c.y0 == 3 && c.x1 == 20 && c.y1 == 10 && c.flags == LAYERSMART);
    assert(c.copy.BytesPerRow == 4 && c.copy.Rows == 16 && c.copy.Depth == 3 &&
           c.copy.Flags == BMF_STANDARD && c.copy.pad == 0);
    for (int i = 0; i < 3; ++i)
        assert(c.copy.Planes[i] == f.ctx.mem + planes[i]);
    for (int i = 3; i < 8; ++i)
        assert(c.copy.Planes[i] == nullptr);
    assert(default_fills == 1);
    /* the 68k Layer: classic fields, own objects, no native pointers */
    Layer *n1 = native_layer(f, l1);
    assert(n1 && native_of(n1));
    assert(f.W(l1 + M68K_LAYER_MINX) == 2 && f.W(l1 + M68K_LAYER_MINY) == 3);
    assert(f.W(l1 + M68K_LAYER_MAXX) == 20 && f.W(l1 + M68K_LAYER_MAXY) == 10);
    assert(f.W(l1 + M68K_LAYER_WIDTH) == 19 && f.W(l1 + M68K_LAYER_HEIGHT) == 8);
    assert(f.W(l1 + M68K_LAYER_FLAGS) == LAYERSMART && f.L(l1 + M68K_LAYER_WINDOW) == 0);
    assert(f.L(l1 + M68K_LAYER_LAYERINFO) == li && f.L(l1 + M68K_LAYER_BACKFILL) == 0);
    assert(!f.L(l1 + M68K_LAYER_FRONT) && !f.L(l1 + M68K_LAYER_BACK) && f.L(li + M68K_LI_TOP_LAYER) == l1);
    assert(!f.L(l1 + M68K_LAYER_CLIPRECT) && !f.L(l1 + M68K_LAYER_SUPERBITMAP) &&
           f.zero(l1 + M68K_LAYER_CR, 20) && !f.L(l1 + M68K_LAYER_DAMAGELIST));
    assert(f.memory[l1 + M68K_LAYER_LOCK + M68K_SS_LN_TYPE] == 15);
    const ULONG rp1 = f.L(l1 + M68K_LAYER_RP);
    assert(rp1 && Kaffeine_ShadowNative(&f.ctx, rp1, KAFFEINE_SHADOW_RASTPORT) == n1->rp);
    assert(f.L(rp1 + M68K_RP_LAYER) == l1 && f.L(rp1 + M68K_RP_BITMAP) == bm);
    assert(Kaffeine_RastPortIn(&f.ctx, rp1) == n1->rp);
    /* never a Layer in the shadow map */
    assert(!f.shadowed(l1) && m68k_to_host_or_shadow(&f.ctx, l1) == f.ctx.mem + l1);
    assert(f.ctx.num_shadows == shadows + 1);
    assert(f.no_native(l1, M68K_SIZEOF_LAYER, natives()) && f.no_native(rp1, M68K_SIZEOF_RASTPORT, natives()) &&
           f.no_native(li, M68K_SIZEOF_LAYER_INFO, natives()));

    /* the next one in front, a backdrop one behind both: real native order */
    const ULONG l2 = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE);
    const ULONG l3 = f.create(li, bm, 0, 0, 31, 15, LAYERSIMPLE | LAYERBACKDROP);
    assert(l2 && l3 && l1 != l2 && l2 != l3 && f.ctx.running);
    assert(f.L(li + M68K_LI_TOP_LAYER) == l2);
    assert(f.L(l2 + M68K_LAYER_BACK) == l1 && f.L(l1 + M68K_LAYER_FRONT) == l2);
    assert(f.L(l1 + M68K_LAYER_BACK) == l3 && f.L(l3 + M68K_LAYER_FRONT) == l1 && !f.L(l3 + M68K_LAYER_BACK));
    assert(f.W(l3 + M68K_LAYER_FLAGS) == (LAYERSIMPLE | LAYERBACKDROP));
    /* identities stay */
    assert(f.L(l1 + M68K_LAYER_RP) == rp1);

    /* DeleteLayer(): the native result; its neighbours close up */
    const ULONG rp2 = f.L(l2 + M68K_LAYER_RP);
    const unsigned fills = default_fills;
    assert(f.destroy(l1) == TRUE && f.ctx.running && deletes == 1);
    assert(default_fills == fills + 1);     /* the backdrop layer behind showed */
    assert(f.zero(l1, M68K_SIZEOF_LAYER) && f.zero(rp1, M68K_SIZEOF_RASTPORT));
    assert(!Kaffeine_ShadowNative(&f.ctx, rp1, KAFFEINE_SHADOW_RASTPORT));
    assert(f.L(l2 + M68K_LAYER_BACK) == l3 && f.L(l3 + M68K_LAYER_FRONT) == l2);
    assert(f.L(l2 + M68K_LAYER_RP) == rp2);
    /* stale: refused before the native call */
    assert(f.destroy(l1) == 20 && f.stopped() && deletes == 1);
    assert(f.destroy(l2) == TRUE && f.L(li + M68K_LI_TOP_LAYER) == l3 && !f.L(l3 + M68K_LAYER_FRONT));
    assert(f.destroy(l3) == TRUE && f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(native_layers.empty() && f.avail() == base && f.ctx.num_shadows == shadows);
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running && native_infos.empty());
}

static void test_hook_during_constructor()
{
    Fixture f;
    std::vector<ULONG> planes;
    const ULONG li = f.new_info(), bm = f.bitmap(2, 8, 1, &planes), h = f.hook();

    action = GUEST_DRAW;
    const ULONG l = f.create(li, bm, 0, 2, 15, 7, LAYERSIMPLE, h);
    assert(l && f.ctx.running && hook_calls.size() == 1);
    const HookCall c = hook_calls[0];
    const ULONG rp = f.L(l + M68K_LAYER_RP);
    Layer *n = native_layer(f, l);
    /* before the constructor returned, the hook already saw the 68k Layer,
       its RastPort and its LayerInfo, in the native order */
    assert(c.cpu == 1 && c.a0 == h && c.a2 == rp && c.msg_layer == l);
    assert(c.rp_native == n->rp && c.rp_layer == l && c.rp_bitmap == bm);
    assert(c.layer_rp == rp && c.layer_info == li && c.info_top == l && c.layer_backfill == h);
    assert(c.bounds[0] == 0 && c.bounds[1] == 2 && c.bounds[2] == 15 && c.bounds[3] == 7);
    assert(c.ox == 0 && c.oy == 2);
    /* the message is the bridge's own, classic and without native pointers */
    assert(c.a1 >= Fixture::heap_start && c.a1 < Fixture::heap_end && c.a1 != l && c.a1 != rp);
    assert(!f.shadowed(c.a1) && f.no_native(c.a1, M68K_SIZEOF_BFM, natives()));
    assert(f.no_native(l, M68K_SIZEOF_LAYER, natives()) && f.no_native(rp, M68K_SIZEOF_RASTPORT, natives()));
    /* the hook drew into the guest planes of the layer */
    assert(f.memory[planes[0] + 2 * 2] == 0x5A);
    /* the native hook is the bridge's: never the 68k Hook or its code */
    Hook *native_hook = creates[0].hook;
    assert(n->BackFill == native_hook && native_hook != LAYERS_BACKFILL && native_hook != LAYERS_NOBACKFILL);
    const UBYTE *nh = reinterpret_cast<const UBYTE *>(native_hook);
    assert(nh < f.ctx.mem || nh >= f.ctx.mem + f.ctx.mem_size);
    assert(native_hook->h_Entry != reinterpret_cast<APTR>(static_cast<uintptr_t>(HOOK_ENTRY)));
    assert(f.L(h + 16) == 0xDA7A);

    /* InstallLayerHook(): sentinels and the round trip of its own 68k Hook */
    assert(f.call(Fixture::layers_base, 198, { l, 1 }) == h && f.ctx.running);
    assert(n->BackFill == LAYERS_NOBACKFILL && f.L(l + M68K_LAYER_BACKFILL) == 1);
    assert(f.call(Fixture::layers_base, 198, { l, h }) == 1 && n->BackFill == native_hook);
    assert(f.call(Fixture::layers_base, 198, { l, 0 }) == h && n->BackFill == LAYERS_BACKFILL);
    assert(f.call(Fixture::layers_base, 198, { l, h }) == 0 && n->BackFill == native_hook);
    assert(f.L(l + M68K_LAYER_BACKFILL) == h && held_locks == 0);
    /* another 68k Hook or a pointer is no backfill hook of this layer */
    const unsigned installs = native_installs;
    for (ULONG bad : { f.hook(), h + 2, 0x5555u, l })
    {
        assert(f.call(Fixture::layers_base, 198, { l, bad }) == 20 && f.stopped());
        assert(native_installs == installs && n->BackFill == native_hook);
    }

    /* locks of a private layer, nested; the hook may lock its own layer */
    assert(f.call(Fixture::layers_base, 96, { 0, l }) == 0 && f.ctx.running);
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, l }) == TRUE && layer_locks[n] == 2);
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, l }) == 0);
    assert(f.call(Fixture::layers_base, 102, { l }) == 0 && held_locks == 0);

    /* a layer in front goes: the hook of the one behind runs during the
       deletion, on the deleting thunk's CPU, and every register comes back */
    const ULONG front = f.create(li, bm, 0, 0, 7, 3, LAYERSMART);
    assert(front && f.L(li + M68K_LI_TOP_LAYER) == front && f.L(front + M68K_LAYER_BACK) == l);
    action = GUEST_LOCK;
    const size_t calls = hook_calls.size();
    assert(f.call_on(f.cpu2, Fixture::layers_base, 90, { 0, front }) == TRUE && f.ctx.running);
    assert(hook_calls.size() == calls + 1 && hook_calls.back().cpu == 2 &&
           hook_calls.back().msg_layer == l && hook_calls.back().info_top == front);
    assert(f.L(li + M68K_LI_TOP_LAYER) == l && held_locks == 0);
}

static void test_cliprect_bitmap()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(2, 8, 2), h = f.hook();

    /* the hidden part of a smart layer: layers draws in a ClipRect BitMap
       of its own, which has no 68k BitMap */
    swap_bitmap = true;
    const ULONG l = f.create(li, bm, 0, 0, 15, 7, LAYERSMART, h);
    assert(l && hook_calls.size() == 1 && hook_calls[0].rp_bitmap == 0 &&
           hook_calls[0].rp_native == native_layer(f, l)->rp);
    /* afterwards, the layer's own BitMap again */
    assert(f.L(f.L(l + M68K_LAYER_RP) + M68K_RP_BITMAP) == bm);
    swap_bitmap = false;
}

static void test_current_cpu_and_task()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(2, 8, 1), h = f.hook();

    /* the creation runs the hook on the CPU of its thunk */
    const ULONG l = f.call_on(f.cpu2, Fixture::layers_base, 186, { li, bm, 0, h }, { 0, 0, 15, 7, LAYERSIMPLE });
    assert(l && hook_calls.size() == 1 && hook_calls[0].cpu == 2);
    Layer *n = native_layer(f, l);

    /* Later another guest task draws: native graphics run the backfill from
       within its thunk. The current guest CPU runs the hook, not the one
       that made the layer, and gets all of its state back. */
    auto prepare = [&](TestCPU &s, ULONG sp) {
        for (int i = 0; i < 8; ++i) { s.d[i] = 0x11110000u + (ULONG)i; s.a[i] = 0x22220000u + (ULONG)i; }
        s.a[7] = sp;
        s.sr = 0x2704;
        s.pc = 0x00005000;
    };
    prepare(f.state, Fixture::stack);
    prepare(f.state2, Fixture::stack2);
    const TestCPU one = f.state, two = f.state2;
    auto same = [](const TestCPU &a, const TestCPU &b) {
        return std::equal(a.d, a.d + 8, b.d) && std::equal(a.a, a.a + 8, b.a) && a.sr == b.sr && a.pc == b.pc;
    };
    f.ctx.running = TRUE;
    f.ctx.callback_failed = FALSE;
    f.ctx.cpu = &f.cpu;
    backfill(n, n->bounds, 0, 0);
    assert(hook_calls.size() == 2 && hook_calls[1].cpu == 1 && hook_calls[1].msg_layer == l);
    assert(same(f.state, one) && same(f.state2, two));
    f.ctx.cpu = &f.cpu2;
    backfill(n, n->bounds, 0, 0);
    assert(hook_calls.size() == 3 && hook_calls[2].cpu == 2);
    assert(same(f.state, one) && same(f.state2, two));

    /* from another native task, without a guest CPU, after a stop: nothing */
    hook_task = foreign_task;
    backfill(n, n->bounds, 0, 0);
    hook_task = nullptr;
    f.ctx.cpu = nullptr;
    backfill(n, n->bounds, 0, 0);
    f.ctx.cpu = &f.cpu2;
    f.ctx.running = FALSE;
    backfill(n, n->bounds, 0, 0);
    f.ctx.running = TRUE;
    f.ctx.exec_task = nullptr;
    backfill(n, n->bounds, 0, 0);
    f.ctx.exec_task = executor;
    assert(hook_calls.size() == 3 && same(f.state, one) && same(f.state2, two));
    f.ctx.cpu = nullptr;

    /* a constructor whose layers runs the hook on another task: no 68k code
       there; the layer is bound when the constructor returns */
    hook_task = foreign_task;
    const ULONG l2 = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h);
    hook_task = nullptr;
    assert(l2 && f.ctx.running && hook_calls.size() == 3);
    const ULONG rp2 = f.L(l2 + M68K_LAYER_RP);
    assert(rp2 && Kaffeine_ShadowNative(&f.ctx, rp2, KAFFEINE_SHADOW_RASTPORT) == native_layer(f, l2)->rp);
    assert(f.L(li + M68K_LI_TOP_LAYER) == l2 && f.L(l2 + M68K_LAYER_BACK) == l);
}

static void test_malformed()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 3), h = f.hook();
    const ULONG disposed = f.new_info();
    assert(f.call(Fixture::layers_base, 150, { disposed }) == 0);
    const ULONG own_info = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_LAYER_INFO, MEMF_CLEAR);
    /* the widest BitMap gets a LayerInfo of its own: one BitMap per LayerInfo */
    const ULONG wide_info = f.new_info(), wide = f.bitmap(KAFFEINE_LAYER_MAX_BPR, 1, 1);

    /* malformed 68k BitMaps, one block each; one row too wide, whose
       planes are fine, so that its root layer could not have WORD bounds */
    std::vector<ULONG> bad_bitmaps = { 0, bm + 1, f.ctx.mem_size - 20, 0xFFFFFFF0u,
                                       f.bitmap(KAFFEINE_LAYER_MAX_BPR + 2, 1, 1) };
    auto variant = [&](auto change) {
        const ULONG b = f.bitmap(4, 8, 3);
        change(b);
        bad_bitmaps.push_back(b);
    };
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_BYTESPERROW, 0); });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_BYTESPERROW, 3); });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_BYTESPERROW, KAFFEINE_LAYER_MAX_BPR + 2); });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_ROWS, 0); });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_ROWS, KAFFEINE_LAYER_MAX_ROWS + 1); });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_ROWS, 0xFFFF); });
    /* a plane that is not word aligned, the first one and one past it */
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES, f.L(b + M68K_BM_PLANES) + 1); });
    variant([&](ULONG b) { f.memory[b + M68K_BM_FLAGS] = BMF_INTERLEAVED; });
    variant([&](ULONG b) { f.memory[b + M68K_BM_FLAGS] = BMF_STANDARD | 0x80; });
    variant([&](ULONG b) { f.memory[b + M68K_BM_DEPTH] = 0; });
    variant([&](ULONG b) { f.memory[b + M68K_BM_DEPTH] = 9; });
    variant([&](ULONG b) { m68k_write16(&f.ctx, b + M68K_BM_PAD, 1); });
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES, 0); });
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES + 4, f.L(b + M68K_BM_PLANES + 4) + 1); });
    /* the last plane of the depth outside the arena, past the heap, below
       it, and across its end */
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES + 8, 0x00FFFFF0u); });
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES + 8, Fixture::heap_end); });
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES + 8, 0x3000); });
    variant([&](ULONG b) { m68k_write32(&f.ctx, b + M68K_BM_PLANES + 8, Fixture::heap_end - 4 * 8 + 2); });
    /* malformed 68k Hooks */
    std::vector<ULONG> bad_hooks = { h + 1, f.ctx.mem_size - 18 };
    for (ULONG entry : { 0u, HOOK_ENTRY + 1, KAFFEINE_CALLBACK_RETURN_BASE, f.ctx.mem_size, 0x80000000u })
        bad_hooks.push_back(f.hook(entry));

    const auto heap = f.heap();
    const UWORD shadows = f.ctx.num_shadows;
    auto refused = [&](ULONG li_, ULONG bm_, ULONG hook_, ULONG bm2_, LONG x0, LONG y0, LONG x1, LONG y1,
                       ULONG flags) {
        assert(f.call(Fixture::layers_base, 186, { li_, bm_, bm2_, hook_ },
                      { (ULONG)x0, (ULONG)y0, (ULONG)x1, (ULONG)y1, flags }) == 20 && f.stopped());
        assert(creates.empty() && init_bitmaps == 0 && f.heap_is(heap) && f.ctx.num_shadows == shadows);
    };
    /* LayerInfos: none, inner, the program's own, foreign, disposed of */
    for (ULONG bad : { 0u, li + 2, li + M68K_LI_LOCK, own_info, 0x5555u, disposed, bm })
        refused(bad, bm, 0, 0, 0, 0, 7, 7, LAYERSIMPLE);
    for (ULONG bad : bad_bitmaps)
        refused(li, bad, 0, 0, 0, 0, 7, 7, LAYERSIMPLE);
    /* flags: none, both kinds, super, internal and unknown bits */
    for (ULONG flags : { 0u, (ULONG)(LAYERSIMPLE | LAYERSMART), (ULONG)(LAYERSMART | LAYERSUPER),
                         (ULONG)(LAYERSMART | LAYERUPDATING), (ULONG)(LAYERSIMPLE | LAYERREFRESH),
                         (ULONG)LAYERBACKDROP, LAYERSIMPLE | 0x8000u, 0xFFFFFFFFu })
        refused(li, bm, 0, 0, 0, 0, 7, 7, flags);
    /* a super BitMap */
    refused(li, bm, 0, bm, 0, 0, 7, 7, LAYERSMART);
    /* bounds outside the 32x8 BitMap, reversed, negative, beyond WORD */
    struct { LONG x0, y0, x1, y1; } bounds[] =
    {
        { -1, 0, 7, 7 }, { 0, -1, 7, 7 }, { 8, 0, 7, 7 }, { 0, 5, 7, 4 }, { 0, 0, 32, 7 },
        { 0, 0, 31, 8 }, { 0, 0, 0x10000, 7 }, { (LONG)0x80000000, 0, 7, 7 }, { 0, 0, 7, (LONG)0x80000007 }
    };
    for (const auto &b : bounds)
        refused(li, bm, 0, 0, b.x0, b.y0, b.x1, b.y1, LAYERSIMPLE);
    /* Beyond the bitmap's full signed-WORD width. */
    refused(wide_info, wide, 0, 0, 0, 0, 40000, 0, LAYERSIMPLE);
    refused(wide_info, wide, 0, 0, 32768, 0, 32768, 0, LAYERSIMPLE);
    for (ULONG bad : bad_hooks)
        refused(li, bm, bad, 0, 0, 0, 7, 7, LAYERSIMPLE);

    /* The full root width/height, as well as its bounds, fit signed WORD. */
    const ULONG edge = f.create(li, bm, 31, 7, 31, 7, LAYERSMART | LAYERBACKDROP);
    const ULONG widest = f.create(wide_info, wide, 32751, 0, 32751, 0, LAYERSIMPLE);
    assert(edge && widest && f.ctx.running && creates.size() == 2);
    assert(creates[1].copy.BytesPerRow == KAFFEINE_LAYER_MAX_BPR && creates[1].copy.Rows == 1);
    assert(root_makes == 2 && roots.at(creates[1].li)->layer.bounds.MaxX == 32751 &&
           roots.at(creates[1].li)->layer.bounds.MaxY == 0);
    /* a private layer, its RastPort and the hook are no LayerInfo */
    const size_t made = creates.size();
    for (ULONG bad : { edge, f.L(edge + M68K_LAYER_RP), h })
    {
        assert(f.call(Fixture::layers_base, 186, { bad, bm, 0, 0 }, { 0, 0, 7, 7, LAYERSIMPLE }) == 20 &&
               f.stopped() && creates.size() == made);
    }
}

static void test_memory_and_native_failures()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1), h = f.hook();
    const UWORD shadows = f.ctx.num_shadows;

    /* no guest memory: 0 before anything native */
    {
        std::vector<ULONG> fillers;
        for (ULONG b; (b = Kaffeine_HeapAlloc(&f.ctx, 16, 0)) != 0;)
            fillers.push_back(b);
        const auto heap = f.heap();
        assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h) == 0 && f.ctx.running);
        assert(creates.empty() && f.heap_is(heap) && f.ctx.num_shadows == shadows);
        for (ULONG b : fillers)
            Kaffeine_HeapFree(&f.ctx, b, 16);
    }
    const ULONG base = f.avail();
    /* no room in the shadow map for its RastPort */
    {
        const UWORD n = f.ctx.num_shadows;
        f.ctx.num_shadows = KAFFEINE_MAX_SHADOWS;
        assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE) == 0 && f.ctx.running && creates.empty());
        f.ctx.num_shadows = n;
        assert(f.avail() == base);
    }
    /* none of these reached layers */
    assert(root_makes == 0 && native_infos[0]->top_layer == nullptr);
    /* layers makes no layer, but its root, which the program never sees */
    create_mode = CREATE_FAILS_EARLY;
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h) == 0 && f.ctx.running);
    assert(creates.size() == 1 && hook_calls.empty() && deletes == 0);
    assert(f.avail() == base && f.ctx.num_shadows == shadows && f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(root_makes == 1 && native_infos[0]->top_layer == native_infos[0]->check_lp);
    /* layers ran the hook, bound it, then failed: the binding goes too */
    create_mode = CREATE_FAILS_LATE;
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h) == 0 && f.ctx.running);
    assert(creates.size() == 2 && hook_calls.size() == 1 && hook_calls[0].info_top == hook_calls[0].msg_layer);
    assert(f.zero(hook_calls[0].msg_layer, M68K_SIZEOF_LAYER) && f.zero(hook_calls[0].a2, M68K_SIZEOF_RASTPORT));
    assert(f.avail() == base && f.ctx.num_shadows == shadows && f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(native_layers.empty() && deletes == 0 && root_makes == 1);
    assert(creates[1].bm == creates[0].bm);
    create_mode = CREATE_OK;

    /* a full table of private layers: 0 before anything native */
    std::vector<ULONG> layers;
    for (int i = 0; i < KAFFEINE_LAYER_MAX_PRIVATE; ++i)
    {
        const ULONG l = f.create(li, bm, i, 0, i, 7, LAYERSIMPLE);
        assert(l && f.ctx.running);
        layers.push_back(l);
    }
    const size_t made = creates.size();
    const auto heap = f.heap();
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE) == 0 && f.ctx.running);
    assert(creates.size() == made && f.heap_is(heap));
    /* top_layer is the last one, and the list runs back to the first */
    ULONG g = f.L(li + M68K_LI_TOP_LAYER), count = 0;
    assert(g == layers.back());
    for (; g; g = f.L(g + M68K_LAYER_BACK)) ++count;
    assert(count == KAFFEINE_LAYER_MAX_PRIVATE);
    for (ULONG l : layers)
        assert(f.destroy(l) == TRUE && f.ctx.running);
    assert(f.avail() == base && f.ctx.num_shadows == shadows && native_layers.empty());
}

static void test_callback_failures()
{
    Fixture f;
    const ULONG li = f.new_info(), other = f.new_info(), bm = f.bitmap(4, 8, 1), h = f.hook();
    const ULONG faulting = f.hook(), recursing = f.hook();

    /* a layer with a 68k Hook, which every later constructor and deletion
       could uncover */
    const ULONG back = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE, h);
    assert(back && hook_calls.size() == 1);
    const ULONG base = f.avail();
    const UWORD shadows = f.ctx.num_shadows;

    /* A fault in the hook while the constructor runs: the program stops
       with it, and the layer that layers made anyway goes without running
       any 68k hook again, also not that of the layer behind. */
    action = GUEST_FAULT;
    assert(f.call_on(f.cpu, Fixture::layers_base, 186, { li, bm, 0, faulting },
                     { 0, 0, 7, 7, LAYERSIMPLE }, false) == 20);
    assert(!f.ctx.running && f.ctx.exit_code == 20 && f.ctx.callback_depth == 0);
    assert(creates.size() == 2 && deletes == 1 && hook_calls.size() == 2);
    assert(f.ctx.num_shadows == shadows && f.avail() == base);
    assert(f.L(li + M68K_LI_TOP_LAYER) == back && !f.L(back + M68K_LAYER_FRONT) && native_layers.size() == 1);

    /* A constructor from a constructor's hook stops before its native call;
       the outer layer goes again, quietly. */
    action = GUEST_LVO;
    guest_lvo = 186;
    guest_lvo_a = { other, bm, 0, 0 };
    guest_lvo_d = { 0, 0, 7, 7, LAYERSIMPLE };
    assert(f.call_on(f.cpu, Fixture::layers_base, 186, { li, bm, 0, recursing },
                     { 0, 0, 3, 3, LAYERSIMPLE }, false) == 20);
    assert(!f.ctx.running && creates.size() == 3 && deletes == 2);
    assert(hook_calls.size() == 3 && hook_calls.back().nested == 20);
    assert(f.ctx.num_shadows == shadows && f.avail() == base && native_layers.size() == 1);
    assert(f.L(li + M68K_LI_TOP_LAYER) == back && native_infos.size() == 2);

    /* A deletion that uncovers a layer with a 68k Hook runs it with the
       deletion's CPU: a deletion, a disposal and a constructor from inside
       stop the program before their native calls, and the outer deletion
       gives the program's stop. A hook that failed never runs again, so
       each round uncovers a layer of its own. */
    for (ULONG lvo : { 90u, 150u, 186u })
    {
        action = GUEST_DRAW;
        const ULONG victim = f.create(li, bm, 0, 0, 15, 7, LAYERSIMPLE, f.hook());
        const ULONG front = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE);
        assert(victim && front && f.ctx.running && f.L(front + M68K_LAYER_BACK) == victim);
        action = GUEST_LVO;
        guest_lvo = lvo;
        guest_lvo_a = lvo == 90 ? std::vector<ULONG>{ 0, back } :
                      lvo == 150 ? std::vector<ULONG>{ other } : std::vector<ULONG>{ other, bm, 0, 0 };
        guest_lvo_d = { 0, 0, 7, 7, LAYERSIMPLE };
        const unsigned dels = deletes, infos = disposed_infos;
        const size_t calls = hook_calls.size(), cr = creates.size(), alive = native_layers.size();
        assert(f.call_on(f.cpu, Fixture::layers_base, 90, { 0, front }, {}, false) == 20);
        assert(!f.ctx.running && f.ctx.callback_depth == 0);
        assert(hook_calls.size() == calls + 1 && hook_calls.back().nested == 20 &&
               hook_calls.back().msg_layer == victim);
        assert(deletes == dels + 1 && disposed_infos == infos && creates.size() == cr);
        assert(native_layers.size() == alive - 1 && f.L(li + M68K_LI_TOP_LAYER) == victim);
        assert(f.zero(front, M68K_SIZEOF_LAYER) && !f.L(victim + M68K_LAYER_FRONT));
        assert(native_infos.size() == 2);
    }
    action = GUEST_DRAW;
}

static void test_nested_backfill()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(2, 8, 1), h = f.hook();
    const ULONG base = f.avail();
    const UWORD shadows = f.ctx.num_shadows;

    /* Native code that runs the same layer's backfill again from inside its
       68k Hook, as a scroll in the hook would: the program stops, and the
       message the hook reads is never rewritten under it. */
    action = GUEST_NESTED;
    assert(f.call_on(f.cpu, Fixture::layers_base, 186, { li, bm, 0, h }, { 0, 0, 7, 7, LAYERSMART }, false) == 20);
    action = GUEST_DRAW;
    assert(!f.ctx.running && f.ctx.callback_depth == 0 && hook_calls.size() == 1);
    assert(deletes == 1 && native_layers.empty() && f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(f.avail() == base && f.ctx.num_shadows == shadows);
}

static void test_ownership_and_deletion()
{
    static Screen scr;
    static Layer bar;
    NativeLayer wl = {};
    Window w = {};
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1), h = f.hook();
    const ULONG l = f.create(li, bm, 0, 0, 7, 7, LAYERSMART, h);
    Layer *n = native_layer(f, l);
    assert(l && n);

    /* a window of the program on a screen, through the window bridge */
    std::memset(static_cast<void *>(&scr), 0, sizeof(scr));
    std::memset(static_cast<void *>(&bar), 0, sizeof(bar));
    bar.Flags = LAYERSIMPLE | LAYERBACKDROP;
    bar.LayerInfo = &scr.LayerInfo;
    scr.BarLayer = &bar;
    wl.layer.Flags = LAYERSMART;
    wl.layer.rp = &wl.rp;
    wl.layer.LayerInfo = &scr.LayerInfo;
    wl.layer.Window = &w;
    wl.rp.Layer = &wl.layer;
    w.WLayer = &wl.layer;
    w.WScreen = &scr;
    w.RPort = &wl.rp;
    scr.LayerInfo.top_layer = &bar;
    bar.back = &wl.layer;
    wl.layer.front = &bar;
    const ULONG mw = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_WINDOW, MEMF_CLEAR);
    assert(mw && f.ctx.layer_ops->window_opened(&f.ctx, mw, &w));
    const ULONG wlayer = f.L(mw + M68K_WIN_WLAYER), screen_info = f.L(wlayer + M68K_LAYER_LAYERINFO);
    const ULONG gbar = f.L(screen_info + M68K_LI_TOP_LAYER);
    assert(wlayer && screen_info && gbar && gbar != wlayer);

    /* DeleteLayer() takes nothing else: windows, borrowed layers, screen
       LayerInfos, inner and foreign pointers */
    const auto heap = f.heap();
    for (ULONG bad : { 0u, wlayer, gbar, screen_info, li, l + 2, f.L(l + M68K_LAYER_RP), bm, h, mw, 0x5555u })
    {
        assert(f.destroy(bad) == 20 && f.stopped() && deletes == 0 && f.heap_is(heap));
    }
    /* nor does CreateUpfrontHookLayer() take a screen's LayerInfo */
    assert(f.create(screen_info, bm, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped() && creates.size() == 1);
    /* the private layers stay out of the screen's snapshot */
    Kaffeine_LayersRefresh(&f.ctx);
    for (ULONG g = f.L(screen_info + M68K_LI_TOP_LAYER); g; g = f.L(g + M68K_LAYER_BACK))
        assert(g != l);
    assert(f.L(li + M68K_LI_TOP_LAYER) == l);

    /* held by another guest task: refused before anything native */
    const ULONG task_addr = Fixture::exec_base + M68K_EXECBASE_THISTASK;
    assert(f.call(Fixture::layers_base, 96, { 0, l }) == 0);
    m68k_write32(&f.ctx, task_addr, Fixture::task + 0x100);
    assert(f.destroy(l) == 20 && f.stopped() && deletes == 0 && layer_locks[n] == 1);
    m68k_write32(&f.ctx, task_addr, Fixture::task);

    /* layers keeps it: FALSE, and the program has its layer, RastPort and
       locks as before */
    assert(f.call(Fixture::layers_base, 96, { 0, l }) == 0 && layer_locks[n] == 2);
    delete_fails = true;
    events.clear();
    assert(f.destroy(l) == FALSE && f.ctx.running && deletes == 1);
    assert(event_last("UnlockLayer") < event_at("DeleteLayer") && event_at("LockLayer") > event_at("DeleteLayer"));
    assert(layer_locks[n] == 2 && held_locks == 2);
    const ULONG rp = f.L(l + M68K_LAYER_RP);
    assert(rp && Kaffeine_ShadowNative(&f.ctx, rp, KAFFEINE_SHADOW_RASTPORT) == n->rp);
    assert(f.L(rp + M68K_RP_LAYER) == l && f.L(rp + M68K_RP_BITMAP) == bm && f.L(li + M68K_LI_TOP_LAYER) == l);
    assert(f.call(Fixture::layers_base, 102, { l }) == 0 && f.ctx.running);
    delete_fails = false;

    /* DisposeLayerInfo() waits for its layers */
    assert(f.call(Fixture::layers_base, 150, { li }) == 20 && f.stopped() && disposed_infos == 0);

    /* now it goes, with the lock the program still held, which goes back
       before the native call, with the RastPort */
    events.clear();
    assert(f.destroy(l) == TRUE && f.ctx.running && deletes == 2 && held_locks == 0);
    assert(event_at("UnlockLayer") < event_at("DeleteLayer"));
    assert(!Kaffeine_ShadowNative(&f.ctx, rp, KAFFEINE_SHADOW_RASTPORT) && f.zero(l, M68K_SIZEOF_LAYER));
    /* its handle is stale for every layer call */
    for (ULONG lvo : { 90u, 96u, 102u, 198u })
    {
        const ULONG a0 = lvo == 96 || lvo == 90 ? 0 : l, a1 = lvo == 96 || lvo == 90 ? l : 0;
        assert(f.call(Fixture::layers_base, lvo, { a0, a1 }) == 20 && f.stopped());
    }
    assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, l }) == 20 && f.stopped());
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running && disposed_infos == 1);

    f.ctx.layer_ops->window_closing(&f.ctx, mw, &w);
    assert(!f.L(mw + M68K_WIN_WLAYER));
}

static void test_common_bitmap_and_root()
{
    Fixture f;
    std::vector<ULONG> planes, spare;
    const ULONG li = f.new_info(), li2 = f.new_info();
    const ULONG bm = f.bitmap(4, 8, 2, &planes), other = f.bitmap(4, 8, 2, &spare);
    Layer_Info *const n = native_infos[0];

    /* the first layer makes the root over the whole common BitMap, behind
       it; the program sees its own layer alone */
    const ULONG a = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(a && f.ctx.running && root_makes == 1);
    Layer *const root = n->check_lp;
    BitMap *const common = creates[0].bm;
    assert(root && root->rp->BitMap == common && root->bounds.MaxX == 31 && root->bounds.MaxY == 7);
    assert(n->top_layer == native_layer(f, a) && native_layer(f, a)->back == root && !root->back);
    assert(f.L(li + M68K_LI_TOP_LAYER) == a && !f.L(a + M68K_LAYER_BACK));
    /* the next ones share it, the root stays the one; a backdrop layer
       goes behind the others, still in front of the root */
    const ULONG b = f.create(li, bm, 0, 0, 7, 7, LAYERSMART);
    const ULONG c = f.create(li, bm, 8, 0, 15, 7, LAYERSIMPLE | LAYERBACKDROP);
    assert(b && c && f.ctx.running && root_makes == 1 && creates[1].bm == common && creates[2].bm == common);
    assert(f.L(li + M68K_LI_TOP_LAYER) == b && f.L(b + M68K_LAYER_BACK) == a &&
           f.L(a + M68K_LAYER_BACK) == c && !f.L(c + M68K_LAYER_BACK));
    assert(native_layer(f, c)->back == root);
    assert(f.no_native(li, M68K_SIZEOF_LAYER_INFO, natives()) && f.no_native(c, M68K_SIZEOF_LAYER, natives()));

    /* Another BitMap, or the same one changed, is no BitMap of this
       LayerInfo: refused before anything native, also with a fitting
       bbox; the common BitMap stays as it was. */
    const BitMap before = *common;
    const size_t made = creates.size();
    auto refused = [&](ULONG bitmap) {
        assert(f.create(li, bitmap, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped());
        assert(creates.size() == made && std::memcmp(common, &before, sizeof(BitMap)) == 0);
    };
    refused(other);
    auto changed = [&](auto change) {
        std::vector<UBYTE> saved(f.memory.begin() + bm, f.memory.begin() + bm + M68K_SIZEOF_BITMAP);
        change();
        refused(bm);
        std::copy(saved.begin(), saved.end(), f.memory.begin() + bm);
    };
    changed([&] { m68k_write32(&f.ctx, bm + M68K_BM_PLANES + 4, spare[1]); });
    changed([&] { m68k_write16(&f.ctx, bm + M68K_BM_ROWS, 4); });
    changed([&] { m68k_write16(&f.ctx, bm + M68K_BM_BYTESPERROW, 2); });
    changed([&] { f.memory[bm + M68K_BM_DEPTH] = 1; });
    changed([&] { f.memory[bm + M68K_BM_FLAGS] = BMF_STANDARD; });
    /* a plane past the depth is not the BitMap's: it may change */
    m68k_write32(&f.ctx, bm + M68K_BM_PLANES + 12, 0x5555);
    /* another LayerInfo has a common BitMap of its own */
    const ULONG d = f.create(li2, other, 0, 0, 7, 7, LAYERSIMPLE);
    assert(d && f.ctx.running && root_makes == 2 && creates.back().bm != common);
    assert(native_infos[1]->check_lp->rp->BitMap == creates.back().bm);

    /* The first layer, whose creation made the root, goes first: the
       root, uncovered, reads the common BitMap after that layer's own
       storage is gone, and keeps it. */
    unsigned reads = root_reads;
    assert(f.destroy(a) == TRUE && f.ctx.running && root_reads > reads);
    assert(n->check_lp == root && root->rp->BitMap == common);
    assert(f.L(b + M68K_LAYER_BACK) == c && f.L(c + M68K_LAYER_FRONT) == b && !f.L(c + M68K_LAYER_BACK));
    /* still the common BitMap for a new layer after that */
    const ULONG e = f.create(li, bm, 0, 0, 3, 3, LAYERSIMPLE);
    assert(e && f.ctx.running && creates.back().bm == common && root_makes == 2);
    /* no disposal while any layer of the program is in front of the root */
    for (ULONG l : { b, e })
    {
        assert(f.destroy(l) == TRUE && f.ctx.running);
        assert(f.call(Fixture::layers_base, 150, { li }) == 20 && f.stopped());
        assert(disposed_infos == 0 && root_deletes == 0 && n->check_lp == root);
    }
    reads = root_reads;
    assert(f.destroy(c) == TRUE && f.ctx.running && root_reads > reads);
    /* AROS keeps the root after the last layer of the program: the
       program sees an empty list */
    assert(n->top_layer == root && !root->front && f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(native_layers.size() == 1 && root->rp->BitMap == common && std::memcmp(common, &before, sizeof(BitMap)) == 0);

    /* the disposal deletes the root, which reads the BitMap then */
    reads = root_reads;
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running);
    assert(disposed_infos == 1 && root_deletes == 1 && root_reads == reads + 1 && !roots.count(n));
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped());
    /* the other LayerInfo's root goes at the end, after its layer */
    Kaffeine_LayersCleanup(&f.ctx);
    assert(root_deletes == 2 && disposed_infos == 2 && roots.empty() && native_infos.empty());
}

static void test_partial_creation_root()
{
    Fixture f;
    const ULONG li = f.new_info(), li2 = f.new_info(), li3 = f.new_info();
    const ULONG bm = f.bitmap(4, 8, 1), other = f.bitmap(2, 4, 1), h = f.hook();
    const ULONG base = f.avail();
    const UWORD shadows = f.ctx.num_shadows;

    /* layers made the root, then no layer: the root keeps the common
       BitMap, which the LayerInfo holds from now on */
    create_mode = CREATE_FAILS_EARLY;
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h) == 0 && f.ctx.running);
    Layer *const root = native_infos[0]->check_lp;
    BitMap *const common = creates[0].bm;
    assert(root && root->rp->BitMap == common && native_infos[0]->top_layer == root);
    assert(f.L(li + M68K_LI_TOP_LAYER) == 0 && f.avail() == base && f.ctx.num_shadows == shadows);
    /* another BitMap now would leave the root with one and the layer with
       another: refused before anything native */
    create_mode = CREATE_OK;
    assert(f.create(li, other, 0, 0, 7, 3, LAYERSIMPLE) == 20 && f.stopped() && creates.size() == 1);
    /* the same one makes a layer in front of that root */
    const ULONG l = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h);
    assert(l && f.ctx.running && root_makes == 1 && creates.back().bm == common);
    assert(native_layer(f, l)->back == root && f.L(li + M68K_LI_TOP_LAYER) == l);
    unsigned reads = root_reads;
    assert(f.destroy(l) == TRUE && root_reads > reads);

    /* layers failed before even the root: neither root nor list survived,
       so nothing native reads the BitMap, the bridge lets it go, and a
       retry may choose another one, which then gets its own root */
    create_mode = CREATE_ROOT_FAILS;
    const unsigned inits = init_bitmaps;
    assert(f.create(li2, bm, 0, 0, 7, 7, LAYERSIMPLE) == 0 && f.ctx.running);
    create_mode = CREATE_OK;
    assert(!native_infos[1]->check_lp && !native_infos[1]->top_layer && creates.size() == 3);
    assert(init_bitmaps == inits + 1 && root_makes == 1 && f.L(li2 + M68K_LI_TOP_LAYER) == 0);
    const ULONG retry = f.create(li2, other, 0, 0, 7, 3, LAYERSIMPLE);
    assert(retry && f.ctx.running && creates.size() == 4 && init_bitmaps == inits + 2);
    assert(creates[3].copy.BytesPerRow == 2 && creates[3].copy.Rows == 4 && root_makes == 2);
    assert(native_infos[1]->check_lp && native_infos[1]->check_lp->rp->BitMap == creates[3].bm);
    /* and that BitMap is the common one from now on */
    assert(f.create(li2, bm, 0, 0, 7, 3, LAYERSIMPLE) == 20 && f.stopped() && creates.size() == 4);
    assert(f.destroy(retry) == TRUE && f.ctx.running);
    reads = root_reads;
    assert(f.call(Fixture::layers_base, 150, { li2 }) == 0 && f.ctx.running && disposed_infos == 1);
    assert(root_deletes == 1 && root_reads == reads + 1);
    const ULONG before_fault = f.avail(); /* li2's mirror has now been freed */

    /* a root over a 68k Hook's partial failure: the faulting hook ran
       before layers returned, the layer goes quietly, the root stays */
    action = GUEST_FAULT;
    assert(f.call_on(f.cpu, Fixture::layers_base, 186, { li3, bm, 0, h }, { 0, 0, 7, 7, LAYERSIMPLE }, false) == 20);
    action = GUEST_DRAW;
    assert(!f.ctx.running && native_infos.size() == 2 && native_infos[1]->check_lp);
    assert(native_infos[1]->top_layer == native_infos[1]->check_lp && f.L(li3 + M68K_LI_TOP_LAYER) == 0);
    assert(f.avail() == before_fault && f.ctx.num_shadows == shadows);

    /* the roots go with their LayerInfos: li by the program, li3 at the end */
    reads = root_reads;
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running);
    assert(root_deletes == 2 && root_reads == reads + 1);
    Kaffeine_LayersCleanup(&f.ctx);
    assert(root_deletes == 3 && disposed_infos == 3 && roots.empty() && native_infos.empty());
}

static void test_root_geometry()
{
    {
        /* The tallest bitmap has height 32767, ending at row 32766. */
        Fixture f;
        const ULONG li = f.new_info(), tall = f.bitmap(2, KAFFEINE_LAYER_MAX_ROWS, 1);
        const ULONG l = f.create(li, tall, 0, 32766, 15, 32766, LAYERSIMPLE);
        assert(l && f.ctx.running && root_makes == 1);
        Layer *const root = native_infos[0]->check_lp;
        assert(root->bounds.MinY == 0 && root->bounds.MaxX == 15 && root->bounds.MaxY == 32766);
        assert(f.destroy(l) == TRUE && f.call(Fixture::layers_base, 150, { li }) == 0 && root_deletes == 1);
    }
    {
        /* One row more overflows signed height even though the rectangle
           endpoint fits; refuse before anything native. */
        Fixture f;
        const ULONG li = f.new_info(), tall = f.bitmap(2, KAFFEINE_LAYER_MAX_ROWS + 1, 1);
        const auto heap = f.heap();
        assert(f.create(li, tall, 0, 0, 15, 7, LAYERSIMPLE) == 20 && f.stopped());
        assert(creates.empty() && init_bitmaps == 0 && root_makes == 0 && f.heap_is(heap));
    }
}

static void test_private_lock_isolation()
{
    Screen screen = {};
    Layer public_layer = {};
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1), hook = f.hook();
    const ULONG layer = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(layer && f.call(Fixture::layers_base, 96, { 0, layer }) == 0);
    const ULONG sg = Kaffeine_MirrorAlloc(&f.ctx, M68K_SIZEOF_SCREEN);
    assert(sg && Kaffeine_ShadowAdd(&f.ctx, sg, &screen, KAFFEINE_SHADOW_PUBSCREEN));
    Kaffeine_LayerSemaphoreInit(&f.ctx, sg + M68K_SCR_LAYERINFO + M68K_LI_LOCK);
    f.ctx.pubscreens[0].guest = sg;
    f.ctx.pubscreens[0].native = &screen;
    f.ctx.pubscreens[0].locks = 1;
    screen.LayerInfo.top_layer = &public_layer;
    public_layer.LayerInfo = &screen.LayerInfo;
    public_layer.Width = 31;
    public_layer.Height = 1;
    Kaffeine_LayersRefresh(&f.ctx);
    const ULONG gl = f.L(sg + M68K_SCR_LAYERINFO + M68K_LI_TOP_LAYER);
    assert(gl && f.W(gl + M68K_LAYER_WIDTH) == 31);
    public_layer.Width = 57;
    Kaffeine_LayersRefresh(&f.ctx);
    assert(f.W(gl + M68K_LAYER_WIDTH) == 57 && held_locks == 1);
    assert(f.call(Fixture::layers_base, 174, { layer, 0 }) == 20 && f.stopped());
    assert(f.call(Fixture::layers_base, 102, { layer }) == 0 && held_locks == 0);
    assert(f.destroy(layer) == TRUE);
    f.ctx.layer_ops->screen_closing(&f.ctx, &screen);
    f.ctx.pubscreens[0] = {};
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_PUBSCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);

    /* Defensive model: bound callback locks, then native frees the layer
       before returning NULL. Its guest count must not poison later calls. */
    action = GUEST_LOCK_KEEP;
    create_mode = CREATE_FAILS_LATE;
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, hook) == 0 && f.ctx.running);
    assert(held_locks == 0);
    action = GUEST_DRAW;
    create_mode = CREATE_OK;
    const ULONG next = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE);
    assert(next && f.call(Fixture::layers_base, 174, { next, 0 }) == 0 && f.ctx.running);
}

/* All guest tasks share one native executor, so a native layer lock of
   one guest task would let another one in by recursion. While another
   guest task holds the lock of any layer of a LayerInfo, a creation or a
   deletion there stops the program before anything native or of the
   guest heap changes; locks of the same task, and those of another
   LayerInfo, keep nothing out. */
static void test_sibling_lock_contention()
{
    Fixture f;
    const ULONG li = f.new_info(), li2 = f.new_info();
    const ULONG bm = f.bitmap(4, 8, 1), bm2 = f.bitmap(4, 8, 1), h = f.hook();
    const ULONG task_addr = Fixture::exec_base + M68K_EXECBASE_THISTASK;
    const ULONG held = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE, h);
    const ULONG sibling = f.create(li, bm, 0, 0, 7, 7, LAYERSMART);
    const ULONG apart = f.create(li2, bm2, 0, 0, 7, 7, LAYERSIMPLE);
    assert(held && sibling && apart && f.ctx.running);
    Layer *const nh = native_layer(f, held), *const ns = native_layer(f, sibling);
    assert(nh && ns && f.L(li + M68K_LI_TOP_LAYER) == sibling && f.L(sibling + M68K_LAYER_BACK) == held);

    /* the owner task locks one layer of li */
    assert(f.call(Fixture::layers_base, 96, { 0, held }) == 0 && f.ctx.running && layer_locks[nh] == 1);

    /* another guest task: nothing of li may change */
    m68k_write32(&f.ctx, task_addr, Fixture::task + 0x100);
    const auto heap = f.heap();
    const size_t made = creates.size(), alive = native_layers.size(), evs = events.size();
    const size_t calls = hook_calls.size();
    const unsigned dels = deletes, inits = init_bitmaps, fills = default_fills, roots_made = root_makes;
    const UWORD shadows = f.ctx.num_shadows;
    auto unchanged = [&] {
        assert(creates.size() == made && native_layers.size() == alive && events.size() == evs);
        assert(deletes == dels && init_bitmaps == inits && default_fills == fills && root_makes == roots_made);
        assert(hook_calls.size() == calls && f.ctx.num_shadows == shadows && f.heap_is(heap));
        assert(held_locks == 1 && layer_locks[nh] == 1 && layer_locks[ns] == 0);
        assert(native_infos[0]->top_layer == ns && ns->back == nh && nh->back == native_infos[0]->check_lp);
    };
    /* a creation in front of the locked layer, with and without a hook */
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped());
    unchanged();
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSMART, f.hook()) == 20 && f.stopped());
    /* f.hook() itself took heap; the refusal took nothing more */
    const auto heap2 = f.heap();
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h) == 20 && f.stopped() && f.heap_is(heap2));
    assert(creates.size() == made && events.size() == evs && deletes == dels);
    /* the deletion of the locked layer itself, and of its sibling, whose
       deletion would rebuild and backfill the locked one */
    assert(f.destroy(held) == 20 && f.stopped() && f.heap_is(heap2));
    assert(f.destroy(sibling) == 20 && f.stopped() && f.heap_is(heap2));
    assert(creates.size() == made && events.size() == evs && deletes == dels && held_locks == 1);
    assert(f.L(sibling + M68K_LAYER_RP) && Kaffeine_ShadowNative(&f.ctx, f.L(sibling + M68K_LAYER_RP),
                                                                 KAFFEINE_SHADOW_RASTPORT) == ns->rp);
    /* the lock itself: the blocking ones stop, the attempt gives FALSE */
    assert(f.call(Fixture::layers_base, 96, { 0, held }) == 20 && f.stopped());
    assert(f.call(Fixture::graphics_base, 432, { 0, 0, 0, 0, 0, held }) == 20 && f.stopped());
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, held }) == FALSE && f.ctx.running);
    assert(held_locks == 1 && layer_locks[nh] == 1);
    /* the unlocked sibling: this task takes it with the attempt, and
       still the other task's lock of held stops a creation */
    assert(f.call(Fixture::graphics_base, 654, { 0, 0, 0, 0, 0, sibling }) == TRUE && f.ctx.running);
    assert(layer_locks[ns] == 1);
    assert(f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped() && creates.size() == made);
    assert(f.call(Fixture::graphics_base, 438, { 0, 0, 0, 0, 0, sibling }) == 0 && layer_locks[ns] == 0);

    /* another LayerInfo is not blocked by li's lock */
    const ULONG extra = f.create(li2, bm2, 0, 0, 3, 3, LAYERSIMPLE);
    assert(extra && f.ctx.running && creates.size() == made + 1);
    assert(f.destroy(extra) == TRUE && f.destroy(apart) == TRUE && f.ctx.running);
    assert(layer_locks[nh] == 1 && held_locks == 1);

    /* the owner task: its own lock keeps nothing out */
    m68k_write32(&f.ctx, task_addr, Fixture::task);
    const ULONG front = f.create(li, bm, 0, 0, 3, 3, LAYERSIMPLE);
    assert(front && f.ctx.running && f.L(li + M68K_LI_TOP_LAYER) == front);
    assert(layer_locks[nh] == 1);
    assert(f.destroy(front) == TRUE && f.destroy(sibling) == TRUE && f.ctx.running);
    assert(layer_locks[nh] == 1 && f.L(li + M68K_LI_TOP_LAYER) == held && !f.L(held + M68K_LAYER_FRONT));
    /* and it deletes its locked layer itself; the lock goes back first */
    events.clear();
    assert(f.destroy(held) == TRUE && f.ctx.running && held_locks == 0);
    assert(event_at("UnlockLayer") < event_at("DeleteLayer"));
    assert(f.L(li + M68K_LI_TOP_LAYER) == 0);
}

static void test_clear_clip_region()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1);
    const ULONG layer = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(layer);
    const unsigned before = clip_calls;
    assert(f.call(Fixture::layers_base, 174, { layer, 0 }) == 0 && f.ctx.running);
    assert(clip_calls == before + 1 && !f.L(layer + M68K_LAYER_CLIPREGION));
    assert(f.call(Fixture::layers_base, 174, { layer, bm }) == 20 && f.stopped());
    assert(f.call(Fixture::layers_base, 174, { li, 0 }) == 20 && f.stopped());
    assert(clip_calls == before + 1);
    Layer *n = native_layer(f, layer);
    n->ClipRegion = reinterpret_cast<Region *>(0x12345678);
    assert(f.call(Fixture::layers_base, 174, { layer, 0 }) == 20 && f.stopped());
    assert(clip_calls == before + 1);
    n->ClipRegion = nullptr;
    assert(f.destroy(layer) == TRUE);
}

/* A pixel of a guest plane of bpr bytes per row, as 68k code sees it */
static bool guest_bit(Fixture &f, ULONG plane, UWORD bpr, int x, int y)
{
    return f.memory[plane + (ULONG)y * bpr + (ULONG)(x >> 3)] & (0x80 >> (x & 7));
}

static void guest_set(Fixture &f, ULONG plane, UWORD bpr, int x, int y, bool on)
{
    UBYTE &byte = f.memory[plane + (ULONG)y * bpr + (ULONG)(x >> 3)];
    const UBYTE bit = (UBYTE)(0x80 >> (x & 7));
    byte = on ? (UBYTE)(byte | bit) : (UBYTE)(byte & ~bit);
}

static void test_blit_empty_clip()
{
    Fixture f;
    const ULONG src = f.bitmap(4, 8, 2), dst = f.bitmap(4, 8, 2);
    /* The dispatcher fixture writes this synthetic JSR return on each call. */
    m68k_write32(&f.ctx, Fixture::stack, 0x76543210);
    for (auto d : { std::initializer_list<ULONG>{32,0,0,0,1,1,0xC0,0xFF},
                    {0,8,0,0,1,1,0xC0,0xFF}, {0,0,32,0,1,1,0xC0,0xFF},
                    {0,0,0,8,1,1,0xC0,0xFF}, {0,0,0,0,32767,32767,0xC0,0} })
    {
        const auto memory = f.memory;
        const auto heap = f.heap();
        const size_t n = blits.size();
        const unsigned inits = init_bitmaps, waits = wait_blits;
        assert(f.call(Fixture::graphics_base,30,{src,dst,0},d)==0 && f.ctx.running);
        assert(blits.size()==n && init_bitmaps==inits && wait_blits==waits && f.ctx.bitmap_blits==0);
        assert(f.memory==memory && f.heap_is(heap));
    }
}

static void test_blit_mapping()
{
    Fixture f;
    std::vector<ULONG> p1, p2;
    const ULONG li1 = f.new_info(), li2 = f.new_info();
    const ULONG bm1 = f.bitmap(4, 8, 2, &p1), bm2 = f.bitmap(6, 10, 3, &p2);
    const ULONG l1 = f.create(li1, bm1, 0, 0, 31, 7, LAYERSIMPLE);
    const ULONG l2 = f.create(li2, bm2, 0, 0, 47, 9, LAYERSIMPLE);
    assert(l1 && l2 && f.ctx.running && creates.size() == 2);
    BitMap *const n1 = creates[0].bm, *const n2 = creates[1].bm;
    const UWORD shadows = f.ctx.num_shadows;
    const std::vector<UBYTE> d1(f.memory.begin() + bm1, f.memory.begin() + bm1 + M68K_SIZEOF_BITMAP);
    const std::vector<UBYTE> d2(f.memory.begin() + bm2, f.memory.begin() + bm2 + M68K_SIZEOF_BITMAP);

    bool registered = false;
    for (const M68KThunkEntry &e : graphics_manual)
        if (e.lvo == 30)
        {
            assert(e.thunk == Kaffeine_BltBitMap && !registered);
            registered = true;
        }
    assert(registered);

    /* plane 0 of bm1 a checkerboard, plane 1 set, in (3..10, 2..4); plane
       2 of bm2 all set, which a mask of planes 0 and 1 leaves alone */
    for (int y = 2; y < 5; ++y)
        for (int x = 3; x < 11; ++x)
        {
            guest_set(f, p1[0], 4, x, y, (x + y) & 1);
            guest_set(f, p1[1], 4, x, y, true);
        }
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 48; ++x)
            guest_set(f, p2[2], 6, x, y, true);

    /* WORD coordinates and sizes and UBYTE minterm and mask, from the low
       halves of their registers alone; the native LONG comes back in D0 */
    events.clear();
    const ULONG r = f.call(Fixture::graphics_base, 30, { bm1, bm2, 0 },
                           { 0xABCD0003u, 0x80000002u, 0x00010005u, 0xFFFF0004u,
                             0x12340008u, 0x00000003u, 0x123456C0u, 0xFFFFFF03u });
    assert(f.ctx.running && blits.size() == 1 && wait_blits == 1 && !blit_pending);
    const BlitCall b = blits[0];
    assert(b.src == n1 && b.dst == n2 && !b.temp);
    assert(b.xs == 3 && b.ys == 2 && b.xd == 5 && b.yd == 4 && b.w == 8 && b.h == 3);
    assert(b.minterm == 0xC0 && b.mask == 0x03 && b.result == 2 && r == 2);
    assert(events.size() == 2 && events[0] == "BltBitMap" && events[1] == "WaitBlit");
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 48; ++x)
        {
            const bool inside = x >= 5 && x < 13 && y >= 4 && y < 7;
            assert(guest_bit(f, p2[0], 6, x, y) == (inside && guest_bit(f, p1[0], 4, x - 2, y - 2)));
            assert(guest_bit(f, p2[1], 6, x, y) == inside);
            assert(guest_bit(f, p2[2], 6, x, y));
        }
    /* the 68k BitMaps stay the program's, unchanged; no native BitMap is in
       the shadow map, and the generic map gives guest memory for them */
    assert(std::equal(d1.begin(), d1.end(), f.memory.begin() + bm1));
    assert(std::equal(d2.begin(), d2.end(), f.memory.begin() + bm2));
    assert(f.ctx.num_shadows == shadows && !f.shadowed(bm1) && !f.shadowed(bm2));
    assert(m68k_to_host_or_shadow(&f.ctx, bm1) == f.ctx.mem + bm1);

    /* clear of plane 0 alone */
    assert(f.call(Fixture::graphics_base, 30, { bm1, bm2, 0 }, { 0, 0, 5, 4, 8, 3, 0x00, 0x01 }) == 1);
    assert(f.ctx.running && blits.size() == 2 && blits[1].minterm == 0 && blits[1].mask == 0x01);
    for (int y = 4; y < 7; ++y)
        for (int x = 5; x < 13; ++x)
            assert(!guest_bit(f, p2[0], 6, x, y) && guest_bit(f, p2[1], 6, x, y));

    /* inside one BitMap, up to its very edges */
    assert(f.call(Fixture::graphics_base, 30, { bm1, bm1, 0 }, { 0, 2, 24, 5, 8, 3, 0xC0, 0xFF }) == 2);
    assert(f.ctx.running && blits[2].src == n1 && blits[2].dst == n1);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 8; ++x)
            for (int p = 0; p < 2; ++p)
                assert(guest_bit(f, p1[p], 4, 24 + x, 5 + y) == guest_bit(f, p1[p], 4, x, 2 + y));

    /* the whole LONG of the native result */
    blit_force = true;
    blit_forced = (LONG)0x80000007u;
    assert(f.call(Fixture::graphics_base, 30, { bm2, bm1, 0 }, { 1, 1, 0, 0, 4, 4, 0xC0, 0x01 }) ==
           0x80000007u && f.ctx.running);
    blit_force = false;

    /* empty sizes: 0 without the native call */
    assert(f.call(Fixture::graphics_base, 30, { bm1, bm2, 0 }, { 0, 0, 0, 0, 0x00010000u, 3, 0xC0, 0xFF }) == 0);
    assert(f.call(Fixture::graphics_base, 30, { bm1, bm2, 0 }, { 0, 0, 0, 0, 8, 0xFFFF0000u, 0xC0, 0xFF }) == 0);
    assert(f.ctx.running && blits.size() == 4 && wait_blits == 4);
}

static void test_blit_refusals()
{
    Fixture f;
    std::vector<ULONG> p1, po;
    const ULONG li1 = f.new_info(), bm1 = f.bitmap(4, 8, 2, &p1);
    const ULONG l1 = f.create(li1, bm1, 0, 0, 31, 7, LAYERSIMPLE);
    const ULONG li3 = f.new_info(), big = f.bitmap(8, 16, 1);
    const ULONG l3 = f.create(li3, big, 0, 0, 63, 15, LAYERSIMPLE);
    /* a LayerInfo that has no common BitMap yet, and a BitMap of no
       LayerInfo: both classic BitMaps of the program all the same */
    const ULONG li2 = f.new_info(), bm2 = f.bitmap(4, 8, 2), other = f.bitmap(4, 8, 2, &po);
    const ULONG rp1 = f.L(l1 + M68K_LAYER_RP);
    assert(l1 && l3 && li2 && rp1 && f.ctx.running);

    auto refused = [&f](std::initializer_list<ULONG> a, std::initializer_list<ULONG> d) {
        const auto heap = f.heap();
        const size_t n = blits.size();
        const unsigned w = wait_blits;
        return f.call(Fixture::graphics_base, 30, a, d) == 20 && f.stopped() && blits.size() == n &&
               wait_blits == w && f.heap_is(heap);
    };
    auto passes = [&f](ULONG src, ULONG dst) {
        const size_t n = blits.size();
        return f.call(Fixture::graphics_base, 30, { src, dst, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }) != 20 &&
               f.ctx.running && blits.size() == n + 1;
    };

    /* malformed and plain wrong objects, as source and destination, before
       any native call, InitBitMap() included */
    for (ULONG bad : { 0u, bm1 + 2, bm1 + 1, li1, l1, rp1, 0xFFFFFFF0u })
    {
        const unsigned inits = init_bitmaps;
        assert(refused({ bad, bm1, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
        assert(refused({ bm1, bad, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
        assert(refused({ bad, other, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
        assert(init_bitmaps == inits);
    }
    /* a classic BitMap of a LayerInfo without a common BitMap yet, and one
       of no LayerInfo, are the program's ordinary BitMaps: each gets a
       native BitMap of the call alone, over its own guest planes */
    assert(passes(bm2, bm1) && blits.back().src_origin == ORIGIN_ADAPTER &&
           blits.back().dst_origin == ORIGIN_COMMON);
    assert(passes(bm1, other) && blits.back().dst_origin == ORIGIN_ADAPTER &&
           blits.back().dst_copy.Planes[1] == f.memory.data() + po[1]);
    assert(passes(other, bm2) && blits.back().src != blits.back().dst);
    assert(!f.shadowed(bm2) && !f.shadowed(other) && creates.size() == 2);
    /* TempA */
    assert(refused({ bm1, bm1, other }, { 0, 0, 8, 0, 8, 8, 0xC0, 0xFF }));
    assert(refused({ bm1, bm1, Fixture::heap_start }, { 0, 0, 8, 0, 0, 0, 0xC0, 0xFF }));
    /* negative WORDs, however the LONG reads */
    for (int i = 0; i < 6; ++i)
    {
        ULONG d[6] = { 0, 0, 0, 0, 8, 8 };
        d[i] = i & 1 ? 0xFFFFu : 0x00018000u;
        assert(refused({ bm1, bm1, 0 }, { d[0], d[1], d[2], d[3], d[4], d[5], 0xC0, 0xFF }));
    }
    /* Native clipping keeps every delegated rectangle inside both sides. */
    auto clipped = [&f](ULONG src, ULONG dst, std::initializer_list<ULONG> d, WORD w, WORD h) {
        const size_t n = blits.size();
        assert(f.call(Fixture::graphics_base, 30, { src, dst, 0 }, d) != 20 && f.ctx.running);
        assert(blits.size() == n + 1 && blits.back().w == w && blits.back().h == h);
    };
    clipped(bm1, bm1, { 1, 0, 0, 0, 32, 1, 0xC0, 0xFF }, 31, 1);
    clipped(bm1, bm1, { 0, 1, 0, 0, 1, 8, 0xC0, 0xFF }, 1, 7);
    clipped(bm1, bm1, { 0, 0, 25, 0, 8, 1, 0xC0, 0xFF }, 7, 1);
    clipped(bm1, bm1, { 0, 0, 0, 1, 1, 8, 0xC0, 0xFF }, 1, 7);
    clipped(bm1, bm1, { 0, 0, 0, 0, 0x7FFF, 1, 0xC0, 0xFF }, 32, 1);
    clipped(bm1, bm1, { 0, 0, 0, 0, 1, 0x7FFF, 0xC0, 0xFF }, 1, 8);
    clipped(big, bm1, { 0, 0, 0, 0, 40, 4, 0xC0, 0xFF }, 32, 4);
    clipped(bm1, big, { 0, 0, 0, 0, 4, 12, 0xC0, 0xFF }, 4, 8);
    assert(passes(big, bm1) && passes(bm1, big));

    /* a 68k BitMap that no longer describes its common BitMap */
    const ULONG plane1 = f.L(bm1 + M68K_BM_PLANES + 4);
    struct Change { ULONG at; ULONG size; ULONG value; };
    const Change changes[] = {
        { bm1 + M68K_BM_PLANES, 4, po[1] },
        { bm1 + M68K_BM_PLANES + 4, 4, po[0] },
        { bm1 + M68K_BM_BYTESPERROW, 2, 2 },
        { bm1 + M68K_BM_ROWS, 2, 7 },
        { bm1 + M68K_BM_DEPTH, 1, 1 },
        { bm1 + M68K_BM_FLAGS, 1, BMF_STANDARD },
        { bm1 + M68K_BM_PAD, 2, 1 },
    };
    for (const Change &c : changes)
    {
        const std::vector<UBYTE> saved(f.memory.begin() + c.at, f.memory.begin() + c.at + c.size);
        if (c.size == 4)
            m68k_write32(&f.ctx, c.at, c.value);
        else if (c.size == 2)
            m68k_write16(&f.ctx, c.at, (UWORD)c.value);
        else
            f.memory[c.at] = (UBYTE)c.value;
        assert(refused({ bm1, big, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
        assert(refused({ big, bm1, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
        std::copy(saved.begin(), saved.end(), f.memory.begin() + c.at);
        assert(passes(bm1, big));
    }
    assert(f.L(bm1 + M68K_BM_PLANES + 4) == plane1);

    /* the common BitMap outlives the layers, until the LayerInfo goes */
    BitMap *const common1 = creates[0].bm;
    assert(f.destroy(l1) == TRUE && passes(bm1, bm1) && blits.back().src == common1);
    assert(f.call(Fixture::layers_base, 150, { li1 }) == 0 && f.ctx.running);
    /* then the 68k BitMap is the program's ordinary BitMap again: a new
       native BitMap of each call, never the disposed common one */
    assert(passes(bm1, big) && blits.back().src_origin == ORIGIN_ADAPTER &&
           blits.back().src != common1 && blits.back().src_copy.Planes[1] == f.memory.data() + p1[1]);
    assert(passes(big, bm1) && blits.back().dst_origin == ORIGIN_ADAPTER);
    /* and also without the bridge, with every LayerInfo gone */
    Kaffeine_LayersCleanup(&f.ctx);
    assert(!f.ctx.layers && passes(big, big) && blits.back().src_origin == ORIGIN_ADAPTER);
    assert(blits.back().src == blits.back().dst && f.ctx.bitmap_blits == 0);
    assert(refused({ big, big, other }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
    assert(refused({ big, bm1 + 1, 0 }, { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF }));
}

static void test_blit_lifetime()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1);
    const ULONG l = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(l && f.destroy(l) == TRUE && f.ctx.running);

    /* Only the root is left, so DisposeLayerInfo() would go through, but
       not while a blit uses the BitMap: it stops the program, and the
       LayerInfo, its root and the common BitMap stay until the blit and
       WaitBlit() are done. */
    ULONG nested = 0;
    blit_reenter = [&f, &nested, li] {
        assert(f.ctx.bitmap_blits == 1);
        f.state2.a[0] = li;
        f.state2.a[7] = Fixture::stack2;
        m68k_write32(&f.ctx, Fixture::stack2, NESTED_RETURN);
        KaffeineDispatchResult r = {};
        assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu2, Fixture::layers_base - 150, &r));
        nested = f.state2.d[0];
        assert(native_infos.size() == 1 && disposed_infos == 0 && root_deletes == 0);
    };
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 8, 0xC0, 0x01 }) == 20 &&
           f.stopped());
    blit_reenter = nullptr;
    assert(nested == 20 && blits.size() == 1 && wait_blits == 1 && !blit_pending);
    assert(event_at("BltBitMap") < event_at("WaitBlit") && disposed_infos == 0);
    assert(f.ctx.bitmap_blits == 0 && blits[0].src_origin == ORIGIN_COMMON);

    /* afterwards the disposal goes through; the 68k BitMap and its planes
       are still the program's, so it is an ordinary BitMap now, with a new
       native BitMap of each call and never the disposed common one */
    BitMap *const old = creates.back().bm;
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running && disposed_infos == 1);
    const unsigned inits = init_bitmaps;
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 8, 0xC0, 0x01 }) == 1);
    assert(f.ctx.running && blits.size() == 2 && init_bitmaps == inits + 1);
    assert(blits[1].src_origin == ORIGIN_ADAPTER && blits[1].src == blits[1].dst && blits[1].src != old);

    /* a bare planar blit holds every LayerInfo of the program too */
    const ULONG idle = f.new_info();
    nested = 0;
    blit_reenter = [&f, &nested, idle] {
        f.state2.a[0] = idle;
        f.state2.a[7] = Fixture::stack2;
        m68k_write32(&f.ctx, Fixture::stack2, NESTED_RETURN);
        KaffeineDispatchResult r = {};
        assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu2, Fixture::layers_base - 150, &r));
        nested = f.state2.d[0];
    };
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 8, 0xC0, 0x01 }) == 20 &&
           f.stopped());
    blit_reenter = nullptr;
    assert(nested == 20 && blits.size() == 3 && disposed_infos == 1 && f.ctx.bitmap_blits == 0);
    assert(f.call(Fixture::layers_base, 150, { idle }) == 0 && f.ctx.running && disposed_infos == 2);

    /* a new LayerInfo over the same 68k BitMap has a common BitMap of its
       own, which the table gives from now on */
    const ULONG li2 = f.new_info();
    const ULONG l2 = f.create(li2, bm, 0, 0, 7, 7, LAYERSIMPLE);
    assert(l2 && f.ctx.running);
    BitMap *const now = creates.back().bm;
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 8, 0xC0, 0x01 }) == 1);
    assert(f.ctx.running && blits.size() == 4 && blits[3].src == now && blits[3].dst == now);
}

/* A pixel of plane p of the native BitMap of a screen */
static bool screen_bit(const NativeScreenBitMap &s, int p, int x, int y)
{
    return plane_bit(&s.bm, p, x, y);
}

/* An ordinary planar BitMap of the program, of no LayerInfo, to the
   BitMap of a screen that the program opened and back: graphics gets a
   native BitMap of the call over the guest planes and the native screen's
   own BitMap, never one read from guest bytes; nothing enters the shadow
   map; WORD, UBYTE and LONG mapping, and every register but D0 kept. */
static void test_blit_screen_transfer()
{
    Fixture f;
    NativeScreenBitMap screen;
    /* 40 pixels wide by GetBitMapAttr(), though BytesPerRow says 48 */
    const ULONG sg = f.open_screen(screen, 6, 12, 3, 40);
    const ULONG handle = sg + M68K_SCR_BITMAP;
    std::vector<ULONG> p, q;
    const ULONG bm = f.bitmap(4, 8, 2, &p), back = f.bitmap(6, 10, 3, &q);
    const UWORD shadows = f.ctx.num_shadows;
    const std::vector<UBYTE> desc(f.memory.begin() + bm, f.memory.begin() + bm + M68K_SIZEOF_BITMAP);
    const std::vector<UBYTE> snap(f.memory.begin() + sg, f.memory.begin() + sg + M68K_SIZEOF_SCREEN);

    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 32; ++x)
        {
            guest_set(f, p[0], 4, x, y, (x * 3 + y) % 5 == 0);
            guest_set(f, p[1], 4, x, y, (x + y) % 3 == 0);
        }
    /* plane 2 of the screen all set: a mask of planes 0 and 1 leaves it */
    std::fill(screen.planes.begin() + 2 * 6 * 12, screen.planes.end(), 0xFF);

    /* planar to screen, the WORDs and UBYTEs from the low halves alone */
    events.clear();
    ULONG r = f.call(Fixture::graphics_base, 30, { bm, handle, 0 },
                     { 0xABCD0002u, 0x80000001u, 0x00010007u, 0xFFFF0003u,
                       0x12340010u, 0x00000005u, 0x123456C0u, 0xFFFFFF03u });
    assert(f.ctx.running && r == 2 && blits.size() == 1 && wait_blits == 1 && !blit_pending);
    assert(init_bitmaps == 1 && bitmap_attrs == 2 && f.ctx.bitmap_blits == 0);
    BlitCall b = blits[0];
    assert(b.src_origin == ORIGIN_ADAPTER && b.dst_origin == ORIGIN_SCREEN && b.dst == &screen.bm);
    assert(b.xs == 2 && b.ys == 1 && b.xd == 7 && b.yd == 3 && b.w == 16 && b.h == 5);
    assert(b.minterm == 0xC0 && b.mask == 0x03 && !b.temp);
    /* graphics' planar BitMap over the guest planes themselves */
    assert(b.src_copy.BytesPerRow == 4 && b.src_copy.Rows == 8 && b.src_copy.Depth == 2);
    assert(b.src_copy.Planes[0] == f.memory.data() + p[0] && b.src_copy.Planes[1] == f.memory.data() + p[1]);
    assert(events.size() == 2 && events[0] == "BltBitMap" && events[1] == "WaitBlit");
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 48; ++x)
        {
            const bool inside = x >= 7 && x < 23 && y >= 3 && y < 8;
            assert(screen_bit(screen, 0, x, y) == (inside && guest_bit(f, p[0], 4, x - 5, y - 2)));
            assert(screen_bit(screen, 1, x, y) == (inside && guest_bit(f, p[1], 4, x - 5, y - 2)));
            assert(screen_bit(screen, 2, x, y));
        }
    /* both 68k objects as they were, no shadow, guest memory for both */
    assert(std::equal(desc.begin(), desc.end(), f.memory.begin() + bm));
    assert(std::equal(snap.begin(), snap.end(), f.memory.begin() + sg));
    assert(f.ctx.num_shadows == shadows && !f.shadowed(bm) && !f.shadowed(handle));
    assert(m68k_to_host_or_shadow(&f.ctx, handle) == f.memory.data() + handle);
    for (ULONG off = 0; off < M68K_SIZEOF_BITMAP; off += 2)
        for (const void *n : { (const void *)&screen.bm, (const void *)screen.planes.data(),
                               (const void *)&screen.screen })
            assert(f.L(handle + off) != (ULONG)reinterpret_cast<uintptr_t>(n));

    /* and back, from the screen into another ordinary BitMap, all planes */
    r = f.call(Fixture::graphics_base, 30, { handle, back, 0 }, { 7, 3, 1, 2, 16, 5, 0xC0, 0x07 });
    assert(f.ctx.running && r == 3 && blits.size() == 2 && init_bitmaps == 2 && bitmap_attrs == 4);
    b = blits[1];
    assert(b.src_origin == ORIGIN_SCREEN && b.src == &screen.bm && b.dst_origin == ORIGIN_ADAPTER);
    for (int i = 0; i < 3; ++i)
        assert(b.dst_copy.Planes[i] == f.memory.data() + q[i]);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 48; ++x)
        {
            const bool inside = x >= 1 && x < 17 && y >= 2 && y < 7;
            for (int i = 0; i < 3; ++i)
                assert(guest_bit(f, q[i], 6, x, y) == (inside && screen_bit(screen, i, x + 6, y + 1)));
            /* the round trip gives the program's own pixels */
            if (inside)
                assert(guest_bit(f, q[0], 6, x, y) == guest_bit(f, p[0], 4, x + 1, y - 1) &&
                       guest_bit(f, q[1], 6, x, y) == guest_bit(f, p[1], 4, x + 1, y - 1) &&
                       guest_bit(f, q[2], 6, x, y));
        }

    /* inside the screen up to the width that graphics gives, and inside one
       ordinary BitMap through one native BitMap of the call */
    r = f.call(Fixture::graphics_base, 30, { handle, handle, 0 }, { 0, 0, 24, 0, 16, 12, 0xC0, 0xFF });
    assert(f.ctx.running && r == 3 && blits.back().src == &screen.bm && blits.back().dst == &screen.bm);
    const unsigned inits = init_bitmaps;
    r = f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 16, 0, 16, 8, 0xC0, 0xFF });
    assert(f.ctx.running && r == 2 && init_bitmaps == inits + 1 && blits.back().src == blits.back().dst);

    /* the whole native LONG */
    blit_force = true;
    blit_forced = (LONG)0x80000007u;
    assert(f.call(Fixture::graphics_base, 30, { handle, bm, 0 }, { 1, 1, 0, 0, 4, 4, 0xC0, 0x01 }) ==
           0x80000007u && f.ctx.running);
    blit_forced = 0;
    assert(f.call(Fixture::graphics_base, 30, { bm, handle, 0 }, { 1, 1, 0, 0, 4, 4, 0xC0, 0x01 }) == 0 &&
           f.ctx.running);
    blit_force = false;

    /* empty sizes: 0 without any native call, also no size query */
    const size_t n = blits.size();
    const unsigned attrs = bitmap_attrs, made = init_bitmaps;
    assert(f.call(Fixture::graphics_base, 30, { bm, handle, 0 }, { 0, 0, 0, 0, 0x00010000u, 3, 0xC0, 0xFF }) == 0);
    assert(f.call(Fixture::graphics_base, 30, { handle, bm, 0 }, { 0, 0, 0, 0, 3, 0xFFFF0000u, 0xC0, 0xFF }) == 0);
    assert(f.ctx.running && blits.size() == n && bitmap_attrs == attrs && init_bitmaps == made);

    /* a common BitMap and the screen: the bridge's common BitMap, and no
       InitBitMap() during the call */
    const ULONG li = f.new_info(), cbm = f.bitmap(4, 8, 1);
    assert(f.create(li, cbm, 0, 0, 31, 7, LAYERSIMPLE) && f.ctx.running);
    BitMap *const common = creates.back().bm;
    const unsigned before = init_bitmaps;
    assert(f.call(Fixture::graphics_base, 30, { cbm, handle, 0 }, { 0, 0, 0, 0, 32, 8, 0xC0, 0x01 }) == 1);
    assert(blits.back().src == common && blits.back().src_origin == ORIGIN_COMMON);
    assert(f.call(Fixture::graphics_base, 30, { handle, cbm, 0 }, { 8, 4, 0, 0, 32, 8, 0xC0, 0x01 }) == 1);
    assert(blits.back().dst == common && blits.back().src == &screen.bm && init_bitmaps == before);
    assert(f.ctx.running && f.ctx.num_shadows == shadows + 1);
    f.close_screen(screen, sg);
}

/* What BltBitMap() refuses around screens, before the blit and, but for
   the size query of an open screen of the program, before any native call:
   the BitMap of a screen that the program only uses, also when its bytes
   describe a planar BitMap of the heap; addresses near a handle; a screen
   whose RastPort has no BitMap; a closed screen's handle; rectangles
   outside the size that graphics gives, whatever BytesPerRow and the
   snapshot say. A blit holds every screen of the program until WaitBlit(). */
static void test_blit_screen_refusals()
{
    Fixture f;
    NativeScreenBitMap mine, theirs;
    const ULONG sg = f.open_screen(mine, 6, 12, 3, 40);
    const ULONG pg = f.open_screen(theirs, 6, 12, 3, 40, false);
    const ULONG handle = sg + M68K_SCR_BITMAP, borrowed = pg + M68K_SCR_BITMAP;
    const ULONG bm = f.bitmap(4, 8, 2), other = f.bitmap(4, 8, 2);
    const ULONG plane = Kaffeine_HeapAlloc(&f.ctx, 4 * 16, MEMF_CLEAR);
    assert(plane);
    /* both snapshots now describe a valid 32 x 16 planar BitMap of the heap */
    for (ULONG h : { handle, borrowed })
    {
        m68k_write16(&f.ctx, h + M68K_BM_BYTESPERROW, 4);
        m68k_write16(&f.ctx, h + M68K_BM_ROWS, 16);
        f.memory[h + M68K_BM_FLAGS] = 0;
        f.memory[h + M68K_BM_DEPTH] = 1;
        m68k_write32(&f.ctx, h + M68K_BM_PLANES, plane);
    }

    auto early = [&f](std::initializer_list<ULONG> a, std::initializer_list<ULONG> d) {
        const auto heap = f.heap();
        const size_t n = blits.size();
        const unsigned w = wait_blits, attrs = bitmap_attrs, inits = init_bitmaps;
        return f.call(Fixture::graphics_base, 30, a, d) == 20 && f.stopped() && blits.size() == n &&
               wait_blits == w && bitmap_attrs == attrs && init_bitmaps == inits && f.heap_is(heap) &&
               f.ctx.bitmap_blits == 0;
    };
    auto clipped = [&f](std::initializer_list<ULONG> a, std::initializer_list<ULONG> d, WORD w, WORD h) {
        const size_t n = blits.size();
        return f.call(Fixture::graphics_base, 30, a, d) != 20 && f.ctx.running && blits.size() == n + 1 &&
               blits.back().w == w && blits.back().h == h && f.ctx.bitmap_blits == 0;
    };
    auto passes = [&f](std::initializer_list<ULONG> a, std::initializer_list<ULONG> d) {
        const size_t n = blits.size();
        return f.call(Fixture::graphics_base, 30, a, d) != 20 && f.ctx.running && blits.size() == n + 1;
    };
    const std::initializer_list<ULONG> small = { 0, 0, 0, 0, 8, 8, 0xC0, 0xFF };

    /* a borrowed screen's BitMap, either side, never read as planar */
    assert(early({ borrowed, bm, 0 }, small));
    assert(early({ bm, borrowed, 0 }, small));
    assert(early({ borrowed, handle, 0 }, small));
    /* the 68k Screens themselves, their RastPorts, near the handle */
    for (ULONG bad : { sg, sg + M68K_SCR_RASTPORT, handle + 2, handle - 2, pg, borrowed + 2 })
    {
        assert(early({ bad, handle, 0 }, small));
        assert(early({ handle, bad, 0 }, small));
    }
    /* the owned handle is the screen, not the planar BitMap its bytes now
       describe: the screen's 40 pixels and 12 rows count */
    assert(passes({ handle, bm, 0 }, { 24, 0, 0, 0, 16, 8, 0xC0, 0xFF }));
    assert(blits.back().src == &mine.bm && blits.back().src_origin == ORIGIN_SCREEN);
    assert(f.L(handle + M68K_BM_PLANES) == plane && !f.shadowed(handle));
    /* TempA and negative WORDs, before the size query */
    assert(early({ bm, handle, other }, small));
    for (int i = 0; i < 6; ++i)
    {
        ULONG d[6] = { 0, 0, 0, 0, 8, 8 };
        d[i] = i & 1 ? 0xFFFFu : 0x00018000u;
        assert(early({ handle, bm, 0 }, { d[0], d[1], d[2], d[3], d[4], d[5], 0xC0, 0xFF }));
    }
    /* outside the size of graphics, though inside BytesPerRow * 8 */
    assert(clipped({ bm, handle, 0 }, { 0, 0, 25, 0, 16, 1, 0xC0, 0xFF }, 15, 1));
    assert(clipped({ handle, bm, 0 }, { 25, 0, 0, 0, 16, 1, 0xC0, 0xFF }, 15, 1));
    assert(clipped({ handle, bm, 0 }, { 0, 5, 0, 0, 8, 8, 0xC0, 0xFF }, 8, 7));
    assert(clipped({ bm, handle, 0 }, { 0, 0, 0, 5, 8, 8, 0xC0, 0xFF }, 8, 7));
    assert(clipped({ handle, handle, 0 }, { 0, 0, 0, 0, 41, 1, 0xC0, 0xFF }, 40, 1));
    assert(passes({ handle, handle, 0 }, { 0, 0, 0, 0, 40, 12, 0xC0, 0xFF }));

    /* a screen whose native RastPort has no BitMap */
    mine.screen.RastPort.BitMap = nullptr;
    assert(early({ handle, bm, 0 }, small));
    mine.screen.RastPort.BitMap = &mine.bm;

    /* a blit holds every LayerInfo of the program; CloseScreen() of the
       screen bridge refuses on the same count (test_screens) */
    const ULONG li = f.new_info();
    ULONG nested = 0;
    blit_reenter = [&f, &nested, li] {
        assert(f.ctx.bitmap_blits == 1 && f.ctx.layers);
        f.state2.a[0] = li;
        f.state2.a[7] = Fixture::stack2;
        m68k_write32(&f.ctx, Fixture::stack2, NESTED_RETURN);
        KaffeineDispatchResult r = {};
        assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu2, Fixture::layers_base - 150, &r));
        nested = f.state2.d[0];
    };
    assert(f.call(Fixture::graphics_base, 30, { bm, handle, 0 }, small) == 20 && f.stopped());
    blit_reenter = nullptr;
    assert(nested == 20 && disposed_infos == 0 && f.ctx.bitmap_blits == 0 && !blit_pending);

    /* closed: the handle names nothing, and its cleared bytes no BitMap */
    f.close_screen(mine, sg);
    assert(early({ handle, bm, 0 }, small));
    assert(early({ bm, handle, 0 }, small));
    /* a new screen has a handle of its own, which works */
    NativeScreenBitMap next;
    const ULONG ng = f.open_screen(next, 4, 8, 1, 32);
    assert(passes({ ng + M68K_SCR_BITMAP, bm, 0 }, small) && blits.back().src == &next.bm);
    f.close_screen(next, ng);
    f.close_screen(theirs, pg);
}

static void test_cleanup()
{
    {
        /* deleted before their LayerInfos, without 68k code, locks first */
        Fixture f;
        const ULONG li1 = f.new_info(), li2 = f.new_info(), bm = f.bitmap(4, 8, 2), h = f.hook();
        /* a goes first and uncovers b, whose 68k Hook must not run then */
        const ULONG a = f.create(li1, bm, 0, 0, 31, 7, LAYERSIMPLE, h);
        const ULONG b = f.create(li1, bm, 0, 0, 7, 7, LAYERSMART | LAYERBACKDROP, h);
        const ULONG c = f.create(li2, bm, 0, 0, 7, 7, LAYERSIMPLE, h);
        assert(a && b && c && hook_calls.size() == 3);
        assert(f.L(li1 + M68K_LI_TOP_LAYER) == a && f.L(a + M68K_LAYER_BACK) == b);
        const ULONG rps[] = { f.L(a + M68K_LAYER_RP), f.L(b + M68K_LAYER_RP), f.L(c + M68K_LAYER_RP) };
        assert(f.call(Fixture::layers_base, 96, { 0, a }) == 0 && held_locks == 1);
        const UWORD shadows = f.ctx.num_shadows;
        events.clear();
        Kaffeine_LayersCleanup(&f.ctx);
        assert(!f.ctx.layers && held_locks == 0 && native_layers.empty() && native_infos.empty());
        assert(deletes == 3 && disposed_infos == 2 && hook_calls.size() == 3);
        /* each root went inside its disposal, over its intact BitMap */
        assert(root_makes == 2 && root_deletes == 2 && roots.empty());
        assert(event_at("UnlockLayer") < event_at("DeleteLayer"));
        assert(event_last("DeleteLayer") < event_at("DisposeLayerInfo"));
        for (ULONG g : { a, b, c })
            assert(f.zero(g, M68K_SIZEOF_LAYER));
        for (ULONG r : rps)
            assert(f.zero(r, M68K_SIZEOF_RASTPORT) && !Kaffeine_ShadowNative(&f.ctx, r, KAFFEINE_SHADOW_RASTPORT));
        assert(f.ctx.num_shadows == shadows - 3);
        assert(f.zero(li1, M68K_SIZEOF_LAYER_INFO) && f.zero(li2, M68K_SIZEOF_LAYER_INFO));
        /* again: nothing */
        Kaffeine_LayersCleanup(&f.ctx);
        assert(deletes == 3 && disposed_infos == 2);
        /* without the bridge, the calls stop before anything native */
        assert(f.create(li1, bm, 0, 0, 7, 7, LAYERSIMPLE) == 20 && f.stopped());
        assert(f.destroy(a) == 20 && f.stopped() && deletes == 3);
    }
    {
        /* layers keeps a layer at the end: it gets no hook of the bridge,
           its LayerInfo is never disposed of, nothing is freed under it */
        Fixture f;
        const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1), h = f.hook();
        const ULONG l = f.create(li, bm, 0, 0, 7, 7, LAYERSIMPLE, h);
        Layer *n = native_layer(f, l);
        assert(l && n && n->BackFill != LAYERS_NOBACKFILL);
        BitMap *const kept = creates[0].bm;
        delete_fails = true;
        Kaffeine_LayersCleanup(&f.ctx);
        delete_fails = false;
        assert(deletes == 1 && disposed_infos == 0 && native_infos.size() == 1 && native_layers.size() == 1);
        assert(n->BackFill == LAYERS_NOBACKFILL && held_locks == 0 && n->rp->BitMap == kept);
        /* the common BitMap of the bridge is still there for it and for the
           root behind it, which reads it as the root would */
        assert(kept->BytesPerRow == 4 && kept->Rows == 8 && kept->Flags == BMF_STANDARD);
        Layer *const root = native_infos[0]->check_lp;
        assert(root && n->back == root && root->rp->BitMap == kept && root_deletes == 0);
        root_read(root);
        assert(f.zero(l, M68K_SIZEOF_LAYER));
        /* the test's own leftovers; the bridge's BitMap stays retained */
        forget(native_layers[0]);
        forget_root(native_infos[0]);
        native_infos[0]->top_layer = nullptr;
        delete native_infos[0];
        native_infos.clear();
    }
}

static void test_bitmap_attributes()
{
    Fixture f;
    const ULONG bm = f.bitmap(4, 8, 2);
    auto query = [&](ULONG bitmap, ULONG attribute) {
        return f.call(Fixture::graphics_base, 960, { bitmap }, { 0x11223344u, attribute });
    };
    const auto heap = f.heap();
    assert(query(bm, BMA_WIDTH) == 32 && query(bm, BMA_HEIGHT) == 8);
    assert(query(bm, BMA_DEPTH) == 2 && query(bm, BMA_FLAGS) == 0);
    assert(query(bm, 0x10004u) == 0 && f.ctx.running && f.heap_is(heap));
    assert(init_bitmaps == 0 && bitmap_attrs == 0 && blits.empty());
    f.memory[bm + M68K_BM_FLAGS] = BMF_STANDARD;
    assert(query(bm, BMA_FLAGS) == BMF_STANDARD);
    const ULONG li = f.new_info(), layer = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(layer && query(bm, BMA_DEPTH) == 2 && f.ctx.running);
    const unsigned made = init_bitmaps;
    f.memory[bm + M68K_BM_FLAGS] = 0;
    assert(query(bm, BMA_FLAGS) == 20 && f.stopped() && init_bitmaps == made && !bitmap_attrs);
    f.memory[bm + M68K_BM_FLAGS] = BMF_STANDARD;
    assert(query(bm, BMA_FLAGS) == BMF_STANDARD && f.ctx.running);

    NativeScreenBitMap own, other;
    const ULONG sg = f.open_screen(own, 6, 12, 3, 40);
    const ULONG pg = f.open_screen(other, 6, 12, 3, 40, false);
    const ULONG handle = sg + M68K_SCR_BITMAP;
    own.bm.Flags = 0x8F;
    m68k_write16(&f.ctx, handle + M68K_BM_BYTESPERROW, 0xFFFF);
    m68k_write16(&f.ctx, handle + M68K_BM_ROWS, 1);
    f.memory[handle + M68K_BM_DEPTH] = 7;
    assert(query(handle, BMA_WIDTH) == 40 && query(handle, BMA_HEIGHT) == 12);
    assert(query(handle, BMA_DEPTH) == 3 && query(handle, BMA_FLAGS) == 14);
    const unsigned attrs = bitmap_attrs;
    assert(query(handle, 0x10004u) == 0 && bitmap_attrs == attrs && f.ctx.running);
    assert(query(pg + M68K_SCR_BITMAP, BMA_DEPTH) == 20 && f.stopped());
    assert(bitmap_attrs == attrs);
    f.close_screen(own, sg);
    assert(query(handle, BMA_WIDTH) == 20 && f.stopped() && bitmap_attrs == attrs);
    f.close_screen(other, pg);
    for (ULONG bad : { 0u, bm + 2, 0xFFFFFFF0u })
        assert(query(bad, BMA_WIDTH) == 20 && f.stopped());
    assert(bitmap_attrs == attrs && init_bitmaps == made && blits.empty());
    Kaffeine_LayersCleanup(&f.ctx);
    assert(query(bm, BMA_WIDTH) == 32 && f.ctx.running);
}

/* What graphics' InitBitMap() leaves in the first 8 bytes of a classic
   BitMap (graphics/gfx.h), big endian: BytesPerRow, Rows, Flags
   BMF_STANDARD, Depth, pad 0 */
static std::vector<UBYTE> classic_head(UWORD bpr, UWORD rows, UBYTE depth)
{
    return { (UBYTE)(bpr >> 8), (UBYTE)bpr, (UBYTE)(rows >> 8), (UBYTE)rows, BMF_STANDARD, depth, 0, 0 };
}

/* InitBitMap() (-390) through its literal LVO gives D0 0 and changes the
   8 bytes at bm to classic_head(bpr, rows, depth) and nothing else of
   guest memory: not the planes, not a byte around. Nothing native runs,
   no BitMap is set up natively, no blit, no guest memory and no shadow. */
static bool init_writes(Fixture &f, ULONG bm, ULONG d0, ULONG d1, ULONG d2,
                        UWORD bpr, UWORD rows, UBYTE depth)
{
    m68k_write32(&f.ctx, Fixture::stack, 0x76543210);
    std::vector<UBYTE> expected = f.memory;
    const std::vector<UBYTE> head = classic_head(bpr, rows, depth);
    std::copy(head.begin(), head.end(), expected.begin() + bm);
    const unsigned inits = init_bitmaps, attrs = bitmap_attrs, waits = wait_blits;
    const size_t n = blits.size(), e = events.size(), c = creates.size(), d = deletes;
    const UWORD shadows = f.ctx.num_shadows;
    const ULONG avail = f.avail();
    return f.call(Fixture::graphics_base, 390, { bm }, { d0, d1, d2 }) == 0 && f.ctx.running &&
           f.memory == expected && init_bitmaps == inits && bitmap_attrs == attrs &&
           wait_blits == waits && blits.size() == n && events.size() == e && creates.size() == c &&
           deletes == d && f.ctx.num_shadows == shadows && f.avail() == avail;
}

/* InitBitMap() stops the program (20) with no byte of guest memory
   changed and nothing native */
static bool init_refused(Fixture &f, ULONG bm, ULONG d0 = 3, ULONG d1 = 320, ULONG d2 = 200)
{
    m68k_write32(&f.ctx, Fixture::stack, 0x76543210);
    const std::vector<UBYTE> before = f.memory;
    const unsigned inits = init_bitmaps, attrs = bitmap_attrs, waits = wait_blits;
    const size_t n = blits.size(), e = events.size(), c = creates.size(), d = deletes;
    const UWORD shadows = f.ctx.num_shadows;
    return f.call(Fixture::graphics_base, 390, { bm }, { d0, d1, d2 }) == 20 && f.stopped() &&
           f.memory == before && init_bitmaps == inits && bitmap_attrs == attrs &&
           wait_blits == waits && blits.size() == n && events.size() == e && creates.size() == c &&
           deletes == d && f.ctx.num_shadows == shadows;
}

static void test_bitmap_vector_guards()
{
    Fixture f;
    const ULONG bm = f.bitmap(4, 8, 2);
    const auto heap = f.heap();
    const auto before = events;
    const unsigned made = init_bitmaps, attrs = bitmap_attrs;
    for (ULONG lvo : { 606u, 636u, 678u, 918u, 924u, 942u, 1020u, 1206u })
    {
        assert(f.call(Fixture::graphics_base, lvo, { bm, bm, bm, bm }, { 1, 2, 3, 4, 5, 6, 7, 8 }) == 20);
        assert(f.stopped() && f.heap_is(heap) && events == before);
        assert(init_bitmaps == made && bitmap_attrs == attrs && blits.empty() && !wait_blits);
    }
    for (ULONG lvo : { 96u, 168u, 174u, 180u, 186u })
    {
        assert(f.call(Fixture::cybergraphics_base, lvo, { bm, bm, bm, bm }, { 1, 2, 3, 4, 5, 6, 7, 8 }) == 20);
        assert(f.stopped() && f.heap_is(heap) && events == before);
        assert(init_bitmaps == made && bitmap_attrs == attrs && blits.empty() && !wait_blits);
    }
    /* InitBitMap() is converted, not refused: the same ordinary BitMap
       gets graphics' scalars of depth 1, width 2 and height 3 */
    assert(init_writes(f, bm, 1, 2, 3, 2, 3, 1));
    assert(init_bitmaps == made && events == before && blits.empty());
}

/* InitBitMap() (-390) as graphics' initbitmap.c: A0 bm, D0 a BYTE depth,
   D1 and D2 UWORD width and height, from the low halves alone; Depth,
   Rows, Flags BMF_STANDARD, pad 0 and BytesPerRow ((width + 15) >> 3) & ~1
   big endian, and no Plane pointer or other byte changes. Any scalars,
   zero ones and the full narrowed range included, on a BitMap whose
   other bytes are anything, in static, heap and stack storage and at the
   very end of guest memory. Later calls check what it wrote. */
static void test_init_bitmap_scalars()
{
    Fixture f;
    struct Case { ULONG d0, d1, d2; UWORD bpr, rows; UBYTE depth; };
    const Case cases[] = {
        { 3, 320, 200, 40, 200, 3 },
        { 0xABCDEF08u, 0x12340011u, 0xFFFF0007u, 4, 7, 8 },
        { 0, 0, 0, 0, 0, 0 },
        { 0xFFFFFFFFu, 1, 1, 2, 1, 0xFF },
        { 0x80u, 0xFFFFu, 0xFFFFu, 0x2000, 0xFFFF, 0x80 },
        { 0x7Fu, 0xFFF0u, 0x8000u, 0x1FFE, 0x8000, 0x7F },
        { 1, 15, 2, 2, 2, 1 },
        { 1, 16, 2, 2, 2, 1 },
        { 1, 17, 2, 4, 2, 1 },
        { 0x00000108u, 0x00010000u, 0x00010000u, 0, 0, 8 },
    };
    /* static storage below the libraries, the heap, a frame below the
       stack pointer, and the last 40 bytes of guest memory */
    const ULONG in_heap = f.bitmap(4, 8, 2);
    const ULONG storage[] = { 0x0600, in_heap, Fixture::stack - 0x30,
                              f.ctx.mem_size - M68K_SIZEOF_BITMAP };
    UBYTE fill = 0x31;
    for (ULONG bm : storage)
        for (const Case &c : cases)
        {
            /* arbitrary bytes, planes neither NULL nor aligned nor inside
               guest memory among them, and the same around the BitMap but
               for the heap's own chunk headers; init_writes() compares
               every byte of guest memory all the same */
            const ULONG margin = bm == in_heap ? 0 : 16;
            for (ULONG i = bm - margin; i < bm + M68K_SIZEOF_BITMAP + margin && i < f.ctx.mem_size; ++i)
                f.memory[i] = (UBYTE)(fill + 7 * i);
            m68k_write32(&f.ctx, bm + M68K_BM_PLANES, 0xFFFFFFFFu);
            m68k_write32(&f.ctx, bm + M68K_BM_PLANES + 4, 0x00000001u);
            fill = (UBYTE)(fill + 0x0B);
            assert(init_writes(f, bm, c.d0, c.d1, c.d2, c.bpr, c.rows, c.depth));
            assert(f.L(bm + M68K_BM_PLANES) == 0xFFFFFFFFu && f.L(bm + M68K_BM_PLANES + 4) == 1);
        }
    assert(init_bitmaps == 0 && events.empty() && blits.empty() && creates.empty() && !bitmap_attrs);

    /* over real planes: later calls see the new scalars and the planes
       that the program set up, and check them themselves */
    std::vector<ULONG> planes;
    const ULONG bm = f.bitmap(4, 8, 2, &planes);
    assert(init_writes(f, bm, 2, 64, 4, 8, 4, 2));
    assert(f.L(bm + M68K_BM_PLANES) == planes[0] && f.L(bm + M68K_BM_PLANES + 4) == planes[1]);
    assert(f.call(Fixture::graphics_base, 960, { bm }, { 0, BMA_WIDTH }) == 64 && f.ctx.running);
    assert(f.call(Fixture::graphics_base, 960, { bm }, { 0, BMA_HEIGHT }) == 4);
    assert(f.call(Fixture::graphics_base, 960, { bm }, { 0, BMA_FLAGS }) == BMF_STANDARD);
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 4, 0xC0, 0xFF }) == 2);
    assert(f.ctx.running && blits.size() == 1 && blits[0].src_copy.BytesPerRow == 8 &&
           blits[0].src_copy.Rows == 4 && blits[0].src_copy.Planes[1] == f.memory.data() + planes[1]);
    /* a depth of 0 is written as such, and refused by the next consumer */
    assert(init_writes(f, bm, 0, 64, 4, 8, 4, 0));
    assert(f.call(Fixture::graphics_base, 960, { bm }, { 0, BMA_DEPTH }) == 20 && f.stopped());
}

/* Before any write: NULL, odd, incomplete, outside or wrapping addresses
   and a context without guest memory; the BitMap of a screen of the
   program, owned or borrowed, by the table of screens alone. A copy of a
   handle's bytes elsewhere is an ordinary BitMap, and a handle that the
   table no longer names is one too. */
static void test_init_bitmap_refusals()
{
    Fixture f;
    const ULONG size = f.ctx.mem_size;

    for (ULONG bad : { 0u, 1u, 0x0601u, size - M68K_SIZEOF_BITMAP + 1, size - M68K_SIZEOF_BITMAP + 2,
                       size - 2, size, size + 0x100, 0x80000000u, 0xFFFFFFD8u, 0xFFFFFFFEu })
        assert(init_refused(f, bad));
    assert(init_writes(f, size - M68K_SIZEOF_BITMAP, 1, 8, 1, 2, 1, 1));

    /* no guest memory at all: nothing is read or written */
    f.state.a[0] = 0x0600;
    f.state.d[0] = 1;
    f.state.d[1] = 16;
    f.state.d[2] = 16;
    f.ctx.running = TRUE;
    f.ctx.exit_code = 0;
    UBYTE *const mem = f.ctx.mem;
    const std::vector<UBYTE> before = f.memory;
    f.ctx.mem = nullptr;
    assert(Kaffeine_InitBitMap(&f.ctx, &f.cpu) == 20 && !f.ctx.running && f.ctx.exit_code == 20);
    f.ctx.mem = mem;
    assert(f.memory == before);

    NativeScreenBitMap mine, theirs;
    const ULONG sg = f.open_screen(mine, 6, 12, 3, 40);
    const ULONG pg = f.open_screen(theirs, 6, 12, 3, 40, false);
    const ULONG handle = sg + M68K_SCR_BITMAP, borrowed = pg + M68K_SCR_BITMAP;
    const BitMap native = mine.bm;
    assert(init_refused(f, handle) && init_refused(f, borrowed));
    assert(init_refused(f, handle, 0, 0, 0) && init_refused(f, borrowed, 8, 0xFFFF, 0xFFFF));
    assert(std::memcmp(&mine.bm, &native, sizeof(native)) == 0 && init_bitmaps == 0);
    /* the handle's bytes describe nothing: a copy is the program's own */
    const ULONG copy = Kaffeine_HeapAlloc(&f.ctx, M68K_SIZEOF_BITMAP, MEMF_CLEAR);
    assert(copy);
    std::copy(f.memory.begin() + handle, f.memory.begin() + handle + M68K_SIZEOF_BITMAP,
              f.memory.begin() + copy);
    assert(init_writes(f, copy, 3, 48, 12, 6, 12, 3));

    /* a screen that the table still lists as the program's, but whose
       shadow has gone, is no ordinary BitMap either */
    int slot = 0;
    while (f.ctx.pubscreens[slot].guest != sg)
        ++slot;
    Kaffeine_ShadowForget(&f.ctx, sg, KAFFEINE_SHADOW_SCREEN);
    assert(init_refused(f, handle));
    /* stale: the table names it no longer. Its 68k Screen is kept as guest
       memory here, so that the write lands in storage of the program. */
    f.ctx.layer_ops->screen_closing(&f.ctx, &mine.screen);
    f.ctx.pubscreens[slot] = {};
    mine.open = false;
    screen_bitmaps.erase(std::find(screen_bitmaps.begin(), screen_bitmaps.end(), &mine));
    assert(init_writes(f, handle, 2, 48, 12, 6, 12, 2));
    std::memset(f.memory.data() + sg, 0, M68K_SIZEOF_SCREEN);
    Kaffeine_MirrorRelease(&f.ctx, sg);
    /* the borrowed one stays refused until it goes */
    assert(init_refused(f, borrowed));
    f.close_screen(theirs, pg);
}

/* A common BitMap is the LayerInfo's from its first native creation that
   leaves a root or a list until DisposeLayerInfo() has returned: its 68k
   BitMap is refused all that time, also with only the internal root left
   or after a partial creation, and is the program's ordinary BitMap
   before and after, and without the bridge. Other ordinary BitMaps are
   initialized throughout. */
static void test_init_bitmap_common_lifetime()
{
    Fixture f;
    std::vector<ULONG> planes;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1, &planes), other = f.bitmap(4, 8, 1);

    /* a LayerInfo without a common BitMap yet holds nothing */
    assert(init_writes(f, bm, 1, 32, 8, 4, 8, 1));
    const ULONG l = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE);
    assert(l && f.ctx.running);
    BitMap *const common = creates.back().bm;
    const BitMap kept = *common;
    assert(init_refused(f, bm) && init_refused(f, bm, 1, 32, 8));
    assert(init_writes(f, other, 1, 16, 4, 2, 4, 1));
    /* the common BitMap is still the one graphics gets */
    assert(f.call(Fixture::graphics_base, 30, { bm, bm, 0 }, { 0, 0, 8, 0, 8, 8, 0xC0, 0x01 }) == 1);
    assert(f.ctx.running && blits.back().src == common && blits.back().src_origin == ORIGIN_COMMON);

    /* only the internal root is left, which still reads it */
    assert(f.destroy(l) == TRUE && f.ctx.running && native_layers.empty());
    assert(native_infos[0]->check_lp && native_infos[0]->top_layer == native_infos[0]->check_lp);
    assert(f.L(li + M68K_LI_TOP_LAYER) == 0);
    assert(init_refused(f, bm) && std::memcmp(common, &kept, sizeof(kept)) == 0);
    root_read(native_infos[0]->check_lp);

    /* disposed of: the 68k BitMap is the program's again */
    assert(f.call(Fixture::layers_base, 150, { li }) == 0 && f.ctx.running && disposed_infos == 1);
    assert(init_writes(f, bm, 1, 64, 4, 8, 4, 1));
    assert(f.L(bm + M68K_BM_PLANES) == planes[0]);

    /* a first creation that fails after making the root keeps the BitMap;
       one that leaves neither root nor list fixes none */
    const ULONG li2 = f.new_info(), bm2 = f.bitmap(4, 8, 1);
    const ULONG li3 = f.new_info(), bm3 = f.bitmap(4, 8, 1);
    create_mode = CREATE_FAILS_EARLY;
    assert(f.create(li2, bm2, 0, 0, 31, 7, LAYERSIMPLE) == 0 && f.ctx.running);
    create_mode = CREATE_ROOT_FAILS;
    assert(f.create(li3, bm3, 0, 0, 31, 7, LAYERSIMPLE) == 0 && f.ctx.running);
    create_mode = CREATE_OK;
    assert(native_infos[0]->check_lp && !native_infos[1]->check_lp);
    assert(init_refused(f, bm2));
    assert(init_writes(f, bm3, 1, 32, 8, 4, 8, 1));

    /* without the bridge, after every LayerInfo went with its root */
    Kaffeine_LayersCleanup(&f.ctx);
    assert(!f.ctx.layers && native_infos.empty() && roots.empty());
    assert(init_writes(f, bm2, 2, 33, 9, 6, 9, 2));
}

/* A 68k backfill hook that runs while CreateUpfrontHookLayer() makes the
   first layer of a LayerInfo, so after the bridge bound the layer and its
   common BitMap but before the constructor returned, cannot initialize
   that BitMap: the nested call stops the program with the descriptor
   unchanged, no native InitBitMap() beyond the bridge's own, and the
   layer goes again; the root keeps the BitMap. Another ordinary BitMap is
   initialized from the same hook. */
static void test_init_bitmap_from_constructor_hook()
{
    Fixture f;
    const ULONG li = f.new_info(), bm = f.bitmap(4, 8, 1), h = f.hook();
    const ULONG li2 = f.new_info(), bm2 = f.bitmap(4, 8, 1), plain = f.bitmap(2, 4, 1);
    const std::vector<UBYTE> desc(f.memory.begin() + bm, f.memory.begin() + bm + M68K_SIZEOF_BITMAP);

    action = GUEST_LVO;
    layers_lib_base = Fixture::graphics_base;
    guest_lvo = 390;
    guest_lvo_a = { bm };
    guest_lvo_d = { 2, 64, 4 };
    assert(f.call_on(f.cpu, Fixture::layers_base, 186, { li, bm, 0, h }, { 0, 0, 31, 7, LAYERSIMPLE }, false) == 20);
    assert(!f.ctx.running && f.ctx.exit_code == 20 && f.ctx.callback_depth == 0);
    assert(hook_calls.size() == 1 && hook_calls[0].nested == 20 && hook_calls[0].a0 == h);
    assert(hook_calls[0].rp_bitmap == bm && hook_calls[0].info_top == hook_calls[0].msg_layer);
    assert(std::equal(desc.begin(), desc.end(), f.memory.begin() + bm));
    assert(creates.size() == 1 && init_bitmaps == 1 && deletes == 1 && native_layers.empty());
    assert(native_infos[0]->check_lp && f.L(li + M68K_LI_TOP_LAYER) == 0);
    action = GUEST_DRAW;
    layers_lib_base = Fixture::layers_base;
    assert(init_refused(f, bm));

    /* callbacks are not closed to ordinary BitMaps */
    const std::vector<UBYTE> desc2(f.memory.begin() + bm2, f.memory.begin() + bm2 + M68K_SIZEOF_BITMAP);
    const ULONG plane = f.L(plain + M68K_BM_PLANES);
    action = GUEST_LVO;
    layers_lib_base = Fixture::graphics_base;
    guest_lvo_a = { plain };
    guest_lvo_d = { 0xFF01u, 0xABCD0010u, 0x00000006u };
    const ULONG l = f.create(li2, bm2, 0, 0, 31, 7, LAYERSIMPLE, h);
    action = GUEST_DRAW;
    layers_lib_base = Fixture::layers_base;
    assert(l && f.ctx.running && hook_calls.size() == 2 && hook_calls[1].nested == 0);
    const std::vector<UBYTE> head = classic_head(2, 6, 1);
    assert(std::equal(head.begin(), head.end(), f.memory.begin() + plain));
    assert(f.L(plain + M68K_BM_PLANES) == plane);
    assert(std::equal(desc2.begin(), desc2.end(), f.memory.begin() + bm2));
    assert(creates.size() == 2 && init_bitmaps == 2);
}

static void test_layerinfo_backfill_mode()
{
    Fixture f;
    const ULONG li = f.new_info(); assert(li && native_infos.size() == 1);
    Layer_Info *n = native_infos[0];
    auto install = [&](ULONG pointer, ULONG hook) {
        return f.call(Fixture::layers_base, 204, {pointer, hook});
    };
    assert(install(li, 1) == 0 && n->BlankHook == LAYERS_NOBACKFILL);
    assert(m68k_read32(&f.ctx, li + 94) == 1);
    assert(install(li, 1) == 1 && install(li, 0) == 1 && n->BlankHook == LAYERS_BACKFILL);
    const ULONG bm = f.bitmap(4, 8, 1), layer = f.create(li, bm, 0, 0, 31, 7, LAYERSIMPLE, 1);
    assert(layer && n->check_lp);
    assert(install(li, 1) == 0 && n->check_lp->BackFill == LAYERS_NOBACKFILL);
    assert(install(li, 0) == 1 && n->check_lp->BackFill == LAYERS_BACKFILL);
    // A changed guest snapshot cannot replace the native old identity.
    m68k_write32(&f.ctx, li + 94, 0xDEADBEEF);
    assert(install(li, 1) == 0 && m68k_read32(&f.ctx, li + 94) == 1);
    for (auto bad : {std::pair<ULONG, ULONG>{0, 0}, {li + 2, 1}, {0xFFFFFFF0, 0}, {li, f.hook()}})
    {
        const auto before = f.memory; const auto count = events.size();
        assert(install(bad.first, bad.second) == 20 && f.stopped());
        assert(before == f.memory && events.size() == count && n->BlankHook == LAYERS_NOBACKFILL);
    }
    assert(f.call(Fixture::layers_base, 96, {0, layer}) == 0);
    auto count = events.size(); auto before = f.memory;
    assert(install(li, 0) == 20 && f.stopped() && count == events.size() && before == f.memory);
    assert(f.call(Fixture::layers_base, 102, {layer}) == 0);
    f.ctx.bitmap_blits = 1; count = events.size(); before = f.memory;
    assert(install(li, 0) == 20 && f.stopped() && count == events.size() && before == f.memory);
    f.ctx.bitmap_blits = 0;
    Hook foreign{}; n->BlankHook = &foreign; count = events.size(); before = f.memory;
    assert(install(li, 0) == 20 && f.stopped() && count == events.size() && before == f.memory);
    n->BlankHook = LAYERS_NOBACKFILL;
    assert(f.destroy(layer) == 1);
    assert(install(li, 0) == 1 && n->check_lp->BackFill == LAYERS_BACKFILL);
    assert(f.call(Fixture::layers_base, 150, {li}) == 0);
    count = events.size(); before = f.memory;
    assert(install(li, 1) == 20 && f.stopped() && count == events.size() && before == f.memory);

    NativeScreenBitMap owned{}, borrowed{};
    ULONG sg = f.open_screen(owned, 4, 8, 1, 32, true);
    ULONG bg = f.open_screen(borrowed, 4, 8, 1, 32, false);
    const ULONG sli = sg + M68K_SCR_LAYERINFO, bli = bg + M68K_SCR_LAYERINFO;
    assert(install(sli, 1) == 0 && owned.screen.LayerInfo.BlankHook == LAYERS_NOBACKFILL);
    assert(f.L(sli + M68K_LI_BLANKHOOK) == 1 && install(sli, 0) == 1);
    count = events.size(); before = f.memory;
    assert(install(bli, 1) == 20 && f.stopped() && count == events.size() && before == f.memory);
    assert(install(sli + 2, 1) == 20 && f.stopped() && count == events.size());
    f.close_screen(owned, sg);
    count = events.size(); before = f.memory;
    assert(install(sli, 1) == 20 && f.stopped() && count == events.size() && before == f.memory);
    f.close_screen(borrowed, bg);
}

/* layer_ops->draw_target for a RastPort that the program built: without a
   Layer, exactly the planar BitMaps that BltBitMap() takes, converted the
   same way into the caller's adapter over the very guest planes, with no
   blit, layer call or allocation; with the 68k Layer of a live private
   layer, its native layer and the common BitMap, only for the 68k BitMap
   that its RastPort shows. Common and screen BitMaps without a Layer,
   malformed ones, a deleted layer and one that another guest task holds
   stop the program. */
static void test_draw_targets()
{
    Fixture f;
    NativeScreenBitMap screen;
    std::vector<ULONG> planes;
    const ULONG bm = f.bitmap(4, 16, 2, &planes), sg = f.open_screen(screen, 4, 16, 2, 32);
    const auto *ops = f.ctx.layer_ops;
    BitMap adapter;
    RastPort rp;
    auto target = [&](ULONG layer, ULONG bitmap) {
        std::memset(static_cast<void *>(&rp), 0, sizeof(rp));
        std::memset(static_cast<void *>(&adapter), 0x5A, sizeof(adapter));
        f.ctx.running = TRUE;
        f.ctx.exit_code = 0;
        call_inits.clear();
        const BOOL ok = ops->draw_target(&f.ctx, layer, bitmap, &rp, &adapter);
        assert(ok ? f.ctx.running && !f.ctx.exit_code : !f.ctx.running && f.ctx.exit_code == 20);
        if (!ok)
            assert(!rp.Layer && !rp.BitMap);
        return ok;
    };
    assert(ops->draw_target);
    const ULONG avail = f.avail();
    const auto heap = f.heap();

    /* an ordinary planar BitMap: graphics' own over the guest planes */
    assert(target(0, bm) && !rp.Layer && rp.BitMap == &adapter);
    /* graphics' InitBitMap() made it, never from guest bytes */
    assert(call_inits.size() == 1 && reinterpret_cast<const UBYTE *>(call_inits[0]) != f.ctx.mem + bm);
    assert(adapter.BytesPerRow == 4 && adapter.Rows == 16 && adapter.Depth == 2 &&
           adapter.Flags == BMF_STANDARD && adapter.pad == 0);
    assert(adapter.Planes[0] == f.ctx.mem + planes[0] && adapter.Planes[1] == f.ctx.mem + planes[1]);
    for (int i = 2; i < 8; ++i)
        assert(!adapter.Planes[i]);
    assert(blits.empty() && wait_blits == 0 && creates.empty() && f.heap_is(heap) && f.avail() == avail);

    /* what BltBitMap() refuses, and a screen's BitMap */
    const std::function<void()> breaks[] = {
        [&] { f.memory[bm + M68K_BM_DEPTH] = 0; },
        [&] { f.memory[bm + M68K_BM_DEPTH] = 9; },
        [&] { m68k_write16(&f.ctx, bm + M68K_BM_BYTESPERROW, 3); },
        [&] { m68k_write16(&f.ctx, bm + M68K_BM_ROWS, 0); },
        [&] { f.memory[bm + M68K_BM_FLAGS] = 0x80; },
        [&] { m68k_write16(&f.ctx, bm + M68K_BM_PAD, 1); },
        [&] { m68k_write32(&f.ctx, bm + M68K_BM_PLANES + 4, planes[1] + 1); },
        [&] { m68k_write32(&f.ctx, bm + M68K_BM_PLANES + 4, 0); },
        [&] { m68k_write32(&f.ctx, bm + M68K_BM_PLANES, Fixture::stack - 8); },
    };
    for (const auto &breakage : breaks)
    {
        const std::vector<UBYTE> saved(f.memory.begin() + bm, f.memory.begin() + bm + M68K_SIZEOF_BITMAP);
        breakage();
        assert(!target(0, bm));
        std::copy(saved.begin(), saved.end(), f.memory.begin() + bm);
    }
    for (ULONG bad : { 0U, bm + 1, sg + M68K_SCR_BITMAP, (ULONG)f.memory.size() - 38 })
        assert(!target(0, bad));
    assert(blits.empty() && creates.empty() && f.heap_is(heap));

    /* a private layer: its native layer and the common BitMap */
    const ULONG li = f.new_info(), layer = f.create(li, bm, 2, 3, 20, 10, LAYERSIMPLE);
    assert(layer && creates.size() == 1);
    Layer *native = native_layer(f, layer);
    assert(target(layer, bm) && rp.Layer == native && rp.BitMap == creates[0].bm);
    assert(rp.BitMap == native->rp->BitMap && !call_inits.size());
    /* A copied RastPort must not silently use the cached old planes or
       geometry after its published descriptor changes. */
    m68k_write16(&f.ctx, bm + M68K_BM_ROWS, 15);
    assert(!target(layer, bm));
    m68k_write16(&f.ctx, bm + M68K_BM_ROWS, 16);
    m68k_write32(&f.ctx, bm + M68K_BM_PLANES, planes[1]);
    assert(!target(layer, bm));
    m68k_write32(&f.ctx, bm + M68K_BM_PLANES, planes[0]);
    assert(target(layer, bm));
    for (ULONG bad : { 0U, bm + 2, sg + M68K_SCR_BITMAP, f.L(layer + M68K_LAYER_RP) })
        assert(!target(layer, bad));
    /* the common BitMap without the Layer would draw past the layers */
    assert(!target(0, bm));
    /* the 68k Layer's RastPort, the LayerInfo or raw bytes are no Layer */
    for (ULONG bad : { f.L(layer + M68K_LAYER_RP), li, layer + 2, 0x5000U })
        assert(!target(bad, bm));
    /* held by another guest task: refused; by this one: taken */
    const ULONG task_addr = Fixture::exec_base + M68K_EXECBASE_THISTASK;
    assert(f.call(Fixture::layers_base, 96, { 0, layer }) == 0 && held_locks == 1);
    assert(target(layer, bm));
    m68k_write32(&f.ctx, task_addr, Fixture::task + 0x100);
    assert(!target(layer, bm));
    m68k_write32(&f.ctx, task_addr, Fixture::task);
    assert(f.call(Fixture::layers_base, 102, { layer }) == 0 && held_locks == 0);
    const size_t layer_events = events.size();
    assert(target(layer, bm) && events.size() == layer_events);
    /* deleted: a copy of its RastPort draws nothing */
    assert(f.destroy(layer) == TRUE);
    assert(!target(layer, bm));
    assert(blits.empty() && wait_blits == 0);
    f.close_screen(screen, sg);
}

extern "C" BOOL Request(struct Requester *,struct Window *) { assert(false); return FALSE; }
extern "C" void EndRequest(struct Requester *,struct Window *) { assert(false); }
extern "C" BOOL SetDMRequest(struct Window *,struct Requester *) { assert(false); return FALSE; }

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "LayerInfo backfill sentinel identity, hidden root update and ownership/lifetime guards", test_layerinfo_backfill_mode },
        { "LVO 186 and 90 are bridged; the other constructors stay refused", test_registration },
        { "Native planar BitMap over the guest planes; stable identities in native order; deletion", test_conversion_and_identity },
        { "A 68k backfill hook during the constructor sees bound guest objects; round trips; locks", test_hook_during_constructor },
        { "A ClipRect BitMap of layers is never shown as the 68k BitMap", test_cliprect_bitmap },
        { "Hooks run on the current guest CPU, only on the guest's native task", test_current_cpu_and_task },
        { "Malformed infos, BitMaps, flags, bounds and hooks stop before anything native", test_malformed },
        { "Missing memory, room and native layers leave nothing behind", test_memory_and_native_failures },
        { "Callback faults and recursion from hooks stop without running 68k code again", test_callback_failures },
        { "A layer's backfill from inside its own hook stops the program", test_nested_backfill },
        { "DeleteLayer() takes current private layers only; native refusal keeps the layer", test_ownership_and_deletion },
        { "One common BitMap per LayerInfo outlives every layer, for the root, until disposal", test_common_bitmap_and_root },
        { "Partial creations that leave a root keep its BitMap; one that leaves none lets it go", test_partial_creation_root },
        { "The root over the whole BitMap keeps WORD bounds; one row more is refused", test_root_geometry },
        { "Clearing private clipping is typed; foreign Regions and layers are refused", test_clear_clip_region },
        { "Private locks do not freeze screen snapshots; failed constructors retire their counts", test_private_lock_isolation },
        { "Another guest task's lock of any layer of a LayerInfo stops creation and deletion there", test_sibling_lock_contention },
        { "BltBitMap() empty clipped rectangles leave bytes and resources unchanged", test_blit_empty_clip },
        { "BltBitMap() (-30) between common BitMaps: native identity, WORD/UBYTE/LONG mapping, copy, clear, mask", test_blit_mapping },
        { "BltBitMap() takes ordinary planar BitMaps; refuses malformed, changed common, TempA and negative inputs before native; clips oversized rectangles", test_blit_refusals },
        { "A blit keeps every LayerInfo until WaitBlit(); a disposed common's 68k BitMap gets a new call-local BitMap", test_blit_lifetime },
        { "BltBitMap() from an ordinary planar BitMap to an opened screen's BitMap and back, by identity", test_blit_screen_transfer },
        { "BltBitMap() refuses borrowed, foreign, stale and NULL screen BitMaps and clips to native bitmap dimensions", test_blit_screen_refusals },
        { "GetBitMapAttr() uses typed bitmap identity and preserves classic scalar flags", test_bitmap_attributes },
        { "Unconverted graphics and cybergraphics bitmap vectors stop before native code and guest writes", test_bitmap_vector_guards },
        { "InitBitMap() (-390) writes graphics' BE scalars from narrowed registers, never planes or other bytes", test_init_bitmap_scalars },
        { "InitBitMap() refuses bad addresses, no memory, owned and borrowed screen BitMaps before any write", test_init_bitmap_refusals },
        { "InitBitMap() refuses a common BitMap while its LayerInfo or root keeps it, and only then", test_init_bitmap_common_lifetime },
        { "InitBitMap() from a constructor's backfill hook cannot change the bound common BitMap", test_init_bitmap_from_constructor_hook },
        { "Teardown deletes private layers before their LayerInfos, once, without 68k code", test_cleanup },
        { "Drawing targets: planar BitMaps as BltBitMap() takes them, live private layers by their BitMap", test_draw_targets }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Private layers of 68k programs: %zu groups passed\n", std::size(tests));
    return 0;
}

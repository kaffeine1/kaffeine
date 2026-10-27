/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Regions of 68k code. NewRegion() makes a real native Region with
    graphics' own NewRegion() and gives the program a classic 68k Region
    of 12 bytes for it: Bounds and a RegionRectangle pointer, big-endian.
    The pair lives in a table of its own, at most KAFFEINE_REGION_MAX at
    once, and never in the generic shadow map, so that no generated thunk
    can read a 68k Region as a native one or a native one as guest memory;
    every other Region call of graphics.library is refused before anything
    native (kaffeine_regions.h). The 68k Region stays the same guest object
    until DisposeRegion(); it is bridge memory that the program cannot free.

    What the program reads is a snapshot of the native Region, taken after
    each change: Bounds exactly as the native ones, and a list of classic
    RegionRectangles of 16 bytes (Next, Prev, Bounds relative to the
    Region's MinX and MinY, as graphics keeps them) in one guest block of
    the bridge, at most KAFFEINE_REGION_MAX_RECTS, linked with guest
    addresses only. An empty Region has a NULL list. Nothing of the
    snapshot is ever read back: the native Region is the truth.

    OrRectRegion() takes such a Region and a word aligned classic Rectangle
    of guest memory. A Rectangle with MinX > MaxX or MinY > MaxY adds
    nothing and gives TRUE, as in graphics. Any other change is made on a
    native copy: a new native Region, OrRegionRegion() of the current one
    into it, then OrRectRegion() of the Rectangle, then a new snapshot list
    in guest memory; only when all of that worked does the copy become the
    Region, the old native Region and list go, and TRUE comes back. When
    native memory, guest memory or the snapshot bound runs out, the copy
    goes, the Region stays as it was, native and guest, and the call gives
    FALSE, as graphics does without memory.

    The layer bridge leases a Region for each private layer whose clip
    Region it is (Kaffeine_RegionLease()); a leased Region cannot be
    changed or disposed of. Neither can any Region while native layers
    code runs on behalf of the program, such as from a 68k backfill hook:
    those calls stop the program before anything native. Anything that is
    no live Region of NewRegion() of the program stops it too.
*/

#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_regions.h"

#include <aros/debug.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/regions.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>

/* A Region of the program; a free slot has no guest. list holds count
   classic RegionRectangles, or is 0 for an empty Region. */
struct RegionRecord
{
    ULONG guest;
    ULONG list;
    struct Region *native;
    UWORD count;
    UWORD leases;              /* private layers whose clip Region it is */
};

struct KaffeineRegions
{
    struct RegionRecord records[KAFFEINE_REGION_MAX];
};

/* Stop the program with return code 20, which the thunk returns to D0. */
static IPTR regions_refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* The record of a live 68k Region of the program, or NULL */
static struct RegionRecord *regions_find(struct KaffeineContext *ctx, ULONG guest)
{
    struct KaffeineRegions *st = ctx->regions;
    int i;

    for (i = 0; st && guest && i < KAFFEINE_REGION_MAX; i++)
        if (st->records[i].guest == guest)
            return &st->records[i];
    return NULL;
}

/* The number of rectangles of native Region n, or FALSE when it has more
   than a snapshot takes, which a cycle has too */
static BOOL regions_count(const struct Region *n, UWORD *count)
{
    const struct RegionRectangle *rr;
    UWORD k = 0;

    for (rr = n->RegionRectangle; rr; rr = rr->Next)
        if (++k > KAFFEINE_REGION_MAX_RECTS)
            return FALSE;
    *count = k;
    return TRUE;
}

static void regions_list_free(struct KaffeineContext *ctx, ULONG list, UWORD count)
{
    if (!list)
        return;
    if (ctx->mem)
        memset(ctx->mem + list, 0, (ULONG)count * M68K_SIZEOF_RR);
    Kaffeine_MirrorRelease(ctx, list);
}

static void regions_write_rect(struct KaffeineContext *ctx, ULONG a, const struct Rectangle *r)
{
    m68k_write16(ctx, a + 0, (UWORD)r->MinX);
    m68k_write16(ctx, a + 2, (UWORD)r->MinY);
    m68k_write16(ctx, a + 4, (UWORD)r->MaxX);
    m68k_write16(ctx, a + 6, (UWORD)r->MaxY);
}

/* The snapshot of native Region n in a new guest list for count
   rectangles, which regions_count() gave: *list, 0 when n is empty, or
   FALSE without guest memory, and then nothing was written */
static BOOL regions_list_make(struct KaffeineContext *ctx, const struct Region *n, UWORD count,
                              ULONG *list)
{
    const struct RegionRectangle *rr = n->RegionRectangle;
    ULONG base = 0, a;
    UWORD i;

    if (count && !(base = Kaffeine_MirrorAlloc(ctx, (ULONG)count * M68K_SIZEOF_RR)))
        return FALSE;
    for (i = 0; i < count; i++, rr = rr->Next)
    {
        a = base + (ULONG)i * M68K_SIZEOF_RR;
        m68k_write32(ctx, a + M68K_RR_NEXT, i + 1 < count ? a + M68K_SIZEOF_RR : 0);
        m68k_write32(ctx, a + M68K_RR_PREV, i ? a - M68K_SIZEOF_RR : 0);
        regions_write_rect(ctx, a + M68K_RR_BOUNDS, &rr->bounds);
    }
    *list = base;
    return TRUE;
}

/* The 68k Region of record r shows native Region n with snapshot list */
static void regions_publish(struct KaffeineContext *ctx, struct RegionRecord *r,
                            struct Region *n, ULONG list, UWORD count)
{
    regions_write_rect(ctx, r->guest + M68K_REGION_BOUNDS, &n->bounds);
    m68k_write32(ctx, r->guest + M68K_REGION_RECTANGLE, list);
    r->native = n;
    r->list = list;
    r->count = count;
}

/* Record r goes: its 68k Region and list, cleared, and its native Region */
static void regions_free(struct KaffeineContext *ctx, struct RegionRecord *r)
{
    regions_list_free(ctx, r->list, r->count);
    if (ctx->mem && r->guest)
        memset(ctx->mem + r->guest, 0, M68K_SIZEOF_REGION);
    Kaffeine_MirrorRelease(ctx, r->guest);
    if (r->native)
        DisposeRegion(r->native);
    memset(r, 0, sizeof(*r));
}

/* The record of a Region that the program may change or dispose of now,
   or NULL after stopping the program */
static struct RegionRecord *regions_mutable(struct KaffeineContext *ctx, ULONG guest)
{
    struct RegionRecord *r = regions_find(ctx, guest);

    if (!r)
        regions_refuse(ctx, "not a Region of NewRegion() of the program");
    else if (Kaffeine_LayersNativeBusy(ctx))
        regions_refuse(ctx, "a Region change while native layers code runs");
    else if (r->leases)
        regions_refuse(ctx, "a change of the clip Region of a layer");
    else
        return r;
    return NULL;
}

/* graphics -516: NewRegion() -> D0=region. A native Region of the program
   and its empty 68k Region, or 0 without room, guest memory or native
   memory, and then nothing of it stays. */
IPTR Kaffeine_NewRegion(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineRegions *st = ctx->regions;
    struct RegionRecord *r = NULL;
    struct Region *native;
    ULONG guest;
    int i;

    (void)cpu;
    if (!st)
    {
        if (!(st = AllocVec(sizeof(*st), MEMF_ANY | MEMF_CLEAR)))
            return 0;
        ctx->regions = st;
    }
    for (i = 0; i < KAFFEINE_REGION_MAX && !r; i++)
        if (!st->records[i].guest)
            r = &st->records[i];
    if (!r)
    {
        bug("[kaffeine] NewRegion(): no room for another Region\n");
        return 0;
    }
    if (!(guest = Kaffeine_MirrorAlloc(ctx, M68K_SIZEOF_REGION)))
        return 0;
    if (!(native = NewRegion()))
    {
        Kaffeine_MirrorRelease(ctx, guest);
        return 0;
    }
    if (native->RegionRectangle)
    {
        bug("[kaffeine] NewRegion(): graphics made a Region that is not empty\n");
        DisposeRegion(native);
        Kaffeine_MirrorRelease(ctx, guest);
        return 0;
    }
    r->guest = guest;
    r->leases = 0;
    regions_publish(ctx, r, native, 0, 0);
    return guest;
}

/* graphics -510: OrRectRegion(A0=region, A1=rectangle) -> D0=success. A
   Region of NewRegion() that no layer uses, and a word aligned Rectangle
   in guest memory; see above. */
IPTR Kaffeine_OrRectRegion(struct KaffeineContext *ctx, void *cpu)
{
    ULONG rect = THUNK_A(1), list;
    struct RegionRecord *r = regions_mutable(ctx, THUNK_A(0));
    struct Region *copy, *old;
    struct Rectangle native;
    UWORD count;

    if (!r)
        return ctx->exit_code;
    if (!rect || (rect & 1) || !ctx->mem || rect >= ctx->mem_size ||
        ctx->mem_size - rect < M68K_SIZEOF_RECTANGLE)
        return regions_refuse(ctx, "OrRectRegion(): not a Rectangle in guest memory");
    native.MinX = (WORD)m68k_read16(ctx, rect + 0);
    native.MinY = (WORD)m68k_read16(ctx, rect + 2);
    native.MaxX = (WORD)m68k_read16(ctx, rect + 4);
    native.MaxY = (WORD)m68k_read16(ctx, rect + 6);
    if (native.MinX > native.MaxX || native.MinY > native.MaxY)
        return TRUE;

    if (!(copy = NewRegion()))
        return FALSE;
    if (copy->RegionRectangle || !OrRegionRegion(r->native, copy) ||
        !OrRectRegion(copy, &native))
    {
        DisposeRegion(copy);
        return FALSE;
    }
    if (!regions_count(copy, &count))
    {
        bug("[kaffeine] OrRectRegion(): more rectangles than a Region snapshot takes\n");
        DisposeRegion(copy);
        return FALSE;
    }
    if (!regions_list_make(ctx, copy, count, &list))
    {
        DisposeRegion(copy);
        return FALSE;
    }
    old = r->native;
    regions_list_free(ctx, r->list, r->count);
    regions_publish(ctx, r, copy, list, count);
    DisposeRegion(old);
    return TRUE;
}

/* graphics -534: DisposeRegion(A0=region). Only a Region of NewRegion()
   that is still the program's and no layer uses; NULL, one already
   disposed of and any other pointer stop the program before anything
   native. */
IPTR Kaffeine_DisposeRegion(struct KaffeineContext *ctx, void *cpu)
{
    struct RegionRecord *r = regions_mutable(ctx, THUNK_A(0));

    if (!r)
        return ctx->exit_code;
    regions_free(ctx, r);
    return 0;
}

/* Every other Region call */
IPTR kaffeine_regions_Unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return regions_refuse(ctx, "unsupported Region call");
}

/* ── For the layer bridge ────────────────────────────────────────── */

struct Region *Kaffeine_RegionLease(struct KaffeineContext *ctx, ULONG guest)
{
    struct RegionRecord *r = regions_find(ctx, guest);

    if (!r)
    {
        regions_refuse(ctx, "InstallClipRegion(): not a Region of NewRegion() of the program");
        return NULL;
    }
    if (r->leases == 0xFFFF)
    {
        regions_refuse(ctx, "InstallClipRegion(): too many layers with one clip Region");
        return NULL;
    }
    r->leases++;
    return r->native;
}

void Kaffeine_RegionUnlease(struct KaffeineContext *ctx, ULONG guest)
{
    struct RegionRecord *r = regions_find(ctx, guest);

    if (r && r->leases)
        r->leases--;
}

void Kaffeine_RegionsCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineRegions *st = ctx->regions;
    int i;

    if (!st)
        return;
    for (i = 0; i < KAFFEINE_REGION_MAX; i++)
    {
        struct RegionRecord *r = &st->records[i];

        if (!r->guest)
            continue;
        if (r->leases)
        {
            /* a layer that layers kept still clips with it */
            bug("[kaffeine] a Region of the program is still a clip Region, not disposed of\n");
            r->native = NULL;
        }
        regions_free(ctx, r);
    }
    ctx->regions = NULL;
    FreeVec(st);
}

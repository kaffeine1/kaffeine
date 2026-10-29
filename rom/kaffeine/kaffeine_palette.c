/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Palette access through the ColorMap borrowed from a live Screen.
    Identity comes only from the native screen registry. Guest ColorMap
    snapshots cannot redirect a native call or transfer ownership.

    ObtainBestPenA() of such a ColorMap gets a native pen of the
    program: each success is a lease that the bridge counts by screen and
    pen, never in guest memory. While the program holds any lease of a
    screen, its 68k Screen and ColorMap snapshot stay, a screen that it did
    not open keeps one native visitor lock of the bridge, and CloseScreen()
    refuses a screen that it opened. ReleasePen() gives back one lease of
    exactly that pen; the last one retires the screen's leases. A pen that
    the program holds no lease of may be another program's, so its release
    changes nothing. The pens that the program leaves go back at its end,
    before any screen of it goes.
*/
#include <aros/debug.h>
#include <graphics/view.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#include "kaffeine_intern.h"
#include "kaffeine_palette.h"
#include "kaffeine_screens.h"
#include "kaffeine_taglist.h"

#define MAX_COLORS 256
/* The pens that a screen's leases count, 0 to MAX_PENS - 1 */
#define MAX_PENS KAFFEINE_MAX_SCREEN_PENS
/* The most steps, control tags included, of one ObtainBestPenA() list */
#define MAX_PEN_TAG_STEPS 1024
static IPTR refuse(struct KaffeineContext *ctx)
{
    bug("[kaffeine] palette: unsupported object or invalid range, stopping (20)\n");
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}
static BOOL bytes(struct KaffeineContext *ctx, ULONG a, ULONG n)
{
    return ctx->mem && a && !(a & 1) && a < ctx->mem_size && n <= ctx->mem_size - a;
}
/* The entry of the live screen whose 68k ColorMap that is, or -1 */
static int colormap_entry(struct KaffeineContext *ctx, ULONG a)
{
    int i;
    for (i = 0; a && i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].colormap == a && ctx->pubscreens[i].native &&
            ctx->pubscreens[i].native->ViewPort.ColorMap &&
            Kaffeine_ScreenNative(ctx, ctx->pubscreens[i].guest) == ctx->pubscreens[i].native)
            return i;
    return -1;
}
static struct ColorMap *colormap(struct KaffeineContext *ctx, ULONG a)
{
    int i = colormap_entry(ctx, a);
    return i < 0 ? NULL : ctx->pubscreens[i].native->ViewPort.ColorMap;
}
static struct ViewPort *viewport(struct KaffeineContext *ctx, ULONG a)
{
    struct ViewPort *vp = Kaffeine_ShadowNative(ctx, a, KAFFEINE_SHADOW_VIEWPORT);
    unsigned i;
    for (i = 0; vp && i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].native && vp == &ctx->pubscreens[i].native->ViewPort &&
            Kaffeine_ScreenNative(ctx, ctx->pubscreens[i].guest) == ctx->pubscreens[i].native)
            return vp;
    return NULL;
}
static BOOL pens(struct ColorMap *cm, ULONG first, ULONG n)
{
    return cm && first <= cm->Count && n <= MAX_COLORS && n <= cm->Count - first;
}
IPTR Kaffeine_LoadRGB4(struct KaffeineContext *ctx, void *cpu)
{
    struct ViewPort *vp = viewport(ctx, THUNK_A(0));
    ULONG a = THUNK_A(1), n = (UWORD)THUNK_D(0), i;
    UWORD table[MAX_COLORS];
    if (!vp) return refuse(ctx);
    if (!n) return 0;
    if (!pens(vp->ColorMap, 0, n) || !bytes(ctx, a, n * 2)) return refuse(ctx);
    for (i = 0; i < n; i++) table[i] = m68k_read16(ctx, a + i * 2);
    LoadRGB4(vp, table, n);
    return 0;
}
IPTR Kaffeine_LoadRGB32(struct KaffeineContext *ctx, void *cpu)
{
    struct ViewPort *vp = viewport(ctx, THUNK_A(0));
    ULONG a = THUNK_A(1), table[4 * MAX_COLORS + 1], used = 0, total = 0, h, n, i;
    if (!vp) return refuse(ctx);
    if (!a) return 0; /* Native LoadRGB32 accepts a NULL table. */
    /* Preflight the entire bounded stream before changing any color. */
    for (;;)
    {
        if (!bytes(ctx, a, 4) || used == 4 * MAX_COLORS + 1) return refuse(ctx);
        h = m68k_read32(ctx, a); n = h >> 16;
        table[used++] = h; a += 4;
        if (!n) break;
        if (n > MAX_COLORS - total || !pens(vp->ColorMap, h & 0xffff, n) ||
            !bytes(ctx, a, n * 12)) return refuse(ctx);
        total += n;
        for (i = 0; i < n * 3; i++) table[used++] = m68k_read32(ctx, a + i * 4);
        a += n * 12;
    }
    LoadRGB32(vp, table);
    return 0;
}
IPTR Kaffeine_GetRGB32(struct KaffeineContext *ctx, void *cpu)
{
    struct ColorMap *cm = colormap(ctx, THUNK_A(0));
    ULONG first = THUNK_D(0), n = THUNK_D(1), a = THUNK_A(1), table[3 * MAX_COLORS], i;
    if (!cm) return refuse(ctx);
    if (!n) return 0;
    if (!pens(cm, first, n) || !bytes(ctx, a, n * 12)) return refuse(ctx);
    GetRGB32(cm, first, n, table);
    for (i = 0; i < n * 3; i++) m68k_write32(ctx, a + i * 4, table[i]);
    return 0;
}
IPTR Kaffeine_GetRGB4(struct KaffeineContext *ctx, void *cpu)
{
    struct ColorMap *cm = colormap(ctx, THUNK_A(0));
    if (!pens(cm, THUNK_D(0), 1)) return refuse(ctx);
    return GetRGB4(cm, THUNK_D(0));
}
static IPTR set_rgb(struct KaffeineContext *ctx, void *cpu, BOOL map, BOOL wide)
{
    struct ViewPort *vp = map ? NULL : viewport(ctx, THUNK_A(0));
    struct ColorMap *cm = map ? colormap(ctx, THUNK_A(0)) : vp ? vp->ColorMap : NULL;
    ULONG n = wide ? THUNK_D(0) : (UWORD)THUNK_D(0);
    ULONG r = THUNK_D(1), g = THUNK_D(2), b = THUNK_D(3);
    if (!pens(cm, n, 1)) return refuse(ctx);
    if (map)
    {
        if (wide) SetRGB32CM(cm, n, r, g, b);
        else SetRGB4CM(cm, n, r, g, b);
    }
    else
    {
        if (wide) SetRGB32(vp, n, r, g, b);
        else SetRGB4(vp, n, r, g, b);
    }
    return 0;
}
IPTR Kaffeine_SetRGB4(struct KaffeineContext *c, void *cpu) { return set_rgb(c, cpu, FALSE, FALSE); }
IPTR Kaffeine_SetRGB32(struct KaffeineContext *c, void *cpu) { return set_rgb(c, cpu, FALSE, TRUE); }
IPTR Kaffeine_SetRGB4CM(struct KaffeineContext *c, void *cpu) { return set_rgb(c, cpu, TRUE, FALSE); }
IPTR Kaffeine_SetRGB32CM(struct KaffeineContext *c, void *cpu) { return set_rgb(c, cpu, TRUE, TRUE); }
/* The tags that ObtainBestPenA() reads, as GetTagData() finds them: the
   first OBP_Precision, a LONG, so that PRECISION_EXACT stays -1 at native
   width, and the first OBP_FailIfBad, a BOOL; AROS ignores every other
   user tag, so they stay out of the flat native list. FALSE for a list
   outside guest memory, with a cycle or longer than the bridge walks. */
static BOOL pen_tags(struct KaffeineContext *ctx, ULONG list, struct TagItem *tags)
{
    struct KaffeineTagWalk walk;
    BOOL precision = FALSE, fail = FALSE;
    ULONG item, tag, data;
    int n = 0, step;

    Kaffeine_TagWalkInit(&walk, list);
    while ((step = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM &&
           walk.steps <= MAX_PEN_TAG_STEPS)
    {
        tag = m68k_read32(ctx, item);
        data = m68k_read32(ctx, item + 4);
        if (tag == OBP_Precision && !precision)
        {
            tags[n].ti_Tag = tag;
            tags[n++].ti_Data = (IPTR)(LONG)data;
            precision = TRUE;
        }
        else if (tag == OBP_FailIfBad && !fail)
        {
            tags[n].ti_Tag = tag;
            tags[n++].ti_Data = data;
            fail = TRUE;
        }
    }
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0;
    return step == KAFFEINE_TAG_END && walk.steps <= MAX_PEN_TAG_STEPS;
}
/* The last lease of the screen of entry i has gone, its counts already 0:
   the entry, and with it the 68k Screen and ColorMap snapshot when nothing
   else keeps them, goes while the visitor lock of the bridge still holds,
   then that lock. */
static void pens_retire(struct KaffeineContext *ctx, int i)
{
    struct Screen *scr = ctx->pubscreens[i].native;
    BOOL locked = ctx->pubscreens[i].pen_lock;

    ctx->pubscreens[i].pen_lock = FALSE;
    if (ctx->screen_release)
        ctx->screen_release(ctx, scr);
    if (locked)
        UnlockPubScreen(NULL, scr);
}
/* graphics -840: ObtainBestPenA(A0=cm, D1=r, D2=g, D3=b, A1=tags) -> D0: the
   native pen, a lease of the program, or -1 as from graphics, also when
   the program holds 4096 leases, a screen that it did not open cannot be
   held by a visitor lock, or graphics gives a pen past the 256 that the
   bridge counts, which goes back at once. A ColorMap that is not one of a
   live 68k Screen of the program, NULL included, which graphics would
   read, and bad tags stop the program before anything native. */
IPTR Kaffeine_ObtainBestPenA(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(1), r = THUNK_D(1), g = THUNK_D(2), b = THUNK_D(3);
    int i = colormap_entry(ctx, THUNK_A(0));
    struct TagItem tags[3];
    struct ColorMap *cm;
    struct Screen *scr;
    BOOL hold;
    LONG pen;

    if (i < 0 || !pen_tags(ctx, list, tags)) return refuse(ctx);
    if (ctx->pen_leases >= KAFFEINE_MAX_PEN_LEASES)
    {
        bug("[kaffeine] ObtainBestPenA(): %u pen leases, no more\n", (unsigned)ctx->pen_leases);
        return (ULONG)-1;
    }
    scr = ctx->pubscreens[i].native;
    cm = scr->ViewPort.ColorMap;
    /* the first lease of a screen that the program did not open */
    hold = !ctx->pubscreens[i].pens && !ctx->pubscreens[i].owned;
    if (hold && !Kaffeine_ScreenPublicHold(scr))
    {
        bug("[kaffeine] ObtainBestPenA(): screen %p cannot be held, no pen\n", scr);
        return (ULONG)-1;
    }
    pen = ObtainBestPenA(cm, r, g, b, list ? tags : NULL);
    if (pen >= 0 && pen < MAX_PENS)
    {
        ctx->pubscreens[i].pen_refs[pen]++;
        ctx->pubscreens[i].pens++;
        if (hold) ctx->pubscreens[i].pen_lock = TRUE;
        ctx->pen_leases++;
        return (ULONG)pen;
    }
    if (pen >= 0)
    {
        bug("[kaffeine] ObtainBestPenA(): pen %ld past the table, given back\n", (long)pen);
        ReleasePen(cm, (ULONG)pen);
    }
    if (hold) UnlockPubScreen(NULL, scr);
    return (ULONG)-1;
}
/* graphics -948: ReleasePen(A0=cm, D0=n): one lease of the program of that
   pen goes back to graphics, and the screen's leases retire with the
   last. NULL changes nothing, as in graphics; so does a pen that the
   program holds no lease of, out of range or not, as it may be another
   program's, and also when the program released it already. A ColorMap
   that is not one of a live 68k Screen of the program stops it. */
IPTR Kaffeine_ReleasePen(struct KaffeineContext *ctx, void *cpu)
{
    ULONG a = THUNK_A(0), n = THUNK_D(0);
    int i;

    if (!a) return 0;
    if ((i = colormap_entry(ctx, a)) < 0) return refuse(ctx);
    if (n >= MAX_PENS || !ctx->pubscreens[i].pen_refs[n])
    {
        D(bug("[kaffeine] ReleasePen(%lu): no lease of the program, nothing released\n",
              (unsigned long)n));
        return 0;
    }
    ReleasePen(ctx->pubscreens[i].native->ViewPort.ColorMap, n);
    ctx->pubscreens[i].pen_refs[n]--;
    ctx->pen_leases--;
    if (!--ctx->pubscreens[i].pens)
        pens_retire(ctx, i);
    return 0;
}
void Kaffeine_PensCleanup(struct KaffeineContext *ctx)
{
    ULONG left = 0, n;
    int i;

    /* every pen first, while all screens are still held */
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        for (n = 0; ctx->pubscreens[i].pens && n < MAX_PENS; n++)
            for (; ctx->pubscreens[i].pen_refs[n]; ctx->pubscreens[i].pen_refs[n]--, left++)
                ReleasePen(ctx->pubscreens[i].native->ViewPort.ColorMap, n);
    for (i = 0; i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].pens)
        {
            ctx->pen_leases -= ctx->pubscreens[i].pens;
            ctx->pubscreens[i].pens = 0;
            pens_retire(ctx, i);
        }
    if (left)
        bug("[kaffeine] %lu pens that the program left, released\n", (unsigned long)left);
}
/* Owned ColorMaps and the other pen calls need their own lifetime bridge. */
IPTR Kaffeine_PaletteUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx);
}
IPTR Kaffeine_FreeColorMap(struct KaffeineContext *ctx, void *cpu)
{
    return THUNK_A(0) ? refuse(ctx) : 0; /* A borrowed screen ColorMap cannot be freed. */
}
IPTR Kaffeine_VideoControl(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx; (void)cpu;
    /* Native tags can contain monitor handles, viewports and output pointers.
       Report the API's error until those tags have typed conversions. */
    return 1;
}

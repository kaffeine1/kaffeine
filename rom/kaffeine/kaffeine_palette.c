/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Palette access through the ColorMap borrowed from a live Screen.
    Identity comes only from the native screen registry. Guest ColorMap
    snapshots cannot redirect a native call or transfer ownership.
*/
#include <aros/debug.h>
#include <graphics/view.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include "kaffeine_intern.h"
#include "kaffeine_palette.h"
#include "kaffeine_screens.h"

#define MAX_COLORS 256
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
static struct ColorMap *colormap(struct KaffeineContext *ctx, ULONG a)
{
    unsigned i;
    for (i = 0; a && i < KAFFEINE_MAX_PUBSCREENS; i++)
        if (ctx->pubscreens[i].colormap == a && ctx->pubscreens[i].native &&
            Kaffeine_ScreenNative(ctx, ctx->pubscreens[i].guest) == ctx->pubscreens[i].native)
            return ctx->pubscreens[i].native->ViewPort.ColorMap;
    return NULL;
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
/* Owned ColorMaps and pen allocations need their own lifetime bridge. */
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

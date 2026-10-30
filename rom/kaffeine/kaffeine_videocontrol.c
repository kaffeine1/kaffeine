/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <exec/nodes.h>
#include <graphics/view.h>
#include <graphics/videocontrol.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <utility/tagitem.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_messages.h"
#include "kaffeine_rastport.h"
#include "kaffeine_videocontrol.h"

struct KaffeineVideoExtra
{
    struct KaffeineVideoExtra *next;
    struct Screen *screen;
    struct ColorMap *colormap;
    struct ViewPortExtra *native;
    ULONG screen_guest, colormap_guest, guest;
    BOOL active;
    UBYTE snapshot[KAFFEINE_VPE_SIZE];
};

static BOOL video_protected(struct KaffeineContext *ctx, ULONG a, ULONG size)
{
    struct KaffeineVideoExtra *v;
    for (v = ctx->video_extras; v; v = v->next)
        if (Kaffeine_Overlap(a, size, v->guest, KAFFEINE_VPE_SIZE)) return TRUE;
    return FALSE;
}

static int video_screen(struct KaffeineContext *ctx, ULONG cm)
{
    unsigned i;
    for (i = 0; cm && i < KAFFEINE_MAX_PUBSCREENS; ++i)
    {
        struct Screen *s = ctx->pubscreens[i].native;
        ULONG g = ctx->pubscreens[i].guest;
        if (ctx->pubscreens[i].colormap == cm && s && s->ViewPort.ColorMap &&
            Kaffeine_ScreenNative(ctx, g) == s &&
            Kaffeine_ShadowNative(ctx, g + M68K_SCR_VIEWPORT, KAFFEINE_SHADOW_VIEWPORT) == &s->ViewPort &&
            cm < ctx->mem_size && ctx->mem_size - cm >= 52 && !(cm & 1))
            return (int)i;
    }
    return -1;
}

static struct KaffeineVideoExtra *video_current(struct KaffeineContext *ctx, ULONG cm,
                                                struct Screen *screen)
{
    struct KaffeineVideoExtra *v;
    for (v = ctx->video_extras; v; v = v->next)
        if (v->active && v->screen == screen && v->colormap_guest == cm) return v;
    return NULL;
}

/* Screen snapshot refreshes may clear the ColorMap. Recover only an
 * already-published identity whose native owner/association is unchanged;
 * never preserve a value read from guest cm_vpe or allocate during refresh. */
static ULONG video_extra_guest(struct KaffeineContext *ctx, struct Screen *screen, ULONG cm_guest)
{
    struct KaffeineVideoExtra *v = video_current(ctx, cm_guest, screen);
    if (!v || ctx->video_closing || video_screen(ctx, cm_guest) < 0 ||
        v->colormap != screen->ViewPort.ColorMap ||
        v->native != screen->ViewPort.ColorMap->cm_vpe ||
        v->native->ViewPort != &screen->ViewPort || v->native->n.xln_Type != NT_GRAPHICS ||
        v->native->n.xln_Subsystem != SS_GRAPHICS ||
        v->native->n.xln_Subtype != VIEWPORT_EXTRA_TYPE) return 0;
    return v->guest;
}

static BOOL video_tag_range(struct KaffeineContext *ctx, ULONG at)
{
    return !(at & 1) && Kaffeine_GuestBytes(ctx, at, 8) &&
           Kaffeine_TagItemsRange(ctx, at, 8) && Kaffeine_MessageMemory(ctx, at, 8);
}

/* Every control step is budgeted here, not inside an unbounded helper.
 * ti_Data of GET is ignored: native VideoControl overwrites that item. */
static BOOL video_tags(struct KaffeineContext *ctx, ULONG at, ULONG *outputs, ULONG *count)
{
    ULONG visited[KAFFEINE_VIDEO_TAG_STEPS], steps, i, tag, data;
    *count = 0;
    for (steps = 0; steps < KAFFEINE_VIDEO_TAG_STEPS; ++steps)
    {
        if (!video_tag_range(ctx, at)) return FALSE;
        for (i = 0; i < steps; ++i)
            if (Kaffeine_Overlap(at, 8, visited[i], 8)) return FALSE;
        visited[steps] = at;
        tag = m68k_read32(ctx, at);
        data = m68k_read32(ctx, at + 4);
        if (tag == TAG_DONE) return TRUE;
        if (tag == TAG_MORE)
        {
            if (!data) return TRUE;
            at = data;
        }
        else if (tag == TAG_SKIP)
        {
            if (data > (ctx->mem_size - at - 8) / 8) return FALSE;
            at += (data + 1) * 8;
        }
        else if (tag == TAG_IGNORE)
            at += 8;
        else if (tag == VTAG_VIEWPORTEXTRA_GET)
        {
            outputs[(*count)++] = at;
            at += 8;
        }
        else return FALSE; /* including every pointer-bearing SET */
    }
    return FALSE;
}

static void video_word(UBYTE *g, unsigned at, UWORD value)
{
    g[at] = (UBYTE)(value >> 8); g[at + 1] = (UBYTE)value;
}
static void video_long(UBYTE *g, unsigned at, ULONG value)
{
    video_word(g, at, (UWORD)(value >> 16)); video_word(g, at + 2, (UWORD)value);
}

/* Classic offsets are independently recorded in m68k_offsets.txt. Never
 * copy native links, library/init vectors, driver data or copper addresses. */
static void video_snapshot(UBYTE *g, const struct ViewPortExtra *v, ULONG viewport)
{
    memset(g, 0, KAFFEINE_VPE_SIZE);
    g[8] = NT_GRAPHICS; g[9] = (UBYTE)v->n.xln_Pri;
    g[14] = SS_GRAPHICS; g[15] = VIEWPORT_EXTRA_TYPE;
    video_long(g, 24, viewport);
    video_word(g, 28, (UWORD)v->DisplayClip.MinX);
    video_word(g, 30, (UWORD)v->DisplayClip.MinY);
    video_word(g, 32, (UWORD)v->DisplayClip.MaxX);
    video_word(g, 34, (UWORD)v->DisplayClip.MaxY);
    video_word(g, 48, v->Flags);
    video_word(g, 50, (UWORD)v->Origin[0].x); video_word(g, 52, (UWORD)v->Origin[0].y);
    video_word(g, 54, (UWORD)v->Origin[1].x); video_word(g, 56, (UWORD)v->Origin[1].y);
}

void Kaffeine_VideoControlRetireScreen(struct KaffeineContext *ctx, struct Screen *screen)
{
    struct KaffeineVideoExtra *v;
    for (v = ctx->video_extras; v; v = v->next)
        if (v->active && v->screen == screen)
        {
            if (ctx->mem && v->guest < ctx->mem_size && ctx->mem_size - v->guest >= KAFFEINE_VPE_SIZE)
                memset(ctx->mem + v->guest, 0, KAFFEINE_VPE_SIZE);
            v->active = FALSE;
            v->screen = NULL; v->colormap = NULL; v->native = NULL;
        }
}

static IPTR video_control(struct KaffeineContext *ctx, ULONG cm_guest, ULONG tags)
{
    ULONG outputs[KAFFEINE_VIDEO_TAG_STEPS], count, result = 0, i, viewport;
    struct Screen *screen;
    struct ColorMap *cm;
    struct ViewPortExtra *native;
    struct KaffeineVideoExtra *old, *v = NULL;
    UBYTE snapshot[KAFFEINE_VPE_SIZE];
    int entry;

    if (!ctx || ctx->video_closing || (entry = video_screen(ctx, cm_guest)) < 0 ||
        !video_tags(ctx, tags, outputs, &count)) return 1;
    if (!count) return 0;
    screen = ctx->pubscreens[entry].native;
    cm = screen->ViewPort.ColorMap;
    viewport = ctx->pubscreens[entry].guest + M68K_SCR_VIEWPORT;
    old = video_current(ctx, cm_guest, screen);
    if (m68k_read32(ctx, cm_guest + 20) != viewport ||
        (m68k_read32(ctx, cm_guest + 8) &&
         (!old || m68k_read32(ctx, cm_guest + 8) != old->guest)) ||
        (old && memcmp(ctx->mem + old->guest, old->snapshot, KAFFEINE_VPE_SIZE))) return 1;
    native = cm->cm_vpe;
    if (native)
    {
        if (native->ViewPort != &screen->ViewPort || native->n.xln_Type != NT_GRAPHICS ||
            native->n.xln_Subsystem != SS_GRAPHICS || native->n.xln_Subtype != VIEWPORT_EXTRA_TYPE)
            return 1;
        video_snapshot(snapshot, native, viewport);
        if (old && old->native == native && old->colormap == cm) v = old;
        else
        {
            if (ctx->video_extra_count >= KAFFEINE_VPE_LIMIT) return 1;
            ++ctx->video_extra_count;
            ctx->video_destroy = Kaffeine_VideoControlCleanup;
            v = AllocVec(sizeof(*v), MEMF_CLEAR);
            if (!v) { --ctx->video_extra_count; return 1; }
            if (!ctx->video_closing) v->guest = Kaffeine_MirrorAlloc(ctx, KAFFEINE_VPE_SIZE);
            if (!v->guest || ctx->video_closing)
            {
                if (v->guest && !Kaffeine_MirrorAbort(ctx, v->guest))
                {
                    v->next = ctx->video_extras; ctx->video_extras = v;
                    ctx->video_protected = video_protected;
                    return 1;
                }
                FreeVec(v); --ctx->video_extra_count; return 1;
            }
            v->screen = screen; v->colormap = cm; v->native = native;
            v->screen_guest = ctx->pubscreens[entry].guest; v->colormap_guest = cm_guest;
        }
        result = v->guest;
    }
    /* Nothing fallible remains. Retire the old identity only after the
     * replacement was reserved; old mirrors are never recycled in this arena. */
    if (old && old != v) Kaffeine_VideoControlRetireScreen(ctx, screen);
    if (v)
    {
        memcpy(v->snapshot, snapshot, KAFFEINE_VPE_SIZE);
        memcpy(ctx->mem + v->guest, snapshot, KAFFEINE_VPE_SIZE);
        if (v != old) { v->next = ctx->video_extras; ctx->video_extras = v; }
        v->active = TRUE;
        ctx->video_protected = video_protected;
        ctx->video_screen_retire = Kaffeine_VideoControlRetireScreen;
        ctx->video_extra_guest = video_extra_guest;
    }
    m68k_write32(ctx, cm_guest + 8, result);
    for (i = 0; i < count; ++i)
    {
        m68k_write32(ctx, outputs[i], VTAG_VIEWPORTEXTRA_SET);
        m68k_write32(ctx, outputs[i] + 4, result);
    }
    return 0;
}

IPTR Kaffeine_VideoControl(struct KaffeineContext *ctx, void *cpu)
{
    SIPTR error = IoErr();
    IPTR result = video_control(ctx, THUNK_A(0), THUNK_A(1));
    SetIoErr(error);
    return result;
}

IPTR Kaffeine_VideoControlUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    Kaffeine_MessageFailure(ctx, "untyped graphics extended node");
    ctx->callback_failed = TRUE;
    return 20;
}

void Kaffeine_VideoControlCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineVideoExtra *v;
    SIPTR error;
    if (!ctx) return;
    error = IoErr(); ctx->video_closing = TRUE;
    ctx->video_screen_retire = NULL; ctx->video_protected = NULL;
    ctx->video_extra_guest = NULL;
    while ((v = ctx->video_extras))
    {
        ctx->video_extras = v->next;
        --ctx->video_extra_count;
        FreeVec(v);
    }
    SetIoErr(error);
}

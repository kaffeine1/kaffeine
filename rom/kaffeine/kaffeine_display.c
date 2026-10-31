/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Read-only display metadata, queried by mode ID. Native handles and the
    backend's reserved pointer fields never enter the guest arena.
*/
#include <aros/debug.h>
#include <graphics/displayinfo.h>
#include <graphics/monitor.h>
#include <proto/graphics.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_display.h"

static IPTR refuse(struct KaffeineContext *ctx, const char *why)
{
    bug("[kaffeine] %s, stopping (20)\n", why);
    ctx->running = FALSE;
    ctx->exit_code = 20;
    return 20;
}

/* -726: An opaque native DisplayInfoHandle has no guest identity yet. */
IPTR thunk_gfx_FindDisplayInfo(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "FindDisplayInfo(): native handles are unsupported");
}

/* These APIs may not reinterpret a borrowed guest snapshot as a native
   MonitorSpec. Owned opens and mutation need a separate ownership bridge. */
IPTR thunk_gfx_OpenMonitor(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "OpenMonitor(): owned native monitors are unsupported");
}
IPTR thunk_gfx_CloseMonitor(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return refuse(ctx, "CloseMonitor(): borrowed snapshots cannot close native monitors");
}

static void word(UBYTE *out, ULONG at, UWORD value)
{
    out[at] = value >> 8;
    out[at + 1] = value;
}
static void longword(UBYTE *out, ULONG at, ULONG value)
{
    word(out, at, value >> 16);
    word(out, at + 2, value);
}
static void rectangle(UBYTE *out, ULONG at, const struct Rectangle *r)
{
    word(out, at, r->MinX); word(out, at + 2, r->MinY);
    word(out, at + 4, r->MaxX); word(out, at + 6, r->MaxY);
}

static BOOL overlap(ULONG a, ULONG asize, ULONG b, ULONG bsize)
{
    return a < b ? b - a < asize : a - b < bsize;
}

/* Only public scalar/name metadata is published. No native callbacks,
   monitor list, HIDD object alias, owner/waiter or library pointer escapes.
   Empty guest list/semaphore headers make bounded readers safe. */
static ULONG monitor_snapshot(struct KaffeineContext *ctx,
                              const struct MonitorSpec *s, ULONG output, ULONG count)
{
    ULONG i, slot = KAFFEINE_MAX_MONITORS, len = 0, size, guest;
    const char *name;
    UBYTE *out;
    if (!s) return 0;
    for (i = 0; i < KAFFEINE_MAX_MONITORS; i++)
    {
        if (ctx->monitors[i].native == s) return ctx->monitors[i].guest;
        if (!ctx->monitors[i].native && slot == KAFFEINE_MAX_MONITORS) slot = i;
    }
    if (slot == KAFFEINE_MAX_MONITORS) return 0;
    name = s->ms_Node.xln_Name;
    if (name)
    {
        while (len < 512 && name[len]) len++;
        if (len == 512) return 0;
    }
    size = 160 + (name ? len + 1 : 0);
    guest = Kaffeine_MirrorAlloc(ctx, size);
    if (!guest) return 0;
    if (overlap(guest, size, output, count))
    {
        Kaffeine_MirrorRelease(ctx, guest);
        refuse(ctx, "GetDisplayInfoData(): output overlaps a new monitor snapshot");
        return 0;
    }
    out = ctx->mem + guest;
    memset(out, 0, size);
    out[8] = s->ms_Node.xln_Type; out[9] = s->ms_Node.xln_Pri;
    out[14] = s->ms_Node.xln_Subsystem; out[15] = s->ms_Node.xln_Subtype;
    if (name)
    {
        longword(out, 10, guest + 160);
        memcpy(out + 160, name, len);
    }
    word(out, 24, s->ms_Flags);
    longword(out, 26, s->ratioh); longword(out, 30, s->ratiov);
    word(out, 34, s->total_rows); word(out, 36, s->total_colorclocks);
    word(out, 38, s->DeniseMaxDisplayColumn); word(out, 40, s->BeamCon0);
    word(out, 42, s->min_row); word(out, 48, s->ms_OpenCount);
    word(out, 62, s->ms_xoffset); word(out, 64, s->ms_yoffset);
    rectangle(out, 66, &s->ms_LegalView);
    word(out, 82, s->DeniseMinDisplayColumn);
    longword(out, 84, s->DisplayCompatible);
    longword(out, 88, guest + 92); longword(out, 96, guest + 88);
    out[110] = 15; /* NT_SIGNALSEM, without a native owner or waiter. */
    longword(out, 118, guest + 122); longword(out, 126, guest + 118);
    word(out, 146, 0xFFFF);
    ctx->monitors[slot].native = s;
    ctx->monitors[slot].guest = guest;
    ctx->monitors[slot].size = size;
    return guest;
}

/* -756: A0=handle (NULL only), A1=buffer, D0=size, D1=tag, D2=mode ID.
   Classic AmigaOS transfers at most 48/66/48/88 bytes for
   DISP/DIMS/NAME/MNTR,
   leaving reserved tails intact. DIMS copies its first 26 bytes as a
   prefix, then complete eight-byte rectangles. QueryHeader.Length counts
   local data in double-longwords: 4/8/4, not the native sizeof(struct).
   These transfer boundaries are covered by the original native fixture. */
IPTR thunk_gfx_GetDisplayInfoData(struct KaffeineContext *ctx, void *cpu)
{
    ULONG handle = THUNK_A(0), buffer = THUNK_A(1), size = THUNK_D(0);
    ULONG tag = THUNK_D(1), id = THUNK_D(2), bytes, native_size, got;
    UBYTE out[88] = {0};
    union
    {
        struct DisplayInfo display;
        struct DimensionInfo dims;
        struct NameInfo name;
        struct MonitorInfo monitor;
    } native;
    const struct QueryHeader *header;

    if (handle) return refuse(ctx, "GetDisplayInfoData(): non-NULL handles are unsupported");
    switch (tag)
    {
        case DTAG_DISP: bytes = 48; native_size = sizeof(native.display); break;
        case DTAG_DIMS: bytes = 66; native_size = sizeof(native.dims); break;
        case DTAG_NAME: bytes = 48; native_size = sizeof(native.name); break;
        case DTAG_MNTR: bytes = 88; native_size = sizeof(native.monitor); break;
        default:
            bug("[kaffeine] GetDisplayInfoData(): unsupported tag %08lx\n", (unsigned long)tag);
            return refuse(ctx, "GetDisplayInfoData(): this chunk is unsupported");
    }
    if (size < bytes) bytes = size;
    if (tag == DTAG_DIMS && bytes > 26)
        bytes = 26 + ((bytes - 26) / 8) * 8;
    if (!bytes) return 0;
    if (!ctx->mem || !buffer || buffer >= ctx->mem_size || bytes > ctx->mem_size - buffer)
        return refuse(ctx, "GetDisplayInfoData(): output is outside guest memory");

    for (ULONG i = 0; i < KAFFEINE_MAX_MONITORS; i++)
        if (ctx->monitors[i].native && overlap(buffer, bytes, ctx->monitors[i].guest, ctx->monitors[i].size))
            return refuse(ctx, "GetDisplayInfoData(): output overlaps a borrowed monitor snapshot");

    /* Always query into a complete native temporary. Passing a classic size
       to the backend truncates different fields on a 64-bit machine. */
    memset(&native, 0, sizeof(native));
    got = GetDisplayInfoData(NULL, (UBYTE *)&native, native_size, tag, id);
    if (!got) return 0;
    if (got != native_size)
        return refuse(ctx, "GetDisplayInfoData(): incomplete native metadata");
    header = tag == DTAG_DISP ? &native.display.Header :
             tag == DTAG_DIMS ? &native.dims.Header :
             tag == DTAG_MNTR ? &native.monitor.Header : &native.name.Header;
    longword(out, 0, header->StructID);
    longword(out, 4, header->DisplayID);
    longword(out, 8, 3); /* TAG_SKIP */
    longword(out, 12, tag == DTAG_DIMS ? 8 : tag == DTAG_MNTR ? 9 : 4);
    if (tag == DTAG_DISP)
    {
        const struct DisplayInfo *d = &native.display;
        word(out, 16, d->NotAvailable);
        longword(out, 18, d->PropertyFlags);
        word(out, 22, d->Resolution.x); word(out, 24, d->Resolution.y);
        word(out, 26, d->PixelSpeed); word(out, 28, d->NumStdSprites);
        word(out, 30, d->PaletteRange);
        word(out, 32, d->SpriteResolution.x); word(out, 34, d->SpriteResolution.y);
        out[40] = d->RedBits; out[41] = d->GreenBits; out[42] = d->BlueBits;
    }
    else if (tag == DTAG_DIMS)
    {
        const struct DimensionInfo *d = &native.dims;
        word(out, 16, d->MaxDepth);
        word(out, 18, d->MinRasterWidth); word(out, 20, d->MinRasterHeight);
        word(out, 22, d->MaxRasterWidth); word(out, 24, d->MaxRasterHeight);
        rectangle(out, 26, &d->Nominal); rectangle(out, 34, &d->MaxOScan);
        rectangle(out, 42, &d->VideoOScan); rectangle(out, 50, &d->TxtOScan);
        rectangle(out, 58, &d->StdOScan);
    }
    else if (tag == DTAG_MNTR)
    {
        const struct MonitorInfo *m = &native.monitor;
        ULONG guest = 0;
        if (bytes > 16 && m->Mspc)
        {
            guest = monitor_snapshot(ctx, m->Mspc, buffer, bytes);
            if (!guest) return ctx->running ? 0 : ctx->exit_code;
        }
        longword(out, 16, guest);
        word(out, 20, m->ViewPosition.x); word(out, 22, m->ViewPosition.y);
        word(out, 24, m->ViewResolution.x); word(out, 26, m->ViewResolution.y);
        rectangle(out, 28, &m->ViewPositionRange);
        word(out, 36, m->TotalRows); word(out, 38, m->TotalColorClocks);
        word(out, 40, m->MinRow); word(out, 42, m->Compatibility);
        word(out, 76, m->MouseTicks.x); word(out, 78, m->MouseTicks.y);
        word(out, 80, m->DefaultViewPosition.x); word(out, 82, m->DefaultViewPosition.y);
        longword(out, 84, m->PreferredModeID);
    }
    else
        memcpy(out + 16, native.name.Name, 32);
    memcpy(ctx->mem + buffer, out, bytes);
    return bytes;
}

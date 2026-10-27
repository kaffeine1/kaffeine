/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Read-only display metadata, queried by mode ID. Native handles and the
    backend's reserved pointer fields never enter the guest arena.
*/
#include <aros/debug.h>
#include <graphics/displayinfo.h>
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

/* -756: A0=handle (NULL only), A1=buffer, D0=size, D1=tag, D2=mode ID.
   Classic AmigaOS transfers at most 48/66/48 bytes for DISP/DIMS/NAME,
   leaving reserved tails intact. DIMS copies its first 26 bytes as a
   prefix, then complete eight-byte rectangles. QueryHeader.Length counts
   local data in double-longwords: 4/8/4, not the native sizeof(struct).
   These transfer boundaries are covered by the original native fixture. */
IPTR thunk_gfx_GetDisplayInfoData(struct KaffeineContext *ctx, void *cpu)
{
    ULONG handle = THUNK_A(0), buffer = THUNK_A(1), size = THUNK_D(0);
    ULONG tag = THUNK_D(1), id = THUNK_D(2), bytes, native_size, got;
    UBYTE out[66] = {0};
    union
    {
        struct DisplayInfo display;
        struct DimensionInfo dims;
        struct NameInfo name;
    } native;
    const struct QueryHeader *header;

    if (handle) return refuse(ctx, "GetDisplayInfoData(): non-NULL handles are unsupported");
    switch (tag)
    {
        case DTAG_DISP: bytes = 48; native_size = sizeof(native.display); break;
        case DTAG_DIMS: bytes = 66; native_size = sizeof(native.dims); break;
        case DTAG_NAME: bytes = 48; native_size = sizeof(native.name); break;
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

    /* Always query into a complete native temporary. Passing a classic size
       to the backend truncates different fields on a 64-bit machine. */
    memset(&native, 0, sizeof(native));
    got = GetDisplayInfoData(NULL, (UBYTE *)&native, native_size, tag, id);
    if (!got) return 0;
    if (got != native_size)
        return refuse(ctx, "GetDisplayInfoData(): incomplete native metadata");
    header = tag == DTAG_DISP ? &native.display.Header :
             tag == DTAG_DIMS ? &native.dims.Header : &native.name.Header;
    longword(out, 0, header->StructID);
    longword(out, 4, header->DisplayID);
    longword(out, 8, 3); /* TAG_SKIP */
    longword(out, 12, tag == DTAG_DIMS ? 8 : 4);
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
    else
        memcpy(out + 16, native.name.Name, 32);
    memcpy(ctx->mem + buffer, out, bytes);
    return bytes;
}

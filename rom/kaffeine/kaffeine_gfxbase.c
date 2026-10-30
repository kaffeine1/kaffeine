/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_gfxbase.h"
#include "kaffeine_rastport.h"

ULONG Kaffeine_SetupGraphicsBase(struct KaffeineContext *ctx,
    const struct M68KThunkEntry *manual, ULONG manual_count,
    const struct M68KThunkEntry *generated, ULONG generated_count)
{
    struct GfxBase *native = NULL;
    struct KaffeineFontHold hold = {0};
    ULONG base = 0;
    SIPTR error = IoErr();

    if (!ctx || !ctx->mem) goto done;
    base = Kaffeine_FindLibBase(ctx, "graphics.library");
    if (base) goto done;
    /* An unregistered base could not keep a stable OpenLibrary identity. */
    if (ctx->num_libmap >= KAFFEINE_MAX_LIBMAP) goto done;
    native = (struct GfxBase *)OpenLibrary("graphics.library", 0);
    if (!native || !Kaffeine_FontHoldBegin(ctx, native->DefaultFont, &hold)) goto done;
    base = Kaffeine_SetupLibBaseSized(ctx, ctx->num_libs, "graphics.library",
                                     KAFFEINE_MAX_LVO, KAFFEINE_GFXBASE_SIZE,
                                     manual, manual_count, generated, generated_count);
    if (!base) goto done;
    /* Only this reserved positive area is cleared. Neighboring fake bases
     * and the negative jump vectors have already been placed by the heap. */
    memset(ctx->mem + base, 0, KAFFEINE_GFXBASE_SIZE);
    Kaffeine_FontHoldEnd(ctx, &hold, TRUE);
    m68k_write32(ctx, base + KAFFEINE_GFXBASE_DEFAULTFONT, hold.guest);

done:
    Kaffeine_FontHoldEnd(ctx, &hold, FALSE);
    if (native) CloseLibrary((struct Library *)native);
    SetIoErr(error);
    return base;
}

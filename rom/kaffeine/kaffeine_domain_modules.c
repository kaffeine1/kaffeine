/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/doshunks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "kaffeine_intern.h"
#include "kaffeine_domain_modules.h"
#include "kaffeine_domain_memory.h"

struct KaffeineDomainModule
{
    struct KaffeineDomainModule *next;
    ULONG count;
    struct KaffeineDomainBlock blocks[];
};

/* DestroyContext calls this only after every CPU/callback/native borrower has
 * retired. Guest queue references no longer prevent whole-arena destruction;
 * the allocator destructor releases its intervals immediately afterwards. */
static void destroy(struct KaffeineContext *ctx)
{
    struct KaffeineDomainModule *module;
    ctx->domain_modules_closing = TRUE;
    ctx->domain_cleanup_running = TRUE;
    while ((module = ctx->domain_modules))
    {
        ctx->domain_modules = module->next;
        --ctx->domain_module_count;
        FreeVec(module);
    }
    ctx->domain_cleanup_running = FALSE;
}

static BOOL fail(SIPTR error)
{
    SetIoErr(error);
    return FALSE;
}

/* The pointers themselves are trusted native loader storage, held by the
 * caller. Bound every walk before the existing copier can inspect them. */
static BOOL validate(struct KaffeineContext *ctx, BPTR native,
                     const struct HunkRelocTable *relocs,
                     struct KaffeineDomainModule *module)
{
    ULONG i;
    unsigned long long total = 0;
    BPTR cursor = native;
    for (i = 0; i < module->count; ++i)
    {
        UBYTE *link;
        ULONG allocated, payload;
        if (!cursor) return fail(ERROR_BAD_HUNK);
        link = BADDR(cursor);
        allocated = *((ULONG *)link - 1);
        if (allocated < sizeof(ULONG) + sizeof(BPTR)) return fail(ERROR_BAD_HUNK);
        payload = allocated - sizeof(ULONG) - sizeof(BPTR);
        if ((!i && payload < 2) || payload > (ULONG)~0UL - 8)
            return fail(ERROR_BAD_HUNK);
        module->blocks[i].size = payload + 8;
        total += payload + 8;
        if (total > ctx->mem_size) return fail(ERROR_NO_FREE_STORE);
        cursor = *((BPTR *)link);
    }
    /* A cycle cannot reach NULL in exactly NumHunks links. No unbounded
       cycle scan or native chain walk is passed through to the copier. */
    if (cursor) return fail(ERROR_BAD_HUNK);
    for (i = 0; i < relocs->hrt_NumRelocs; ++i)
    {
        const struct HunkRelocation *r = &relocs->hrt_Relocs[i];
        ULONG size;
        if (r->hr_Source >= module->count || r->hr_Target >= module->count ||
            (r->hr_Type != HRT_ABSOLUTE && r->hr_Type != HRT_RELATIVE))
            return fail(ERROR_BAD_HUNK);
        size = module->blocks[r->hr_Source].size - 8;
        if (size < 4 || r->hr_Offset > size - 4) return fail(ERROR_BAD_HUNK);
    }
    return TRUE;
}

BOOL Kaffeine_DomainLoad(struct KaffeineContext *ctx, BPTR native,
                        const struct HunkRelocTable *relocs,
                        struct KaffeineDomainImage *out)
{
    struct KaffeineDomainModule *module;
    struct KaffeineDomainImage image;
    SIPTR saved_error = IoErr(), error;
    ULONG count;

    if (!ctx || !ctx->mem || !out || ctx->domain_modules_closing)
        return fail(ERROR_OBJECT_WRONG_TYPE);
    if (!native || !relocs || !relocs->hrt_NumHunks ||
        (relocs->hrt_NumRelocs && !relocs->hrt_Relocs)) return fail(ERROR_BAD_HUNK);
    if (ctx->domain_module_count >= KAFFEINE_DOMAIN_MAX_MODULES ||
        relocs->hrt_NumHunks > KAFFEINE_DOMAIN_MAX_HUNKS ||
        relocs->hrt_NumRelocs > KAFFEINE_DOMAIN_MAX_RELOCS)
        return fail(ERROR_NO_FREE_STORE);
    count = relocs->hrt_NumHunks;
    /* Reserve capacity before an allocator can reenter the context. */
    ++ctx->domain_module_count;
    module = AllocVec(sizeof(*module) + count * sizeof(module->blocks[0]), MEMF_CLEAR);
    if (!module) { --ctx->domain_module_count; return fail(ERROR_NO_FREE_STORE); }
    module->count = count;
    if (!validate(ctx, native, relocs, module)) goto failed;
    if (ctx->domain_modules_closing) { SetIoErr(ERROR_OBJECT_WRONG_TYPE); goto failed; }
    image.seglist = Kaffeine_LoadDomainHunks(ctx, native, relocs, module->blocks, count);
    if (!image.seglist) { SetIoErr(ERROR_NO_FREE_STORE); goto failed; }
    if (ctx->domain_modules_closing)
    {
        Kaffeine_AbortDomainHunks(ctx, module->blocks, count);
        SetIoErr(ERROR_OBJECT_WRONG_TYPE);
        goto failed;
    }
    image.entry = module->blocks[0].base + 8;
    image.module = module;
    /* No fallible operation remains after the copy has succeeded. */
    module->next = ctx->domain_modules;
    ctx->domain_modules = module;
    ctx->domain_cleanup = Kaffeine_DomainCleanup;
    ctx->domain_destroy = destroy;
    ++ctx->jit_generation; /* also reaches sibling/new caches when cpu is NULL */
    *out = image;
    SetIoErr(saved_error);
    return TRUE;

failed:
    error = IoErr();
    FreeVec(module);
    --ctx->domain_module_count;
    return fail(error);
}

BOOL Kaffeine_DomainUnload(struct KaffeineContext *ctx, struct KaffeineDomainModule *module)
{
    struct KaffeineDomainModule **link;
    SIPTR saved_error = IoErr();
    if (!ctx || !module) return fail(ERROR_OBJECT_WRONG_TYPE);
    for (link = &ctx->domain_modules; *link && *link != module; link = &(*link)->next) {}
    if (!*link) return fail(ERROR_OBJECT_WRONG_TYPE);
    if (!Kaffeine_ReleaseDomainHunks(ctx, module->blocks, module->count))
        return fail(ERROR_OBJECT_IN_USE);
    *link = module->next; /* unlink before any native disposal/reentry */
    --ctx->domain_module_count;
    FreeVec(module);
    SetIoErr(saved_error);
    return TRUE;
}

void Kaffeine_DomainCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineDomainModule *module, *next;
    SIPTR saved_error;
    if (!ctx) return;
    ctx->domain_modules_closing = TRUE;
    if (ctx->domain_cleanup_running) return;
    ctx->domain_cleanup_running = TRUE;
    saved_error = IoErr();
    for (module = ctx->domain_modules; module; module = next)
    {
        next = module->next;
        Kaffeine_DomainUnload(ctx, module);
    }
    ctx->domain_cleanup_running = FALSE;
    SetIoErr(saved_error);
}

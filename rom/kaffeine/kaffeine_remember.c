/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "kaffeine_intern.h"
#include "kaffeine_messages.h"
#include "kaffeine_remember.h"
#include "kaffeine_remember_memory.h"

struct KaffeineRemember
{
    struct KaffeineRemember *all_next, *chain_next;
    ULONG node, memory, size;
    BOOL active, data_live;
};

static ULONG refuse(struct KaffeineContext *ctx, const char *operation)
{
    Kaffeine_MessageFailure(ctx, operation);
    ctx->callback_failed = TRUE;
    return 0;
}

static BOOL key_range(struct KaffeineContext *ctx, ULONG key)
{
    struct KaffeineRemember *r;
    if ((key & 1) || (key >= ctx->heap_start && key < ctx->heap_end &&
                      ctx->heap_end - key < 4) ||
        !Kaffeine_GuestBytes(ctx, key, 4) ||
        !Kaffeine_HeapGuestRange(ctx, key, 4) ||
        !Kaffeine_MessageMemory(ctx, key, 4)) return FALSE;
    /* A key is a separate caller-owned pointer cell, never one of the
     * immutable public link fields that the adapter itself publishes. */
    for (r = ctx->remembers; r; r = r->all_next)
        if (Kaffeine_Overlap(key, 4, r->node, KAFFEINE_SIZEOF_REMEMBER))
            return FALSE;
    return TRUE;
}

static struct KaffeineRemember *find(struct KaffeineContext *ctx, ULONG node)
{
    struct KaffeineRemember *r;
    for (r = ctx->remembers; r; r = r->all_next)
        if (r->node == node) return r;
    return NULL;
}

static BOOL chain(struct KaffeineContext *ctx, ULONG key,
                  struct KaffeineRemember **head)
{
    struct KaffeineRemember *r;
    ULONG node, count = 0;
    if (!key_range(ctx, key)) return FALSE;
    node = m68k_read32(ctx, key);
    *head = node ? find(ctx, node) : NULL;
    if (node && !*head) return FALSE;
    for (r = *head; r; r = r->chain_next)
    {
        if (++count > KAFFEINE_REMEMBER_LIMIT || !r->active || !r->data_live ||
            !Kaffeine_RememberMemoryCheck(ctx, r->node, KAFFEINE_SIZEOF_REMEMBER, FALSE) ||
            !Kaffeine_RememberMemoryCheck(ctx, r->memory, r->size, FALSE) ||
            m68k_read32(ctx, r->node) != (r->chain_next ? r->chain_next->node : 0) ||
            m68k_read32(ctx, r->node + 4) != r->size ||
            m68k_read32(ctx, r->node + 8) != r->memory)
            return FALSE;
    }
    return TRUE;
}

static void retain(struct KaffeineContext *ctx, struct KaffeineRemember *r)
{
    r->all_next = ctx->remembers;
    ctx->remembers = r;
}

/* Only fresh unpublished blocks use this bypass. A malformed unrelated
 * guest queue must not turn ordinary allocation failure into a leak. */
static void discard(struct KaffeineContext *ctx, struct KaffeineRemember *r)
{
    BOOL okay = TRUE;
    if (r->memory)
    {
        if (Kaffeine_RememberMemoryRelease(ctx, r->memory, r->size, TRUE)) r->memory = 0;
        else okay = FALSE;
    }
    if (r->node)
    {
        if (Kaffeine_RememberMemoryRelease(ctx, r->node, KAFFEINE_SIZEOF_REMEMBER, TRUE)) r->node = 0;
        else okay = FALSE;
    }
    if (!okay)
    {
        /* Corrupt allocator mirrors are a separate controlled failure.
         * Keep every unreleased interval privately owned until destruction. */
        retain(ctx, r);
        refuse(ctx, "AllocRemember rollback");
        return;
    }
    FreeVec(r);
    --ctx->remember_count;
}

static ULONG allocate(struct KaffeineContext *ctx, ULONG key, ULONG size, ULONG flags)
{
    struct KaffeineRemember *head, *r;
    if (!ctx || ctx->remember_closing) return 0;
    if (!chain(ctx, key, &head)) return refuse(ctx, "AllocRemember");
    if (!size || ctx->remember_count >= KAFFEINE_REMEMBER_LIMIT) return 0;
    ++ctx->remember_count;
    ctx->remember_destroy = Kaffeine_RememberCleanup;
    r = AllocVec(sizeof(*r), MEMF_CLEAR);
    if (!r) { --ctx->remember_count; return 0; }
    r->size = size;
    if (ctx->remember_closing) { discard(ctx, r); return 0; }
    r->node = Kaffeine_RememberMemoryAlloc(ctx, KAFFEINE_SIZEOF_REMEMBER,
                                          MEMF_PUBLIC | MEMF_CLEAR);
    if (!r->node || ctx->remember_closing) { discard(ctx, r); return 0; }
    r->memory = Kaffeine_RememberMemoryAlloc(ctx, size, flags);
    if (!r->memory || ctx->remember_closing) { discard(ctx, r); return 0; }
    r->chain_next = head;
    r->active = r->data_live = TRUE;
    m68k_write32(ctx, r->node, head ? head->node : 0);
    m68k_write32(ctx, r->node + 4, size);
    m68k_write32(ctx, r->node + 8, r->memory);
    retain(ctx, r);
    m68k_write32(ctx, key, r->node); /* final, infallible publication */
    return r->memory;
}

static void release(struct KaffeineContext *ctx, ULONG key, BOOL really_forget)
{
    struct KaffeineRemember *head, *r;
    if (!ctx || ctx->remember_closing) return;
    if (!chain(ctx, key, &head)) { refuse(ctx, "FreeRemember"); return; }
    /* Validate every node and every data owner before changing the key or
     * releasing anything. A queue/request may retain a data block. */
    for (r = head; r; r = r->chain_next)
        if ((really_forget && Kaffeine_Overlap(key, 4, r->memory, r->size)) ||
            !Kaffeine_RememberMemoryCheck(ctx, r->memory, r->size, really_forget))
        { refuse(ctx, "FreeRemember memory"); return; }
    for (r = head; r; r = r->chain_next)
    {
        BOOL okay = really_forget ?
            Kaffeine_RememberMemoryRelease(ctx, r->memory, r->size, FALSE) :
            Kaffeine_RememberMemoryForget(ctx, r->memory, r->size);
        if (!okay) { refuse(ctx, "FreeRemember commit"); return; }
        /* Never release/reclassify this data twice, even if a native
         * collaborator breaks its probe/commit contract and refuses later. */
        r->data_live = FALSE;
    }
    for (r = head; r; r = r->chain_next) r->active = FALSE;
    /* Nodes stay quarantined, so an old copied key can never gain the
     * identity of a later allocation that happens to reuse its address.
     * FALSE transfers data to normal FreeMem/FreeVec ownership immediately. */
    m68k_write32(ctx, key, 0);
}

IPTR thunk_intuition_AllocRemember(struct KaffeineContext *ctx, void *cpu)
{
    SIPTR saved_error = IoErr();
    ULONG result = allocate(ctx, THUNK_A(0), THUNK_D(0), THUNK_D(1));
    SetIoErr(saved_error);
    return result;
}

IPTR thunk_intuition_FreeRemember(struct KaffeineContext *ctx, void *cpu)
{
    SIPTR saved_error = IoErr();
    release(ctx, THUNK_A(0), THUNK_D(0) != 0);
    SetIoErr(saved_error);
    return 0;
}

void Kaffeine_RememberCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineRemember *r;
    SIPTR saved_error;
    if (!ctx) return;
    saved_error = IoErr();
    ctx->remember_closing = TRUE;
    /* Arena destruction follows immediately. A key may already have been
     * freed with its original code/data module; never inspect guest bytes. */
    while ((r = ctx->remembers))
    {
        ctx->remembers = r->all_next;
        --ctx->remember_count;
        FreeVec(r);
    }
    SetIoErr(saved_error);
}

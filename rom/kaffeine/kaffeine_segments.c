/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Owned native loader modules. Guest numbers are never native addresses. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_segments.h"

enum { SEG_UNKNOWN, SEG_HUNK, SEG_ELF };
struct KaffeineSegment {
    struct KaffeineSegment *next;
    struct KaffeineContext *owner;
    BPTR native;
    IPTR relocs; /* borrowed from DOS, valid only while native is owned */
    ULONG token, loans;
    unsigned kind;
    BOOL live, transferring;
};

void Kaffeine_SegmentsInit(struct KaffeineLibBase *base)
{
    InitSemaphore(&base->segment_ids_lock);
    base->segment_next_token = KAFFEINE_SEGMENT_TOKEN_FIRST;
}
BOOL Kaffeine_SegmentToken(ULONG token)
{
    return token >= KAFFEINE_SEGMENT_TOKEN_FIRST && token <= KAFFEINE_SEGMENT_TOKEN_LAST;
}
static struct KaffeineSegment *find(struct KaffeineContext *ctx, ULONG token)
{
    struct KaffeineSegment *entry;
    for (entry = ctx->segments; entry; entry = entry->next)
        if (entry->token == token) return entry;
    return NULL;
}
static BOOL member(struct KaffeineContext *ctx, struct KaffeineSegment *wanted, ULONG token)
{
    return wanted && find(ctx, token) == wanted;
}
static BOOL wrong_type(void)
{
    SetIoErr(ERROR_OBJECT_WRONG_TYPE);
    return FALSE;
}
/* Unlink before native disposal; a native cleanup cannot observe this entry.
   Never inspect segment bytes, relocation storage or child Process here. */
static void drop(struct KaffeineContext *ctx, struct KaffeineSegment *entry, BOOL consumed)
{
    struct KaffeineSegment **link = &ctx->segments;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) *link = entry->next;
    if (!consumed) UnLoadSeg(entry->native);
    FreeVec(entry);
    ctx->segment_count--;
}
BOOL Kaffeine_SegmentLoadBegin(struct KaffeineContext *ctx, struct KaffeineSegmentLoad *load)
{
    struct KaffeineLibBase *base = ctx->emuBase;
    struct KaffeineSegment *entry;
    SIPTR ioerr = IoErr();
    ULONG ticket;
    load->entry = NULL;
    if (ctx->segments_closing || !base) return wrong_type();
    if (ctx->segment_count >= KAFFEINE_SEGMENT_LIMIT)
    { SetIoErr(ERROR_NO_FREE_STORE); return FALSE; }
    /* Count a reservation before calling the native allocator: reentrant
       loads must see it even before an entry exists or the loader runs. */
    ctx->segment_count++;
    entry = AllocVec(sizeof(*entry), MEMF_CLEAR);
    if (!entry) { ctx->segment_count--; SetIoErr(ERROR_NO_FREE_STORE); return FALSE; }
    if (ctx->segments_closing)
    { FreeVec(entry); ctx->segment_count--; return wrong_type(); }
    ObtainSemaphore(&base->segment_ids_lock);
    ticket = base->segment_next_token;
    if (Kaffeine_SegmentToken(ticket))
        base->segment_next_token = ticket == KAFFEINE_SEGMENT_TOKEN_LAST ? 0 : ticket + 1;
    ReleaseSemaphore(&base->segment_ids_lock);
    if (!Kaffeine_SegmentToken(ticket))
    {
        FreeVec(entry);
        ctx->segment_count--;
        SetIoErr(ERROR_NO_FREE_STORE);
        return FALSE;
    }
    entry->owner = ctx;
    entry->token = ticket;
    load->entry = entry;
    ctx->segments_cleanup = Kaffeine_SegmentsCleanup;
    SetIoErr(ioerr);
    return TRUE;
}
ULONG Kaffeine_SegmentLoadEnd(struct KaffeineContext *ctx, struct KaffeineSegmentLoad *load, BPTR native)
{
    struct KaffeineSegment *entry = load->entry;
    SIPTR ioerr = IoErr();
    IPTR hunk = 0, relocs = 0, elf = 0;
    ULONG acknowledged;
    struct TagItem tags[] = {
        { GSLI_68KHUNK, (IPTR)&hunk }, { GSLI_68KRelocs, (IPTR)&relocs },
        { GSLI_ElfHandle, (IPTR)&elf }, { TAG_DONE, 0 }
    };
    if (!entry || entry->owner != ctx) { wrong_type(); return 0; }
    load->entry = NULL;
    if (!native) { FreeVec(entry); ctx->segment_count--; SetIoErr(ioerr); return 0; }
    if (ctx->segments_closing)
    {
        UnLoadSeg(native); FreeVec(entry); ctx->segment_count--; wrong_type(); return 0;
    }
    acknowledged = GetSegListInfo(native, tags);
    entry->native = native;
    /* Each output is acknowledged independently; absence is not ELF proof. */
    if (acknowledged && hunk == (IPTR)native && !elf && (!relocs || acknowledged >= 2))
    { entry->kind = SEG_HUNK; entry->relocs = relocs; }
    else if (acknowledged && elf == (IPTR)native && !hunk && !relocs) entry->kind = SEG_ELF;
    entry->live = TRUE;
    entry->next = ctx->segments;
    ctx->segments = entry;
    SetIoErr(ioerr);
    return entry->token;
}
BOOL Kaffeine_SegmentLoanBegin(struct KaffeineContext *ctx, ULONG token, struct KaffeineSegmentLoan *loan)
{
    struct KaffeineSegment *entry = find(ctx, token);
    memset(loan, 0, sizeof(*loan));
    if (ctx->segments_closing || !entry || !entry->live || entry->transferring ||
        entry->loans == (ULONG)~0UL ||
        (entry->kind != SEG_ELF && !(entry->kind == SEG_HUNK && entry->relocs))) return wrong_type();
    entry->loans++;
    loan->native = entry->native; loan->token = token; loan->entry = entry; loan->active = TRUE;
    return TRUE;
}
void Kaffeine_SegmentLoanEnd(struct KaffeineContext *ctx, struct KaffeineSegmentLoan *loan)
{
    SIPTR ioerr = IoErr();
    struct KaffeineSegment *entry;
    if (!loan->active || !member(ctx, loan->entry, loan->token)) return;
    entry = loan->entry;
    memset(loan, 0, sizeof(*loan));
    if (entry->loans) entry->loans--;
    if (!entry->live && !entry->loans && !entry->transferring) drop(ctx, entry, FALSE);
    SetIoErr(ioerr);
}
BOOL Kaffeine_SegmentRelease(struct KaffeineContext *ctx, ULONG token)
{
    struct KaffeineSegment *entry;
    SIPTR ioerr = IoErr();
    if (!token) return TRUE;
    entry = find(ctx, token);
    if (ctx->segments_closing || !entry || !entry->live || entry->transferring) return wrong_type();
    entry->live = FALSE;
    if (!entry->loans) drop(ctx, entry, FALSE);
    SetIoErr(ioerr);
    return TRUE;
}
BOOL Kaffeine_SegmentTransferBegin(struct KaffeineContext *ctx, ULONG token, struct KaffeineSegmentTransfer *transfer)
{
    struct KaffeineSegment *entry = find(ctx, token);
    memset(transfer, 0, sizeof(*transfer));
    if (ctx->segments_closing || !entry || !entry->live || entry->transferring || entry->loans ||
        entry->kind != SEG_HUNK || !entry->relocs) return wrong_type();
    entry->live = FALSE; entry->transferring = TRUE;
    transfer->native = entry->native; transfer->token = token;
    transfer->entry = entry; transfer->active = TRUE;
    return TRUE;
}
void Kaffeine_SegmentTransferEnd(struct KaffeineContext *ctx, struct KaffeineSegmentTransfer *transfer, BOOL consumed)
{
    SIPTR ioerr = IoErr();
    struct KaffeineSegment *entry;
    if (!transfer->active || !member(ctx, transfer->entry, transfer->token)) return;
    entry = transfer->entry;
    memset(transfer, 0, sizeof(*transfer));
    entry->transferring = FALSE;
    if (consumed) drop(ctx, entry, TRUE);
    else if (ctx->segments_closing) drop(ctx, entry, FALSE);
    else entry->live = TRUE;
    SetIoErr(ioerr);
}
void Kaffeine_SegmentsCleanup(struct KaffeineContext *ctx)
{
    struct KaffeineSegment *entry, *next;
    SIPTR ioerr = IoErr();
    ctx->segments_closing = TRUE;
    if (ctx->segments_cleanup_running) return;
    ctx->segments_cleanup_running = TRUE;
    for (entry = ctx->segments; entry; entry = next)
    {
        next = entry->next;
        entry->live = FALSE;
        if (!entry->loans && !entry->transferring) drop(ctx, entry, FALSE);
    }
    ctx->segments_cleanup_running = FALSE;
    SetIoErr(ioerr);
}

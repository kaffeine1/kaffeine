/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)

   Typed native Workbench startup, copied into the classic 68k layout.
   The native sender retains ownership of every lock, name and segment.
   The explicit native Workbench entry takes the message before calling
   DOS; the guest's independent driver publishes and replies to it. */
#include "kaffeine_startup.h"
#include "kaffeine_locks.h"
#include "kaffeine_messages.h"
#include "kaffeine_offsets.h"
#include <workbench/startup.h>
#include <proto/exec.h>
#include <string.h>

#define STARTUP_SIZE 40
#define STARTUP_ARGS 128
#define STARTUP_TEXT 4096
#define STARTUP_BYTES (64 * 1024)

/* Native Workbench supplied the pointers, not guest code. Bound every
   string and the aggregate before allocating or publishing guest bytes. */
static BOOL text_size(const char *text, ULONG *size)
{
    ULONG n;
    if (!text) { *size = 0; return TRUE; }
    for (n = 0; n < STARTUP_TEXT; ++n)
        if (!text[n]) { *size = n + 1; return TRUE; }
    return FALSE;
}

static void copy_text(struct KaffeineContext *ctx, const char *text,
                      ULONG size, ULONG field, ULONG *at)
{
    m68k_write32(ctx, field, size ? *at : 0);
    if (size)
    {
        CopyMem((APTR)text, ctx->mem + *at, size);
        *at += size;
    }
}

BOOL Kaffeine_StartupPublish(struct KaffeineContext *ctx)
{
    struct WBStartup *native = (struct WBStartup *)ctx->startup_native;
    struct KaffeineQueue queue;
    ULONG lengths[STARTUP_ARGS], tool, size, at, block, port, i, count;
    if (!native) return TRUE;
    if (ctx->startup_message || !ctx->guest_exec_task || !ctx->startup_seglist ||
        native->sm_NumArgs < 0 || native->sm_NumArgs > STARTUP_ARGS ||
        (native->sm_NumArgs && !native->sm_ArgList) ||
        !text_size(native->sm_ToolWindow, &tool)) return FALSE;
    count = native->sm_NumArgs;
    size = STARTUP_SIZE + count * 8 + tool;
    for (i = 0; i < count; ++i)
    {
        if (!text_size((const char *)native->sm_ArgList[i].wa_Name, &lengths[i]) ||
            lengths[i] > STARTUP_BYTES - size) return FALSE;
        size += lengths[i];
    }
    port = ctx->guest_exec_task + M68K_PR_MSGPORT;
    if (!Kaffeine_QueueRead(ctx, port, &queue) ||
        !(block = Kaffeine_MirrorAlloc(ctx, size))) return FALSE;
    memset(ctx->mem + block, 0, size);
    m68k_write16(ctx, block + 18, STARTUP_SIZE);
    m68k_write32(ctx, block + 20, port);
    m68k_write32(ctx, block + 24, ctx->startup_seglist);
    m68k_write32(ctx, block + 28, count);
    m68k_write32(ctx, block + 36, count ? block + STARTUP_SIZE : 0);
    at = block + STARTUP_SIZE + count * 8;
    for (i = 0; i < count; ++i)
    {
        BPTR lock = native->sm_ArgList[i].wa_Lock;
        ULONG view = lock ? Kaffeine_LockBorrow(ctx, lock, KAFFEINE_LOCK_WBARG, block) : 0;
        if (lock && !view) goto rollback;
        m68k_write32(ctx, block + STARTUP_SIZE + i * 8, view);
        copy_text(ctx, (const char *)native->sm_ArgList[i].wa_Name, lengths[i],
                   block + STARTUP_SIZE + i * 8 + 4, &at);
    }
    copy_text(ctx, native->sm_ToolWindow, tool, block + 32, &at);
    if (at != block + size || !Kaffeine_MessagePreflight(ctx, &queue, block, STARTUP_SIZE))
        goto rollback;
    /* Preflight precedes the typed reservation. Only this first publication
       can insert the bridge-owned message; guest PutMsg may not forward it. */
    Kaffeine_MessageAppend(ctx, &queue, block, NT_MESSAGE);
    ctx->startup_message = block;
    ctx->startup_size = size;
    ctx->startup_port = port;
    return TRUE;
rollback:
    Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_WBARG, block);
    Kaffeine_MirrorAbort(ctx, block);
    return FALSE;
}

void Kaffeine_StartupFinish(struct KaffeineContext *ctx)
{
    struct Message *native = ctx->startup_native;
    if (!native) return;
    if (ctx->startup_message)
    {
        /* Only unlink through the bounded queue API. If the guest damaged
           the list, retain the quarantined storage until the arena dies. */
        Kaffeine_GuestRemoveMsg(ctx, ctx->startup_port, ctx->startup_message);
        Kaffeine_LockScopeDrop(ctx, KAFFEINE_LOCK_WBARG, ctx->startup_message);
    }
    ctx->startup_native = NULL;
    ReplyMsg(native);
}

BOOL Kaffeine_StartupReply(struct KaffeineContext *ctx, ULONG message, IPTR *result)
{
    struct KaffeineQueue queue;
    unsigned i;
    if (!ctx->startup_message || message != ctx->startup_message) return FALSE;
    *result = 0;
    /* The retained address is a tombstone after the first reply. Never let
       a duplicate reach generic message/native-pointer conversion. */
    if (!ctx->startup_native) return TRUE;
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
        if (ctx->message_ports[i] &&
            (!Kaffeine_QueueRead(ctx, ctx->message_ports[i], &queue) ||
             !Kaffeine_QueueDisjoint(&queue, message, STARTUP_SIZE)))
        {
            *result = Kaffeine_MessageFailure(ctx, "ReplyMsg queued WBStartup");
            return TRUE;
        }
    Kaffeine_StartupFinish(ctx);
    return TRUE;
}

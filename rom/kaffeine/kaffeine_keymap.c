/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    keymap.library's classic InputEvent boundary. No guest KeyMap table or
    InputEvent pointer is passed to the native library.
*/
#include "kaffeine_keymap.h"
#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_messages.h"

#include <devices/inputevent.h>
#include <proto/keymap.h>
#include <string.h>

enum
{
    G_IE_CLASS = 4, G_IE_SUBCLASS = 5, G_IE_CODE = 6, G_IE_QUALIFIER = 8,
    G_IE_PREV1_CODE = 10, G_IE_PREV1_QUAL = 11,
    G_IE_PREV2_CODE = 12, G_IE_PREV2_QUAL = 13,
    G_IE_SECONDS = 14, G_IE_MICRO = 18, G_IE_SIZE = 22
};

/* SetKeyMapDefault would retain a guest pointer as the system's native
   default. AskKeyMapDefault exposes that native table, and MapANSI also
   needs typed table/buffer conversion. None belongs to the NULL-map-only
   MapRawKey profile. Refuse before invoking any native keymap function. */
IPTR Kaffeine_KeymapUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return Kaffeine_IORefuse(ctx, "keymap.library: unsupported keymap table operation");
}

/* Ordinary caller storage may be in a hunk, on the stack or in a live
   allocation. Bridge mirrors, heap headers, queues and library bases are
   not writable conversion buffers. Keep fonts excluded even when a
   retained font's guest storage is outside a registered shadow. */
static BOOL ordinary(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    UWORD i;

    if (!ctx->mem || !addr || addr >= ctx->mem_size || size > ctx->mem_size - addr)
        return FALSE;
    if (!size) return TRUE;
    if (!Kaffeine_MessageMemory(ctx, addr, size) || !Kaffeine_HeapGuestRange(ctx, addr, size) ||
        (ctx->memory_info && Kaffeine_Overlap(addr, size, ctx->memory_info, KAFFEINE_MEMINFO_SIZE)))
        return FALSE;
    for (i = 0; i < KAFFEINE_MAX_FONTS; i++)
        if (ctx->fonts[i].guest &&
            Kaffeine_Overlap(addr, size, ctx->fonts[i].guest, ctx->fonts[i].allocation_size))
            return FALSE;
    for (i = 0; i < ctx->num_libs; i++)
        if (ctx->libs[i].m68k_addr >= ctx->libs[i].jt_start &&
            Kaffeine_Overlap(addr, size, ctx->libs[i].jt_start,
                             ctx->libs[i].m68k_addr - ctx->libs[i].jt_start + ctx->libs[i].pos_size))
            return FALSE;
    return TRUE;
}

/* -42: A0=classic InputEvent, A1=output, D1=signed LONG length,
   A2=KeyMap. AROS MapRawKey ignores ie_NextEvent: leave the native link
   NULL without reading the guest link. The dead-key history is four
   scalar bytes, never a native pointer. Custom guest KeyMaps remain
   unsupported until their pointer-bearing tables have a typed owner. */
IPTR Kaffeine_MapRawKey(struct KaffeineContext *ctx, void *cpu)
{
    ULONG event = THUNK_A(0), output = THUNK_A(1);
    LONG length = (LONG)THUNK_D(1);
    struct InputEvent native;

    if (THUNK_A(2))
        return Kaffeine_IORefuse(ctx, "MapRawKey(): custom guest keymaps are not supported yet");
    if (length < 0 || (event & 1) || !ordinary(ctx, event, G_IE_SIZE) ||
        !ordinary(ctx, output, (ULONG)length))
        return Kaffeine_IORefuse(ctx, "MapRawKey(): invalid event or output storage");
    if (Kaffeine_Overlap(event, G_IE_SIZE, output, (ULONG)length))
        return Kaffeine_IORefuse(ctx, "MapRawKey(): the event and output overlap");

    memset(&native, 0, sizeof(native));
    native.ie_Class = ctx->mem[event + G_IE_CLASS];
    native.ie_SubClass = ctx->mem[event + G_IE_SUBCLASS];
    native.ie_Code = m68k_read16(ctx, event + G_IE_CODE);
    native.ie_Qualifier = m68k_read16(ctx, event + G_IE_QUALIFIER);
    native.ie_Prev1DownCode = ctx->mem[event + G_IE_PREV1_CODE];
    native.ie_Prev1DownQual = ctx->mem[event + G_IE_PREV1_QUAL];
    native.ie_Prev2DownCode = ctx->mem[event + G_IE_PREV2_CODE];
    native.ie_Prev2DownQual = ctx->mem[event + G_IE_PREV2_QUAL];
    native.ie_TimeStamp.tv_secs = m68k_read32(ctx, event + G_IE_SECONDS);
    native.ie_TimeStamp.tv_micro = m68k_read32(ctx, event + G_IE_MICRO);
    return (IPTR)(LONG)MapRawKey(&native, (STRPTR)(ctx->mem + output), length, NULL);
}

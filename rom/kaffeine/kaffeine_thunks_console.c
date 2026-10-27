/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    console.device CONU_LIBRARY. Guest events and keymaps must never be
    passed to native console or keymap functions without marshalling.
*/

#include "kaffeine_intern.h"
#include "kaffeine_exec_io.h"
#include "kaffeine_thunks_console.h"

#include <devices/inputevent.h>
#include <proto/keymap.h>
#include <string.h>

/* The 68k InputEvent is 22 bytes, with two-byte alignment. Its native
   pointer, union and timestamp alignment must never be used as offsets. */
enum
{
    G_IE_NEXT = 0, G_IE_CLASS = 4, G_IE_SUBCLASS = 5, G_IE_CODE = 6,
    G_IE_QUALIFIER = 8, G_IE_PREV1_CODE = 10, G_IE_PREV1_QUAL = 11,
    G_IE_PREV2_CODE = 12, G_IE_PREV2_QUAL = 13,
    G_IE_SECONDS = 14, G_IE_MICRO = 18, G_IE_SIZE = 22
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && size <= ctx->mem_size - addr;
}

static IPTR unsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)cpu;
    return Kaffeine_IORefuse(ctx, "console.device: this device vector is not supported yet");
}

/* -48: A0=InputEvent, A1=output, D1=signed LONG length, A2=KeyMap.
   The measured supported contract is a single event and the default map.
   Classic linked-event behavior differs from AROS MapRawKey, so refuse
   links without dereferencing them. Negative lengths are a containment
   refusal, not a claim to reproduce classic's legacy unsigned behavior.

   AROS console's wrapper delegates to MapRawKey. A native event copy and
   the validated arena buffer let that backend provide the actual default
   keymap, dead-key and overflow behavior, without exposing guest pointers
   as native structures. No arbitrary buffer ceiling is needed. */
IPTR kaffeine_console_RawKeyConvert(struct KaffeineContext *ctx, void *cpu)
{
    ULONG event = THUNK_A(0), output = THUNK_A(1);
    LONG length = (LONG)THUNK_D(1);
    struct InputEvent native;

    if (THUNK_A(2))
        return Kaffeine_IORefuse(ctx, "RawKeyConvert(): custom guest keymaps are not supported yet");
    if (length < 0)
        return Kaffeine_IORefuse(ctx, "RawKeyConvert(): negative buffer length is not supported");
    if (!guest_range(ctx, event, G_IE_SIZE) || !guest_range(ctx, output, (ULONG)length))
        return Kaffeine_IORefuse(ctx, "RawKeyConvert(): the event or output is outside guest memory");
    if (length && (output >= event ? output - event < G_IE_SIZE :
                                    event - output < (ULONG)length))
        return Kaffeine_IORefuse(ctx, "RawKeyConvert(): the event and output overlap");
    if (m68k_read32(ctx, event + G_IE_NEXT))
        return Kaffeine_IORefuse(ctx, "RawKeyConvert(): linked guest events are not supported yet");

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

/* Every exposed unsupported vector refuses explicitly, including
   CDInputHandler (-42), asynchronous I/O and system-keymap changes. */
const struct M68KThunkEntry kaffeine_thunks_console[] = {
    { 6, unsupported }, { 12, unsupported }, { 18, unsupported }, { 24, unsupported },
    { 30, unsupported }, { 36, unsupported }, { 42, unsupported },
    { 48, kaffeine_console_RawKeyConvert },
    { 54, unsupported }, { 60, unsupported }, { 66, unsupported }, { 72, unsupported },
    { 0, NULL }
};
const ULONG kaffeine_thunks_console_count =
    sizeof(kaffeine_thunks_console) / sizeof(kaffeine_thunks_console[0]) - 1;

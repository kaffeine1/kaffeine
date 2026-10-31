/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)
*/
#include <aros/asmcall.h>
#include <string.h>
#include "kaffeine_intern.h"
#include "kaffeine_hook.h"

AROS_UFH3S(IPTR, guest_hook_entry,
    AROS_UFHA(struct Hook *, native_hook, A0),
    AROS_UFHA(APTR, object, A2),
    AROS_UFHA(IPTR, message, A1))
{
    AROS_USERFUNC_INIT

    struct KaffeineNativeHook *hook = native_hook->h_Data;
    struct KaffeineGuestCall call = { {0}, {0}, 0, 7u };

    /* Native code may keep asking for characters after a stop. In particular,
       a parser can mistake the first zero result for a successful EOF. */
    if (hook->failed) return 0;
    if (!hook->ctx->running || hook->ctx->callback_failed)
    {
        hook->failed = TRUE;
        return 0;
    }
    call.a[0] = hook->guest_hook;
    if (!hook->translate(hook->data, object, message, &call.a[2], &call.a[1]))
    {
        hook->failed = TRUE;
        Kaffeine_StopGuest(hook->ctx, hook->cpu, 20);
        return 0;
    }
    if (!Kaffeine_CallGuest(hook->ctx, hook->cpu, hook->entry, &call))
    {
        hook->failed = TRUE;
        return 0;
    }
    return (IPTR)call.d[0];

    AROS_USERFUNC_EXIT
}

BOOL Kaffeine_InitNativeHook(struct KaffeineNativeHook *hook,
                          struct KaffeineContext *ctx, void *cpu,
                          ULONG guest_hook, KaffeineHookTranslate translate,
                          void *data)
{
    ULONG entry;

    if (!guest_hook || (guest_hook & 1) || guest_hook >= ctx->mem_size ||
        ctx->mem_size - guest_hook < 20)
        return FALSE;
    entry = m68k_read32(ctx, guest_hook + 8);
    if (!entry || (entry & 1) || entry >= ctx->mem_size ||
        ctx->mem_size - entry < 2 || Kaffeine_IsCallbackReturn(entry))
        return FALSE;

    memset(hook, 0, sizeof(*hook));
    hook->hook.h_Entry = (APTR)guest_hook_entry;
    hook->hook.h_Data = hook;
    hook->ctx = ctx;
    hook->cpu = cpu;
    hook->guest_hook = guest_hook;
    hook->entry = entry;
    hook->translate = translate;
    hook->data = data;
    return TRUE;
}

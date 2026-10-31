/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Native Hooks for synchronous calls made during a thunk.
*/
#ifndef KAFFEINE_HOOK_H
#define KAFFEINE_HOOK_H

#include <utility/hooks.h>
#include "kaffeine_callback.h"

/* Translate checked native arguments to guest values. FALSE stops the guest
   with rc 20. The translator and its data must outlive the native call. */
typedef BOOL (*KaffeineHookTranslate)(void *data, APTR object, IPTR message,
                                   ULONG *guest_object, ULONG *guest_message);

struct KaffeineNativeHook
{
    struct Hook hook;
    struct KaffeineContext *ctx;
    void *cpu;
    ULONG guest_hook, entry;
    KaffeineHookTranslate translate;
    void *data;
    BOOL failed;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Invalid classic Hooks return FALSE without executing or stopping the guest.
   The entry is captured here; h_SubEntry/h_Data remain guest-owned. The native
   API must finish using the Hook on this task before the thunk returns. */
BOOL Kaffeine_InitNativeHook(struct KaffeineNativeHook *hook,
                          struct KaffeineContext *ctx, void *cpu,
                          ULONG guest_hook, KaffeineHookTranslate translate,
                          void *data);

#ifdef __cplusplus
}
#endif
#endif

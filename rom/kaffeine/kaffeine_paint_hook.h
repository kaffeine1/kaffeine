/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_PAINT_HOOK_H
#define KAFFEINE_PAINT_HOOK_H
#include <exec/types.h>
#include <stdint.h>
struct KaffeineContext;
struct KaffeinePaintHooks;
struct Hook;
struct Layer;

#ifdef __cplusplus
extern "C" {
#endif
/* Driver-only operations. The caller has already resolved one exact owned
 * window WLayer. Unknown native baseline Hooks are not adopted. No native
 * pointer, CPU or native signal number is ever published in guest memory.
 * Native callers must retain the Layer semaphore through Hook return. An
 * unowned native invocation is ignored without a deferred notification;
 * only a caller with that lifetime capability may publish pending work. */
BOOL Kaffeine_PaintHookChange(struct KaffeineContext *,
    struct KaffeinePaintHooks **, struct Layer *, ULONG guest_layer,
    ULONG guest_hook, ULONG *previous_guest_hook);
ULONG Kaffeine_PaintHookGuest(const struct KaffeinePaintHooks *,
    const struct Layer *, const struct Hook *);
BOOL Kaffeine_PaintHookHasLayer(const struct KaffeinePaintHooks *, const struct Layer *);
BOOL Kaffeine_PaintHookRetireLayer(struct KaffeineContext *,
    struct KaffeinePaintHooks *, struct Layer *);
BOOL Kaffeine_PaintHookRetireTask(struct KaffeineContext *,
    struct KaffeinePaintHooks *, ULONG private_task);
BOOL Kaffeine_PaintHooksCleanup(struct KaffeineContext *, struct KaffeinePaintHooks **);

/* Query-only preflights. Absent state is a no-op. Release callers must ask
 * before any member of a multi-object transaction changes. They never
 * detach, allocate or wait, and they are safe on the paused DOS coordinator. */
BOOL Kaffeine_PaintHookMemoryReleasable(const struct KaffeinePaintHooks *, ULONG, ULONG);
BOOL Kaffeine_PaintHookSignalReleasable(const struct KaffeinePaintHooks *, ULONG bits);

/* The mask accessor has no native effect. Poll runs on the captured driver
 * only, routes pending guest signals without a CPU callback, and makes an
 * asynchronous validation failure authoritative there. */
ULONG Kaffeine_PaintHookSignalMask(const struct KaffeinePaintHooks *);
BOOL Kaffeine_PaintHookPoll(struct KaffeineContext *, struct KaffeinePaintHooks *);
#ifdef __cplusplus
}
#endif
#endif

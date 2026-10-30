/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Typed classic keymap.library events.
*/
#ifndef KAFFEINE_KEYMAP_H
#define KAFFEINE_KEYMAP_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_MapRawKey(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_KeymapUnsupported(struct KaffeineContext *ctx, void *cpu);
extern const struct M68KThunkEntry kaffeine_thunks_keymap_manual[];
extern const ULONG kaffeine_thunks_keymap_manual_count;

#define KAFFEINE_KEYMAP_THUNKS \
    { 30, Kaffeine_KeymapUnsupported }, \
    { 36, Kaffeine_KeymapUnsupported }, \
    { 42, Kaffeine_MapRawKey }, \
    { 48, Kaffeine_KeymapUnsupported }

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_KEYMAP_H */

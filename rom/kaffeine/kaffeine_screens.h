/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Public screens of 68k code: LockPubScreen() and the calls that hand out
    the same screens or their names.
*/
#ifndef KAFFEINE_SCREENS_H
#define KAFFEINE_SCREENS_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_LockPubScreen(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_UnlockPubScreen(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_LockPubScreenList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_UnlockPubScreenList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_NextPubScreen(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GetDefaultPubScreen(struct KaffeineContext *ctx, void *cpu);

/* At the end of the program, in the task that ran it: the screen locks
   and the list lock that the program left go back to Intuition. */
void Kaffeine_ScreensCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host tests. */
#define KAFFEINE_INTUITION_PUBSCREEN_THUNKS \
    { 510, Kaffeine_LockPubScreen }, \
    { 516, Kaffeine_UnlockPubScreen }, \
    { 522, Kaffeine_LockPubScreenList }, \
    { 528, Kaffeine_UnlockPubScreenList }, \
    { 534, Kaffeine_NextPubScreen }, \
    { 582, Kaffeine_GetDefaultPubScreen }

#endif /* KAFFEINE_SCREENS_H */

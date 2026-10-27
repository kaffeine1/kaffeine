/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Screens of 68k code: the screens that it opens and closes, and public
    screens: LockPubScreen() and the calls that hand out the same screens or
    their names.
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
IPTR Kaffeine_OpenScreen(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OpenScreenTagList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CloseScreen(struct KaffeineContext *ctx, void *cpu);

/* At the end of the program, in the task that ran it: the screen locks
   and the list lock that the program left go back to Intuition, then the
   screens that it opened and left close, those without a window of the
   program on them. */
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

#define KAFFEINE_INTUITION_SCREEN_THUNKS \
    { 66, Kaffeine_CloseScreen }, \
    { 198, Kaffeine_OpenScreen }, \
    { 612, Kaffeine_OpenScreenTagList }

#endif /* KAFFEINE_SCREENS_H */

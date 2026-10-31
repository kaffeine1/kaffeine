/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Screens of 68k code: the screens that it opens and closes, and public
    screens: LockPubScreen() and the calls that hand out the same screens or
    their names.
*/
#ifndef KAFFEINE_SCREENS_H
#define KAFFEINE_SCREENS_H

#include "kaffeine_thunks.h"
#include "kaffeine_intern.h"
#include "kaffeine_rastport.h"

/* The 68k BitMap embedded in a 68k Screen (intuition/screens.h), which
   Screen.RastPort.BitMap names: a snapshot of the scalars of the BitMap
   that the native screen draws into, without planes, and the typed handle
   of that native BitMap. */
#define M68K_SCR_BITMAP 184

/* What a 68k BitMap is as the BitMap of a 68k Screen of the program */
enum
{
    KAFFEINE_SCREEN_BITMAP_NONE,     /* no 68k Screen's BitMap */
    KAFFEINE_SCREEN_BITMAP_OWNED,    /* that of an open screen that the program opened */
    KAFFEINE_SCREEN_BITMAP_BORROWED  /* that of a screen that the program only uses */
};

/* Whether bm is exactly the BitMap inside a 68k Screen that the program
   has now, and of which screen: *screen is the native screen for one that
   the program opened and has not closed, else NULL. Only the table of
   screens and the shadow map count, never guest bytes, so the program
   cannot make another address a screen's BitMap, nor a screen's BitMap
   something else; a 68k Screen that has gone names nothing. Here rather
   than in kaffeine_screens.c, so that the layer bridge resolves screens
   without linking the screen bridge. */
static inline int Kaffeine_ScreenBitMapOf(struct KaffeineContext *ctx, ULONG bm,
                                          struct Screen **screen)
{
    int i;

    *screen = NULL;
    for (i = 0; bm && i < KAFFEINE_MAX_PUBSCREENS; i++)
    {
        ULONG guest = ctx->pubscreens[i].guest;

        if (!guest || !ctx->pubscreens[i].native || bm != guest + M68K_SCR_BITMAP)
            continue;
        if (!ctx->pubscreens[i].owned ||
            Kaffeine_ShadowNative(ctx, guest, KAFFEINE_SHADOW_SCREEN) != ctx->pubscreens[i].native)
            return KAFFEINE_SCREEN_BITMAP_BORROWED;
        *screen = ctx->pubscreens[i].native;
        return KAFFEINE_SCREEN_BITMAP_OWNED;
    }
    return KAFFEINE_SCREEN_BITMAP_NONE;
}

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
/* CloseScreen() stops the program while a BltBitMap() of it runs
   (ctx->bitmap_blits), before Intuition gets the screen. */
IPTR Kaffeine_CloseScreen(struct KaffeineContext *ctx, void *cpu);
/* GetScreenDrawInfo() gives a 68k copy of the native DrawInfo of a screen
   of the program, the same for nested gets; FreeScreenDrawInfo() takes
   back one get of exactly that pair. */
IPTR Kaffeine_GetScreenDrawInfo(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeScreenDrawInfo(struct KaffeineContext *ctx, void *cpu);
/* These consumers stop with 20 until Images and DrawInfo attributes have
   typed conversions; no classic structure reaches a native API. */
IPTR Kaffeine_DrawImageState(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GetDrawInfoAttr(struct KaffeineContext *ctx, void *cpu);

/* One native visitor lock of the bridge on a screen that the program did
   not open, taken by its name in the list of public screens, as a DrawInfo
   copy and the pen leases of kaffeine_palette.c keep such a screen; FALSE,
   holding nothing, for a screen that is not public there, is private or
   has 32767 visitors. UnlockPubScreen(NULL, screen) gives it back. */
BOOL Kaffeine_ScreenPublicHold(struct Screen *screen);

/* At the end of the program, in the task that ran it: the pens that the
   program left go back to their ColorMaps (Kaffeine_PensCleanup()), before
   anything that keeps a screen open goes; then the DrawInfos that
   the program left go back to Intuition, then the screen locks and the
   list lock that it left, then the screens that it opened and left close,
   those without a window of the program on them, and last the fonts. */
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
    { 612, Kaffeine_OpenScreenTagList }, \
    { 618, Kaffeine_DrawImageState }, \
    { 690, Kaffeine_GetScreenDrawInfo }, \
    { 696, Kaffeine_FreeScreenDrawInfo }, \
    { 936, Kaffeine_GetDrawInfoAttr }

#endif /* KAFFEINE_SCREENS_H */

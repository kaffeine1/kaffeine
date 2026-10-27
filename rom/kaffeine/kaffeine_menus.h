/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GadTools menus of 68k code: VisualInfo handles, menu strips made by
    CreateMenusA() with a 68k mirror of every Menu, MenuItem, IntuiText,
    TextAttr and separator image, menu strips that the program built
    itself with a native copy for Intuition, and the Intuition calls on
    them.
*/
#ifndef KAFFEINE_MENUS_H
#define KAFFEINE_MENUS_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The native VisualInfo of a 68k handle from GetVisualInfoA(), or NULL. */
APTR Kaffeine_VisualInfoNative(struct KaffeineContext *ctx, ULONG guest);

IPTR Kaffeine_CreateMenusA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeMenus(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_LayoutMenuItemsA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_LayoutMenusA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_GetVisualInfoA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FreeVisualInfo(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_DrawBevelBoxA(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ClearMenuStrip(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ItemAddress(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OffMenu(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OnMenu(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_SetMenuStrip(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ResetMenuStrip(struct KaffeineContext *ctx, void *cpu);

/* At the end of the program: the strips come off the windows that it left
   open, and every strip of the program goes. */
void Kaffeine_MenusCleanup(struct KaffeineContext *ctx);
/* After menus/gadgets, before releasing native screens. */
void Kaffeine_VisualInfosCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual tables and the host dispatch tests. */
#define KAFFEINE_GADTOOLS_MENU_THUNKS \
    { 48,  Kaffeine_CreateMenusA }, \
    { 54,  Kaffeine_FreeMenus }, \
    { 60,  Kaffeine_LayoutMenuItemsA }, \
    { 66,  Kaffeine_LayoutMenusA }, \
    { 120, Kaffeine_DrawBevelBoxA }, \
    { 126, Kaffeine_GetVisualInfoA }, \
    { 132, Kaffeine_FreeVisualInfo }

#define KAFFEINE_INTUITION_MENU_THUNKS \
    { 54,  Kaffeine_ClearMenuStrip }, \
    { 144, Kaffeine_ItemAddress }, \
    { 180, Kaffeine_OffMenu }, \
    { 192, Kaffeine_OnMenu }, \
    { 264, Kaffeine_SetMenuStrip }, \
    { 702, Kaffeine_ResetMenuStrip }

#endif /* KAFFEINE_MENUS_H */

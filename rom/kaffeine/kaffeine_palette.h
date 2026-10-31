/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_PALETTE_H
#define KAFFEINE_PALETTE_H
#include "kaffeine_thunks.h"
#include "kaffeine_videocontrol.h"
#ifdef __cplusplus
extern "C" {
#endif
IPTR Kaffeine_LoadRGB4(struct KaffeineContext *, void *);
IPTR Kaffeine_LoadRGB32(struct KaffeineContext *, void *);
IPTR Kaffeine_GetRGB32(struct KaffeineContext *, void *);
IPTR Kaffeine_GetRGB4(struct KaffeineContext *, void *);
IPTR Kaffeine_SetRGB4(struct KaffeineContext *, void *);
IPTR Kaffeine_SetRGB32(struct KaffeineContext *, void *);
IPTR Kaffeine_SetRGB4CM(struct KaffeineContext *, void *);
IPTR Kaffeine_SetRGB32CM(struct KaffeineContext *, void *);
IPTR Kaffeine_PaletteUnsupported(struct KaffeineContext *, void *);
IPTR Kaffeine_VideoControl(struct KaffeineContext *, void *);
IPTR Kaffeine_VideoControlUnsupported(struct KaffeineContext *, void *);
IPTR Kaffeine_FreeColorMap(struct KaffeineContext *, void *);
/* ObtainBestPenA() and ReleasePen() on the ColorMap of a live 68k Screen
   of the program, each successful result a lease of the program that
   keeps the screen until ReleasePen() gives it back. */
IPTR Kaffeine_ObtainBestPenA(struct KaffeineContext *, void *);
IPTR Kaffeine_ReleasePen(struct KaffeineContext *, void *);
/* At the end of the program, before any screen lock, visitor lock or
   screen of the program goes (Kaffeine_ScreensCleanup()): every pen that
   the program left goes back to its ColorMap once, then each screen's
   lease retires. Nothing is left for a second call. */
void Kaffeine_PensCleanup(struct KaffeineContext *);
#ifdef __cplusplus
}
#endif
#define KAFFEINE_GRAPHICS_PALETTE_THUNKS \
    { 192, Kaffeine_LoadRGB4 }, \
    { 288, Kaffeine_SetRGB4 }, \
    { 570, Kaffeine_PaletteUnsupported }, \
    { 576, Kaffeine_FreeColorMap }, \
    { 582, Kaffeine_GetRGB4 }, \
    { 630, Kaffeine_SetRGB4CM }, \
    KAFFEINE_GRAPHICS_VIDEO_NODE_THUNKS, \
    { 708, Kaffeine_VideoControl }, \
    { 834, Kaffeine_PaletteUnsupported }, \
    { 840, Kaffeine_ObtainBestPenA }, \
    { 852, Kaffeine_SetRGB32 }, \
    { 882, Kaffeine_LoadRGB32 }, \
    { 900, Kaffeine_GetRGB32 }, \
    { 948, Kaffeine_ReleasePen }, \
    { 954, Kaffeine_PaletteUnsupported }, \
    { 996, Kaffeine_SetRGB32CM }, \
    { 1008, Kaffeine_PaletteUnsupported }
#endif

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_PALETTE_H
#define KAFFEINE_PALETTE_H
#include "kaffeine_thunks.h"
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
IPTR Kaffeine_FreeColorMap(struct KaffeineContext *, void *);
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
    { 708, Kaffeine_VideoControl }, \
    { 834, Kaffeine_PaletteUnsupported }, \
    { 840, Kaffeine_PaletteUnsupported }, \
    { 852, Kaffeine_SetRGB32 }, \
    { 882, Kaffeine_LoadRGB32 }, \
    { 900, Kaffeine_GetRGB32 }, \
    { 948, Kaffeine_PaletteUnsupported }, \
    { 954, Kaffeine_PaletteUnsupported }, \
    { 996, Kaffeine_SetRGB32CM }, \
    { 1008, Kaffeine_PaletteUnsupported }
#endif

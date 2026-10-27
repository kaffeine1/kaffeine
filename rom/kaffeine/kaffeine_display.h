/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_DISPLAY_H
#define KAFFEINE_DISPLAY_H
#include "kaffeine_thunks.h"
#ifdef __cplusplus
extern "C" {
#endif
IPTR thunk_gfx_GetDisplayInfoData(struct KaffeineContext *, void *);
IPTR thunk_gfx_FindDisplayInfo(struct KaffeineContext *, void *);
#ifdef __cplusplus
}
#endif
#define KAFFEINE_GRAPHICS_DISPLAY_THUNKS \
    { 726, thunk_gfx_FindDisplayInfo }, \
    { 756, thunk_gfx_GetDisplayInfoData }
#endif

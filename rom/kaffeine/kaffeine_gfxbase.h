/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_GFXBASE_H
#define KAFFEINE_GFXBASE_H
#include "kaffeine_thunks.h"
#define KAFFEINE_GFXBASE_SIZE 552
#define KAFFEINE_GFXBASE_DEFAULTFONT 154
#ifdef __cplusplus
extern "C" {
#endif
/* Only the supported positive fields are published. No broad native header,
 * version, font list, display or device pointers are fabricated. */
ULONG Kaffeine_SetupGraphicsBase(struct KaffeineContext *,
    const struct M68KThunkEntry *, ULONG,
    const struct M68KThunkEntry *, ULONG);
#ifdef __cplusplus
}
#endif
#endif

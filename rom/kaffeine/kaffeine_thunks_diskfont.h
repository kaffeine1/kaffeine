/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)
*/
#ifndef KAFFEINE_THUNKS_DISKFONT_H
#define KAFFEINE_THUNKS_DISKFONT_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif
IPTR thunk_diskfont_OpenDiskFont(struct KaffeineContext *ctx, void *cpu);
extern const struct M68KThunkEntry kaffeine_thunks_diskfont_manual[];
extern const ULONG kaffeine_thunks_diskfont_manual_count;
#ifdef __cplusplus
}
#endif
#endif

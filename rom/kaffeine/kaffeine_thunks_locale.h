/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    kaffeine.library: locale.library manual thunks
*/

#ifndef KAFFEINE_THUNKS_LOCALE_H
#define KAFFEINE_THUNKS_LOCALE_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct M68KThunkEntry kaffeine_thunks_locale_manual[];
extern const ULONG kaffeine_thunks_locale_manual_count;

void Kaffeine_LocaleCleanup(struct KaffeineContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_THUNKS_LOCALE_H */

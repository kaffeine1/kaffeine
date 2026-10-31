/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    console.device's no-window CONU_LIBRARY adapter.
*/
#ifndef KAFFEINE_THUNKS_CONSOLE_H
#define KAFFEINE_THUNKS_CONSOLE_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR kaffeine_console_RawKeyConvert(struct KaffeineContext *ctx, void *cpu);
extern const struct M68KThunkEntry kaffeine_thunks_console[];
extern const ULONG kaffeine_thunks_console_count;
/* Device vectors and the console functions through RemConSnipHook(). */
#define KAFFEINE_CONSOLE_VECTORS 12

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_THUNKS_CONSOLE_H */

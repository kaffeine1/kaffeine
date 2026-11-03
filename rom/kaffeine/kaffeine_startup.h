/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_STARTUP_H
#define KAFFEINE_STARTUP_H
#include "kaffeine_intern.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Workbench captures the native startup before its first DOS call. Publish
   runs later, in the stable driver, after the root Process shadow exists. */
BOOL Kaffeine_StartupPublish(struct KaffeineContext *);
BOOL Kaffeine_StartupReply(struct KaffeineContext *, ULONG, IPTR *);
/* Idempotent. Retire borrowed lock views before the native reply permits
   Workbench to free them. Guest storage stays quarantined with the arena. */
void Kaffeine_StartupFinish(struct KaffeineContext *);
#ifdef __cplusplus
}
#endif
#endif

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_REMEMBER_MEMORY_H
#define KAFFEINE_REMEMBER_MEMORY_H
#include <exec/types.h>
struct KaffeineContext;
#ifdef __cplusplus
extern "C" {
#endif
/* Private native ownership helpers, never guest addresses inferred from
 * edited Remember nodes. Published blocks cannot use unpublished rollback. */
ULONG Kaffeine_RememberMemoryAlloc(struct KaffeineContext *, ULONG, ULONG);
BOOL Kaffeine_RememberMemoryCheck(struct KaffeineContext *, ULONG, ULONG, BOOL releasing);
BOOL Kaffeine_RememberMemoryRelease(struct KaffeineContext *, ULONG, ULONG, BOOL unpublished);
BOOL Kaffeine_RememberMemoryForget(struct KaffeineContext *, ULONG, ULONG);
#ifdef __cplusplus
}
#endif
#endif

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_REMEMBER_H
#define KAFFEINE_REMEMBER_H
#include "kaffeine_thunks.h"
#define KAFFEINE_REMEMBER_LIMIT 4096
#define KAFFEINE_SIZEOF_REMEMBER 12
#ifdef __cplusplus
extern "C" {
#endif
IPTR thunk_intuition_AllocRemember(struct KaffeineContext *, void *);
IPTR thunk_intuition_FreeRemember(struct KaffeineContext *, void *);
/* Quiescent Context destruction only; frees private records without reading
 * or writing any guest key or node (their original module may be gone). */
void Kaffeine_RememberCleanup(struct KaffeineContext *);
#ifdef __cplusplus
}
#endif
#define KAFFEINE_INTUITION_REMEMBER_THUNKS \
    { 396, thunk_intuition_AllocRemember }, \
    { 408, thunk_intuition_FreeRemember }
#endif

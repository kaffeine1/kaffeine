/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Allocate and Deallocate on private, classic 68k MemHeader pools.
*/
#ifndef KAFFEINE_MEMHEADER_H
#define KAFFEINE_MEMHEADER_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_MemHeaderAllocate(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_MemHeaderDeallocate(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host dispatch tests. */
#define KAFFEINE_EXEC_MEMHEADER_THUNKS \
    { 186, Kaffeine_MemHeaderAllocate }, \
    { 192, Kaffeine_MemHeaderDeallocate },

#endif /* KAFFEINE_MEMHEADER_H */

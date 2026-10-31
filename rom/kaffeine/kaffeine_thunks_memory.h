/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Exec allocations and pools in the guest arena.
*/
#ifndef KAFFEINE_THUNKS_MEMORY_H
#define KAFFEINE_THUNKS_MEMORY_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR thunk_exec_AllocMem(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AllocAbs(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AllocEntry(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_FreeEntry(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_FreeMem(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AllocVec(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_FreeVec(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AvailMem(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_CreatePool(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AllocPooled(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_FreePooled(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_CopyMem(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_CopyMemQuick(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

/* DeletePool stays in KAFFEINE_EXEC_COMPAT_THUNKS at -702. */
#define KAFFEINE_EXEC_MEMORY_THUNKS \
    { 198, thunk_exec_AllocMem }, \
    { 204, thunk_exec_AllocAbs }, \
    { 210, thunk_exec_FreeMem }, \
    { 216, thunk_exec_AvailMem }, \
    { 222, thunk_exec_AllocEntry }, \
    { 228, thunk_exec_FreeEntry }, \
    { 684, thunk_exec_AllocVec }, \
    { 690, thunk_exec_FreeVec }, \
    { 696, thunk_exec_CreatePool }, \
    { 708, thunk_exec_AllocPooled }, \
    { 714, thunk_exec_FreePooled }, \
    { 624, thunk_exec_CopyMem }, \
    { 630, thunk_exec_CopyMemQuick },

#endif /* KAFFEINE_THUNKS_MEMORY_H */

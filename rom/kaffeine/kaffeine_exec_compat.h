/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec compatibility handlers whose guest pointers must not reach native Exec.
*/
#ifndef KAFFEINE_EXEC_COMPAT_H
#define KAFFEINE_EXEC_COMPAT_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_DeletePool(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddTask(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemTask(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddPort(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemPort(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FindPort(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_PutMsg(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RoutePutMsg(struct KaffeineContext *ctx, ULONG port, ULONG msg, ULONG d0);
IPTR Kaffeine_FindResident(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_OpenResource(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddResource(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemResource(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CacheClearU(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_CacheClearE(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_InitSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ObtainSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ReleaseSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AttemptSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ObtainSemaphoreList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ReleaseSemaphoreList(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_FindSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AddSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_RemSemaphore(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ObtainSemaphoreShared(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_AttemptSemaphoreShared(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_Procure(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_Vacate(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AVL_AddNode(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AVL_RemNodeByAddress(struct KaffeineContext *ctx, void *cpu);
IPTR thunk_exec_AVL_FindNode(struct KaffeineContext *ctx, void *cpu);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host dispatch tests. */
#define KAFFEINE_EXEC_COMPAT_THUNKS \
    { 96, Kaffeine_FindResident }, \
    { 282, Kaffeine_AddTask }, \
    { 288, Kaffeine_RemTask }, \
    { 354, Kaffeine_AddPort }, \
    { 360, Kaffeine_RemPort }, \
    { 366, Kaffeine_PutMsg }, \
    { 390, Kaffeine_FindPort }, \
    { 486, Kaffeine_AddResource }, \
    { 492, Kaffeine_RemResource }, \
    { 498, Kaffeine_OpenResource }, \
    { 636, Kaffeine_CacheClearU }, \
    { 642, Kaffeine_CacheClearE }, \
    { 540, Kaffeine_Procure }, \
    { 546, Kaffeine_Vacate }, \
    { 558, Kaffeine_InitSemaphore }, \
    { 564, Kaffeine_ObtainSemaphore }, \
    { 570, Kaffeine_ReleaseSemaphore }, \
    { 576, Kaffeine_AttemptSemaphore }, \
    { 582, Kaffeine_ObtainSemaphoreList }, \
    { 588, Kaffeine_ReleaseSemaphoreList }, \
    { 594, Kaffeine_FindSemaphore }, \
    { 600, Kaffeine_AddSemaphore }, \
    { 606, Kaffeine_RemSemaphore }, \
    { 678, Kaffeine_ObtainSemaphoreShared }, \
    { 702, Kaffeine_DeletePool }, \
    { 720, Kaffeine_AttemptSemaphoreShared }, \
    { 852, thunk_exec_AVL_AddNode }, \
    { 858, thunk_exec_AVL_RemNodeByAddress }, \
    { 870, thunk_exec_AVL_FindNode }

#endif /* KAFFEINE_EXEC_COMPAT_H */

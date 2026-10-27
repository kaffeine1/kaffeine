/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exec list operations on the classic 32-bit, big-endian guest layout.
*/
#ifndef KAFFEINE_LISTS_H
#define KAFFEINE_LISTS_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR Kaffeine_ListInsert(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListAddHead(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListAddTail(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListRemove(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListRemHead(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListRemTail(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListEnqueue(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListFindName(struct KaffeineContext *ctx, void *cpu);
IPTR Kaffeine_ListNewMinList(struct KaffeineContext *ctx, void *cpu);

/* Preserve the existing lookup for explicitly registered native list shadows. */
IPTR Kaffeine_FindNativeListName(struct KaffeineContext *ctx,
                              void *native_list, const char *name);

#ifdef __cplusplus
}
#endif

/* Shared by the production manual table and the host dispatch regression test. */
#define KAFFEINE_EXEC_LIST_THUNKS \
    { 234, Kaffeine_ListInsert }, \
    { 240, Kaffeine_ListAddHead }, \
    { 246, Kaffeine_ListAddTail }, \
    { 252, Kaffeine_ListRemove }, \
    { 258, Kaffeine_ListRemHead }, \
    { 264, Kaffeine_ListRemTail }, \
    { 270, Kaffeine_ListEnqueue }, \
    { 276, Kaffeine_ListFindName }, \
    { 828, Kaffeine_ListNewMinList },

#endif /* KAFFEINE_LISTS_H */

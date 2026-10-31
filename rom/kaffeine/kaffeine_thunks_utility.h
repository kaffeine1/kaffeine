/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    utility.library thunks: integer math that returns a second result in D1,
    guest TagItem operations and bounded guest string comparisons.
*/
#ifndef KAFFEINE_THUNKS_UTILITY_H
#define KAFFEINE_THUNKS_UTILITY_H

#include "kaffeine_thunks.h"
#include "kaffeine_callback.h"
#include "kaffeine_format.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR kaffeine_utility_FindTagItem(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_GetTagData(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_PackBoolTags(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_NextTagItem(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_FilterTagChanges(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_MapTags(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_AllocateTagItems(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_CloneTagItems(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_FreeTagItems(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_RefreshTagItemClones(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_TagInArray(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_FilterTagItems(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_Stricmp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_Strnicmp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_Amiga2Date(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_Date2Amiga(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_CheckDate(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_CallHookPkt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_SDivMod32(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_UDivMod32(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_SMult64(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_utility_UMult64(struct KaffeineContext *ctx, void *cpu);

/* The manual utility.library table, preferred over the generated thunks */
extern const struct M68KThunkEntry kaffeine_thunks_utility_manual[];
extern const ULONG kaffeine_thunks_utility_manual_count;

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_THUNKS_UTILITY_H */

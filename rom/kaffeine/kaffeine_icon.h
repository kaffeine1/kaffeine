/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine.library — icon.library metadata-only DiskObject boundary
*/
#ifndef KAFFEINE_ICON_H
#define KAFFEINE_ICON_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

IPTR kaffeine_icon_GetDiskObject(struct KaffeineContext *ctx, void *cpu);
/* Exact owned snapshot only. Positions, stack and bounded string/ToolTypes
   metadata may change; image/drawer pointers, Gadget fields and type stay
   immutable. Native images/extensions are retained through a real duplicate. */
IPTR kaffeine_icon_PutDiskObject(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_FreeDiskObject(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_FindToolType(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_MatchToolValue(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_GetDefDiskObject(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_GetDiskObjectNew(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_icon_NewDiskObject(struct KaffeineContext *ctx, void *cpu);
/* Trusted native lookup for the scheduler's fixed icon-I/O broker route.
   Only 78/84/120/132 are admitted; helpers never recurse public wrappers. */
KaffeineThunkFn Kaffeine_IconIOThunk(ULONG lvo);
/* Every other icon.library call takes or returns native pointer-rich
   structures; it stops the program without looking at its arguments. */
IPTR kaffeine_icon_Unsupported(struct KaffeineContext *ctx, void *cpu);

/* Release every DiskObject still owned by the program, also after a stop. */
void Kaffeine_IconsCleanup(struct KaffeineContext *ctx);

extern const struct M68KThunkEntry kaffeine_thunks_icon_manual[];
extern const ULONG kaffeine_thunks_icon_manual_count;

/* All 21 icon.library LVOs, so no generated thunk is ever reached. */
#define KAFFEINE_ICON_THUNKS \
    {  54, kaffeine_icon_Unsupported },       /* FreeFreeList */ \
    {  72, kaffeine_icon_Unsupported },       /* AddFreeList */ \
    {  78, kaffeine_icon_GetDiskObject }, \
    {  84, kaffeine_icon_PutDiskObject }, \
    {  90, kaffeine_icon_FreeDiskObject }, \
    {  96, kaffeine_icon_FindToolType }, \
    { 102, kaffeine_icon_MatchToolValue }, \
    { 108, kaffeine_icon_Unsupported },       /* BumpRevision */ \
    { 120, kaffeine_icon_GetDefDiskObject }, \
    { 126, kaffeine_icon_Unsupported },       /* PutDefDiskObject */ \
    { 132, kaffeine_icon_GetDiskObjectNew }, \
    { 138, kaffeine_icon_Unsupported },       /* DeleteDiskObject */ \
    { 150, kaffeine_icon_Unsupported },       /* DupDiskObjectA */ \
    { 156, kaffeine_icon_Unsupported },       /* IconControlA */ \
    { 162, kaffeine_icon_Unsupported },       /* DrawIconStateA */ \
    { 168, kaffeine_icon_Unsupported },       /* GetIconRectangleA */ \
    { 174, kaffeine_icon_NewDiskObject }, \
    { 180, kaffeine_icon_Unsupported },       /* GetIconTagList */ \
    { 186, kaffeine_icon_Unsupported },       /* PutIconTagList */ \
    { 192, kaffeine_icon_Unsupported },       /* LayoutIconA */ \
    { 198, kaffeine_icon_Unsupported }        /* ChangeToSelectedIconColor */

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_ICON_H */

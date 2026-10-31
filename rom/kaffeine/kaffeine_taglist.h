/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Walk a 68k TagItem list where it lies in guest memory.
*/
#ifndef KAFFEINE_TAGLIST_H
#define KAFFEINE_TAGLIST_H

#include <exec/types.h>

struct KaffeineContext;

#ifdef __cplusplus
extern "C" {
#endif

/* A guest TagItem is 8 bytes: ti_Tag and ti_Data as big-endian longwords.
   The walk follows AROS NextTagItem(): TAG_DONE ends the list, TAG_IGNORE
   is passed over, TAG_MORE continues at ti_Data (a NULL ti_Data ends the
   list) and TAG_SKIP passes over itself and ti_Data more items. It never
   converts ti_Data: what a value means depends on the tag and the API. */
struct KaffeineTagWalk
{
    ULONG list;          /* guest address of the list */
    ULONG next;          /* guest address of the next item, 0 at the end */
    ULONG steps;         /* steps from list to next, control tags included */
    ULONG limit;         /* in a cycle, the steps before an item repeats */
    ULONG hare;          /* two steps ahead per step, to find cycles */
    BOOL  hare_stopped;
};

enum
{
    KAFFEINE_TAG_ITEM,    /* *item is the guest address of a user tag */
    KAFFEINE_TAG_END,     /* TAG_DONE, or TAG_MORE with a NULL ti_Data */
    KAFFEINE_TAG_BAD      /* an item outside guest memory, or a cycle after
                            every item in it was returned once */
};

void Kaffeine_TagWalkInit(struct KaffeineTagWalk *walk, ULONG list);
int Kaffeine_TagWalkNext(struct KaffeineContext *ctx, struct KaffeineTagWalk *walk,
                        ULONG *item);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_TAGLIST_H */

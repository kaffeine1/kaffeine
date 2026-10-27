/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Walk a 68k TagItem list where it lies in guest memory, with the
    semantics of compiler/arossupport/libnexttagitem.c and the bounds and
    cycle checks that a list from 68k code needs.
*/

#include <utility/tagitem.h>

#include "kaffeine_intern.h"
#include "kaffeine_taglist.h"

enum { STEP_ITEM, STEP_CONTROL, STEP_END, STEP_BAD };

/* One step of NextTagItem() at the guest item pos. *next is set for an
   item or a control tag, and left alone at the end or for a bad item. */
static int tag_step(struct KaffeineContext *ctx, ULONG pos, ULONG *next)
{
    ULONG tag, data;

    if (!pos || pos >= ctx->mem_size || ctx->mem_size - pos < 8)
        return STEP_BAD;
    tag = m68k_read32(ctx, pos);
    data = m68k_read32(ctx, pos + 4);
    switch (tag)
    {
        case TAG_DONE:
            return STEP_END;
        case TAG_IGNORE:
            *next = pos + 8;
            return STEP_CONTROL;
        case TAG_MORE:
            if (!data)
                return STEP_END;
            *next = data;
            return STEP_CONTROL;
        case TAG_SKIP:
            /* The item after the skipped ones must start in guest memory;
               the division keeps (data + 1) * 8 from overflowing. */
            if (data >= (ctx->mem_size - pos) / 8)
                return STEP_BAD;
            *next = pos + (data + 1) * 8;
            return STEP_CONTROL;
        default:
            *next = pos + 8;
            return STEP_ITEM;
    }
}

void Kaffeine_TagWalkInit(struct KaffeineTagWalk *walk, ULONG list)
{
    walk->list = list;
    walk->next = list;
    walk->steps = 0;
    walk->limit = 0;
    walk->hare = list;
    walk->hare_stopped = !list;
}

/* The hare stops for good at the end of the list or at a bad item; the
   walk itself cannot loop then, and stops at the same place. */
static void hare_steps(struct KaffeineContext *ctx, struct KaffeineTagWalk *walk)
{
    int i;

    for (i = 0; i < 2 && !walk->hare_stopped; i++)
    {
        int kind = tag_step(ctx, walk->hare, &walk->hare);

        if (kind == STEP_END || kind == STEP_BAD)
            walk->hare_stopped = TRUE;
    }
}

/* The hare has met the walk after m steps, so the walk is in a cycle
   whose length divides m. The walk repeats an item for the first time
   after mu + lambda steps: mu steps lead into the cycle and lambda go
   round it. Every step counted here was made by the hare already, so
   none of them ends the list. */
static ULONG cycle_steps(struct KaffeineContext *ctx, const struct KaffeineTagWalk *walk)
{
    ULONG from_list = walk->list, from_meeting = walk->next, pos;
    ULONG mu = 0, lambda = 0;

    /* m steps apart, the two meet first where the cycle begins */
    while (from_list != from_meeting)
    {
        tag_step(ctx, from_list, &from_list);
        tag_step(ctx, from_meeting, &from_meeting);
        mu++;
    }
    pos = from_list;
    do
    {
        tag_step(ctx, pos, &pos);
        lambda++;
    } while (pos != from_list);
    return mu + lambda;
}

int Kaffeine_TagWalkNext(struct KaffeineContext *ctx, struct KaffeineTagWalk *walk,
                        ULONG *item)
{
    for (;;)
    {
        ULONG pos = walk->next, next = 0;
        int kind;

        if (!pos)
            return KAFFEINE_TAG_END;
        /* In a cycle, stop where the walk would repeat an item */
        if (walk->limit && walk->steps >= walk->limit)
        {
            walk->next = 0;
            return KAFFEINE_TAG_BAD;
        }
        kind = tag_step(ctx, pos, &next);
        if (kind == STEP_END || kind == STEP_BAD)
        {
            walk->next = 0;
            return kind == STEP_END ? KAFFEINE_TAG_END : KAFFEINE_TAG_BAD;
        }
        walk->next = next;
        walk->steps++;

        /* Floyd's cycle detection: after k steps of the walk the hare has
           made 2k. They meet only if TAG_MORE leads back to an item that
           was already passed, which needs no limit on the length of a list
           that ends. They can meet before the walk has passed every item of
           the cycle, so the walk goes on until it would repeat one. */
        if (!walk->limit)
        {
            hare_steps(ctx, walk);
            if (!walk->hare_stopped && walk->hare == walk->next)
                walk->limit = cycle_steps(ctx, walk);
        }

        if (kind == STEP_ITEM)
        {
            *item = pos;
            return KAFFEINE_TAG_ITEM;
        }
    }
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Exec lists owned by the 68k guest. Links remain guest addresses, including
    the overlapping head and tail sentinels. No host structure is overlaid on
    guest memory, and returned nodes retain their original guest identity.
*/

#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_lists.h"

enum
{
    LIST_HEAD = 0,
    LIST_TAIL = 4,
    LIST_TAILPRED = 8,
    MINLIST_SIZE = 12,
    NODE_SUCC = 0,
    NODE_PRED = 4,
    MINNODE_SIZE = 8,
    NODE_PRI = 9,
    NODE_NAME = 10,
    NODE_SIZE = 14
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    /* Subtraction avoids wrapping a bad 32-bit address before the check. */
    return addr != 0 && size <= ctx->mem_size && addr <= ctx->mem_size - size;
}

static void *native_shadow(struct KaffeineContext *ctx, ULONG addr)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr)
            return ctx->shadow_map[i].native_ptr;
    return NULL;
}

static BOOL guest_struct(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    /* Mutating native list shadows needs a separate synchronization policy. */
    return guest_range(ctx, addr, size) && !native_shadow(ctx, addr);
}

static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    if (!guest_range(ctx, addr, 1))
        return NULL;
    if (!memchr(ctx->mem + addr, 0, ctx->mem_size - addr))
        return NULL;
    return (const char *)ctx->mem + addr;
}

static BOOL insert_between(struct KaffeineContext *ctx, ULONG node,
                           ULONG pred, ULONG next)
{
    if (node == pred || node == next ||
        !guest_struct(ctx, node, MINNODE_SIZE) ||
        !guest_struct(ctx, pred, MINNODE_SIZE) ||
        !guest_struct(ctx, next, MINNODE_SIZE))
        return FALSE;

    /* All destinations have been checked before any link is changed. */
    m68k_write32(ctx, node + NODE_SUCC, next);
    m68k_write32(ctx, node + NODE_PRED, pred);
    m68k_write32(ctx, pred + NODE_SUCC, node);
    m68k_write32(ctx, next + NODE_PRED, node);
    return TRUE;
}

static BOOL remove_node(struct KaffeineContext *ctx, ULONG node)
{
    ULONG pred, next;

    if (!guest_struct(ctx, node, MINNODE_SIZE))
        return FALSE;
    pred = m68k_read32(ctx, node + NODE_PRED);
    next = m68k_read32(ctx, node + NODE_SUCC);
    if (node == pred || node == next ||
        !guest_struct(ctx, pred, MINNODE_SIZE) ||
        !guest_struct(ctx, next, MINNODE_SIZE))
        return FALSE;

    m68k_write32(ctx, pred + NODE_SUCC, next);
    m68k_write32(ctx, next + NODE_PRED, pred);
    /* Exec leaves the removed node's own links untouched. */
    return TRUE;
}

static void add_head(struct KaffeineContext *ctx, ULONG list, ULONG node)
{
    if (guest_struct(ctx, list, MINLIST_SIZE))
        insert_between(ctx, node, list + LIST_HEAD,
                       m68k_read32(ctx, list + LIST_HEAD));
}

static void add_tail(struct KaffeineContext *ctx, ULONG list, ULONG node)
{
    if (guest_struct(ctx, list, MINLIST_SIZE))
        insert_between(ctx, node, m68k_read32(ctx, list + LIST_TAILPRED),
                       list + LIST_TAIL);
}

IPTR Kaffeine_ListInsert(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), node = THUNK_A(1), pred = THUNK_A(2);

    if (!pred)
        add_head(ctx, list, node);
    else if (guest_struct(ctx, pred, MINNODE_SIZE))
    {
        ULONG next = m68k_read32(ctx, pred + NODE_SUCC);

        if (next)
            /* A normal predecessor also permits Insert(NULL, node, pred). */
            insert_between(ctx, node, pred, next);
        else
            /* Inserting after the tail sentinel means appending. */
            add_tail(ctx, list, node);
    }
    return 0;
}

IPTR Kaffeine_ListAddHead(struct KaffeineContext *ctx, void *cpu)
{
    add_head(ctx, THUNK_A(0), THUNK_A(1));
    return 0;
}

IPTR Kaffeine_ListAddTail(struct KaffeineContext *ctx, void *cpu)
{
    add_tail(ctx, THUNK_A(0), THUNK_A(1));
    return 0;
}

IPTR Kaffeine_ListRemove(struct KaffeineContext *ctx, void *cpu)
{
    remove_node(ctx, THUNK_A(1));
    return 0;
}

IPTR Kaffeine_ListRemHead(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), node;

    if (!guest_struct(ctx, list, MINLIST_SIZE))
        return 0;
    node = m68k_read32(ctx, list + LIST_HEAD);
    if (!guest_struct(ctx, node, MINNODE_SIZE) ||
        !m68k_read32(ctx, node + NODE_SUCC))
        return 0;
    return remove_node(ctx, node) ? node : 0;
}

IPTR Kaffeine_ListRemTail(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), node;

    if (!guest_struct(ctx, list, MINLIST_SIZE))
        return 0;
    node = m68k_read32(ctx, list + LIST_TAILPRED);
    if (!guest_struct(ctx, node, MINNODE_SIZE) ||
        !m68k_read32(ctx, node + NODE_PRED))
        return 0;
    return remove_node(ctx, node) ? node : 0;
}

IPTR Kaffeine_ListEnqueue(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), node = THUNK_A(1), next, remaining;
    int priority;

    if (!guest_struct(ctx, list, MINLIST_SIZE) ||
        !guest_struct(ctx, node, NODE_SIZE))
        return 0;
    priority = ctx->mem[node + NODE_PRI];
    if (priority >= 128)
        priority -= 256;
    next = m68k_read32(ctx, list + LIST_HEAD);

    /* A valid list cannot contain more non-overlapping MinNodes than this. */
    remaining = ctx->mem_size / MINNODE_SIZE;
    while (remaining-- && guest_struct(ctx, next, MINNODE_SIZE))
    {
        ULONG succ = m68k_read32(ctx, next + NODE_SUCC);
        int next_priority;

        if (!succ)
        {
            insert_between(ctx, node, m68k_read32(ctx, next + NODE_PRED), next);
            return 0;
        }
        if (!guest_struct(ctx, next, NODE_SIZE))
            return 0;
        next_priority = ctx->mem[next + NODE_PRI];
        if (next_priority >= 128)
            next_priority -= 256;
        if (priority > next_priority)
        {
            insert_between(ctx, node, m68k_read32(ctx, next + NODE_PRED), next);
            return 0;
        }
        next = succ;
    }
    return 0;
}

IPTR Kaffeine_ListFindName(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0), node, remaining;
    const char *name = guest_string(ctx, THUNK_A(1));
    void *native;

    if (!list || !name)
        return 0;
    native = native_shadow(ctx, list);
    if (native)
        return Kaffeine_FindNativeListName(ctx, native, name);
    if (!guest_struct(ctx, list, MINNODE_SIZE))
        return 0;

    node = m68k_read32(ctx, list + LIST_HEAD);
    remaining = ctx->mem_size / MINNODE_SIZE;
    while (remaining-- && guest_struct(ctx, node, MINNODE_SIZE))
    {
        const char *node_name;
        ULONG next = m68k_read32(ctx, node + NODE_SUCC);

        if (!next || !guest_struct(ctx, node, NODE_SIZE))
            return 0;
        node_name = guest_string(ctx, m68k_read32(ctx, node + NODE_NAME));
        if (node_name && !strcmp(node_name, name))
            return node;
        node = next;
    }
    return 0;
}

IPTR Kaffeine_ListNewMinList(struct KaffeineContext *ctx, void *cpu)
{
    ULONG list = THUNK_A(0);

    if (guest_struct(ctx, list, MINLIST_SIZE))
    {
        m68k_write32(ctx, list + LIST_HEAD, list + LIST_TAIL);
        m68k_write32(ctx, list + LIST_TAIL, 0);
        m68k_write32(ctx, list + LIST_TAILPRED, list + LIST_HEAD);
    }
    return 0;
}

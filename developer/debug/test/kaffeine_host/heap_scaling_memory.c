/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Compile the production heap with basic-block coverage and inspect its
    native invariants between batches, without adding production test hooks.
*/
#include "kaffeine_memory.c"

unsigned long test_heap_record_size(void)
{
    return sizeof(struct M68KHeapAllocation);
}

static BOOL check_tree(struct KaffeineContext *ctx, struct M68KHeapAllocation *node,
                        struct M68KHeapAllocation *parent, ULONG *cursor,
                        struct M68KHeapAllocation **free_node, ULONG *largest,
                        unsigned *height)
{
    ULONG left_max = 0, right_max = 0, maximum;
    unsigned left_height = 0, right_height = 0;

    if (!node)
    {
        *largest = 0;
        *height = 0;
        return TRUE;
    }
    if (node->parent != parent ||
        !check_tree(ctx, node->left, node, cursor, free_node, &left_max, &left_height) ||
        node->block != *cursor || !heap_span_valid(ctx, node))
        return FALSE;
    if (node->kind == HEAP_FREE)
    {
        if (node != *free_node || !heap_free_valid(ctx, node) || node->pool ||
            (node->next && (node->next->prev != node || node->block + node->size == node->next->block)))
            return FALSE;
        *free_node = node->next;
    }
    else if (node->pool)
    {
        struct M68KHeapAllocation *owner = pool_find(ctx, node->pool);
        if (!owner || (node->prev ? node->prev->next != node : owner->next != node) ||
            (node->next && node->next->prev != node))
            return FALSE;
    }
    else if (node->kind == HEAP_POOL && node->next && node->next->prev)
        return FALSE;
    *cursor += node->size;
    if (!check_tree(ctx, node->right, node, cursor, free_node, &right_max, &right_height) ||
        left_height > right_height + 1 || right_height > left_height + 1)
        return FALSE;
    *height = 1 + (left_height > right_height ? left_height : right_height);
    maximum = node->kind == HEAP_FREE ? node->size : 0;
    if (left_max > maximum) maximum = left_max;
    if (right_max > maximum) maximum = right_max;
    *largest = maximum;
    return node->height == *height && node->largest_free == maximum;
}

BOOL test_heap_invariants(struct KaffeineContext *ctx)
{
    ULONG cursor = ctx->heap_start, largest = 0;
    unsigned height = 0;
    struct M68KHeapAllocation *free_node = ctx->heap_first_free;

    return heap_ready(ctx) && (!free_node || !free_node->prev) &&
           check_tree(ctx, ctx->heap_allocations, NULL, &cursor, &free_node, &largest, &height) &&
           cursor == ctx->heap_end && !free_node;
}

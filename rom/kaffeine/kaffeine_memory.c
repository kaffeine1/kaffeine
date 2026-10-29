/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    kaffeine.library: containment memory, native heap metadata and HUNK loading
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/bptr.h>
#include <dos/doshunks.h>
#include <string.h>
#include <stdio.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_thunks.h"

enum
{
    HEAP_FREE, HEAP_ALLOCATION, HEAP_POOL, HEAP_TAGITEMS, HEAP_LOCALE,
    HEAP_ABSOLUTE, HEAP_MEMLIST, HEAP_ENTRY, HEAP_ACHAIN, HEAP_MIRROR, HEAP_RESERVED
};

/* The native intervals partition the arena. Guest headers are only checked
   mirrors, so forged links cannot change boundaries or ownership. */
struct M68KHeapAllocation
{
    struct M68KHeapAllocation *left, *right, *parent;
    /* Free-list links, owned-member links, or next = first member for an owner. */
    struct M68KHeapAllocation *prev, *next;
    ULONG block, size, requested, pool, largest_free;
    UBYTE height, kind;
};

#define GUEST_POOL_MAGIC 0x504F4F4CUL
#define GUEST_POOL_SIZE  16

static UBYTE heap_height(struct M68KHeapAllocation *node)
{
    return node ? node->height : 0;
}

static void heap_update(struct M68KHeapAllocation *node)
{
    UBYTE left = heap_height(node->left), right = heap_height(node->right);
    ULONG largest = node->kind == HEAP_FREE ? node->size : 0;

    node->height = 1 + (left > right ? left : right);
    if (node->left && node->left->largest_free > largest)
        largest = node->left->largest_free;
    if (node->right && node->right->largest_free > largest)
        largest = node->right->largest_free;
    node->largest_free = largest;
}

static void heap_replace(struct KaffeineContext *ctx, struct M68KHeapAllocation *node,
                         struct M68KHeapAllocation *replacement)
{
    if (!node->parent)
        ctx->heap_allocations = replacement;
    else if (node == node->parent->left)
        node->parent->left = replacement;
    else
        node->parent->right = replacement;
    if (replacement)
        replacement->parent = node->parent;
}

static struct M68KHeapAllocation *heap_rotate(struct KaffeineContext *ctx,
                                              struct M68KHeapAllocation *node, BOOL left)
{
    struct M68KHeapAllocation *top = left ? node->right : node->left;

    heap_replace(ctx, node, top);
    if (left)
    {
        node->right = top->left;
        if (node->right) node->right->parent = node;
        top->left = node;
    }
    else
    {
        node->left = top->right;
        if (node->left) node->left->parent = node;
        top->right = node;
    }
    node->parent = top;
    heap_update(node);
    heap_update(top);
    return top;
}

static void heap_rebalance(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    while (node)
    {
        int balance;

        heap_update(node);
        balance = (int)heap_height(node->left) - heap_height(node->right);
        if (balance > 1)
        {
            if (heap_height(node->left->right) > heap_height(node->left->left))
                heap_rotate(ctx, node->left, TRUE);
            node = heap_rotate(ctx, node, FALSE);
        }
        else if (balance < -1)
        {
            if (heap_height(node->right->left) > heap_height(node->right->right))
                heap_rotate(ctx, node->right, FALSE);
            node = heap_rotate(ctx, node, TRUE);
        }
        node = node->parent;
    }
}

static void heap_insert(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    struct M68KHeapAllocation **link = &ctx->heap_allocations, *parent = NULL;

    while (*link)
    {
        parent = *link;
        link = node->block < parent->block ? &parent->left : &parent->right;
    }
    *link = node;
    node->parent = parent;
    node->height = 1;
    heap_rebalance(ctx, node);
}

static void heap_remove(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    struct M68KHeapAllocation *start = node->parent;

    if (!node->left || !node->right)
        heap_replace(ctx, node, node->left ? node->left : node->right);
    else
    {
        struct M68KHeapAllocation *successor = node->right;

        while (successor->left)
            successor = successor->left;
        if (successor->parent == node)
            start = successor;
        else
        {
            start = successor->parent;
            heap_replace(ctx, successor, successor->right);
            successor->right = node->right;
            successor->right->parent = successor;
        }
        heap_replace(ctx, node, successor);
        successor->left = node->left;
        successor->left->parent = successor;
        heap_update(successor);
    }
    heap_rebalance(ctx, start);
}

static struct M68KHeapAllocation *heap_find_block(struct KaffeineContext *ctx, ULONG block)
{
    struct M68KHeapAllocation *node = ctx->heap_allocations;

    while (node)
    {
        if (block == node->block)
            return node;
        node = block < node->block ? node->left : node->right;
    }
    return NULL;
}

static struct M68KHeapAllocation *heap_find(struct KaffeineContext *ctx, ULONG addr)
{
    struct M68KHeapAllocation *node = heap_find_block(ctx, addr);

    /* Absolute allocations have no private guest header before their payload. */
    if (node && node->kind == HEAP_ABSOLUTE) return node;
    if (addr < KAFFEINE_HEAP_HDR) return NULL;
    node = heap_find_block(ctx, addr - KAFFEINE_HEAP_HDR);
    return node && node->kind != HEAP_FREE && node->kind != HEAP_ABSOLUTE &&
           node->kind != HEAP_RESERVED ? node : NULL;
}

/* Skip entire allocated subtrees when locating the free insertion point. */
static struct M68KHeapAllocation *heap_free_neighbor(struct M68KHeapAllocation *node,
                                                     BOOL after)
{
    struct M68KHeapAllocation *branch = after ? node->right : node->left;

    while (!branch || !branch->largest_free)
    {
        struct M68KHeapAllocation *parent = node->parent;

        if (!parent) return NULL;
        if (node == (after ? parent->left : parent->right))
        {
            if (parent->kind == HEAP_FREE) return parent;
            branch = after ? parent->right : parent->left;
        }
        node = parent;
    }
    node = branch;
    for (;;)
    {
        branch = after ? node->left : node->right;
        if (branch && branch->largest_free)
            node = branch;
        else if (node->kind == HEAP_FREE)
            return node;
        else
            node = after ? node->right : node->left;
    }
}

static BOOL heap_bounds_valid(struct KaffeineContext *ctx)
{
    return ctx && ctx->mem && ctx->heap_start && ctx->heap_start < ctx->heap_end &&
           ctx->heap_end <= ctx->mem_size && !((ctx->heap_start | ctx->heap_end) & 7);
}

static BOOL heap_span_valid(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    return node->block >= ctx->heap_start && node->block < ctx->heap_end &&
           node->size >= KAFFEINE_HEAP_HDR && node->size <= ctx->heap_end - node->block;
}

static BOOL heap_free_valid(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    if (!node) return TRUE;
    return heap_span_valid(ctx, node) && m68k_read32(ctx, node->block) == node->size &&
           m68k_read32(ctx, node->block + 4) == (node->next ? node->next->block : 0);
}

static BOOL heap_ready(struct KaffeineContext *ctx)
{
    return heap_bounds_valid(ctx) && ctx->heap_allocations &&
           ctx->heap_free == (ctx->heap_first_free ? ctx->heap_first_free->block : 0) &&
           heap_free_valid(ctx, ctx->heap_first_free);
}

static void heap_write_free(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    if (!node) return;
    m68k_write32(ctx, node->block, node->size);
    m68k_write32(ctx, node->block + 4, node->next ? node->next->block : 0);
}

/* On an Amiga no memory answers at $DFF000-$DFFFFF, the custom chips do,
   and the CPU callbacks keep that window for them. The heap reserves the
   part inside its bounds: bounds[0] to bounds[1] and bounds[2] to bounds[3]
   are free, bounds[1] to bounds[2] is reserved, and any of them can be empty. */
static void heap_layout(struct KaffeineContext *ctx, ULONG bounds[4])
{
    ULONG start = CUSTOM_BASE, end = CUSTOM_BASE + CUSTOM_SIZE;

    if (start < ctx->heap_start) start = ctx->heap_start;
    if (end > ctx->heap_end) end = ctx->heap_end;
    if (start >= end) start = end = ctx->heap_end;
    bounds[0] = ctx->heap_start;
    bounds[1] = start;
    bounds[2] = end;
    bounds[3] = ctx->heap_end;
}

/* Free intervals and the reserved window only: no allocation is live. */
static BOOL heap_empty(struct M68KHeapAllocation *node)
{
    return !node || ((node->kind == HEAP_FREE || node->kind == HEAP_RESERVED) &&
                     heap_empty(node->left) && heap_empty(node->right));
}

static void heap_destroy(struct M68KHeapAllocation *node)
{
    if (!node) return;
    heap_destroy(node->left);
    heap_destroy(node->right);
    FreeMem(node, sizeof(*node));
}

/* Initialize or resize an empty native heap. Bounds are native configuration,
   never read from a guest structure. Existing allocations forbid a reset. */
BOOL Kaffeine_HeapInit(struct KaffeineContext *ctx)
{
    struct M68KHeapAllocation *nodes[3] = { NULL, NULL, NULL }, *previous = NULL;
    ULONG bounds[4];
    int i;

    if (!heap_bounds_valid(ctx) || !heap_empty(ctx->heap_allocations)) return FALSE;
    heap_layout(ctx, bounds);
    /* Allocate every record first, so a failed resize keeps the old heap. */
    for (i = 0; i < 3; i++)
    {
        if (bounds[i] == bounds[i + 1]) continue;
        nodes[i] = (struct M68KHeapAllocation *)AllocMem(sizeof(*nodes[i]),
                                                        MEMF_PRIVATE | MEMF_CLEAR);
        if (!nodes[i])
        {
            while (i--)
                if (nodes[i]) FreeMem(nodes[i], sizeof(*nodes[i]));
            return FALSE;
        }
        nodes[i]->block = bounds[i];
        nodes[i]->size = bounds[i + 1] - bounds[i];
        nodes[i]->kind = i == 1 ? HEAP_RESERVED : HEAP_FREE;
    }
    heap_destroy(ctx->heap_allocations);
    ctx->heap_allocations = ctx->heap_first_free = NULL;
    for (i = 0; i < 3; i++)
    {
        if (!nodes[i]) continue;
        heap_insert(ctx, nodes[i]);
        if (nodes[i]->kind != HEAP_FREE) continue;
        nodes[i]->prev = previous;
        if (previous) previous->next = nodes[i];
        else ctx->heap_first_free = nodes[i];
        previous = nodes[i];
    }
    heap_write_free(ctx, nodes[0]);
    heap_write_free(ctx, nodes[2]);
    ctx->heap_free = ctx->heap_first_free ? ctx->heap_first_free->block : 0;
    return TRUE;
}

struct KaffeineContext *Kaffeine_CreateContext(struct KaffeineLibBase *base)
{
    UBYTE *mem = (UBYTE *)AllocMem(KAFFEINE_MEM_SIZE + KAFFEINE_MEM_GUARD, MEMF_CLEAR | MEMF_31BIT);
    struct KaffeineContext *ctx;

    if (!mem) return NULL;
    ctx = (struct KaffeineContext *)AllocMem(sizeof(*ctx), MEMF_CLEAR);
    if (!ctx)
    {
        FreeMem(mem, KAFFEINE_MEM_SIZE + KAFFEINE_MEM_GUARD);
        return NULL;
    }
    ctx->mem = mem;
    ctx->mem_size = KAFFEINE_MEM_SIZE;
    ctx->emuBase = base;
    ctx->stack_top = KAFFEINE_MEM_SIZE;
    ctx->stack_bottom = KAFFEINE_MEM_SIZE - KAFFEINE_STACK_SIZE;
    ctx->heap_start = KAFFEINE_HEAP_BASE;
    ctx->heap_end = ctx->stack_bottom;
    if (!Kaffeine_HeapInit(ctx))
    {
        FreeMem(ctx, sizeof(*ctx));
        FreeMem(mem, KAFFEINE_MEM_SIZE + KAFFEINE_MEM_GUARD);
        return NULL;
    }
    return ctx;
}

void Kaffeine_DestroyContext(struct KaffeineContext *ctx)
{
    if (!ctx) return;
    heap_destroy(ctx->heap_allocations);
    if (ctx->mem) FreeMem(ctx->mem, ctx->mem_size + KAFFEINE_MEM_GUARD);
    FreeMem(ctx, sizeof(*ctx));
}

static struct M68KHeapAllocation *heap_allocate(struct KaffeineContext *ctx, ULONG size,
                                                ULONG requirements)
{
    struct M68KHeapAllocation *chunk, *allocation;
    ULONG needed;

    if (size > (ULONG)~0UL - KAFFEINE_HEAP_HDR - 7 || !heap_ready(ctx))
        return NULL;
    needed = ((size + 7) & ~7UL) + KAFFEINE_HEAP_HDR;
    for (chunk = ctx->heap_first_free; chunk; chunk = chunk->next)
    {
        ULONG remainder;

        if (!heap_free_valid(ctx, chunk)) return NULL;
        if (chunk->size < needed) continue;
        remainder = chunk->size - needed;
        if (remainder >= KAFFEINE_HEAP_MIN_SPLIT)
        {
            allocation = (struct M68KHeapAllocation *)AllocMem(sizeof(*allocation),
                                                              MEMF_PRIVATE | MEMF_CLEAR);
            if (!allocation) return NULL;
            allocation->block = chunk->block + remainder;
            allocation->size = needed;
            allocation->kind = HEAP_ALLOCATION;
            chunk->size = remainder;
            heap_rebalance(ctx, chunk);
            heap_insert(ctx, allocation);
            heap_write_free(ctx, chunk);
        }
        else
        {
            /* Reuse the free interval's record, so whole-block allocation
               needs no additional native bookkeeping. */
            if (chunk->prev)
            {
                if (!heap_free_valid(ctx, chunk->prev)) return NULL;
                chunk->prev->next = chunk->next;
                heap_write_free(ctx, chunk->prev);
            }
            else
                ctx->heap_first_free = chunk->next;
            if (chunk->next) chunk->next->prev = chunk->prev;
            allocation = chunk;
            allocation->kind = HEAP_ALLOCATION;
            allocation->prev = allocation->next = NULL;
            heap_rebalance(ctx, allocation);
        }
        allocation->requested = size;
        m68k_write32(ctx, allocation->block, allocation->size);
        m68k_write32(ctx, allocation->block + 4, 0);
        ctx->heap_free = ctx->heap_first_free ? ctx->heap_first_free->block : 0;
        if (requirements & MEMF_CLEAR)
            memset(ctx->mem + allocation->block + KAFFEINE_HEAP_HDR, 0,
                   allocation->size - KAFFEINE_HEAP_HDR);
        return allocation;
    }
    return NULL;
}

ULONG Kaffeine_HeapAlloc(struct KaffeineContext *ctx, ULONG size, ULONG requirements)
{
    struct M68KHeapAllocation *allocation = heap_allocate(ctx, size, requirements);

    return allocation ? allocation->block + KAFFEINE_HEAP_HDR : 0;
}

ULONG Kaffeine_HeapAllocAbs(struct KaffeineContext *ctx, ULONG size, ULONG addr)
{
    struct M68KHeapAllocation *node, *chunk = NULL, *prefix = NULL, *suffix = NULL;
    struct M68KHeapAllocation *previous, *following;
    ULONG start, needed, before, after;

    if (!size || !ctx->mem || addr >= ctx->mem_size || ctx->mem_size - addr < size ||
        !heap_ready(ctx)) return 0;
    start = addr & ~7UL;
    if (size > (ULONG)~0UL - (addr - start) - 7) return 0;
    needed = (size + (addr - start) + 7) & ~7UL;
    if (start < ctx->heap_start || start >= ctx->heap_end ||
        ctx->heap_end - start < needed) return 0;
    /* The predecessor by address is the only possible containing interval. */
    for (node = ctx->heap_allocations; node; )
        if (node->block <= start)
        {
            chunk = node;
            node = node->right;
        }
        else
            node = node->left;
    if (!chunk || chunk->kind != HEAP_FREE || !heap_free_valid(ctx, chunk) ||
        start - chunk->block >= chunk->size || chunk->size - (start - chunk->block) < needed)
        return 0;
    previous = chunk->prev;
    following = chunk->next;
    if (!heap_free_valid(ctx, previous) || !heap_free_valid(ctx, following)) return 0;
    before = start - chunk->block;
    after = chunk->size - before - needed;
    /* Reserve both possible split records before changing any guest byte or
       ownership. Eight-byte fragments can still hold a free mirror header. */
    if (before)
    {
        prefix = (struct M68KHeapAllocation *)AllocMem(sizeof(*prefix), MEMF_PRIVATE | MEMF_CLEAR);
        if (!prefix) return 0;
        prefix->block = chunk->block;
        prefix->size = before;
        prefix->prev = previous;
    }
    if (after)
    {
        suffix = (struct M68KHeapAllocation *)AllocMem(sizeof(*suffix), MEMF_PRIVATE | MEMF_CLEAR);
        if (!suffix)
        {
            if (prefix) FreeMem(prefix, sizeof(*prefix));
            return 0;
        }
        suffix->block = start + needed;
        suffix->size = after;
        suffix->prev = prefix ? prefix : previous;
        suffix->next = following;
    }
    if (prefix) prefix->next = suffix ? suffix : following;
    if (previous) previous->next = prefix ? prefix : suffix ? suffix : following;
    else ctx->heap_first_free = prefix ? prefix : suffix ? suffix : following;
    if (following) following->prev = suffix ? suffix : prefix ? prefix : previous;
    /* Moving this key inside its former interval preserves tree ordering. */
    chunk->block = start;
    chunk->size = needed;
    chunk->requested = size;
    chunk->kind = HEAP_ABSOLUTE;
    chunk->prev = chunk->next = NULL;
    heap_rebalance(ctx, chunk);
    if (prefix) heap_insert(ctx, prefix);
    if (suffix) heap_insert(ctx, suffix);
    heap_write_free(ctx, previous);
    heap_write_free(ctx, prefix);
    heap_write_free(ctx, suffix);
    ctx->heap_free = ctx->heap_first_free ? ctx->heap_first_free->block : 0;
    /* Do not write an allocated header into the requested absolute range. */
    return start;
}

static BOOL heap_release_valid(struct KaffeineContext *ctx, struct M68KHeapAllocation *node)
{
    return heap_span_valid(ctx, node) &&
           heap_free_valid(ctx, heap_free_neighbor(node, FALSE)) &&
           heap_free_valid(ctx, heap_free_neighbor(node, TRUE));
}

/* A guest may drain a manual port with inline list operations, then release
   its containing allocation without calling GetMsg or DeleteMsgPort. Retire
   only a complete, canonically empty port while its bytes still exist.
   Malformed and nonempty queues retain their ownership checks. */
static void heap_forget_empty_ports(struct KaffeineContext *ctx,
                                    const struct M68KHeapAllocation *allocation)
{
    ULONG header = allocation->kind == HEAP_ABSOLUTE ? 0 : KAFFEINE_HEAP_HDR;
    ULONG block = allocation->block + header, size = allocation->size - header;
    unsigned i;

    /* heap_release has already validated this native interval. Exclude its
       private header and alignment slack beyond the requested payload. */
    if (allocation->requested < size) size = allocation->requested;
    for (i = 0; i < KAFFEINE_MAX_MESSAGE_PORTS; ++i)
    {
        ULONG port = ctx->message_ports[i];
        unsigned j;

        if (!port || (port & 1) || size < 34 || port < block ||
            port - block > size - 34)
            continue;
        /* CreateMsgPort storage has a separate signal/port owner; its
           supported release path is DeleteMsgPort. */
        for (j = 0; j < KAFFEINE_MAX_IO_PORTS; ++j)
            if (ctx->io_ports[j].guest == port) break;
        if (j != KAFFEINE_MAX_IO_PORTS) continue;
        /* Keep the empty FIFO geometry in sync with Kaffeine_QueueRead. */
        if (ctx->mem[port + 8] == 4 &&
            m68k_read32(ctx, port + 20) == port + 24 &&
            m68k_read32(ctx, port + 24) == 0 &&
            m68k_read32(ctx, port + 28) == port + 20)
            ctx->message_ports[i] = 0;
    }
}

static BOOL heap_release(struct KaffeineContext *ctx, struct M68KHeapAllocation *allocation,
                          struct M68KHeapAllocation *pool)
{
    struct M68KHeapAllocation *previous = heap_free_neighbor(allocation, FALSE);
    struct M68KHeapAllocation *following = heap_free_neighbor(allocation, TRUE);
    struct M68KHeapAllocation *kept;

    if (!heap_span_valid(ctx, allocation) || !heap_free_valid(ctx, previous) ||
        !heap_free_valid(ctx, following))
        return FALSE;
    heap_forget_empty_ports(ctx, allocation);
    if (pool)
    {
        if (allocation->prev) allocation->prev->next = allocation->next;
        else pool->next = allocation->next;
        if (allocation->next) allocation->next->prev = allocation->prev;
    }
    if (previous && previous->block + previous->size == allocation->block)
    {
        kept = previous;
        kept->size += allocation->size;
        heap_remove(ctx, allocation);
        FreeMem(allocation, sizeof(*allocation));
    }
    else
    {
        kept = allocation;
        kept->kind = HEAP_FREE;
        kept->requested = kept->pool = 0;
        kept->prev = previous;
        kept->next = following;
        if (previous) previous->next = kept;
        else ctx->heap_first_free = kept;
        if (following) following->prev = kept;
    }
    if (following && kept->block + kept->size == following->block)
    {
        kept->size += following->size;
        kept->next = following->next;
        if (kept->next) kept->next->prev = kept;
        heap_remove(ctx, following);
        FreeMem(following, sizeof(*following));
    }
    heap_rebalance(ctx, kept);
    if (previous != kept) heap_write_free(ctx, previous);
    heap_write_free(ctx, kept);
    ctx->heap_free = ctx->heap_first_free ? ctx->heap_first_free->block : 0;
    return TRUE;
}

void Kaffeine_HeapFree(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    struct M68KHeapAllocation *allocation;

    (void)size;
    if (!addr || addr == ctx->memory_info || !heap_ready(ctx)) return;
    allocation = heap_find(ctx, addr);
    if (allocation && (allocation->kind == HEAP_ALLOCATION || allocation->kind == HEAP_ABSOLUTE) &&
        !allocation->pool)
        heap_release(ctx, allocation, NULL);
}

enum { MEMLIST_HEADER = 16, MEMENTRY_SIZE = 8 };

static BOOL entry_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    if (!ctx->mem || !addr || addr >= ctx->mem_size || ctx->mem_size - addr < size)
        return FALSE;
    return TRUE;
}

static void entry_release(struct KaffeineContext *ctx, struct M68KHeapAllocation *header)
{
    while (header->next)
        if (!heap_release(ctx, header->next, header)) return;
    heap_release(ctx, header, NULL);
}

/* The MemList of count entries of spec, requirements and length pairs that
   the caller owns, or failure with bit 31 set; spec stays the caller's. */
static ULONG alloc_entry(struct KaffeineContext *ctx, const ULONG *spec, ULONG count)
{
    struct M68KHeapAllocation *header, *allocation, *chunk;
    ULONG bytes = MEMLIST_HEADER + count * MEMENTRY_SIZE, addr, i, failure = 0x80000000UL | MEMF_PUBLIC;

    /* Check all mirrors before starting a multi-allocation transaction, so an
       existing corrupt neighbour cannot prevent rollback of a later failure. */
    for (chunk = ctx->heap_first_free; chunk; chunk = chunk->next)
        if (!heap_free_valid(ctx, chunk)) return failure;
    header = heap_allocate(ctx, bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!header) return failure;
    header->kind = HEAP_MEMLIST;
    addr = header->block + KAFFEINE_HEAP_HDR;
    m68k_write16(ctx, addr + 14, count);
    for (i = 0; i < count; i++)
    {
        ULONG member = 0, requirements = spec[2 * i], length = spec[2 * i + 1];

        /* Like NewAllocEntry(), keep zero-length entries with a NULL address. */
        if (length)
        {
            allocation = heap_allocate(ctx, length, requirements);
            if (!allocation)
            {
                entry_release(ctx, header);
                return requirements | 0x80000000UL;
            }
            allocation->kind = HEAP_ENTRY;
            allocation->pool = addr;
            allocation->next = header->next;
            if (header->next) header->next->prev = allocation;
            header->next = allocation;
            member = allocation->block + KAFFEINE_HEAP_HDR;
        }
        m68k_write32(ctx, addr + MEMLIST_HEADER + i * MEMENTRY_SIZE, member);
        m68k_write32(ctx, addr + MEMLIST_HEADER + i * MEMENTRY_SIZE + 4, length);
    }
    return addr;
}

ULONG Kaffeine_HeapAllocEntry(struct KaffeineContext *ctx, ULONG entry)
{
    ULONG count, bytes, addr, i, *spec, failure = 0x80000000UL | MEMF_PUBLIC;

    if (!entry_range(ctx, entry, MEMLIST_HEADER) || !heap_ready(ctx)) return failure;
    count = m68k_read16(ctx, entry + 14);
    /* The count is a guest UWORD, so this multiplication cannot overflow. */
    bytes = MEMLIST_HEADER + count * MEMENTRY_SIZE;
    if (!entry_range(ctx, entry, bytes)) return failure;
    spec = (ULONG *)AllocMem(bytes, MEMF_PRIVATE);
    if (!spec) return failure;
    /* Snapshot before allocation: even a specification in currently free
       guest memory must not change as allocation writes its own mirrors. */
    for (i = 0; i < count; i++)
    {
        spec[2 * i] = m68k_read32(ctx, entry + MEMLIST_HEADER + i * MEMENTRY_SIZE);
        spec[2 * i + 1] = m68k_read32(ctx, entry + MEMLIST_HEADER + i * MEMENTRY_SIZE + 4);
    }
    addr = alloc_entry(ctx, spec, count);
    FreeMem(spec, bytes);
    return addr;
}

ULONG Kaffeine_HeapAllocList(struct KaffeineContext *ctx, const ULONG *spec, UWORD count)
{
    if (!spec || !heap_ready(ctx)) return 0x80000000UL | MEMF_PUBLIC;
    return alloc_entry(ctx, spec, count);
}

void Kaffeine_HeapFreeEntry(struct KaffeineContext *ctx, ULONG entry)
{
    struct M68KHeapAllocation *header, *allocation;
    ULONG count, i;

    if (!entry || !heap_ready(ctx)) return;
    header = heap_find(ctx, entry);
    if (!header || header->kind != HEAP_MEMLIST || !heap_span_valid(ctx, header) ||
        !entry_range(ctx, entry, header->requested)) return;
    count = (header->requested - MEMLIST_HEADER) / MEMENTRY_SIZE;
    if (m68k_read16(ctx, entry + 14) != count || !heap_release_valid(ctx, header)) return;
    allocation = header->next;
    /* Match every address and length to the native membership chain before
       releasing anything. Node links/name/priority may legitimately change. */
    for (i = count; i-- > 0; )
    {
        ULONG member = m68k_read32(ctx, entry + MEMLIST_HEADER + i * MEMENTRY_SIZE);
        ULONG length = m68k_read32(ctx, entry + MEMLIST_HEADER + i * MEMENTRY_SIZE + 4);

        if (!length)
        {
            if (member) return;
            continue;
        }
        if (!allocation || allocation->kind != HEAP_ENTRY || allocation->pool != entry ||
            member != allocation->block + KAFFEINE_HEAP_HDR || length != allocation->requested ||
            !heap_release_valid(ctx, allocation)) return;
        allocation = allocation->next;
    }
    if (allocation) return;
    entry_release(ctx, header);
}

/* Utility tag blocks have their own native owner kind. Neither a forged
   guest header nor a pointer into another allocation can claim ownership. */
ULONG Kaffeine_TagItemsAlloc(struct KaffeineContext *ctx, ULONG count)
{
    struct M68KHeapAllocation *allocation;

    if (!count || count > (ULONG)~0UL / 8) return 0;
    allocation = heap_allocate(ctx, count * 8, MEMF_PUBLIC | MEMF_CLEAR);
    if (!allocation) return 0;
    allocation->kind = HEAP_TAGITEMS;
    return allocation->block + KAFFEINE_HEAP_HDR;
}

ULONG Kaffeine_TagItemsCapacity(struct KaffeineContext *ctx, ULONG addr)
{
    struct M68KHeapAllocation *allocation;

    if (!addr || !heap_ready(ctx)) return 0;
    allocation = heap_find(ctx, addr);
    if (!allocation || allocation->kind != HEAP_TAGITEMS ||
        !heap_span_valid(ctx, allocation)) return 0;
    return allocation->requested / 8;
}

void Kaffeine_TagItemsFree(struct KaffeineContext *ctx, ULONG addr)
{
    struct M68KHeapAllocation *allocation;

    if (!addr || !heap_ready(ctx)) return;
    allocation = heap_find(ctx, addr);
    if (allocation && allocation->kind == HEAP_TAGITEMS)
        heap_release(ctx, allocation, NULL);
}

/* Locale blocks, the AChain copies of MatchFirst() and the 68k mirrors of
   GadTools have their own native owner kinds, so guest FreeMem() cannot
   release them while the bridge still writes them. */
static ULONG owned_alloc(struct KaffeineContext *ctx, ULONG size, UBYTE kind)
{
    struct M68KHeapAllocation *allocation;

    if (!size) return 0;
    allocation = heap_allocate(ctx, size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!allocation) return 0;
    allocation->kind = kind;
    return allocation->block + KAFFEINE_HEAP_HDR;
}

static void owned_free(struct KaffeineContext *ctx, ULONG addr, UBYTE kind)
{
    struct M68KHeapAllocation *allocation;

    if (!addr || !heap_ready(ctx)) return;
    allocation = heap_find(ctx, addr);
    if (allocation && allocation->kind == kind)
        heap_release(ctx, allocation, NULL);
}

ULONG Kaffeine_LocaleAlloc(struct KaffeineContext *ctx, ULONG size)
{
    return owned_alloc(ctx, size, HEAP_LOCALE);
}

void Kaffeine_LocaleFree(struct KaffeineContext *ctx, ULONG addr)
{
    owned_free(ctx, addr, HEAP_LOCALE);
}

ULONG Kaffeine_AChainAlloc(struct KaffeineContext *ctx, ULONG size)
{
    return owned_alloc(ctx, size, HEAP_ACHAIN);
}

void Kaffeine_AChainFree(struct KaffeineContext *ctx, ULONG addr)
{
    owned_free(ctx, addr, HEAP_ACHAIN);
}

ULONG Kaffeine_MirrorAlloc(struct KaffeineContext *ctx, ULONG size)
{
    return owned_alloc(ctx, size, HEAP_MIRROR);
}

void Kaffeine_MirrorRelease(struct KaffeineContext *ctx, ULONG addr)
{
    owned_free(ctx, addr, HEAP_MIRROR);
}

ULONG Kaffeine_HeapAvail(struct KaffeineContext *ctx, ULONG requirements)
{
    struct M68KHeapAllocation *chunk;
    ULONG available = 0;

    if (!heap_ready(ctx)) return 0;
    for (chunk = ctx->heap_first_free; chunk; chunk = chunk->next)
    {
        ULONG bytes = chunk->size - KAFFEINE_HEAP_HDR;

        if (!heap_free_valid(ctx, chunk)) return 0;
        if (requirements & MEMF_LARGEST)
        {
            if (bytes > available) available = bytes;
        }
        else
            available += bytes;
    }
    if ((requirements & MEMF_TOTAL) && !(requirements & MEMF_LARGEST))
    {
        ULONG bounds[4], total = 0;
        int i;

        /* All that an empty heap offers: each free interval less its header. */
        heap_layout(ctx, bounds);
        for (i = 0; i < 4; i += 2)
            if (bounds[i + 1] > bounds[i])
                total += bounds[i + 1] - bounds[i] - KAFFEINE_HEAP_HDR;
        return total;
    }
    return available;
}

static struct M68KHeapAllocation *pool_find(struct KaffeineContext *ctx, ULONG pool)
{
    struct M68KHeapAllocation *allocation = heap_find(ctx, pool);

    return allocation && allocation->kind == HEAP_POOL && heap_span_valid(ctx, allocation)
           ? allocation : NULL;
}

ULONG Kaffeine_PoolCreate(struct KaffeineContext *ctx, ULONG requirements,
                         ULONG puddle_size, ULONG threshold_size)
{
    struct M68KHeapAllocation *allocation = heap_allocate(ctx, GUEST_POOL_SIZE, MEMF_CLEAR);
    ULONG pool;

    if (!allocation) return 0;
    pool = allocation->block + KAFFEINE_HEAP_HDR;
    allocation->kind = HEAP_POOL;
    m68k_write32(ctx, pool, GUEST_POOL_MAGIC);
    m68k_write32(ctx, pool + 4, requirements);
    m68k_write32(ctx, pool + 8, puddle_size);
    m68k_write32(ctx, pool + 12, threshold_size);
    return pool;
}

ULONG Kaffeine_PoolAlloc(struct KaffeineContext *ctx, ULONG pool, ULONG size)
{
    struct M68KHeapAllocation *header, *allocation;

    if (!pool || !size || !heap_ready(ctx) || !(header = pool_find(ctx, pool))) return 0;
    allocation = heap_allocate(ctx, size, m68k_read32(ctx, pool + 4));
    if (!allocation) return 0;
    allocation->pool = pool;
    allocation->next = header->next;
    if (header->next) header->next->prev = allocation;
    header->next = allocation;
    return allocation->block + KAFFEINE_HEAP_HDR;
}

void Kaffeine_PoolFree(struct KaffeineContext *ctx, ULONG pool, ULONG addr, ULONG size)
{
    struct M68KHeapAllocation *header, *allocation;

    if (!pool || !addr || !size || !heap_ready(ctx) || !(header = pool_find(ctx, pool))) return;
    allocation = heap_find(ctx, addr);
    if (allocation && allocation->pool == pool && allocation->requested == size)
        heap_release(ctx, allocation, header);
}

void Kaffeine_PoolDelete(struct KaffeineContext *ctx, ULONG pool)
{
    struct M68KHeapAllocation *header, *allocation;

    if (!pool || !heap_ready(ctx) || !(header = pool_find(ctx, pool))) return;
    /* Check all affected free-list neighbours before starting the deletion.
       Only this pool's member chain is visited, regardless of other pools. */
    if (!heap_release_valid(ctx, header)) return;
    for (allocation = header->next; allocation; allocation = allocation->next)
        if (!heap_release_valid(ctx, allocation)) return;
    while (header->next)
        if (!heap_release(ctx, header->next, header)) return;
    heap_release(ctx, header, NULL);
}

/*
 * Copy a seglist loaded by DOS into the guest arena. DOS has already
 * relocated it against the native segment addresses, so every relocated
 * longword is moved by the difference between the guest and the native
 * address of its target (and, for relative relocations, of its source).
 * The arithmetic is modulo 2^32, as in the DOS loader. With headers each
 * guest hunk gets the [ULONG size][BPTR next] of an AmigaOS seglist in
 * front of its data, and *seglist the BPTR of the first; without, the
 * hunks are their data alone. The data of the first hunk is returned.
 */
static ULONG copy_seglist(struct KaffeineContext *ctx, BPTR segList,
                          const struct HunkRelocTable *relocs, BOOL headers,
                          ULONG *seglist)
{
    ULONG extra = headers ? sizeof(ULONG) + sizeof(ULONG) : 0;
    BPTR seg;
    ULONG num_hunks = 0, i;
    ULONG *native = NULL, *guest = NULL, *sizes = NULL;
    ULONG entry_point = 0;

    /* Without the relocation record the copy cannot be rebased. */
    if (!relocs)
        return 0;

    for (seg = segList; seg; seg = *((BPTR *)BADDR(seg)))
        num_hunks++;
    /* a library seglist has no more hunks than the bridge walks and frees */
    if (num_hunks == 0 || num_hunks != relocs->hrt_NumHunks ||
        (headers && num_hunks > KAFFEINE_MAX_LIBRARY_HUNKS))
        return 0;

    native = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    guest  = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    sizes  = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    if (!native || !guest || !sizes)
        goto done;

    /* Each segment is [ULONG size][BPTR next][data], with the size
       covering the whole allocation. */
    for (seg = segList, i = 0; seg; seg = *((BPTR *)BADDR(seg)), i++)
    {
        UBYTE *baddr     = (UBYTE *)BADDR(seg);
        ULONG alloc_size = *((ULONG *)baddr - 1);
        UBYTE *src       = baddr + sizeof(BPTR);
        ULONG block;

        if (alloc_size < sizeof(ULONG) + sizeof(BPTR) ||
            alloc_size - sizeof(ULONG) - sizeof(BPTR) > 0xFFFFFFFFu - extra)
            goto done;
        sizes[i] = alloc_size - sizeof(ULONG) - sizeof(BPTR);

        block = Kaffeine_HeapAlloc(ctx, sizes[i] + extra, 0);
        if (!block)
            goto done;
        guest[i] = block + extra;

        CopyMem(src, m68k_to_host(ctx, guest[i]), sizes[i]);
        native[i] = (ULONG)(IPTR)src;
    }

    for (i = 0; i < relocs->hrt_NumRelocs; i++)
    {
        const struct HunkRelocation *hr = &relocs->hrt_Relocs[i];
        ULONG delta, addr;

        if (hr->hr_Source >= num_hunks || hr->hr_Target >= num_hunks ||
            sizes[hr->hr_Source] < 4 || hr->hr_Offset > sizes[hr->hr_Source] - 4)
            goto done;

        delta = guest[hr->hr_Target] - native[hr->hr_Target];
        if (hr->hr_Type == HRT_RELATIVE)
            delta -= guest[hr->hr_Source] - native[hr->hr_Source];
        else if (hr->hr_Type != HRT_ABSOLUTE)
            goto done;

        addr = guest[hr->hr_Source] + hr->hr_Offset;
        m68k_write32(ctx, addr, m68k_read32(ctx, addr) + delta);
    }

    /* the seglist: each size counts its header, each next points at the
       next of the following hunk, as LoadSeg() leaves them */
    if (headers)
    {
        for (i = 0; i < num_hunks; i++)
        {
            m68k_write32(ctx, guest[i] - 8, sizes[i] + 8);
            m68k_write32(ctx, guest[i] - 4, i + 1 < num_hunks ? (guest[i + 1] - 4) >> 2 : 0);
        }
        *seglist = (guest[0] - 4) >> 2;
    }
    /* New guest code in memory that a running backend may have seen. */
    for (i = 0; i < num_hunks; i++)
        Kaffeine_FlushCode(ctx->cpu, guest[i], sizes[i]);
    entry_point = guest[0];

done:
    if (!entry_point && guest)
    {
        for (i = 0; i < num_hunks; i++)
            if (guest[i])
                Kaffeine_HeapFree(ctx, guest[i] - extra, sizes[i] + extra);
    }
    if (native) FreeMem(native, num_hunks * sizeof(ULONG));
    if (guest)  FreeMem(guest,  num_hunks * sizeof(ULONG));
    if (sizes)  FreeMem(sizes,  num_hunks * sizeof(ULONG));
    return entry_point;
}

ULONG Kaffeine_LoadHunks(struct KaffeineContext *ctx, BPTR segList,
                        const struct HunkRelocTable *relocs)
{
    ULONG entry_point = copy_seglist(ctx, segList, relocs, FALSE, NULL);

    if (entry_point)
    {
        ctx->entry_point = entry_point;
        ctx->seg_start   = entry_point;
    }
    return entry_point;
}

ULONG Kaffeine_LoadLibraryHunks(struct KaffeineContext *ctx, BPTR segList,
                               const struct HunkRelocTable *relocs)
{
    ULONG seglist = 0;

    return copy_seglist(ctx, segList, relocs, TRUE, &seglist) ? seglist : 0;
}

static ULONG setup_base(struct KaffeineContext *ctx, UWORD lib_id,
                        const char *name, UWORD num_vectors, UWORD pos_size,
                        const struct M68KThunkEntry *thunks, ULONG num_thunks,
                        const struct M68KThunkEntry *gen_thunks, ULONG num_gen_thunks,
                        BOOL library)
{
    ULONG vectors_size, total_size, region_offset, base_addr;
    UWORD i;

    if (ctx->num_libs >= KAFFEINE_MAX_LIBS)
        return 0;

    vectors_size = (ULONG)num_vectors * M68K_JT_SLOT_SIZE;
    total_size   = vectors_size + pos_size;

    /* Find next free spot in LIBBASE_REGION */
    region_offset = KAFFEINE_LIBBASE_REGION;
    for (i = 0; i < ctx->num_libs; i++)
    {
        ULONG lib_end = ctx->libs[i].m68k_addr + ctx->libs[i].pos_size;
        if (lib_end > region_offset)
            region_offset = (lib_end + 3) & ~3;
    }

    if (region_offset + total_size > KAFFEINE_LIBBASE_REGION + KAFFEINE_LIBBASE_SIZE)
        return 0;

    base_addr = region_offset + vectors_size;

    /* Fill ALL jump vector slots with A-line trap + NOP + NOP */
    for (i = 0; i < num_vectors; i++)
    {
        ULONG slot_addr = base_addr - ((ULONG)(i + 1) * M68K_JT_SLOT_SIZE);
        m68k_write16(ctx, slot_addr,     M68K_OP_ALINE);
        m68k_write16(ctx, slot_addr + 2, M68K_OP_NOP);
        m68k_write16(ctx, slot_addr + 4, M68K_OP_NOP);
    }
    Kaffeine_FlushCode(ctx->cpu, base_addr - vectors_size, vectors_size);

    ctx->libs[ctx->num_libs].m68k_addr   = base_addr;
    ctx->libs[ctx->num_libs].pos_size    = pos_size;
    ctx->libs[ctx->num_libs].jt_start    = base_addr - vectors_size;
    ctx->libs[ctx->num_libs].lib_id      = lib_id;
    ctx->libs[ctx->num_libs].num_vectors = num_vectors;
    ctx->libs[ctx->num_libs].thunks      = thunks;
    ctx->libs[ctx->num_libs].num_thunks  = num_thunks;
    ctx->libs[ctx->num_libs].gen_thunks   = gen_thunks;
    ctx->libs[ctx->num_libs].num_gen_thunks = num_gen_thunks;
    if (name)
    {
        snprintf(ctx->libs[ctx->num_libs].name, sizeof(ctx->libs[ctx->num_libs].name), "%s", name);
    }
    /* Once here, so that the dispatcher compares no name for each call */
    ctx->libs[ctx->num_libs].dos = library && name && !strcmp(name, "dos.library");
    ctx->num_libs++;

    /* Register in libmap for OpenLibrary lookups */
    if (library && name && ctx->num_libmap < KAFFEINE_MAX_LIBMAP)
    {
        snprintf(ctx->libmap[ctx->num_libmap].name, sizeof(ctx->libmap[ctx->num_libmap].name), "%s", name);
        ctx->libmap[ctx->num_libmap].m68k_base = base_addr;
        ctx->num_libmap++;
    }

    return base_addr;
}

ULONG Kaffeine_SetupLibBase(struct KaffeineContext *ctx, UWORD lib_id,
                            const char *name, UWORD num_vectors,
                            const struct M68KThunkEntry *thunks, ULONG num_thunks,
                            const struct M68KThunkEntry *gen_thunks, ULONG num_gen_thunks)
{
    return setup_base(ctx, lib_id, name, num_vectors,
                      (lib_id == 0) ? M68K_SIZEOF_EXECBASE : 4,
                      thunks, num_thunks, gen_thunks, num_gen_thunks, TRUE);
}

/* A device base for OpenDevice(): OpenLibrary() does not find it, as Exec
   keeps devices on their own list, and its positive area holds a cleared
   struct Device for the caller to fill. */
ULONG Kaffeine_SetupDeviceBase(struct KaffeineContext *ctx, const char *name,
                               UWORD num_vectors,
                               const struct M68KThunkEntry *thunks, ULONG num_thunks)
{
    ULONG base = setup_base(ctx, ctx->num_libs, name, num_vectors, M68K_SIZEOF_DEVICE,
                            thunks, num_thunks, NULL, 0, FALSE);

    if (base)
        memset(ctx->mem + base, 0, M68K_SIZEOF_DEVICE);
    return base;
}

ULONG Kaffeine_FindLibBase(struct KaffeineContext *ctx, const char *name)
{
    UWORD i;
    if (!name) return 0;
    for (i = 0; i < ctx->num_libmap; i++)
    {
        if (strcmp(ctx->libmap[i].name, name) == 0)
            return ctx->libmap[i].m68k_base;
    }
    return 0;
}

/* ── Hunk file loader (from raw memory buffer) ────────────────────── */

/* Hunk constants now in kaffeine_offsets.h */

static ULONG rd32(UBYTE **p)
{
    ULONG v = ((ULONG)(*p)[0] << 24) | ((ULONG)(*p)[1] << 16) |
              ((ULONG)(*p)[2] << 8)  |  (ULONG)(*p)[3];
    *p += 4;
    return v;
}

ULONG Kaffeine_LoadHunksFromMemory(struct KaffeineContext *ctx, UBYTE *data, LONG size)
{
    UBYTE *p = data;
    UBYTE *end;
    ULONG magic, num_hunks, first, last;
    ULONG *hunk_sizes = NULL;
    ULONG *hunk_addrs = NULL;
    ULONG i, entry_point = 0;

    if (!data || size < 4) return 0;
    end = data + size;
    magic = rd32(&p);
    if (magic != HUNK_HEADER) return 0;

    /* Skip resident library names */
    while (end - p >= 4)
    {
        ULONG n = rd32(&p);
        if (n == 0) break;
        /* Validate the file's longword count before multiplying or moving
           a host pointer, on both 32-bit and 64-bit hosts. */
        if (n > (ULONG)(end - p) / 4) return 0;
        p += n * 4;
    }

    if (end - p < 12) return 0;
    num_hunks = rd32(&p);
    first     = rd32(&p);  /* unused but must be consumed */
    last      = rd32(&p);  /* unused but must be consumed */
    (void)first; (void)last;

    if (num_hunks == 0 || num_hunks > 100) return 0;

    /* Read hunk sizes (in longwords) */
    hunk_sizes = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    hunk_addrs = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    if (!hunk_sizes || !hunk_addrs) goto fail;

    for (i = 0; i < num_hunks; i++)
    {
        if (end - p < 4) goto fail;
        hunk_sizes[i] = (rd32(&p) & HUNK_TYPE_MASK) * 4; /* mask off MEMF flags */
    }

    /* Allocate space for each hunk in containment memory */
    for (i = 0; i < num_hunks; i++)
    {
        ULONG addr = Kaffeine_HeapAlloc(ctx, hunk_sizes[i], 0);
        if (!addr) goto fail;
        hunk_addrs[i] = addr;
        if (i == 0) entry_point = addr;
    }

    /* Parse hunk contents */
    i = 0;
    while (end - p >= 4)
    {
        ULONG type = rd32(&p) & HUNK_TYPE_MASK;

        if (type == HUNK_CODE || type == HUNK_DATA)
        {
            if (end - p < 4) goto fail;
            ULONG longs = rd32(&p);
            if (longs > (ULONG)(end - p) / 4 || i >= num_hunks) goto fail;
            ULONG bytes = longs * 4;
            if (bytes > hunk_sizes[i]) goto fail;

            /* Copy data into containment space */
            memcpy(m68k_to_host(ctx, hunk_addrs[i]), p, bytes);
            p += bytes;
        }
        else if (type == HUNK_BSS)
        {
            if (end - p < 4) goto fail;
            rd32(&p); /* skip size, already zeroed */
        }
        else if (type == HUNK_RELOC32)
        {
            while (end - p >= 4)
            {
                ULONG count = rd32(&p);
                if (count == 0) break;
                if (end - p < 4) goto fail;
                ULONG ref_hunk = rd32(&p);
                if (ref_hunk >= num_hunks) goto fail;

                ULONG ref_base = hunk_addrs[ref_hunk];
                ULONG j;
                for (j = 0; j < count; j++)
                {
                    if (end - p < 4) goto fail;
                    ULONG offset = rd32(&p);
                    if (i < num_hunks && offset < hunk_sizes[i] &&
                        hunk_sizes[i] - offset >= 4)
                    {
                        ULONG addr = hunk_addrs[i] + offset;
                        ULONG val = m68k_read32(ctx, addr);
                        m68k_write32(ctx, addr, val + ref_base);
                    }
                }
            }
        }
        else if (type == HUNK_END)
        {
            i++;
        }
        else
        {
            /* Unknown hunk type — skip */
            break;
        }
    }

    /* New guest code in memory that a running backend may have seen, as
       for a 68k library that OpenLibrary() loads. */
    for (i = 0; i < num_hunks; i++)
        Kaffeine_FlushCode(ctx->cpu, hunk_addrs[i], hunk_sizes[i]);

    ctx->entry_point = entry_point;
    ctx->seg_start   = entry_point;

    FreeMem(hunk_sizes, num_hunks * sizeof(ULONG));
    FreeMem(hunk_addrs, num_hunks * sizeof(ULONG));
    return entry_point;

fail:
    if (hunk_addrs) {
        for (i = 0; i < num_hunks; i++) {
            if (hunk_addrs[i])
                Kaffeine_HeapFree(ctx, hunk_addrs[i], hunk_sizes[i]);
        }
        FreeMem(hunk_addrs, num_hunks * sizeof(ULONG));
    }
    if (hunk_sizes) FreeMem(hunk_sizes, num_hunks * sizeof(ULONG));
    return 0;
}

/* Auto-open libraries: scan containment memory for the C startup pattern:
   N zero longwords (base pointers) followed immediately by N
   consecutive NUL-terminated ".library" name strings. */
void Kaffeine_AutoOpenLibs(struct KaffeineContext *ctx)
{
    UBYTE *base = ctx->mem;
    ULONG sz = ctx->mem_size;
    ULONG off = KAFFEINE_HEAP_BASE;

    while (off + 10 < sz)
    {
        if (base[off] != '.' || memcmp(base + off, ".library", 8) != 0 ||
            base[off + 8] != 0)
        {
            off++;
            continue;
        }

        /* Found ".library\0" — scan back for name start */
        ULONG name_start = off;
        while (name_start > 0 && base[name_start - 1] >= 0x20 &&
               base[name_start - 1] < 0x7f)
            name_start--;

        /* Collect consecutive library name strings */
        ULONG names[16];
        ULONG count = 0;
        ULONG p = name_start;
        while (p < sz && count < 16)
        {
            ULONG q = p;
            while (q < sz && base[q] >= 0x20 && base[q] < 0x7f) q++;
            if (q >= p + 9 && q < sz && base[q] == 0 &&
                memcmp(base + q - 8, ".library", 8) == 0)
            {
                names[count++] = p;
                p = q + 1;
            }
            else
                break;
        }

        if (count == 0) { off++; continue; }

        /* Check if count*4 bytes before first name are all zero */
        if (names[0] < count * 4 + 4) { off = p; continue; }
        ULONG table_start = names[0] - count * 4;

        BOOL all_zero = TRUE;
        ULONG j;
        for (j = 0; j < count * 4; j++)
            if (base[table_start + j] != 0) { all_zero = FALSE; break; }

        if (!all_zero) { off = p; continue; }

        /* Fill base pointer slots */
        for (j = 0; j < count; j++)
        {
            const char *lname = (const char *)(base + names[j]);
            ULONG fakebase = Kaffeine_OpenLibrary(ctx, lname);
            if (fakebase)
                m68k_write32(ctx, table_start + j * 4, fakebase);
        }

        off = p;
    }
}

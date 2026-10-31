/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Private pools retain their classic 32-bit, big-endian MemHeader and
    MemChunk layouts inside guest memory. Only mh_First and mh_Free are
    changed in the header; the Node and attribute fields need not be
    initialized by private-pool users such as libnix.

    Nonzero pool bounds describe [mh_Lower, mh_Upper), as in libnix and
    Exec's standard allocator. Both bounds may be zero for hand-built
    pools; in that case the guest arena is the only external bound.
    Native MemHeader shadows and managed-memory callbacks are not used.
*/

#include "kaffeine_intern.h"
#include "kaffeine_memheader.h"

enum
{
    MEMHEADER_SIZE = 32,
    MH_FIRST = 16,
    MH_LOWER = 20,
    MH_UPPER = 24,
    MH_FREE = 28,
    MEMCHUNK_SIZE = 8,
    MC_NEXT = 0,
    MC_BYTES = 4,
    BLOCK_MASK = 7
};

struct GuestPool
{
    ULONG header;
    ULONG first;
    ULONG lower;
    ULONG upper;
    ULONG free_bytes;
};

static BOOL guest_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    /* Check before adding addresses or calling the byte accessors. */
    return ctx && ctx->mem && addr != 0 && size <= ctx->mem_size &&
           addr <= ctx->mem_size - size;
}

static BOOL native_shadow(struct KaffeineContext *ctx, ULONG addr)
{
    UWORD i;

    for (i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr &&
            ctx->shadow_map[i].native_ptr)
            return TRUE;
    return FALSE;
}

static BOOL pool_range(struct KaffeineContext *ctx,
                       const struct GuestPool *pool, ULONG addr, ULONG size)
{
    ULONG end;

    if (!guest_range(ctx, addr, size) ||
        (ctx->memory_info && addr < ctx->memory_info + KAFFEINE_MEMINFO_SIZE &&
         addr + size > ctx->memory_info))
        return FALSE;
    end = addr + size;
    /* Free chunks must not overwrite their own MemHeader. Its location
       need not otherwise lie inside the pool's declared bounds. */
    if (addr < pool->header + MEMHEADER_SIZE && end > pool->header)
        return FALSE;
    if ((pool->lower || pool->upper) &&
        (addr < pool->lower || end > pool->upper))
        return FALSE;
    return TRUE;
}

static BOOL read_pool(struct KaffeineContext *ctx, ULONG header,
                      struct GuestPool *pool)
{
    ULONG current, total = 0;

    if (!guest_range(ctx, header, MEMHEADER_SIZE) || native_shadow(ctx, header) ||
        (ctx->memory_info && header < ctx->memory_info + KAFFEINE_MEMINFO_SIZE &&
         header + MEMHEADER_SIZE > ctx->memory_info))
        return FALSE;
    pool->header = header;
    pool->first = m68k_read32(ctx, header + MH_FIRST);
    pool->lower = m68k_read32(ctx, header + MH_LOWER);
    pool->upper = m68k_read32(ctx, header + MH_UPPER);
    pool->free_bytes = m68k_read32(ctx, header + MH_FREE);
    if ((pool->lower || pool->upper) &&
        (pool->lower >= pool->upper || pool->upper > ctx->mem_size))
        return FALSE;

    /* Validate the entire chain before changing even the first fitting
       chunk. Strictly increasing, non-overlapping ranges also rule out
       cycles and bound the traversal by the size of the guest arena. */
    current = pool->first;
    while (current)
    {
        ULONG bytes, next;

        if ((current & BLOCK_MASK) ||
            !guest_range(ctx, current, MEMCHUNK_SIZE) || native_shadow(ctx, current))
            return FALSE;
        bytes = m68k_read32(ctx, current + MC_BYTES);
        next = m68k_read32(ctx, current + MC_NEXT);
        if (!bytes || (bytes & BLOCK_MASK) || !pool_range(ctx, pool, current, bytes))
            return FALSE;
        if (next && next < current + bytes)
            return FALSE;
        /* This subtraction both checks mh_Free and prevents sum overflow. */
        if (bytes > pool->free_bytes - total)
            return FALSE;
        total += bytes;
        current = next;
    }
    return total == pool->free_bytes;
}

static BOOL round_size(ULONG size, ULONG *rounded)
{
    if (size > (ULONG)-1 - BLOCK_MASK)
        return FALSE;
    *rounded = (size + BLOCK_MASK) & ~(ULONG)BLOCK_MASK;
    return TRUE;
}

/* -186: Allocate(A0=MemHeader, D0=bytes) -> D0=guest address or zero */
IPTR Kaffeine_MemHeaderAllocate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG header = THUNK_A(0), size = THUNK_D(0);
    ULONG bytes, current, link;
    struct GuestPool pool;

    if (!size || !round_size(size, &bytes) || !read_pool(ctx, header, &pool) ||
        bytes > pool.free_bytes)
        return 0;

    link = header + MH_FIRST;
    current = pool.first;
    while (current)
    {
        ULONG chunk_bytes = m68k_read32(ctx, current + MC_BYTES);
        ULONG next = m68k_read32(ctx, current + MC_NEXT);

        if (chunk_bytes >= bytes)
        {
            ULONG replacement = next;

            if (chunk_bytes != bytes)
            {
                replacement = current + bytes;
                if (native_shadow(ctx, replacement))
                    return 0;
                /* The checked chunk contains the complete remainder header. */
                m68k_write32(ctx, replacement + MC_NEXT, next);
                m68k_write32(ctx, replacement + MC_BYTES, chunk_bytes - bytes);
            }
            m68k_write32(ctx, link, replacement);
            m68k_write32(ctx, header + MH_FREE, pool.free_bytes - bytes);
            return (IPTR)current;
        }
        link = current + MC_NEXT;
        current = next;
    }
    return 0;
}

/* -192: Deallocate(A0=MemHeader, A1=block, D0=bytes). A zero size or
   null block is a no-op. Invalid metadata or ranges cause no writes. */
IPTR Kaffeine_MemHeaderDeallocate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG header = THUNK_A(0), addr = THUNK_A(1), size = THUNK_D(0);
    ULONG start, end, bytes, prefix, previous, next, link;
    ULONG merged_start, merged_end, following;
    struct GuestPool pool;

    if (!addr || !size)
        return 0;
    /* Include the whole caller-specified interval when rounding outward. */
    prefix = addr & BLOCK_MASK;
    if (size > (ULONG)-1 - prefix || !round_size(size + prefix, &bytes))
        return 0;
    start = addr & ~(ULONG)BLOCK_MASK;
    if (!read_pool(ctx, header, &pool) || !pool_range(ctx, &pool, start, bytes) ||
        native_shadow(ctx, start) || bytes > (ULONG)-1 - pool.free_bytes)
        return 0;
    end = start + bytes;

    previous = 0;
    next = pool.first;
    link = header + MH_FIRST;
    while (next && next < start)
    {
        previous = next;
        link = previous + MC_NEXT;
        next = m68k_read32(ctx, previous + MC_NEXT);
    }

    merged_start = start;
    merged_end = end;
    following = next;
    if (previous)
    {
        ULONG previous_end = previous + m68k_read32(ctx, previous + MC_BYTES);

        if (previous_end > start)
            return 0;
        if (previous_end == start)
            merged_start = previous;
    }
    if (next)
    {
        if (end > next)
            return 0;
        if (end == next)
        {
            merged_end = next + m68k_read32(ctx, next + MC_BYTES);
            following = m68k_read32(ctx, next + MC_NEXT);
        }
    }

    /* All ranges, links, overlap checks and counter arithmetic are now
       validated. No fallible operation follows the first write. */
    m68k_write32(ctx, merged_start + MC_NEXT, following);
    m68k_write32(ctx, merged_start + MC_BYTES, merged_end - merged_start);
    if (merged_start == start)
        m68k_write32(ctx, link, start);
    m68k_write32(ctx, header + MH_FREE, pool.free_bytes + bytes);
    return 0;
}

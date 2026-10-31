/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Exercise Allocate()/Deallocate() with a MemHeader and free chunks in
    68k memory. The pool follows libnix malloc's layout: an AllocMem() block
    starts with its MemHeader, and mh_Lower == mh_First == header + 1.
    This program uses the normal libnix startup, but neither malloc nor
    stdio, so allocator checks can run before testing c_stdio again.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "m68ktest.h"

#define POOL_BYTES 256

static BOOL full_pool(struct MemHeader *header, UBYTE *base)
{
    struct MemChunk *chunk = (struct MemChunk *)base;

    return check(header->mh_First == chunk && header->mh_Free == POOL_BYTES &&
                 chunk->mc_Next == NULL && chunk->mc_Bytes == POOL_BYTES,
                 "one coalesced free chunk covers the whole pool");
}

static void exercise_pool(void)
{
    const ULONG total = sizeof(struct MemHeader) + POOL_BYTES;
    struct MemHeader *header;
    struct MemChunk *first;
    UBYTE *base, *a, *b, *c, *d, *whole;
    ULONG i;

    header = AllocMem(total, MEMF_PUBLIC);
    if (!check(header != NULL, "AllocMem() of header and pool"))
        return;
    if (!check(((ULONG)header & 7) == 0, "AllocMem() aligns the pool header to eight bytes"))
        goto done;

    first = (struct MemChunk *)(header + 1);
    base = (UBYTE *)first;
    header->mh_Node.ln_Succ = header->mh_Node.ln_Pred = NULL;
    header->mh_Node.ln_Type = NT_MEMORY;
    header->mh_Node.ln_Pri = 0;
    header->mh_Node.ln_Name = NULL;
    header->mh_Attributes = MEMF_PUBLIC;
    header->mh_Lower = header->mh_First = first;
    header->mh_Upper = base + POOL_BYTES;
    header->mh_Free = first->mc_Bytes = POOL_BYTES;
    first->mc_Next = NULL;

    check(Allocate(header, 0) == NULL && header->mh_Free == POOL_BYTES,
          "Allocate(0) leaves the pool unchanged");
    a = Allocate(header, 1);
    if (!check_long("Allocate(1) returns the first guest address", (LONG)a, (LONG)base))
        goto done;
    check_long("one byte consumes an eight-byte chunk", header->mh_Free, POOL_BYTES - 8);
    a[0] = 0xa5;

    b = Allocate(header, 9);
    if (!check_long("Allocate(9) returns the next chunk", (LONG)b, (LONG)(base + 8)))
        goto done;
    check_long("nine bytes consume sixteen", header->mh_Free, POOL_BYTES - 24);
    for (i = 0; i < 9; i++)
        b[i] = (UBYTE)(0x30 + i);

    c = Allocate(header, 16);
    if (!check_long("Allocate(16) follows the rounded chunks", (LONG)c, (LONG)(base + 24)))
        goto done;
    c[0] = 0x5a;
    c[15] = 0xc3;
    check_long("free byte count after three allocations", header->mh_Free, POOL_BYTES - 40);
    check(Allocate(header, POOL_BYTES) == NULL && header->mh_Free == POOL_BYTES - 40,
          "out-of-memory request does not consume free bytes");
    check(a[0] == 0xa5 && b[8] == 0x38 && c[0] == 0x5a && c[15] == 0xc3,
          "allocated payloads do not overlap");

    Deallocate(header, b, 9);
    check_long("Deallocate(9) returns sixteen bytes", header->mh_Free, POOL_BYTES - 24);
    d = Allocate(header, 8);
    if (!check_long("Allocate() reuses the freed hole", (LONG)d, (LONG)b))
        goto done;
    check(header->mh_First == (struct MemChunk *)(base + 16) &&
          header->mh_First->mc_Bytes == 8,
          "splitting a freed hole leaves one eight-byte chunk");
    check(a[0] == 0xa5 && c[0] == 0x5a && c[15] == 0xc3,
          "freeing and reusing a hole preserves its allocated neighbours");

    Deallocate(header, d, 8);
    check(header->mh_First == (struct MemChunk *)b &&
          header->mh_First->mc_Bytes == 16,
          "Deallocate() coalesces with the following free chunk");
    Deallocate(header, a, 1);
    check_long("Deallocate(1) returns eight bytes", header->mh_Free, POOL_BYTES - 16);
    Deallocate(header, c, 16);
    if (!full_pool(header, base))
        goto done;

    whole = Allocate(header, POOL_BYTES - 3);
    if (!check_long("a rounded request can consume the whole pool", (LONG)whole, (LONG)base))
        goto done;
    check(header->mh_First == NULL && header->mh_Free == 0,
          "an exhausted pool has no free-list head");
    check(Allocate(header, 1) == NULL, "allocation from an exhausted pool returns NULL");
    Deallocate(header, whole, POOL_BYTES - 3);
    full_pool(header, base);

done:
    FreeMem(header, total);
}

int main(void)
{
    static const ULONG sizes[4] = { 1, 5, 9, 13 };
    APTR probes[4];
    ULONG i;

    if (!check(sizeof(struct MemHeader) == 32 && sizeof(struct MemChunk) == 8,
               "test uses the 32-bit Amiga memory structure layout"))
        return test_done("exec_memheader");

    /* Leave differently sized allocations live while creating the pool.
       A four-byte-only heap must not happen to pass on its first block. */
    for (i = 0; i < 4; i++)
    {
        probes[i] = AllocMem(sizes[i], MEMF_PUBLIC);
        check(probes[i] != NULL && ((ULONG)probes[i] & 7) == 0,
              "small AllocMem() blocks keep eight-byte alignment");
    }
    exercise_pool();
    for (i = 0; i < 4; i++)
        if (probes[i])
            FreeMem(probes[i], sizes[i]);
    return test_done("exec_memheader");
}

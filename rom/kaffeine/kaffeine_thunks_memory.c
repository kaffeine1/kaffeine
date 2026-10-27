/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Keep guest memory requirements and pool handles inside the guest arena.
*/
#include "kaffeine_intern.h"
#include "kaffeine_thunks_memory.h"

#include <string.h>

/* Every byte of [addr, addr + size) lies in guest memory; address 0 is
   refused as m68k_to_host() refuses it. */
static BOOL guest_bytes(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return ctx->mem && addr && addr < ctx->mem_size && ctx->mem_size - addr >= size;
}

/* -624: CopyMem(A0=source, A1=dest, D0=size) and -630: CopyMemQuick(A0=source,
   A1=dest, D0=size). Both copy the guest bytes where they lie, also at the
   guest copy of a shadowed native structure, and copy nothing when either
   range leaves guest memory. Exec does not promise overlapping copies;
   memmove() makes them harmless. */
static IPTR copy_guest_memory(struct KaffeineContext *ctx, void *cpu)
{
    ULONG source = THUNK_A(0), dest = THUNK_A(1), size = THUNK_D(0);

    if (size && guest_bytes(ctx, source, size) && guest_bytes(ctx, dest, size))
        memmove(ctx->mem + dest, ctx->mem + source, size);
    return 0;
}

IPTR thunk_exec_CopyMem(struct KaffeineContext *ctx, void *cpu)
{
    return copy_guest_memory(ctx, cpu);
}

IPTR thunk_exec_CopyMemQuick(struct KaffeineContext *ctx, void *cpu)
{
    return copy_guest_memory(ctx, cpu);
}

/* -198: AllocMem(D0=byteSize, D1=requirements) -> D0=memoryBlock */
IPTR thunk_exec_AllocMem(struct KaffeineContext *ctx, void *cpu)
{
    ULONG size = THUNK_D(0);
    return size ? Kaffeine_HeapAlloc(ctx, size, THUNK_D(1)) : 0;
}

/* -204: AllocAbs(D0=byteSize, A1=location) -> D0=aligned guest address. */
IPTR thunk_exec_AllocAbs(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_HeapAllocAbs(ctx, THUNK_D(0), THUNK_A(1));
}

/* -222: AllocEntry(A0=entry) -> D0=guest MemList or failed requirements. */
IPTR thunk_exec_AllocEntry(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_HeapAllocEntry(ctx, THUNK_A(0));
}

/* -228: FreeEntry(A0=entry). Preserve registers for this void operation. */
IPTR thunk_exec_FreeEntry(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_HeapFreeEntry(ctx, THUNK_A(0));
    return THUNK_D(0);
}

/* -210: FreeMem(A1=memoryBlock, D0=byteSize) */
IPTR thunk_exec_FreeMem(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_HeapFree(ctx, THUNK_A(1), THUNK_D(0));
    return 0;
}

/* -216: AvailMem(D1=requirements) -> D0=size */
IPTR thunk_exec_AvailMem(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_HeapAvail(ctx, THUNK_D(1));
}

/* -684: AllocVec(D0=byteSize, D1=requirements) -> D0=memory */
IPTR thunk_exec_AllocVec(struct KaffeineContext *ctx, void *cpu)
{
    ULONG size = THUNK_D(0);
    return size ? Kaffeine_HeapAlloc(ctx, size, THUNK_D(1)) : 0;
}

/* -690: FreeVec(A1=memoryBlock) */
IPTR thunk_exec_FreeVec(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_HeapFree(ctx, THUNK_A(1), 0);
    return 0;
}

/* -696: CreatePool(D0=requirements, D1=puddleSize, D2=threshSize) -> D0=pool */
IPTR thunk_exec_CreatePool(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_PoolCreate(ctx, THUNK_D(0), THUNK_D(1), THUNK_D(2));
}

/* -708: AllocPooled(A0=poolHeader, D0=memSize) -> D0=memory */
IPTR thunk_exec_AllocPooled(struct KaffeineContext *ctx, void *cpu)
{
    return Kaffeine_PoolAlloc(ctx, THUNK_A(0), THUNK_D(0));
}

/* -714: FreePooled(A0=poolHeader, A1=memory, D0=memSize) */
IPTR thunk_exec_FreePooled(struct KaffeineContext *ctx, void *cpu)
{
    Kaffeine_PoolFree(ctx, THUNK_A(0), THUNK_A(1), THUNK_D(0));
    return 0;
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   FileLock BPTRs identify checked classic views, never native addresses. */
#include "kaffeine_thunks_locks.h"
#include "kaffeine_locks.h"
#include <dos/dos.h>
#include <proto/dos.h>
#include <string.h>

static const char *name(struct KaffeineContext *ctx, ULONG address)
{
    if (!ctx->mem || !address || address >= ctx->mem_size ||
        Kaffeine_LockProtected(ctx, address, 1) ||
        !memchr(ctx->mem + address, 0, ctx->mem_size - address))
    {
        SetIoErr(ERROR_BAD_NUMBER);
        return NULL;
    }
    return (const char *)ctx->mem + address;
}

IPTR kaffeine_dos_DupLock(struct KaffeineContext *ctx, void *cpu)
{
    BPTR lock;
    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;
    return Kaffeine_LockAdopt(ctx, DupLock(lock));
}

IPTR kaffeine_dos_ParentDir(struct KaffeineContext *ctx, void *cpu)
{
    BPTR lock;
    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &lock)) return 20;
    if (!lock) { SetIoErr(ERROR_BAD_NUMBER); return 0; }
    return Kaffeine_LockAdopt(ctx, ParentDir(lock));
}

IPTR kaffeine_dos_SameLock(struct KaffeineContext *ctx, void *cpu)
{
    BPTR first, second;
    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &first) ||
        !Kaffeine_LockResolve(ctx, THUNK_D(2), &second)) return 20;
    return SameLock(first, second);
}

IPTR kaffeine_dos_SameDevice(struct KaffeineContext *ctx, void *cpu)
{
    BPTR first, second;
    if (!Kaffeine_LockResolve(ctx, THUNK_D(1), &first) ||
        !Kaffeine_LockResolve(ctx, THUNK_D(2), &second)) return 20;
    return SameDevice(first, second);
}

IPTR kaffeine_dos_SetProgramDir(struct KaffeineContext *ctx, void *cpu)
{
    ULONG previous;
    if (!Kaffeine_LockSwapInstalled(ctx, KAFFEINE_LOCK_PROGRAM, THUNK_D(1), &previous))
        return ctx->running ? 0 : 20;
    return previous;
}

IPTR kaffeine_dos_GetProgramDir(struct KaffeineContext *ctx, void *cpu)
{
    ULONG token;
    (void)cpu;
    if (!Kaffeine_LockGetInstalled(ctx, KAFFEINE_LOCK_PROGRAM, &token))
        return ctx->running ? 0 : 20;
    return token;
}

IPTR kaffeine_dos_MakeLink(struct KaffeineContext *ctx, void *cpu)
{
    const char *link = name(ctx, THUNK_D(1));
    IPTR destination;
    BPTR lock;
    if (!link) return 0;
    if (THUNK_D(3))
    {
        const char *target = name(ctx, THUNK_D(2));
        if (!target) return 0;
        destination = (IPTR)target;
    }
    else
    {
        if (!Kaffeine_LockResolve(ctx, THUNK_D(2), &lock)) return 20;
        /* Cross-filesystem hard links need a separate handler-affinity
           preflight before a native filesystem may inspect this lock. */
        SetIoErr(ERROR_NOT_IMPLEMENTED);
        return 0;
    }
    return MakeLink(link, destination, (LONG)THUNK_D(3));
}

IPTR kaffeine_dos_AssignAddToList(struct KaffeineContext *ctx, void *cpu)
{
    const char *assign = name(ctx, THUNK_D(1));
    struct KaffeineLockTransfer lease;
    LONG result;
    if (!assign || !THUNK_D(2)) return 0;
    if (!Kaffeine_LockTransferBegin(ctx, THUNK_D(2), &lease)) return 20;
    result = AssignAddToList(assign, lease.native, (LONG)THUNK_D(3));
    Kaffeine_LockTransferEnd(ctx, &lease, result != 0);
    return result;
}

/* These APIs expose DosList nodes, polymorphic native pointers, or a
   handler protocol not yet covered by typed wrappers. Fail before DOS. */
IPTR kaffeine_dos_UnsupportedLockObject(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return 0;
}

IPTR kaffeine_dos_ReadLinkUnsupported(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    SetIoErr(ERROR_NOT_IMPLEMENTED);
    return -1;
}

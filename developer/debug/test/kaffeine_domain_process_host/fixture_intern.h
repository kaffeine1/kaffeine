/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Minimal native collaborators. The complete production source is compiled. */
#ifndef KAFFEINE_INTERN_H
#define KAFFEINE_INTERN_H
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/bptr.h>
#include <stdlib.h>
struct KaffeineCPU;
struct KaffeineCPUFactory { int fixture; };
struct KaffeineProcessStart;
struct KaffeineDosBroker;
struct KaffeineDomainModule;
struct KaffeineDomainStartRecovery;
struct KaffeineContext {
    struct KaffeineCPU *cpu;
    const struct KaffeineCPUFactory *cpu_factory;
    struct Task *exec_task, *broker_owner;
    BOOL broker_call, running, callback_failed;
    unsigned callback_depth;
    LONG exit_code;
    UBYTE *mem;
    ULONG mem_size, guest_exec_task;
    struct KaffeineDomainStartRecovery *domain_start_recovery;
    BOOL (*domain_drain)(struct KaffeineContext *);
};
static inline void m68k_write32(struct KaffeineContext *ctx, ULONG at, ULONG value)
{
    if (at > ctx->mem_size || ctx->mem_size - at < 4) abort();
    ctx->mem[at] = (UBYTE)(value >> 24);
    ctx->mem[at + 1] = (UBYTE)(value >> 16);
    ctx->mem[at + 2] = (UBYTE)(value >> 8);
    ctx->mem[at + 3] = (UBYTE)value;
}
#endif
#ifndef KAFFEINE_SCHED_H
#define KAFFEINE_SCHED_H
#ifdef __cplusplus
extern "C" {
#endif
ULONG Kaffeine_SchedStartProcess(struct KaffeineContext *, void *,
    const struct KaffeineProcessStart *, ULONG, ULONG,
    struct KaffeineDosBroker *, struct KaffeineDomainModule *, SIPTR *);
BOOL Kaffeine_SchedHasRecoveryStart(struct KaffeineContext *, struct KaffeineDosBroker *);
BOOL Kaffeine_SchedRecoverStart(struct KaffeineContext *, struct KaffeineDosBroker *);
BOOL Kaffeine_DosBrokerIsIdle(struct KaffeineDosBroker *);
#ifdef __cplusplus
}
#endif
#endif
#ifndef KAFFEINE_FILEHANDLE_H
#define KAFFEINE_FILEHANDLE_H
#ifdef __cplusplus
extern "C" {
#endif
ULONG Kaffeine_FileImport(struct KaffeineContext *, BPTR, BOOL);
void Kaffeine_FileDropNative(struct KaffeineContext *, BPTR, BOOL);
#ifdef __cplusplus
}
#endif
#endif
#ifndef KAFFEINE_LOCKS_H
#define KAFFEINE_LOCKS_H
enum KaffeineLockInstalled { KAFFEINE_LOCK_CURRENT, KAFFEINE_LOCK_PROGRAM };
#ifdef __cplusplus
extern "C" {
#endif
BOOL Kaffeine_LockGetInstalled(struct KaffeineContext *, enum KaffeineLockInstalled, ULONG *);
void Kaffeine_LockOwnerDrop(struct KaffeineContext *, struct Task *);
#ifdef __cplusplus
}
#endif
#endif

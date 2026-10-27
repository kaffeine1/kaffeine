/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Synchronous guest calls from native thunks, without exception unwinding.
*/
#ifndef KAFFEINE_CALLBACK_H
#define KAFFEINE_CALLBACK_H

#include "kaffeine_cpu.h"

/* Unused vector space, after the RTE (0x100) and Supervisor trampoline
   (0x108). Each level owns two NOP words for the backend's prefetch. */
#define KAFFEINE_CALLBACK_DEPTH 16u
#define KAFFEINE_CALLBACK_RETURN_BASE 0x000120u
#define KAFFEINE_CALLBACK_RETURN_SIZE (4u * KAFFEINE_CALLBACK_DEPTH)

struct KaffeineGuestCall
{
    ULONG d[8];
    ULONG a[8];
    UWORD d_mask;
    UWORD a_mask; /* A7 is owned by the call primitive, never an input. */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Only masked registers are inputs. On success all D/A results are copied
   out and the caller's D/A, SR and PC are restored. On failure no registers
   are restored: return immediately from the thunk with the live D0. */
BOOL Kaffeine_CallGuest(struct KaffeineContext *ctx, void *cpu, ULONG entry,
                     struct KaffeineGuestCall *call);
void Kaffeine_StopGuest(struct KaffeineContext *ctx, void *cpu, LONG code);

#ifdef __cplusplus
}
#endif

static inline BOOL Kaffeine_IsCallbackReturn(ULONG pc)
{
    return pc >= KAFFEINE_CALLBACK_RETURN_BASE &&
           pc - KAFFEINE_CALLBACK_RETURN_BASE < KAFFEINE_CALLBACK_RETURN_SIZE;
}

#endif

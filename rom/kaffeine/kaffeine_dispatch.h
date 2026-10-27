/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Backend-independent library trap dispatch.
*/

#ifndef KAFFEINE_DISPATCH_H
#define KAFFEINE_DISPATCH_H

#include "kaffeine_cpu.h"

struct KaffeineContext;

struct KaffeineDispatchResult
{
    ULONG return_pc;
    BOOL supervisor;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a backend when it encounters an A-line trap. If pc belongs to
   a fake library jump table, pop the guest JSR return address and invoke the
   matching thunk, preferring manual entries over generated entries. Missing
   LVOs retain the existing return-zero behavior and diagnostic.

   A TRUE result leaves exception framing and control-flow resumption to the
   backend. For Supervisor(), ctx->sv_redirect remains pending and D0 is not
   overwritten with the thunk return. A FALSE result leaves CPU state and
   result untouched, so the backend can handle the non-library exception. */
BOOL Kaffeine_DispatchLibraryCall(struct KaffeineContext *ctx,
                               struct KaffeineCPU *cpu, ULONG pc,
                               struct KaffeineDispatchResult *result);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_DISPATCH_H */

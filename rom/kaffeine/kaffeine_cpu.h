/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Internal CPU backend interface for kaffeine.library.
*/

#ifndef KAFFEINE_CPU_H
#define KAFFEINE_CPU_H

#include <exec/types.h>

struct KaffeineContext;

/* All operations but flush_code are required. Registers and addresses use
   guest widths. execute advances one backend slice (an instruction or
   translated block), not a specified number of clock cycles. It must return
   to the run loop so that a stop requested through ctx->running can be
   observed.

   Register operations must expose the current guest architectural state at
   slice boundaries and throughout native thunk calls. A backend that keeps
   guest registers in host registers must synchronize them before dispatch
   and preserve any changes made by the thunk when guest execution resumes.

   set_pc must prepare execution at the new address (including prefetch).
   execute must be reentrant from a thunk. The adapter must preserve its
   in-flight instruction/exception metadata around native dispatch and must
   end a slice at a callback return token. See kaffeine_callback.h.

   flush_code tells a backend that keeps what it derived from guest code,
   such as translations, that the guest bytes [addr, addr + size) may have
   changed, before guest code there runs again; addr 0 with size 0xFFFFFFFF
   means all guest memory. It is called from thunks and loaders, also while
   execute() waits in a thunk, so it must not free code that is running. A
   backend that fetches every instruction from guest memory leaves it NULL.

   This interface does not describe a complete saved CPU state: in particular,
   it does not enable live backend switching or transfer of FPU state. */
struct KaffeineCPUOps
{
    ULONG (*get_d)(void *state, int reg);
    ULONG (*get_a)(void *state, int reg);
    void  (*set_d)(void *state, int reg, ULONG value);
    void  (*set_a)(void *state, int reg, ULONG value);
    UWORD (*get_sr)(void *state);
    void  (*set_sr)(void *state, UWORD value);
    ULONG (*get_pc)(void *state);
    void  (*set_pc)(void *state, ULONG value);
    void  (*execute)(void *state);
    void  (*flush_code)(void *state, ULONG addr, ULONG size);
};

#define KAFFEINE_FLUSH_ALL 0xFFFFFFFFu   /* size of a flush from address 0 */

struct KaffeineCPU
{
    const struct KaffeineCPUOps *ops;
    void *backend_state;
};

#ifdef __cplusplus
extern "C" {
#endif

/* The opaque cpu argument used by thunks points to KaffeineCPU, never to a
   backend's private CPU object. The backend owns both objects and their
   lifetime; all calls are valid only while that backend instance is alive. */
ULONG Kaffeine_GetD(void *cpu, int reg);
ULONG Kaffeine_GetA(void *cpu, int reg);
void  Kaffeine_SetD(void *cpu, int reg, ULONG value);
void  Kaffeine_SetA(void *cpu, int reg, ULONG value);
UWORD Kaffeine_GetSR(void *cpu);
void  Kaffeine_SetSR(void *cpu, UWORD value);
ULONG Kaffeine_GetPC(void *cpu);
void  Kaffeine_SetPC(void *cpu, ULONG value);

/* The backend must finish reset, entry-register setup and return handling
   before calling this loop. It stops by clearing ctx->running. While it
   runs, ctx->cpu is cpu, for code that has the context but no CPU handle. */
LONG Kaffeine_RunCPU(struct KaffeineContext *ctx, struct KaffeineCPU *cpu);

#ifdef __cplusplus
}
#endif

/* Guest code in [addr, addr + size) may have changed. cpu may be NULL, as
   ctx->cpu is outside Kaffeine_RunCPU(); inline, so that the loaders in
   kaffeine_memory.c can call it without the CPU loop. */
static inline void Kaffeine_FlushCode(void *cpu, ULONG addr, ULONG size)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;

    if (handle && handle->ops->flush_code)
        handle->ops->flush_code(handle->backend_state, addr, size);
}

#endif /* KAFFEINE_CPU_H */

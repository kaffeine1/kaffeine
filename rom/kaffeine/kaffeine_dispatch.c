/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Library/LVO lookup and thunk invocation, independent of the CPU backend.
*/

#include <aros/debug.h>

#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
#include "kaffeine_callback.h"

static const struct M68KThunkEntry *find_thunk(
    const struct M68KThunkEntry *table, ULONG count, ULONG lvo)
{
    ULONG i;

    for (i = 0; i < count; i++)
        if (table[i].lvo == lvo && table[i].thunk)
            return &table[i];

    return NULL;
}

BOOL Kaffeine_DispatchLibraryCall(struct KaffeineContext *ctx,
                               struct KaffeineCPU *cpu, ULONG pc,
                               struct KaffeineDispatchResult *result)
{
    UWORD i;

    for (i = 0; i < ctx->num_libs; i++)
    {
        struct M68KFakeLibBase *lib = &ctx->libs[i];
        const struct M68KThunkEntry *entry;
        const char *kind = "CALL";
        ULONG lvo, sp;

        if (pc < lib->jt_start || pc >= lib->m68k_addr)
            continue;

        lvo = lib->m68k_addr - pc;
        entry = find_thunk(lib->thunks, lib->num_thunks, lvo);
        if (!entry)
        {
            entry = find_thunk(lib->gen_thunks, lib->num_gen_thunks, lvo);
            kind = "GEN";
        }

        sp = Kaffeine_GetA(cpu, 7);
        if (ctx->callback_depth && ((sp & 1) || sp >= ctx->mem_size ||
            ctx->mem_size - sp < 4))
        {
            Kaffeine_StopGuest(ctx, cpu, 20);
            result->return_pc = 0;
            result->supervisor = FALSE;
            return TRUE;
        }
        result->return_pc = m68k_read32(ctx, sp);
        result->supervisor = FALSE;
        Kaffeine_SetA(cpu, 7, sp + 4);

        if (entry)
        {
            IPTR ret;

            bug("[kaffeine] %s %s LVO -%lu\n", kind, lib->name,
                (unsigned long)lvo);
            ret = entry->thunk(ctx, cpu);
            if (ctx->after_thunk)
            {
                void (*after)(struct KaffeineContext *) = ctx->after_thunk;

                ctx->after_thunk = NULL;
                after(ctx);
            }
            if (ctx->sv_redirect)
                result->supervisor = TRUE;
            else
                Kaffeine_SetD(cpu, 0, (ULONG)ret);
        }
        else
        {
            Kaffeine_SetD(cpu, 0, 0);
            bug("[kaffeine] UNIMPL %s LVO -%lu\n", lib->name,
                (unsigned long)lvo);
        }

        return TRUE;
    }

    return FALSE;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Backend-independent register access and execution loop.
*/

#include "kaffeine_cpu.h"
#include "kaffeine_intern.h"
#include "kaffeine_callback.h"

ULONG Kaffeine_GetD(void *cpu, int reg)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    return handle->ops->get_d(handle->backend_state, reg);
}

ULONG Kaffeine_GetA(void *cpu, int reg)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    return handle->ops->get_a(handle->backend_state, reg);
}

void Kaffeine_SetD(void *cpu, int reg, ULONG value)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    handle->ops->set_d(handle->backend_state, reg, value);
}

void Kaffeine_SetA(void *cpu, int reg, ULONG value)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    handle->ops->set_a(handle->backend_state, reg, value);
}

UWORD Kaffeine_GetSR(void *cpu)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    return handle->ops->get_sr(handle->backend_state);
}

void Kaffeine_SetSR(void *cpu, UWORD value)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    handle->ops->set_sr(handle->backend_state, value);
}

ULONG Kaffeine_GetPC(void *cpu)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    return handle->ops->get_pc(handle->backend_state);
}

void Kaffeine_SetPC(void *cpu, ULONG value)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    handle->ops->set_pc(handle->backend_state, value);
}

LONG Kaffeine_RunCPU(struct KaffeineContext *ctx, struct KaffeineCPU *cpu)
{
    struct KaffeineCPU *outer = ctx->cpu;

    ctx->running = TRUE;
    ctx->exit_code = 0;
    ctx->callback_depth = 0;
    ctx->callback_failed = FALSE;
    ctx->cpu = cpu;

    while (ctx->running)
        cpu->ops->execute(cpu->backend_state);

    ctx->cpu = outer;
    ctx->exit_code = (LONG)Kaffeine_GetD(cpu, 0);
    return ctx->exit_code;
}

void Kaffeine_StopGuest(struct KaffeineContext *ctx, void *cpu, LONG code)
{
    ctx->running = FALSE;
    ctx->callback_failed = TRUE;
    ctx->exit_code = code;
    Kaffeine_SetD(cpu, 0, (ULONG)code);
}

BOOL Kaffeine_CallGuest(struct KaffeineContext *ctx, void *cpu, ULONG entry,
                     struct KaffeineGuestCall *call)
{
    struct KaffeineCPU *handle = (struct KaffeineCPU *)cpu;
    ULONG d[8], a[8], pc, sp, token;
    UWORD sr;
    unsigned i;

    if (!ctx->running)
        return FALSE;
    sp = Kaffeine_GetA(cpu, 7);
    if (!entry || (entry & 1) || entry >= ctx->mem_size ||
        ctx->mem_size - entry < 2 || Kaffeine_IsCallbackReturn(entry) ||
        ctx->callback_depth >= KAFFEINE_CALLBACK_DEPTH ||
        (sp & 1) || sp < 4 || sp - 4 >= ctx->mem_size ||
        ctx->mem_size - (sp - 4) < 4)
    {
        Kaffeine_StopGuest(ctx, cpu, 20);
        return FALSE;
    }
    for (i = 0; i < 8; i++)
    {
        d[i] = Kaffeine_GetD(cpu, i);
        a[i] = Kaffeine_GetA(cpu, i);
    }
    sr = Kaffeine_GetSR(cpu);
    pc = Kaffeine_GetPC(cpu);
    token = KAFFEINE_CALLBACK_RETURN_BASE + 4u * ctx->callback_depth++;
    m68k_write32(ctx, sp - 4, token);
    for (i = 0; i < 8; i++)
    {
        if (call->d_mask & (1u << i)) Kaffeine_SetD(cpu, i, call->d[i]);
        if (i < 7 && (call->a_mask & (1u << i))) Kaffeine_SetA(cpu, i, call->a[i]);
    }
    Kaffeine_SetA(cpu, 7, sp - 4);
    Kaffeine_SetPC(cpu, entry);

    while (ctx->running)
    {
        ULONG current_pc = Kaffeine_GetPC(cpu);
        ULONG current_sp = Kaffeine_GetA(cpu, 7);
        if (Kaffeine_IsCallbackReturn(current_pc))
        {
            if (current_pc == token && current_sp == sp)
                break;
            Kaffeine_StopGuest(ctx, cpu, 20);
            break;
        }
        if ((current_sp & 1) || current_sp >= ctx->mem_size ||
            ctx->mem_size - current_sp < 2)
        {
            Kaffeine_StopGuest(ctx, cpu, 20);
            break;
        }
        handle->ops->execute(handle->backend_state);
    }
    ctx->callback_depth--;
    if (!ctx->running)
    {
        /* A thunk may have stopped with its own process return code. */
        Kaffeine_StopGuest(ctx, cpu, (LONG)Kaffeine_GetD(cpu, 0));
        return FALSE;
    }

    for (i = 0; i < 8; i++)
    {
        call->d[i] = Kaffeine_GetD(cpu, i);
        call->a[i] = Kaffeine_GetA(cpu, i);
    }
    Kaffeine_SetSR(cpu, sr);
    for (i = 0; i < 8; i++)
    {
        Kaffeine_SetD(cpu, i, d[i]);
        Kaffeine_SetA(cpu, i, a[i]);
    }
    Kaffeine_SetPC(cpu, pc);
    return TRUE;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Backend-independent register access and execution loop.
*/

#include <aros/debug.h>
#include "kaffeine_cpu.h"
#include "kaffeine_async.h"
#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"

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
    struct KaffeineCPU *outer = ctx->cpu, *current;

    ctx->running = TRUE;
    ctx->exit_code = 0;
    ctx->callback_depth = 0;
    ctx->callback_failed = FALSE;
    ctx->cpu = cpu;

    /* With AddTask() children, ctx->cpu is the CPU of the current guest
       task. It changes only here, between slices of the outermost loop,
       where no callback or thunk of any task is in flight. */
    while (ctx->running)
    {
        current = ctx->cpu;
        current->ops->execute(current->backend_state);
        /* Poll every outer slice, also without a scheduler or a guest Wait.
           No thunk or callback is in flight and the driver owns ctx->cpu. */
        if (!outer && ctx->running) (void)Kaffeine_AsyncPoll(ctx);
        /* A failed start can retain native work and a staged CPU. Drain on
           the live owner before a scheduler boundary can retire anything. */
        if (!ctx->running && Kaffeine_LayerInfoLocked(ctx) && ctx->layer_ops->release_locks)
            ctx->layer_ops->release_locks(ctx);
        if (!ctx->running && ctx->domain_drain)
            while (!ctx->domain_drain(ctx)) { }
        /* A completed slice of the outermost loop: no thunk or callback is
           in flight, so a GUI snapshot may change under the guest here. */
        if (!outer && ctx->gui_poll && ctx->running &&
            ++ctx->gui_poll_slices >= KAFFEINE_GUI_POLL_SLICES)
        {
            ctx->gui_poll_slices = 0;
            ctx->gui_poll(ctx);
        }
        if (ctx->sched && !outer)
            ctx->task_ops->boundary(ctx);
    }

    /* Never enter owner/broker cleanup while a native Info scope remains. */
    if (Kaffeine_LayerInfoLocked(ctx) && ctx->layer_ops->release_locks)
        ctx->layer_ops->release_locks(ctx);
    if (ctx->domain_drain)
        while (!ctx->domain_drain(ctx)) { }
    /* A stop in a child ends the whole program with that child's D0. */
    current = ctx->cpu;
    /* A refused thunk may return an ordinary failure value after StopGuest
       set the authoritative failure code. Do not turn that stop into D0=0. */
    if (!ctx->callback_failed)
        ctx->exit_code = (LONG)Kaffeine_GetD(current, 0);
    if (ctx->sched && !outer && !ctx->callback_failed && ctx->task_ops->root_result)
        ctx->task_ops->root_result(ctx, &ctx->exit_code);
    if (current != cpu)
        Kaffeine_SetD(cpu, 0, (ULONG)ctx->exit_code);
    if (!outer && Kaffeine_LayerInfoLocked(ctx) && ctx->layer_ops->release_locks)
        ctx->layer_ops->release_locks(ctx);
    if (ctx->sched && !outer)
        ctx->task_ops->end(ctx);
    else if (!outer && ctx->guest_exec_task && ctx->layer_ops && ctx->layer_ops->task_removed)
        ctx->layer_ops->task_removed(ctx, ctx->guest_exec_task);
    ctx->cpu = outer;
    return ctx->exit_code;
}

void Kaffeine_StopGuest(struct KaffeineContext *ctx, void *cpu, LONG code)
{
    if (code == 20 && !ctx->callback_failed)
        bug("[kaffeine] forced stop (20), caller delta=%ld callback depth=%u\n",
            (long)((IPTR)__builtin_return_address(0) - (IPTR)Kaffeine_StopGuest),
            (unsigned)ctx->callback_depth);
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

    if (ctx->broker_call || Kaffeine_LayerInfoLocked(ctx))
    {
        bug("[kaffeine] guest callback with broker=%u LayerInfo=%u, stopping (20)\n",
            (unsigned)!!ctx->broker_call, (unsigned)!!Kaffeine_LayerInfoLocked(ctx));
        Kaffeine_StopGuest(ctx, cpu, 20);
        return FALSE;
    }
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
        /* A child that returns inside a callback cannot end there. */
        if (current_pc == KAFFEINE_TASK_EXIT_ADDR ||
            (current_sp & 1) || current_sp >= ctx->mem_size ||
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
        /* A refused void thunk may have replaced D0 after StopGuest. Keep
           its authoritative code; an unmarked process stop still uses D0. */
        Kaffeine_StopGuest(ctx, cpu, ctx->callback_failed ? ctx->exit_code :
                           (LONG)Kaffeine_GetD(cpu, 0));
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

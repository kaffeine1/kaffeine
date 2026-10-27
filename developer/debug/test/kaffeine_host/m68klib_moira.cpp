/* Keep the unmodified core in a separate object for quick mutation rebuilds. */
#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

extern "C" KaffeineCPU *m68klib_test_create_cpu(KaffeineContext *ctx)
{
    auto *cpu = new AROSMoira(ctx);
    m68k_write32(ctx, 0, ctx->stack_top);
    m68k_write32(ctx, 4, ctx->entry_point);
    cpu->reset();
    m68k_write32(ctx, 4, ctx->m68k_sysbase);
    m68k_write16(ctx, RTE_ADDR, M68K_OP_RTE);
    m68k_write32(ctx, M68K_VEC_LINEA, RTE_ADDR);
    cpu->setA(7, ctx->stack_top - 256);
    cpu->setA(0, 0xBAD00000);
    cpu->setA(6, 0xBAD60000);
    cpu->setD(0, 0xBADD0000);
    ctx->running = TRUE;
    return &cpu->cpu_handle;
}

extern "C" void m68klib_test_delete_cpu(KaffeineCPU *cpu)
{
    delete static_cast<AROSMoira *>(cpu->backend_state);
}

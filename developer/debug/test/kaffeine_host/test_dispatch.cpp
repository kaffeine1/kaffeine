/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 *
 * Library dispatch tests using a small CPU backend with no Moira dependency.
 */

/* Keep checks and the calls they exercise active with release build flags. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <aros/debug.h>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

#include "../../../../rom/kaffeine/kaffeine_cpu.c"

static char dispatch_debug[1024];

static void capture_dispatch_debug(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(dispatch_debug, sizeof(dispatch_debug), format, args);
    va_end(args);
}

#define bug capture_dispatch_debug
#include "../../../../rom/kaffeine/kaffeine_dispatch.c"
#undef bug

struct TestCPU
{
    KaffeineContext *ctx;
    ULONG d[8];
    ULONG a[8];
    UWORD sr;
    ULONG pc;
    unsigned int manual_calls;
    unsigned int generated_calls;
    unsigned int slices;
    KaffeineCPU *running_cpu;
    unsigned int flushes;
    ULONG flush_addr, flush_size;
};

static ULONG test_get_d(void *state, int reg)
{
    return static_cast<TestCPU *>(state)->d[reg];
}

static ULONG test_get_a(void *state, int reg)
{
    return static_cast<TestCPU *>(state)->a[reg];
}

static void test_set_d(void *state, int reg, ULONG value)
{
    static_cast<TestCPU *>(state)->d[reg] = value;
}

static void test_set_a(void *state, int reg, ULONG value)
{
    static_cast<TestCPU *>(state)->a[reg] = value;
}

static UWORD test_get_sr(void *state)
{
    return static_cast<TestCPU *>(state)->sr;
}

static void test_set_sr(void *state, UWORD value)
{
    static_cast<TestCPU *>(state)->sr = value;
}

static ULONG test_get_pc(void *state)
{
    return static_cast<TestCPU *>(state)->pc;
}

static void test_set_pc(void *state, ULONG value)
{
    static_cast<TestCPU *>(state)->pc = value;
}

static void test_execute(void *state)
{
    auto *cpu = static_cast<TestCPU *>(state);
    assert(cpu->ctx->running);
    assert(cpu->ctx->exit_code == 0);
    cpu->running_cpu = cpu->ctx->cpu;
    if (++cpu->slices == 3)
    {
        cpu->d[0] = static_cast<ULONG>(-7);
        cpu->ctx->running = FALSE;
    }
}

static void test_flush_code(void *state, ULONG addr, ULONG size)
{
    auto *cpu = static_cast<TestCPU *>(state);
    cpu->flushes++;
    cpu->flush_addr = addr;
    cpu->flush_size = size;
}

static const KaffeineCPUOps test_ops =
{
    test_get_d, test_get_a, test_set_d, test_set_a,
    test_get_sr, test_set_sr, test_get_pc, test_set_pc, test_execute, nullptr
};

/* A backend that keeps translations. */
static const KaffeineCPUOps flushing_ops =
{
    test_get_d, test_get_a, test_set_d, test_set_a,
    test_get_sr, test_set_sr, test_get_pc, test_set_pc, test_execute, test_flush_code
};

static IPTR manual_thunk(KaffeineContext *ctx, void *cpu)
{
    auto *state = static_cast<TestCPU *>(static_cast<KaffeineCPU *>(cpu)->backend_state);
    assert(state->ctx == ctx);
    assert(Kaffeine_GetA(cpu, 7) == 0x84);
    state->manual_calls++;
    Kaffeine_SetD(cpu, 2, 0x76543210);
    Kaffeine_SetA(cpu, 3, 0xABCDEF00);
    Kaffeine_SetSR(cpu, 0x0015);
    return Kaffeine_GetD(cpu, 1) + 42;
}

static IPTR generated_thunk(KaffeineContext *ctx, void *cpu)
{
    auto *state = static_cast<TestCPU *>(static_cast<KaffeineCPU *>(cpu)->backend_state);
    assert(state->ctx == ctx);
    assert(Kaffeine_GetA(cpu, 7) == 0x84);
    state->generated_calls++;
    return 99;
}

static IPTR supervisor_thunk(KaffeineContext *ctx, void *cpu)
{
    ctx->sv_redirect = 0x2345;
    Kaffeine_SetD(cpu, 0, 0xAABBAABB);
    return 7;
}

static const M68KThunkEntry manual_table[] =
{
    { 6, manual_thunk },
    { 12, nullptr },
    { 30, supervisor_thunk }
};

static const M68KThunkEntry generated_table[] =
{
    { 6, generated_thunk },
    { 12, generated_thunk }
};

struct Fixture
{
    KaffeineContext ctx = {};
    UBYTE memory[512] = {};
    TestCPU state = {};
    KaffeineCPU cpu = { &test_ops, &state };

    Fixture()
    {
        ctx.mem = memory;
        ctx.mem_size = sizeof(memory);
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.jt_start = 0xF00;
        lib.m68k_addr = 0x1000;
        std::strcpy(lib.name, "test.library");
        lib.thunks = manual_table;
        lib.num_thunks = sizeof(manual_table) / sizeof(manual_table[0]);
        lib.gen_thunks = generated_table;
        lib.num_gen_thunks = sizeof(generated_table) / sizeof(generated_table[0]);
        state.ctx = &ctx;
        state.a[7] = 0x80;
        state.d[0] = 0xDEADBEEF;
        state.d[1] = 8;
        state.pc = 0x400;
        m68k_write32(&ctx, 0x80, 0x10203040);
    }
};

static void test_register_interface()
{
    Fixture first, second;

    for (int i = 0; i < 8; i++)
    {
        Kaffeine_SetD(&first.cpu, i, 0xF0000000u + i);
        Kaffeine_SetA(&first.cpu, i, 0xE0000000u + i);
        assert(Kaffeine_GetD(&first.cpu, i) == 0xF0000000u + i);
        assert(Kaffeine_GetA(&first.cpu, i) == 0xE0000000u + i);
    }
    Kaffeine_SetSR(&first.cpu, 0xA71F);
    Kaffeine_SetPC(&first.cpu, 0xFEDCBA98);
    assert(Kaffeine_GetSR(&first.cpu) == 0xA71F);
    assert(Kaffeine_GetPC(&first.cpu) == 0xFEDCBA98);
    assert(Kaffeine_GetD(&second.cpu, 0) == 0xDEADBEEF);
    assert(Kaffeine_GetA(&second.cpu, 7) == 0x80);
    assert(Kaffeine_GetSR(&second.cpu) == 0);
    assert(Kaffeine_GetPC(&second.cpu) == 0x400);
}

static void test_manual_precedence()
{
    Fixture f;
    KaffeineDispatchResult result = {};

    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 6, &result));
    assert(f.state.manual_calls == 1);
    assert(f.state.generated_calls == 0);
    assert(f.state.a[7] == 0x84);
    assert(f.state.d[0] == 50);
    assert(f.state.d[2] == 0x76543210);
    assert(f.state.a[3] == 0xABCDEF00);
    assert(f.state.sr == 0x0015);
    assert(result.return_pc == 0x10203040);
    assert(!result.supervisor);
    assert(f.state.pc == 0x400); /* Only the backend resumes control flow. */
}

static void test_generated_fallback()
{
    Fixture f;
    KaffeineDispatchResult result = {};

    /* A manual entry without a handler must not hide a generated handler. */
    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 12, &result));
    assert(f.state.manual_calls == 0);
    assert(f.state.generated_calls == 1);
    assert(f.state.a[7] == 0x84);
    assert(f.state.d[0] == 99);
    assert(result.return_pc == 0x10203040);
    assert(!result.supervisor);
}

static void test_second_library()
{
    Fixture f;
    KaffeineDispatchResult result = {};
    f.ctx.libs[1] = f.ctx.libs[0];
    f.ctx.libs[1].jt_start = 0x1F00;
    f.ctx.libs[1].m68k_addr = 0x2000;
    f.ctx.libs[1].thunks = nullptr;
    f.ctx.libs[1].num_thunks = 0;
    std::strcpy(f.ctx.libs[1].name, "second.library");
    f.ctx.num_libs = 2;

    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x2000 - 6, &result));
    assert(f.state.manual_calls == 0);
    assert(f.state.generated_calls == 1);
    assert(f.state.d[0] == 99);
    assert(f.state.a[7] == 0x84);
}

static void test_unimplemented_lvo()
{
    Fixture f;
    KaffeineDispatchResult result = {};

    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 18, &result));
    assert(f.state.d[0] == 0);
    assert(f.state.d[1] == 8);
    assert(f.state.a[7] == 0x84);
    assert(f.state.manual_calls == 0);
    assert(f.state.generated_calls == 0);
    assert(result.return_pc == 0x10203040);
    assert(!result.supervisor);
}

static void test_unknown_aline()
{
    Fixture f;
    KaffeineDispatchResult result = { 0x12345678, TRUE };

    for (ULONG pc : { 0xEFFu, 0x1000u, 0x2000u })
    {
        assert(!Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, pc, &result));
        assert(f.state.d[0] == 0xDEADBEEF);
        assert(f.state.a[7] == 0x80);
        assert(f.state.pc == 0x400);
        assert(result.return_pc == 0x12345678);
        assert(result.supervisor);
    }
}

static void test_unimplemented_register_snapshot()
{
    Fixture f;
    KaffeineDispatchResult result = {};
    UBYTE memory_before[sizeof(f.memory)];

    std::memcpy(memory_before, f.memory, sizeof(f.memory));
    std::strcpy(f.ctx.libs[0].name, "exec.library");
    f.state.d[7] = 0x80000035;
    f.state.a[0] = 0x12345678;
    f.state.a[1] = 0x87654321;
    dispatch_debug[0] = '\0';

    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 108, &result));
    assert(std::strstr(dispatch_debug, "UNIMPL exec.library LVO -108"));
    assert(std::strstr(dispatch_debug, "PC=00000f94 return=10203040"));
    assert(std::strstr(dispatch_debug, "D0=deadbeef D1=00000008 D7=80000035"));
    assert(std::strstr(dispatch_debug, "A0=12345678 A1=87654321"));
    assert(f.state.d[0] == 0 && f.state.d[1] == 8 && f.state.d[7] == 0x80000035);
    assert(f.state.a[0] == 0x12345678 && f.state.a[1] == 0x87654321);
    assert(f.state.a[7] == 0x84 && result.return_pc == 0x10203040 && !result.supervisor);
    assert(std::memcmp(memory_before, f.memory, sizeof(f.memory)) == 0);
    assert(f.state.manual_calls == 0 && f.state.generated_calls == 0);
}

static void test_supervisor_redirect()
{
    Fixture f;
    KaffeineDispatchResult result = {};

    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, 0x1000 - 30, &result));
    assert(result.supervisor);
    assert(result.return_pc == 0x10203040);
    assert(f.ctx.sv_redirect == 0x2345);
    assert(f.state.a[7] == 0x84);
    assert(f.state.d[0] == 0xAABBAABB);
    assert(f.state.pc == 0x400);
}

static void test_execution_loop()
{
    Fixture f;
    KaffeineCPU outer = { &test_ops, nullptr };
    f.ctx.exit_code = 123;
    f.ctx.cpu = &outer;

    assert(Kaffeine_RunCPU(&f.ctx, &f.cpu) == -7);
    assert(f.state.slices == 3);
    assert(!f.ctx.running);
    assert(f.ctx.exit_code == -7);
    /* The running CPU is in the context for the loaders, and only then. */
    assert(f.state.running_cpu == &f.cpu);
    assert(f.ctx.cpu == &outer);
}

static void test_flush_code()
{
    Fixture f;

    /* No CPU, as outside Kaffeine_RunCPU(), and a backend without the
       operation, as Moira: nothing to call. */
    Kaffeine_FlushCode(nullptr, 0, KAFFEINE_FLUSH_ALL);
    Kaffeine_FlushCode(f.ctx.cpu, 0x100, 4);
    Kaffeine_FlushCode(&f.cpu, 0x100, 4);
    assert(f.state.flushes == 0);

    f.cpu.ops = &flushing_ops;
    Kaffeine_FlushCode(&f.cpu, 0x100, 4);
    assert(f.state.flushes == 1 && f.state.flush_addr == 0x100 && f.state.flush_size == 4);
    Kaffeine_FlushCode(&f.cpu, 0, KAFFEINE_FLUSH_ALL);
    assert(f.state.flushes == 2 && f.state.flush_addr == 0 &&
           f.state.flush_size == 0xFFFFFFFFu);
    /* The rest of the state is the backend's own. */
    assert(f.state.d[0] == 0xDEADBEEF && f.state.a[7] == 0x80 && f.state.pc == 0x400);
}

int main()
{
    test_register_interface();
    test_manual_precedence();
    test_generated_fallback();
    test_second_library();
    test_unimplemented_lvo();
    test_unimplemented_register_snapshot();
    test_unknown_aline();
    test_supervisor_redirect();
    test_execution_loop();
    test_flush_code();
    std::puts("PASS: CPU interface and library dispatch (10 cases, no Moira)");
    return 0;
}

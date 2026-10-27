/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    FindTask identity through dispatch and the real ExecBase shadow engine.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_tasks.h"
#include "kaffeine_shadow.h"

static unsigned checks, failures, native_calls, allocations, frees, generated_calls;
static bool allocation_fails;
static ULONG next_allocation;
static Process current_process;
static Task worker_task, third_task;
static Task *native_current;

static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

extern "C" Task *FindTask(CONST_STRPTR name)
{
    ++native_calls;
    if (!name) return native_current;
    if (!std::strcmp(name, "current")) return &current_process.pr_Task;
    if (!std::strcmp(name, "worker") || !std::strcmp(name, "alias")) return &worker_task;
    if (!std::strcmp(name, "third")) return &third_task;
    return nullptr;
}

extern "C" ULONG Kaffeine_HeapAlloc(KaffeineContext *ctx, ULONG size, ULONG)
{
    ++allocations;
    if (allocation_fails) return 0;
    assert(size <= ctx->mem_size && next_allocation <= ctx->mem_size - size);
    ULONG result = next_allocation;
    next_allocation += (size + 7) & ~7U;
    return result;
}
extern "C" void Kaffeine_HeapFree(KaffeineContext *, ULONG, ULONG) { ++frees; }
extern "C" ULONG Kaffeine_GetCLI(KaffeineContext *ctx) { return ctx->m68k_cli = 0x800; }
extern "C" BPTR Input(void) { return reinterpret_cast<BPTR>(0x1234); }
extern "C" BPTR Output(void) { return reinterpret_cast<BPTR>(0x5678); }

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0xABCDEF12;
};
static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static const KaffeineCPUOps ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD00294; }
static const M68KThunkEntry manual[] = { KAFFEINE_EXEC_TASK_THUNKS };
static const M68KThunkEntry generated[] = { {294, poison} };

struct Fixture
{
    static constexpr ULONG base = 0x1000, stack = 0xF00, name = 0x600;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x8000> memory;
    ExecBase native = {};
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };

    Fixture(bool initialize = true)
    {
        static_assert(sizeof(Task) >= 176 && sizeof(ExecBase) >= 1304);
        static_assert(offsetof(Task, tc_Node.ln_Type) == 16);
        static_assert(offsetof(Library, lib_Version) == 38);
        current_process = {}; worker_task = {}; third_task = {};
        current_process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
        current_process.pr_CES = reinterpret_cast<BPTR>(0x9ABC);
        worker_task.tc_Node.ln_Type = NT_PROCESS;
        third_task.tc_Node.ln_Type = NT_TASK;
        native_current = &current_process.pr_Task;
        native_calls = allocations = frees = generated_calls = 0;
        allocation_fails = false;
        next_allocation = 0x2000;
        memory.fill(0xA5);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.m68k_sysbase = base;
        ctx.m68k_argptr = 0x700;
        native.LibNode.lib_Version = 47;
        native.LibNode.lib_Revision = 12;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = base;
        lib.jt_start = base - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual;
        lib.num_thunks = sizeof(manual) / sizeof(manual[0]);
        lib.gen_thunks = generated;
        lib.num_gen_thunks = sizeof(generated) / sizeof(generated[0]);
        m68k_write32(&ctx, stack, 0x76543210);
        if (initialize) init();
    }

    ULONG init() { return shadow_init_execbase(&ctx, base, &native); }
    ULONG this_task() { return m68k_read32(&ctx, base + 276); }
    ULONG call(ULONG argument)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.a[0] = 0xFFFFFFFF;
        state.a[1] = argument;
        state.a[7] = stack;
        const auto before = state;
        const auto native_before = native;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - 294, &result));
        assert(state.a[7] == stack + 4 && result.return_pc == 0x76543210 && !result.supervisor);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(!std::memcmp(&native, &native_before, sizeof(native)));
        return state.d[0];
    }
    ULONG named(const char *text)
    {
        std::strcpy(reinterpret_cast<char *>(memory.data() + name), text);
        return call(name);
    }
};

static void test_current_identity()
{
    Fixture f;
    const ULONG task = f.this_task();
    const unsigned prepared_calls = native_calls, prepared_allocs = allocations;
    const auto prepared = f.memory;
    check(task && f.call(0) == task && f.call(0) == task,
          "literal -294 A1=NULL equals the real ExecBase shadow's ThisTask every time");
    check(native_calls == prepared_calls && allocations == prepared_allocs && f.memory == prepared,
          "FindTask(NULL) does not allocate, call native FindTask or resync guest edits");
    m68k_write32(&f.ctx, f.base + 276, 0);
    check(f.call(0) == 0 && native_calls == prepared_calls && allocations == prepared_allocs,
          "NULL lookup reads the guest ThisTask field each time, including a guest-cleared field");
    m68k_write32(&f.ctx, f.base + 276, task);
    check(m68k_read32(&f.ctx, task + 172) == (0x800 >> 2) &&
          m68k_read32(&f.ctx, task + 204) == f.ctx.m68k_argptr &&
          m68k_read32(&f.ctx, task + 156) == 0x1234 &&
          m68k_read32(&f.ctx, task + 160) == 0x5678 &&
          m68k_read32(&f.ctx, task + 224) == 0x9ABC,
          "the initial Process snapshot retains the current CLI, arguments and streams, "
          "the error stream included");
    check(f.named("current") == task && allocations == prepared_allocs,
          "a named lookup of the initial native task reuses ThisTask");
    m68k_write32(&f.ctx, task + 172, 0xBEEF);
    const auto edited = f.memory;
    native_current = &worker_task;
    check(f.call(0) == task && f.memory == edited,
          "guest identity survives execution in a different native process");
    check(generated_calls == 0, "FindTask override never uses the generated fallback");

    const ULONG pub = f.base + 532;
    check(m68k_read32(&f.ctx, pub) == pub + 4 && !m68k_read32(&f.ctx, pub + 4) &&
          m68k_read32(&f.ctx, pub + 8) == pub && f.memory[pub + 12] == NT_SIGNALSEM,
          "ExecBase initialization creates the guest public semaphore list");
    bool public_is_guest = true;
    for (unsigned i = 0; i < f.ctx.num_shadows; ++i)
        public_is_guest &= f.ctx.shadow_map[i].m68k_addr != pub;
    check(public_is_guest, "the guest public semaphore list is not a native list shadow");
    const ULONG ports = f.base + 392;
    check(m68k_read32(&f.ctx, ports) == ports + 4 && !m68k_read32(&f.ctx, ports + 4) &&
          m68k_read32(&f.ctx, ports + 8) == ports && f.memory[ports + 12] == NT_MSGPORT &&
          !f.memory[ports + 13],
          "ExecBase initialization creates the guest public port list");
    bool ports_are_guest = true;
    for (unsigned i = 0; i < f.ctx.num_shadows; ++i)
        ports_are_guest &= f.ctx.shadow_map[i].m68k_addr != ports;
    check(ports_are_guest, "the guest public port list is not a native list shadow");
}

static void test_named_identity_and_foreign_fields()
{
    Fixture f;
    ULONG guest = f.named("worker");
    check(guest && guest != f.this_task() && f.named("worker") == guest &&
          f.named("alias") == guest,
          "repeated and alias lookups of the same native task return one shadow");
    const unsigned allocated = allocations;
    check(f.named("worker") == guest && allocations == allocated,
          "repeated named lookup does not allocate again");
    bool zero_process = true;
    for (unsigned i = 92; i < 228; ++i) zero_process &= f.memory[guest + i] == 0;
    check(zero_process, "foreign Process fields are zero despite dirty heap memory");
    check(f.memory[guest - 1] == 0xA5 && f.memory[guest + 228] == 0xA5,
          "task construction writes only its complete 228-byte allocation");
    m68k_write32(&f.ctx, guest + 172, 0xDEADBEEF);
    check(f.named("worker") == guest && m68k_read32(&f.ctx, guest + 172) == 0xDEADBEEF,
          "cached named lookups preserve guest changes to the snapshot");
    ULONG third = f.named("third");
    check(third && third != guest && f.named("third") == third &&
          f.memory[third + 8] == NT_TASK && m68k_read32(&f.ctx, third + 172) == 0,
          "ordinary native tasks also have stable zeroed Process-sized snapshots");
    check(f.named("missing") == 0, "missing native task returns NULL");
}

static void test_full_table_and_allocation_failure()
{
    Fixture f;
    const ULONG worker = f.named("worker");
    while (f.ctx.num_shadows < KAFFEINE_MAX_SHADOWS)
        assert(shadow_register(&f.ctx, 0x5000 + f.ctx.num_shadows, reinterpret_cast<void *>(1), 0));
    const unsigned allocated = allocations;
    check(f.named("worker") == worker && f.named("current") == f.this_task() &&
          f.call(0) == f.this_task(),
          "a full registry retains current and existing named identities");
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + f.name), "third");
    const auto before = f.memory;
    check(f.call(f.name) == 0 && f.memory == before && allocations == allocated && !frees,
          "a new native task at capacity returns NULL before allocating or writing");

    Fixture failed;
    allocation_fails = true;
    check(failed.named("worker") == 0 && !frees,
          "named task allocation failure publishes no shadow");
    allocation_fails = false;
    ULONG retry = failed.named("worker");
    check(retry && failed.named("worker") == retry,
          "a failed allocation can be retried without leaving an unstable identity");

    Fixture initial(false);
    allocation_fails = true;
    check(initial.init() == initial.base && initial.this_task() == 0 && initial.call(0) == 0,
          "failed initial Task allocation leaves ThisTask and FindTask(NULL) both NULL");
}

static void test_bad_names_and_bases()
{
    Fixture f;
    for (ULONG address : {0x8000u, 0x7FFFu, 0xFFFFFFFEu, 0xFFFFFFFFu})
    {
        const auto before = f.memory;
        const auto calls_before = native_calls;
        check(f.call(address) == 0 && f.memory == before && native_calls == calls_before,
              "bad or unterminated A1 never reaches native FindTask");
    }
    for (ULONG base : {0u, 0x8000u, 0x7FFFu, 0xFFFFFFFEu, 0xFFFFFFFFu})
    {
        f.ctx.m68k_sysbase = base;
        const auto before = f.memory;
        check(f.call(0) == 0 && f.memory == before,
              "FindTask(NULL) checks the complete ThisTask field without address wrap");
        check(shadow_init_execbase(&f.ctx, base, &f.native) == 0 && f.memory == before,
              "ExecBase initialization checks its whole guest allocation before writing");
    }
    Fixture absent(false);
    native_current = nullptr;
    check(absent.init() == absent.base && !absent.this_task() && absent.call(0) == 0,
          "an absent native current task does not dereference NULL during setup");
}

static void test_fallback_control()
{
    Fixture f;
    f.ctx.libs[0].thunks = nullptr;
    f.ctx.libs[0].num_thunks = 0;
    check(f.call(0) == 0xBAD00294 && generated_calls == 1,
          "generated poison for literal -294 is reachable without the override");
}

int main(int argc, char **argv)
{
    test_current_identity();
    test_named_identity_and_foreign_fields();
    /* Also permits a focused comparison with the old production handler,
       whose unchecked guest names and bases must not be used for bad inputs. */
    if (!(argc == 2 && !std::strcmp(argv[1], "--identity-only")))
    {
        test_full_table_and_allocation_failure();
        test_bad_names_and_bases();
        test_fallback_control();
    }
    std::printf("Guest task regressions: %u/%u checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}

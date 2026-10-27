/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Check the production Exec slot overrides through the real dispatcher.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kaffeine_intern.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_dispatch.h"

/* The linked production heap also contains the library auto-open helper. */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

/* The linked compatibility handlers forward only native messages. */
extern "C" void PutMsg(struct MsgPort *, struct Message *)
{
    assert(false);
}

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0x0015;
    ULONG pc = 0x43210;
};

static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }

static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr
};

static std::array<unsigned, 6> generated_calls;

template <unsigned Index, ULONG Tag>
static IPTR generated_slot(KaffeineContext *, void *)
{
    generated_calls[Index]++;
    return Tag;
}

static const M68KThunkEntry manual_table[] = { KAFFEINE_EXEC_COMPAT_THUNKS, };
/* Literal ABI offsets are independent of the production registration macro. */
static const M68KThunkEntry generated_table[] =
{
    { 702, generated_slot<0, 0xBAD00702U> },
    { 720, generated_slot<1, 0xBAD00720U> },
    { 852, generated_slot<2, 0xBAD00852U> },
    { 858, generated_slot<3, 0xBAD00858U> },
    { 864, generated_slot<4, 0xCAFE0864U> },
    { 870, generated_slot<5, 0xBAD00870U> }
};

struct Fixture
{
    static constexpr ULONG stack = 0xE00, libbase = 0x10000;
    static constexpr ULONG semaphore = 0x200;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x1000> memory;
    std::array<UBYTE, 256> native_memory;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        memory.fill(0xA5);
        native_memory.fill(0xC7);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = libbase;
        lib.jt_start = libbase - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual_table;
        lib.num_thunks = sizeof(manual_table) / sizeof(manual_table[0]);
        lib.gen_thunks = generated_table;
        lib.num_gen_thunks = sizeof(generated_table) / sizeof(generated_table[0]);
        m68k_write32(&ctx, stack, 0x12345678);
        generated_calls.fill(0);
    }

    ULONG call(ULONG lvo, ULONG argument, int expected_gen_idx = -1)
    {
        for (unsigned i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0000000U + i;
            state.a[i] = 0xA0000000U + i;
        }
        state.a[0] = argument;
        state.a[7] = stack;
        m68k_write32(&ctx, stack, 0x12345678);
        const TestCPU before = state;
        const auto before_memory = memory;
        const auto before_native = native_memory;
        auto expected_generated = generated_calls;
        if (expected_gen_idx >= 0)
            expected_generated[expected_gen_idx]++;

        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, libbase - lvo, &result));
        assert(result.return_pc == 0x12345678 && !result.supervisor);
        /* The dispatcher pops the guest return address; only D0 is a result. */
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; i++)
            assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; i++)
            assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(memory == before_memory);
        assert(native_memory == before_native);
        assert(generated_calls == expected_generated);
        return state.d[0];
    }
};

static void test_registration()
{
    assert(sizeof(manual_table) / sizeof(manual_table[0]) == 27);
    const M68KThunkEntry *delete_pool = nullptr, *attempt_shared = nullptr;
    const M68KThunkEntry *avl_add = nullptr, *avl_rem_addr = nullptr, *avl_find = nullptr;
    for (const auto &entry : manual_table)
    {
        if (entry.lvo == 702) { assert(!delete_pool); delete_pool = &entry; }
        if (entry.lvo == 720) { assert(!attempt_shared); attempt_shared = &entry; }
        if (entry.lvo == 852) { assert(!avl_add); avl_add = &entry; }
        if (entry.lvo == 858) { assert(!avl_rem_addr); avl_rem_addr = &entry; }
        if (entry.lvo == 870) { assert(!avl_find); avl_find = &entry; }
        assert(entry.lvo != 864);
    }
    assert(delete_pool && delete_pool->thunk == Kaffeine_DeletePool);
    assert(attempt_shared && attempt_shared->thunk == Kaffeine_AttemptSemaphoreShared);
    assert(avl_add && avl_add->thunk == thunk_exec_AVL_AddNode);
    assert(avl_rem_addr && avl_rem_addr->thunk == thunk_exec_AVL_RemNodeByAddress);
    assert(avl_find && avl_find->thunk == thunk_exec_AVL_FindNode);
}

static void test_delete_pool_containment_handle()
{
    Fixture f;
    /* The former fake pool handle is not a live guest pool. */
    assert(f.call(702, 0x00CAFE00) == 0);
}

static void test_invalid_shared_attempt_fails_safely()
{
    Fixture f;
    /* Valid free semaphore bytes alone are insufficient: this fixture has
       no guest ExecBase/ThisTask identity. The lifecycle fixture supplies it. */
    /* A classic 46-byte SignalSemaphore, initialized and uncontended.
       Even this case must report failure until locking is implemented. */
    std::memset(f.memory.data() + f.semaphore, 0, 46);
    f.memory[f.semaphore + 8] = 15; /* NT_SIGNALSEM */
    m68k_write32(&f.ctx, f.semaphore + 16, f.semaphore + 20);
    m68k_write32(&f.ctx, f.semaphore + 24, f.semaphore + 16);
    m68k_write16(&f.ctx, f.semaphore + 44, 0xFFFF); /* ss_QueueCount = -1 */
    assert(f.call(720, f.semaphore) == 0);
    assert(f.call(720, f.semaphore) == 0);
}

static void test_invalid_guest_arguments_are_not_dereferenced()
{
    Fixture f;
    const ULONG arguments[] =
    {
        0, 1, Fixture::semaphore + 1,
        static_cast<ULONG>(f.memory.size() - 45),
        static_cast<ULONG>(f.memory.size() - 1),
        static_cast<ULONG>(f.memory.size()), 0xFFFFFFFEU, 0xFFFFFFFFU
    };
    for (ULONG argument : arguments)
    {
        assert(f.call(702, argument) == 0);
        assert(f.call(720, argument) == 0);
    }
}

static void test_native_shadows_are_not_acquired_or_dereferenced()
{
    Fixture f;
    f.ctx.num_shadows = 1;
    f.ctx.shadow_map[0].m68k_addr = f.semaphore;
    f.ctx.shadow_map[0].native_ptr = f.native_memory.data();
    assert(f.call(702, f.semaphore) == 0);
    assert(f.call(720, f.semaphore) == 0);

    /* A stale or invalid native mapping must not cause an access either. */
    f.ctx.shadow_map[0].native_ptr = reinterpret_cast<void *>(uintptr_t(1));
    assert(f.call(702, f.semaphore) == 0);
    assert(f.call(720, f.semaphore) == 0);
    f.ctx.shadow_map[0].native_ptr = nullptr;
    assert(f.call(720, f.semaphore) == 0);
}

static void test_avl_findnode_lvo()
{
    Fixture f;
    /* AVL_AddNode (-852), AVL_RemNodeByAddress (-858) and AVL_FindNode (-870)
       are overridden by the manual table, while -864 (AVL_RemNodeByKey) is no
       longer shadowed by AVL_FindNode. */
    assert(f.call(852, 0x300) == 0);
    assert(f.call(858, 0x300) == 0);
    assert(f.call(870, 0x300) == 0);
    assert(f.call(864, 0x300, 4) == 0xCAFE0864U);
}

static void test_generated_fallback_controls()
{
    Fixture f;
    f.ctx.libs[0].thunks = nullptr;
    f.ctx.libs[0].num_thunks = 0;
    /* Prove the poison fallbacks are reachable when the overrides are absent. */
    assert(f.call(702, 0x00CAFE00, 0) == 0xBAD00702U);
    assert(f.call(720, f.semaphore, 1) == 0xBAD00720U);
    assert(f.call(870, 0x300, 5) == 0xBAD00870U);
}

int main()
{
    struct Test { const char *name; void (*run)(); };
    const Test tests[] =
    {
        { "DeletePool, AttemptSemaphoreShared and AVL stubs use their ABI slots", test_registration },
        { "DeletePool keeps the containment handle away from native Exec", test_delete_pool_containment_handle },
        { "shared semaphore attempt rejects invalid guest state", test_invalid_shared_attempt_fails_safely },
        { "invalid guest arguments are not dereferenced", test_invalid_guest_arguments_are_not_dereferenced },
        { "native shadows are not acquired or dereferenced", test_native_shadows_are_not_acquired_or_dereferenced },
        { "AVL_FindNode is at -870 and no longer shadows -864", test_avl_findnode_lvo },
        { "generated fallback controls are reachable", test_generated_fallback_controls }
    };
    for (const Test &test : tests)
    {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("Exec compatibility slots: %zu groups passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}

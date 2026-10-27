/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Dispatch the production Exec list thunks against byte-addressed guest
    structures. kaffeine_lists.c is compiled separately as C by the Makefile.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "kaffeine_intern.h"
#include "kaffeine_lists.h"
#include "kaffeine_dispatch.h"

#include "../../../../rom/kaffeine/kaffeine_cpu.c"
#include "../../../../rom/kaffeine/kaffeine_dispatch.c"

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0x0005;
    ULONG pc = 0x400;
};

static ULONG get_d(void *state, int reg) { return static_cast<TestCPU *>(state)->d[reg]; }
static ULONG get_a(void *state, int reg) { return static_cast<TestCPU *>(state)->a[reg]; }
static void set_d(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->d[reg] = value; }
static void set_a(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->a[reg] = value; }
static UWORD get_sr(void *state) { return static_cast<TestCPU *>(state)->sr; }
static void set_sr(void *state, UWORD value) { static_cast<TestCPU *>(state)->sr = value; }
static ULONG get_pc(void *state) { return static_cast<TestCPU *>(state)->pc; }
static void set_pc(void *state, ULONG value) { static_cast<TestCPU *>(state)->pc = value; }

static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr
};

static unsigned generated_calls;
static unsigned native_calls;
static void *expected_native_list;
static KaffeineContext *expected_native_ctx;

/* The old native-shadow search is a collaborator, never used for guest lists. */
extern "C" IPTR Kaffeine_FindNativeListName(KaffeineContext *ctx,
                                         void *native_list, const char *name)
{
    assert(ctx == expected_native_ctx);
    assert(native_list == expected_native_list);
    assert(std::strcmp(name, "exec.library") == 0);
    native_calls++;
    return 0xB00;
}

static IPTR generated_poison(KaffeineContext *, void *)
{
    generated_calls++;
    return 0xBADCA11;
}

static const M68KThunkEntry manual_table[] = { KAFFEINE_EXEC_LIST_THUNKS };
static const M68KThunkEntry generated_table[] =
{
    { 234, generated_poison }, { 240, generated_poison },
    { 246, generated_poison }, { 252, generated_poison },
    { 258, generated_poison }, { 264, generated_poison },
    { 270, generated_poison }, { 276, generated_poison },
    { 828, generated_poison }
};

struct Fixture
{
    /* Word-aligned addresses deliberately not aligned to a host pointer. */
    static constexpr ULONG list = 0x102;
    static constexpr ULONG n1 = 0x202, n2 = 0x222, n3 = 0x242;
    static constexpr ULONG n4 = 0x262, n5 = 0x282;
    static constexpr ULONG stack = 0xE00;
    static constexpr ULONG libbase = 0x10000;
    KaffeineContext ctx = {};
    std::array<UBYTE, 4096> memory;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        memory.fill(0xA5);
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
        generated_calls = native_calls = 0;
        expected_native_list = nullptr;
        expected_native_ctx = nullptr;
    }

    ULONG call(ULONG lvo, ULONG a0 = 0, ULONG a1 = 0, ULONG a2 = 0)
    {
        for (unsigned i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[2] = a2;
        state.a[7] = stack;
        const TestCPU before = state;
        KaffeineDispatchResult result = {};

        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, libbase - lvo, &result));
        assert(result.return_pc == 0x12345678);
        assert(!result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; i++)
            assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; i++)
            assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);
        return state.d[0];
    }

    void init_list() { assert(call(828, list) == 0); }

    void init_node(ULONG node, int priority = 0, ULONG name = 0)
    {
        m68k_write32(&ctx, node, 0xDEADBEEF);
        m68k_write32(&ctx, node + 4, 0xBAADF00D);
        memory[node + 8] = 0x42;
        memory[node + 9] = static_cast<UBYTE>(priority);
        m68k_write32(&ctx, node + 10, name);
    }

    ULONG string(ULONG addr, const char *text)
    {
        assert(addr + std::strlen(text) + 1 <= memory.size());
        std::memcpy(memory.data() + addr, text, std::strlen(text) + 1);
        return addr;
    }

    void expect(std::initializer_list<ULONG> nodes)
    {
        ULONG previous = list, current = m68k_read32(&ctx, list);
        for (ULONG node : nodes)
        {
            assert(current == node);
            assert(m68k_read32(&ctx, node + 4) == previous);
            previous = node;
            current = m68k_read32(&ctx, node);
        }
        assert(current == list + 4);
        assert(m68k_read32(&ctx, list + 4) == 0);
        assert(m68k_read32(&ctx, list + 8) == previous);
        /* Neither List type/padding nor adjacent bytes may be overwritten. */
        assert(memory[list - 1] == 0xA5);
        for (ULONG i = 12; i < 20; i++)
            assert(memory[list + i] == 0xA5);
    }
};

static void test_empty_and_layout()
{
    Fixture f;
    f.init_list();
    f.expect({});
    const UBYTE header[] = { 0, 0, 1, 6, 0, 0, 0, 0, 0, 0, 1, 2 };
    assert(std::memcmp(f.memory.data() + f.list, header, sizeof(header)) == 0);
    const auto before = f.memory;
    assert(f.call(258, f.list) == 0);
    assert(f.call(264, f.list) == 0);
    assert(f.memory == before);
    assert(f.call(276, f.list, f.string(0x600, "missing")) == 0);
    assert(native_calls == 0);
}

static void test_add_remove_and_guest_identity()
{
    Fixture f;
    f.init_list();
    f.init_node(f.n1, -3, f.string(0x600, "one"));
    f.init_node(f.n2);
    f.init_node(f.n3);
    std::array<UBYTE, 8> payload;
    std::memcpy(payload.data(), f.memory.data() + f.n1 + 8, payload.size());

    assert(f.call(246, f.list, f.n1) == 0);
    f.expect({ f.n1 });
    const UBYTE head[] = { 0, 0, 2, 2 };
    assert(std::memcmp(f.memory.data() + f.list, head, sizeof(head)) == 0);
    assert(f.call(246, f.list, f.n2) == 0);
    assert(f.call(240, f.list, f.n3) == 0);
    f.expect({ f.n3, f.n1, f.n2 });
    assert(f.call(258, f.list) == f.n3);
    f.expect({ f.n1, f.n2 });
    assert(f.call(264, f.list) == f.n2);
    f.expect({ f.n1 });

    const ULONG old_succ = m68k_read32(&f.ctx, f.n1);
    const ULONG old_pred = m68k_read32(&f.ctx, f.n1 + 4);
    assert(f.call(252, 0, f.n1) == 0); /* Remove takes A1, not A0. */
    f.expect({});
    assert(m68k_read32(&f.ctx, f.n1) == old_succ);
    assert(m68k_read32(&f.ctx, f.n1 + 4) == old_pred);
    assert(std::memcmp(payload.data(), f.memory.data() + f.n1 + 8, payload.size()) == 0);

    f.call(240, f.list, f.n1);
    assert(f.call(264, f.list) == f.n1);
    f.expect({});
    f.call(246, f.list, f.n1);
    assert(f.call(258, f.list) == f.n1);
    f.expect({});
    assert(native_calls == 0);
}

static void test_insert_variants_and_min_nodes()
{
    Fixture f;
    f.init_list();
    for (ULONG n : { f.n1, f.n2, f.n3, f.n4 })
        f.init_node(n);
    f.call(234, f.list, f.n1, 0);
    f.call(234, f.list, f.n2, f.n1);
    f.call(234, f.list, f.n3, 0);
    f.expect({ f.n3, f.n1, f.n2 });
    f.call(234, f.list, f.n4, f.list + 4); /* Tail sentinel. */
    f.expect({ f.n3, f.n1, f.n2, f.n4 });
    f.call(252, 0, f.n1);
    f.expect({ f.n3, f.n2, f.n4 });
    f.call(234, 0, f.n1, f.n2); /* No list header needed with a normal predecessor. */
    f.expect({ f.n3, f.n2, f.n1, f.n4 });

    Fixture minimal;
    minimal.init_list();
    const ULONG node = minimal.memory.size() - 8;
    minimal.call(240, minimal.list, node);
    minimal.expect({ node });
    assert(minimal.call(264, minimal.list) == node);
    minimal.expect({});
}

static void test_signed_priority_and_fifo()
{
    Fixture f;
    f.init_list();
    f.init_node(f.n1, 0);
    f.init_node(f.n2, 127);
    f.init_node(f.n3, -128);
    f.init_node(f.n4, 127);
    f.init_node(f.n5, -1);
    for (ULONG n : { f.n1, f.n2, f.n3, f.n4, f.n5 })
        f.call(270, f.list, n);
    f.expect({ f.n2, f.n4, f.n1, f.n5, f.n3 });
    assert(f.memory[f.n3 + 9] == 0x80);
    assert(f.memory[f.n5 + 9] == 0xFF);
    assert(native_calls == 0);
}

static void test_findname()
{
    Fixture f;
    f.init_list();
    f.init_node(f.n1); /* A node without a name must be skipped. */
    f.init_node(f.n2, 0, f.string(0x600, "Alpha"));
    f.init_node(f.n3, 0, f.string(0x620, "alpha"));
    f.init_node(f.n4, 0, f.string(0x640, "alpha"));
    for (ULONG n : { f.n1, f.n2, f.n3, f.n4 })
        f.call(246, f.list, n);
    assert(f.call(276, f.list, f.string(0x700, "Alpha")) == f.n2);
    assert(f.call(276, f.list, f.string(0x700, "alpha")) == f.n3);
    assert(f.call(276, f.list, f.string(0x700, "ALPHA")) == 0);
    assert(f.call(276, f.n3, f.string(0x700, "alpha")) == f.n4);
    assert(f.call(276, f.list, 0) == 0);
    assert(f.call(276, 0, f.string(0x700, "alpha")) == 0);
    f.expect({ f.n1, f.n2, f.n3, f.n4 });
    assert(native_calls == 0);
}

static void test_explicit_native_shadow_lookup()
{
    Fixture f;
    int native_list = 42;
    f.init_list();
    f.init_node(f.n1);
    f.ctx.num_shadows = 1;
    f.ctx.shadow_map[0] = { f.list, &native_list, 17 };
    expected_native_list = &native_list;
    expected_native_ctx = &f.ctx;
    assert(f.call(276, f.list, f.string(0x600, "exec.library")) == 0xB00);
    assert(native_calls == 1);
    assert(native_list == 42);

    /* Guest list mutation must not modify a registered native list shadow. */
    const auto before = f.memory;
    f.call(828, f.list);
    f.call(240, f.list, f.n1);
    f.call(246, f.list, f.n1);
    f.call(270, f.list, f.n1);
    assert(f.memory == before);
    assert(native_calls == 1);
}

static void test_bad_ranges_and_links()
{
    Fixture f;
    f.init_list();
    f.init_node(f.n1);
    auto before = f.memory;
    for (ULONG bad : { 0u, 0xFFFFFFFCu, static_cast<ULONG>(f.memory.size() - 8) })
    {
        f.call(828, bad);
        f.call(240, bad, f.n1);
        f.call(246, bad, f.n1);
        assert(f.call(258, bad) == 0);
        assert(f.call(264, bad) == 0);
    }
    f.call(246, f.list, 0xFFFFFFFC);
    f.call(252, 0, 0xFFFFFFFC);
    assert(f.memory == before);

    m68k_write32(&f.ctx, f.list, 0xFFFFFFFC);
    before = f.memory;
    f.call(240, f.list, f.n1);
    assert(f.call(258, f.list) == 0);
    f.call(270, f.list, f.n1);
    assert(f.memory == before);

    f.init_list();
    f.call(246, f.list, f.n1);
    m68k_write32(&f.ctx, f.n1 + 4, 0xFFFFFFFC);
    before = f.memory;
    f.call(252, 0, f.n1);
    assert(f.memory == before);

    const ULONG unterminated = f.memory.size() - 2;
    f.memory[unterminated] = 'x';
    f.memory[unterminated + 1] = 'y';
    m68k_write32(&f.ctx, f.n1 + 10, unterminated);
    assert(f.call(276, f.list, f.string(0x600, "xy")) == 0);
    assert(f.call(276, f.list, unterminated) == 0);
    assert(native_calls == 0);
}

static void test_cyclic_lists_terminate_without_mutation()
{
    Fixture f;
    f.init_list();
    f.init_node(f.n1, 127, f.string(0x600, "one"));
    f.init_node(f.n2, 127, f.string(0x620, "two"));
    f.init_node(f.n3, -128);
    f.call(246, f.list, f.n1);
    f.call(246, f.list, f.n2);
    m68k_write32(&f.ctx, f.n2, f.n1);
    const ULONG missing = f.string(0x640, "missing");
    const auto before = f.memory;
    assert(f.call(276, f.list, missing) == 0);
    f.call(270, f.list, f.n3);
    assert(f.memory == before);
    assert(native_calls == 0);
}

int main()
{
    test_empty_and_layout();
    std::puts("[PASS] Exec lists: empty sentinels and exact big-endian layout");
    test_add_remove_and_guest_identity();
    std::puts("[PASS] Exec lists: head/tail operations, guest identity and preserved payload");
    test_insert_variants_and_min_nodes();
    std::puts("[PASS] Exec lists: Insert variants, middle removal and eight-byte MinNodes");
    test_signed_priority_and_fifo();
    std::puts("[PASS] Exec lists: signed priorities and FIFO ties");
    test_findname();
    std::puts("[PASS] Exec lists: case-sensitive FindName, NULL names and partial search");
    test_explicit_native_shadow_lookup();
    std::puts("[PASS] Exec lists: explicit native-shadow lookup and mutation boundary");
    test_bad_ranges_and_links();
    std::puts("[PASS] Exec lists: invalid ranges, links and unterminated names");
    test_cyclic_lists_terminate_without_mutation();
    std::puts("[PASS] Exec lists: bounded traversal of cyclic lists");
    return 0;
}

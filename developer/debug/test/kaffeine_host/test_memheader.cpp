/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Exercise the production guest MemHeader allocator through Exec LVOs.
    The allocator, CPU interface and dispatcher are compiled separately as C.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "kaffeine_intern.h"
#include "kaffeine_memheader.h"
#include "kaffeine_dispatch.h"

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0x0005;
    ULONG pc = 0x400;
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

static unsigned generated_calls;
static IPTR generated_poison(KaffeineContext *, void *)
{
    generated_calls++;
    return 0xBADCA11;
}

static const M68KThunkEntry manual_table[] = { KAFFEINE_EXEC_MEMHEADER_THUNKS };
/* Literal ABI numbers are independent of the production registration macro. */
static const M68KThunkEntry generated_table[] =
{
    { 186, generated_poison }, { 192, generated_poison }
};

struct Span { ULONG begin, end; };
using Spans = std::vector<Span>;

/* An interval oracle: no guest links, headers or allocator implementation. */
static ULONG allocate_interval(Spans &free, ULONG bytes)
{
    const uint64_t rounded = (uint64_t(bytes) + 7) & ~uint64_t(7);
    if (!bytes || rounded > UINT32_MAX)
        return 0;
    for (auto it = free.begin(); it != free.end(); ++it)
        if (it->end - it->begin >= rounded)
        {
            const ULONG result = it->begin;
            it->begin += rounded;
            if (it->begin == it->end)
                free.erase(it);
            return result;
        }
    return 0;
}

static void release_interval(Spans &free, ULONG address, ULONG bytes)
{
    const ULONG begin = address & ~ULONG(7);
    const uint64_t end = (uint64_t(address) + bytes + 7) & ~uint64_t(7);
    assert(bytes && address && end <= UINT32_MAX);
    free.push_back({ begin, static_cast<ULONG>(end) });
    std::sort(free.begin(), free.end(), [](Span a, Span b) { return a.begin < b.begin; });
    Spans merged;
    for (Span span : free)
    {
        if (!merged.empty() && merged.back().end == span.begin)
            merged.back().end = span.end;
        else
        {
            assert(merged.empty() || merged.back().end < span.begin);
            merged.push_back(span);
        }
    }
    free = merged;
}

struct Fixture
{
    static constexpr ULONG stack = 0x3C00, libbase = 0x10000;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x4000> memory;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };
    ULONG header, pool, pool_size;

    explicit Fixture(ULONG bytes = 256, ULONG h = 0x100, ULONG p = 0x120,
                     bool bounds = true) : header(h), pool(p), pool_size(bytes)
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
        /* Node and Attributes remain poisoned: libnix does not initialize them. */
        put(header + 20, bounds ? pool : 0);
        put(header + 24, bounds ? pool + pool_size : 0);
        set_free({ { pool, pool + pool_size } });
        generated_calls = 0;
    }

    void put(ULONG address, ULONG value) { m68k_write32(&ctx, address, value); }
    ULONG get(ULONG address) { return m68k_read32(&ctx, address); }

    void set_free(const Spans &free)
    {
        put(header + 16, free.empty() ? 0 : free.front().begin);
        ULONG total = 0;
        for (size_t i = 0; i < free.size(); i++)
        {
            put(free[i].begin, i + 1 < free.size() ? free[i + 1].begin : 0);
            put(free[i].begin + 4, free[i].end - free[i].begin);
            total += free[i].end - free[i].begin;
        }
        put(header + 28, total);
    }

    ULONG call(ULONG lvo, ULONG bytes, ULONG address = 0, ULONG h = UINT32_MAX)
    {
        for (unsigned i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0000000 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.d[0] = bytes;
        state.a[0] = h == UINT32_MAX ? header : h;
        state.a[1] = address;
        state.a[7] = stack;
        const TestCPU before = state;
        const auto before_memory = memory;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, libbase - lvo, &result));
        assert(result.return_pc == 0x12345678 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; i++)
            assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; i++)
            assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == 0);

        /* No writes outside the pool, mh_First and mh_Free are permitted. */
        for (ULONG i = 0; i < memory.size(); i++)
            if (!((i >= pool && i < pool + pool_size) ||
                  (i >= header + 16 && i < header + 20) ||
                  (i >= header + 28 && i < header + 32)))
                assert(memory[i] == before_memory[i]);
        return state.d[0];
    }

    ULONG alloc(ULONG bytes) { return call(186, bytes, 0xABCDEF00); }
    void release(ULONG address, ULONG bytes) { assert(call(192, bytes, address) == 0); }

    void expect(const Spans &free)
    {
        assert(get(header + 16) == (free.empty() ? 0 : free.front().begin));
        ULONG total = 0;
        for (size_t i = 0; i < free.size(); i++)
        {
            assert(get(free[i].begin) == (i + 1 < free.size() ? free[i + 1].begin : 0));
            assert(get(free[i].begin + 4) == free[i].end - free[i].begin);
            total += free[i].end - free[i].begin;
        }
        assert(get(header + 28) == total);
    }

    void reject(ULONG lvo, ULONG bytes, ULONG address = 0, ULONG h = UINT32_MAX)
    {
        const auto before = memory;
        assert(call(lvo, bytes, address, h) == 0);
        assert(memory == before);
    }
};

static void test_layout_rounding_and_exhaustion()
{
    Fixture f(128);
    const UBYTE header_fields[] =
    {
        0, 0, 1, 0x20, 0, 0, 1, 0x20,
        0, 0, 1, 0xA0, 0, 0, 0, 0x80
    };
    const UBYTE chunk[] = { 0, 0, 0, 0, 0, 0, 0, 0x80 };
    assert(std::memcmp(f.memory.data() + f.header + 16,
                       header_fields, sizeof(header_fields)) == 0);
    assert(std::memcmp(f.memory.data() + f.pool, chunk, sizeof(chunk)) == 0);
    Spans expected = { { f.pool, f.pool + 128 } };
    for (ULONG bytes : { 1U, 7U, 8U, 9U, 88U })
    {
        const ULONG result = allocate_interval(expected, bytes);
        assert(result && f.alloc(bytes) == result);
        assert((result & 7) == 0);
        f.expect(expected);
    }
    assert(expected.empty());
    f.reject(186, 1);
}

static void test_first_fit_and_fragmentation()
{
    Fixture f;
    Spans expected =
    {
        { f.pool, f.pool + 16 }, { f.pool + 48, f.pool + 80 },
        { f.pool + 144, f.pool + 208 }
    };
    f.set_free(expected);
    assert(f.alloc(24) == allocate_interval(expected, 24));
    f.expect(expected);
    assert(f.alloc(16) == allocate_interval(expected, 16));
    f.expect(expected);
    assert(f.alloc(60) == allocate_interval(expected, 60));
    f.expect(expected);

    expected = { { f.pool, f.pool + 16 }, { f.pool + 32, f.pool + 48 },
                 { f.pool + 64, f.pool + 80 } };
    f.set_free(expected);
    assert(f.get(f.header + 28) == 48);
    f.reject(186, 24); /* Total free bytes suffice, but no interval does. */
    f.expect(expected);
}

static void test_free_order_and_coalescing()
{
    std::array<unsigned, 3> order = { 0, 1, 2 };
    do
    {
        Fixture f(96);
        Spans expected = { { f.pool, f.pool + 96 } };
        std::array<ULONG, 3> blocks;
        for (ULONG &block : blocks)
        {
            block = f.alloc(32);
            assert(block == allocate_interval(expected, 32));
        }
        f.expect(expected);
        for (unsigned n : order)
        {
            f.release(blocks[n], 32);
            release_interval(expected, blocks[n], 32);
            f.expect(expected);
        }
        assert(expected.size() == 1 && expected[0].begin == f.pool &&
               expected[0].end == f.pool + 96);
    } while (std::next_permutation(order.begin(), order.end()));
}

static void test_deallocate_rounding()
{
    Fixture f;
    ULONG block = f.alloc(13);
    assert(block == f.pool && f.get(f.header + 28) == f.pool_size - 16);
    f.release(block, 13); /* libnix passes the original, unrounded size. */
    f.expect({ { f.pool, f.pool + f.pool_size } });

    block = f.alloc(24);
    assert(block == f.pool);
    f.release(block + 3, 17); /* Include both unaligned endpoints: 24 bytes. */
    f.expect({ { f.pool, f.pool + f.pool_size } });
}

static void test_header_and_optional_bounds()
{
    Fixture unaligned(128, 0x102, 0x180);
    assert(unaligned.alloc(8) == unaligned.pool);
    unaligned.release(unaligned.pool, 8);
    unaligned.expect({ { unaligned.pool, unaligned.pool + 128 } });

    Fixture unbounded(128, 0x100, 0x120, false);
    assert(unbounded.alloc(128) == unbounded.pool);
    unbounded.expect({});
    unbounded.release(unbounded.pool, 128);
    unbounded.expect({ { unbounded.pool, unbounded.pool + 128 } });

    Fixture empty;
    empty.set_free({});
    empty.release(empty.pool + 32, 24);
    empty.expect({ { empty.pool + 32, empty.pool + 56 } });
}

static void test_noops_and_overflow()
{
    Fixture f;
    f.reject(186, 0);
    f.reject(192, 0, f.pool);
    f.reject(192, 8, 0);
    for (ULONG bytes : { 0xFFFFFFF8U, 0xFFFFFFF9U, 0xFFFFFFFFU })
    {
        f.reject(186, bytes);
        f.reject(192, bytes, f.pool);
    }
    f.reject(192, 16, 0xFFFFFFFCU);
    for (ULONG h : { 0U, 0xFFFFFFF0U, ULONG(f.memory.size() - 31) })
    {
        f.reject(186, 8, 0, h);
        f.reject(192, 8, f.pool, h);
    }
    f.reject(192, 8, f.pool - 8);
    f.reject(192, 8, f.pool + f.pool_size);
    f.reject(192, 2, f.pool + f.pool_size - 1);
}

static void test_corrupt_chains_are_atomic()
{
    using Corrupt = std::function<void(Fixture &)>;
    const std::vector<Corrupt> corruptions =
    {
        [](Fixture &f) { f.put(f.header + 16, 0); },
        [](Fixture &f) { f.put(f.header + 28, 0); },
        [](Fixture &f) { f.put(f.header + 28, 8); },
        [](Fixture &f) { f.put(f.header + 28, 0xFFFFFFF8U); },
        [](Fixture &f) { f.put(f.header + 16, f.pool + 4); },
        [](Fixture &f) { f.put(f.header + 16, f.memory.size() - 4); },
        [](Fixture &f) { f.put(f.pool + 4, 0); },
        [](Fixture &f) { f.put(f.pool + 4, 7); },
        [](Fixture &f) { f.put(f.pool + 4, 12); },
        [](Fixture &f) { f.put(f.pool + 4, 0xFFFFFFF8U); },
        [](Fixture &f) { f.put(f.pool, f.pool); },
        [](Fixture &f) { f.put(f.pool, 0xFFFFFFF8U); },
        [](Fixture &f) { f.put(f.header + 20, f.pool + 8); },
        [](Fixture &f) { f.put(f.header + 24, f.pool + 31); },
        [](Fixture &f) { f.put(f.header + 24, f.memory.size() + 8); },
        [](Fixture &f) { f.put(f.header + 20, f.pool + 128);
                        f.put(f.header + 24, f.pool + 64); },
        /* A usable first chunk must not be consumed before checking the tail. */
        [](Fixture &f) { f.set_free({ { f.pool, f.pool + 32 },
                                     { f.pool + 64, f.pool + 96 } });
                        f.put(f.pool + 64 + 4, 7); },
        [](Fixture &f) { f.set_free({ { f.pool, f.pool + 32 },
                                     { f.pool + 64, f.pool + 96 } });
                        f.put(f.pool + 64, f.pool); },
        [](Fixture &f) { f.set_free({ { f.pool, f.pool + 80 },
                                     { f.pool + 64, f.pool + 96 } }); },
        [](Fixture &f) { f.put(f.header + 20, 0); f.put(f.header + 24, 0);
                        f.put(f.header + 16, f.header);
                        f.put(f.header, 0); f.put(f.header + 4, 32); }
    };

    for (const auto &corrupt : corruptions)
    {
        Fixture f;
        f.set_free({ { f.pool, f.pool + 32 } });
        corrupt(f);
        f.reject(186, 8);
        /* The freed range is normally valid and is separate from the chunks. */
        f.reject(192, 8, f.pool + 192);
    }
}

static void test_shadow_and_overlap_rejection()
{
    Fixture capacity;
    capacity.ctx.memory_info = capacity.header;
    capacity.reject(186, 8);
    capacity.reject(192, 8, capacity.pool + 192);
    Fixture protected_chunk;
    protected_chunk.ctx.memory_info = protected_chunk.pool;
    protected_chunk.reject(186, 8);
    protected_chunk.set_free({});
    protected_chunk.reject(192, 8, protected_chunk.pool);

    unsigned native_object = 0x12345678;
    for (unsigned kind = 0; kind < 3; kind++)
    {
        Fixture f;
        f.ctx.num_shadows = 1;
        const ULONG shadow = kind == 0 ? f.header : kind == 1 ? f.pool : f.pool + 8;
        f.ctx.shadow_map[0] = { shadow, &native_object, 1 };
        f.reject(186, 8); /* Third case is the new split-chunk address. */
        if (kind != 2)
            f.reject(192, 8, f.pool + 192);
        assert(native_object == 0x12345678);
    }

    Fixture f;
    f.set_free({ { f.pool, f.pool + 32 }, { f.pool + 96, f.pool + 128 } });
    f.reject(192, 8, f.pool);              /* Double free. */
    f.reject(192, 16, f.pool + 24);       /* Overlap left free interval. */
    f.reject(192, 24, f.pool + 80);       /* Overlap right free interval. */
    f.reject(192, 120, f.pool + 8);       /* Cover both free intervals. */

    Fixture shadow_free;
    shadow_free.set_free({});
    shadow_free.ctx.num_shadows = 1;
    shadow_free.ctx.shadow_map[0] = { shadow_free.pool, &native_object, 1 };
    shadow_free.reject(192, 8, shadow_free.pool);
    assert(native_object == 0x12345678);
}

static void test_deterministic_interval_sequences()
{
    Fixture f(512);
    Spans free = { { f.pool, f.pool + 512 } };
    struct Allocation { ULONG address = 0, size = 0; UBYTE pattern = 0; };
    std::array<Allocation, 16> live;
    uint32_t random = 0x19680816;
    unsigned allocated = 0, released = 0, exhausted = 0;
    auto check_live = [&]()
    {
        for (const auto &a : live)
            if (a.address)
                for (ULONG i = 0; i < ((a.size + 7) & ~ULONG(7)); i++)
                    assert(f.memory[a.address + i] == a.pattern);
    };
    for (unsigned step = 0; step < 400; step++)
    {
        random = random * 1664525U + 1013904223U;
        auto &a = live[(random >> 24) % live.size()];
        check_live();
        if (a.address)
        {
            f.release(a.address, a.size);
            release_interval(free, a.address, a.size);
            a = {};
            released++;
        }
        else
        {
            const ULONG bytes = 1 + ((random >> 8) % 97);
            const auto before = f.memory;
            const ULONG expected = allocate_interval(free, bytes);
            const ULONG address = f.alloc(bytes);
            assert(address == expected);
            if (address)
            {
                a = { address, bytes, static_cast<UBYTE>(1 + step % 254) };
                std::memset(f.memory.data() + address, a.pattern, (bytes + 7) & ~ULONG(7));
                allocated++;
            }
            else
            {
                assert(f.memory == before);
                exhausted++;
            }
        }
        f.expect(free);
        check_live();
    }
    for (auto &a : live)
        if (a.address)
        {
            f.release(a.address, a.size);
            release_interval(free, a.address, a.size);
            a = {};
            f.expect(free);
            check_live();
        }
    assert(allocated > 50 && released > 50 && exhausted > 0);
    f.expect({ { f.pool, f.pool + 512 } });
}

int main()
{
    const struct { const char *name; void (*run)(); } tests[] =
    {
        { "BE layout, guest identity, rounding, split and exact fit", test_layout_rounding_and_exhaustion },
        { "first fit and fragmentation", test_first_fit_and_fragmentation },
        { "all free orders and left/right/both-side coalescing", test_free_order_and_coalescing },
        { "original-size and outward Deallocate rounding", test_deallocate_rounding },
        { "word-aligned header, optional bounds and empty list", test_header_and_optional_bounds },
        { "no-op, null, range and overflow handling", test_noops_and_overflow },
        { "corrupt chains rejected before writes", test_corrupt_chains_are_atomic },
        { "native shadow and overlapping free ranges rejected", test_shadow_and_overlap_rejection },
        { "400 interval operations preserve live payloads", test_deterministic_interval_sequences }
    };
    for (const auto &test : tests)
    {
        std::printf("MemHeader: %s ... ", test.name);
        std::fflush(stdout);
        test.run();
        std::puts("PASS");
    }
    std::puts("9 MemHeader host checks passed");
    return 0;
}

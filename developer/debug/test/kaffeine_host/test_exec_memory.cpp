/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Production heap and memory thunks, compiled as C, through literal Exec
    LVOs from NDK 3.2 exec_lib.fd with poisoned generated fallbacks.
*/
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_thunks_memory.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_dispatch.h"

static std::map<APTR, ULONG> native_blocks;
static bool fail_native_allocation;
static int native_allocations_before_failure = -1;
static std::vector<std::pair<APTR, ULONG>> native_frees;

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    if (fail_native_allocation || native_allocations_before_failure == 0)
    {
        fail_native_allocation = false;
        native_allocations_before_failure = -1;
        return nullptr;
    }
    if (native_allocations_before_failure > 0) --native_allocations_before_failure;
    APTR block = std::malloc(size);
    if (block)
    {
        std::memset(block, (flags & MEMF_CLEAR) ? 0 : 0xCD, size);
        assert(native_blocks.emplace(block, size).second);
    }
    return block;
}

extern "C" void FreeMem(APTR block, ULONG size)
{
    const auto it = native_blocks.find(block);
    assert(it != native_blocks.end() && it->second == size);
    native_frees.push_back({ block, size });
    native_blocks.erase(it);
    std::free(block);
}

/* Collaborators of the library/HUNK functions, not used by these tests. */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
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

static unsigned generated_calls;
static IPTR generated_poison(KaffeineContext *, void *)
{
    generated_calls++;
    return 0xBADCA11;
}

static const M68KThunkEntry manual_table[] =
{
    KAFFEINE_EXEC_MEMORY_THUNKS
    { 702, Kaffeine_DeletePool }
};
static const M68KThunkEntry generated_table[] =
{
    { 198, generated_poison }, { 210, generated_poison }, { 216, generated_poison },
    { 684, generated_poison }, { 690, generated_poison }, { 696, generated_poison },
    { 702, generated_poison }, { 708, generated_poison }, { 714, generated_poison },
    { 624, generated_poison }, { 630, generated_poison },
    { 204, generated_poison }, { 222, generated_poison }, { 228, generated_poison }
};

struct Fixture
{
    static constexpr ULONG stack = 0x200, libbase = 0x10000;
    static constexpr ULONG return_pc = 0x76543210;
    KaffeineContext *ctx;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };
    size_t native_before = native_blocks.size();

    explicit Fixture(ULONG heap_bytes = 0x4000)
    {
        ctx = Kaffeine_CreateContext(nullptr);
        assert(ctx);
        /* A small, poisoned arena makes reuse and exact exhaustion explicit. */
        ctx->heap_end = ctx->heap_start + heap_bytes;
        std::memset(ctx->mem + ctx->heap_start - 16, 0xA5, heap_bytes + 32);
        assert(Kaffeine_HeapInit(ctx));
        ctx->num_libs = 1;
        auto &lib = ctx->libs[0];
        lib.m68k_addr = libbase;
        lib.jt_start = libbase - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual_table;
        lib.num_thunks = std::size(manual_table);
        lib.gen_thunks = generated_table;
        lib.num_gen_thunks = std::size(generated_table);
        m68k_write32(ctx, stack, return_pc);
    }

    ~Fixture()
    {
        Kaffeine_DestroyContext(ctx);
        assert(native_blocks.size() == native_before);
    }

    std::vector<UBYTE> snapshot() const
    {
        return { ctx->mem, ctx->mem + ctx->heap_end + 16 };
    }

    ULONG call(ULONG lvo, ULONG d0 = 0, ULONG d1 = 0, ULONG d2 = 0,
               ULONG a0 = 0, ULONG a1 = 0, bool poison = false)
    {
        for (unsigned i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0000000U + i;
            state.a[i] = 0xA0000000U + i;
        }
        state.d[0] = d0;
        state.d[1] = d1;
        state.d[2] = d2;
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[7] = stack;
        const TestCPU before = state;
        std::array<UBYTE, KAFFEINE_HEAP_BASE> low;
        std::memcpy(low.data(), ctx->mem, low.size());
        const ULONG high = m68k_read32(ctx, ctx->heap_end);
        generated_calls = 0;

        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(ctx, &cpu, libbase - lvo, &result));
        assert(result.return_pc == return_pc && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; i++)
            assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; i++)
            assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(generated_calls == (poison ? 1U : 0U));
        assert(std::memcmp(low.data(), ctx->mem, low.size()) == 0);
        assert(m68k_read32(ctx, ctx->heap_end) == high);
        return state.d[0];
    }

    ULONG alloc(ULONG bytes, ULONG flags = 0, bool vec = false)
    {
        return call(vec ? 684 : 198, bytes, flags);
    }
    void release(ULONG addr, ULONG bytes = 0, bool vec = false)
    {
        assert(call(vec ? 690 : 210, bytes, 0, 0, 0, addr) == 0);
    }
    ULONG avail(ULONG flags = 0) { return call(216, 0xD0D0D0D0, flags); }
    ULONG pool(ULONG flags = 0, ULONG puddle = 4096, ULONG threshold = 1024)
    {
        return call(696, flags, puddle, threshold);
    }
    ULONG pooled(ULONG pool, ULONG bytes) { return call(708, bytes, 0, 0, pool); }
    void free_pooled(ULONG pool, ULONG addr, ULONG bytes)
    {
        assert(call(714, bytes, 0, 0, pool, addr) == 0);
    }
    void delete_pool(ULONG pool) { assert(call(702, 0, 0, 0, pool) == 0); }
    void fill(ULONG addr, ULONG bytes, UBYTE value)
    {
        assert(addr && addr <= ctx->heap_end && bytes <= ctx->heap_end - addr);
        std::memset(ctx->mem + addr, value, bytes);
    }
    void expect_bytes(ULONG addr, ULONG bytes, UBYTE value)
    {
        assert(addr && addr <= ctx->heap_end && bytes <= ctx->heap_end - addr);
        for (ULONG i = 0; i < bytes; i++)
            assert(ctx->mem[addr + i] == value);
    }

    template<class Operation> void unchanged(Operation operation)
    {
        const auto before = snapshot();
        const ULONG head = ctx->heap_free;
        const auto records = ctx->heap_allocations;
        const auto native = native_blocks;
        operation();
        assert(snapshot() == before && ctx->heap_free == head);
        assert(ctx->heap_allocations == records && native_blocks == native);
    }
};

static void test_clear()
{
    Fixture f;
    for (bool vec : { false, true })
        for (ULONG flags : { ULONG(MEMF_ANY), ULONG(MEMF_PUBLIC), ULONG(MEMF_CHIP),
                             ULONG(MEMF_FAST), ULONG(MEMF_CHIP | MEMF_FAST),
                             ULONG(MEMF_REVERSE | MEMF_NO_EXPUNGE) })
            for (ULONG bytes : { 1U, 7U, 8U, 9U, 65U, 4096U })
            {
                ULONG p = f.alloc(bytes, flags | MEMF_CLEAR, vec);
                assert(p && !(p & 7));
                const ULONG rounded = (bytes + 7) & ~ULONG(7);
                f.expect_bytes(p, rounded, 0);
                f.fill(p, rounded, 0x5A);
                f.release(p, bytes, vec);
                ULONG q = f.alloc(bytes, flags, vec);
                assert(q == p);
                f.expect_bytes(q, rounded, 0x5A);
                f.release(q, bytes, vec);
                q = f.alloc(bytes, flags | MEMF_CLEAR, vec);
                assert(q == p);
                f.expect_bytes(q, rounded, 0);
                f.release(q, bytes, vec);
            }
}

static void test_whole_block()
{
    Fixture f(32);
    for (bool vec : { false, true })
    {
        ULONG p = f.alloc(17, 0, vec);
        assert(p);
        f.fill(p, 24, 0xAB);
        f.release(p, 17, vec);
        p = f.alloc(9, MEMF_CLEAR, vec); /* Eight-byte tail cannot be split. */
        f.expect_bytes(p, 24, 0);
        assert(f.avail() == 0 && f.avail(MEMF_LARGEST) == 0);
        f.release(p, 9, vec);
        assert(f.avail() == 24);
    }
}

static void test_avail()
{
    Fixture f(2 * 1024 * 1024);
    const ULONG before = f.avail();
    assert(before == 2 * 1024 * 1024 - 8);
    for (ULONG type : { ULONG(MEMF_ANY), ULONG(MEMF_PUBLIC), ULONG(MEMF_CHIP),
                        ULONG(MEMF_FAST), ULONG(MEMF_CHIP | MEMF_FAST),
                        ULONG(MEMF_CLEAR | MEMF_REVERSE | MEMF_NO_EXPUNGE) })
    {
        assert(f.avail(type) == before);
        assert(f.avail(type | MEMF_LARGEST) == before);
        assert(f.avail(type | MEMF_TOTAL) == before);
        ULONG p = f.alloc(1024 * 1024, type);
        assert(p && f.avail(type) == before - 1024 * 1024 - 8);
        assert(f.avail(type | MEMF_LARGEST) == f.avail());
        assert(f.avail(type | MEMF_TOTAL) == before);
        assert(f.avail(type | MEMF_TOTAL | MEMF_LARGEST) == f.avail());
        f.release(p, 1024 * 1024);
        assert(f.avail(type) == before);
    }

    for (bool vec : { false, true })
        for (ULONG bytes : { 1U, 7U, 8U, 9U, 65U })
        {
            ULONG p = f.alloc(bytes, 0, vec);
            const ULONG consumed = ((bytes + 7) & ~ULONG(7)) + 8;
            assert(p && f.avail() == before - consumed);
            assert(f.avail(MEMF_LARGEST) == before - consumed);
            assert(f.avail(MEMF_TOTAL) == before);
            f.release(p, bytes, vec);
            assert(f.avail() == before);
        }

    /* Free islands have one private heap header each. Their total cannot
       be allocated in a single call; the largest island can. */
    ULONG a = f.alloc(64), b = f.alloc(128), c = f.alloc(192), d = f.alloc(256);
    assert(a && b && c && d);
    f.release(a, 64);
    f.release(c, 192);
    const ULONG largest = before - (64 + 128 + 192 + 256) - 4 * 8;
    assert(f.avail(MEMF_LARGEST) == largest);
    assert(f.avail() == largest + 64 + 192);
    assert(f.avail(MEMF_TOTAL) == before);
    f.unchanged([&] { assert(f.alloc(largest + 1) == 0); });
    ULONG p = f.alloc(largest, MEMF_CLEAR);
    assert(p);
    f.expect_bytes(p, largest, 0);
    assert(f.avail() == 256 && f.avail(MEMF_LARGEST) == 192);
    f.release(p, largest);
    f.release(b, 128);
    f.release(d, 256);
    assert(f.avail() == before && f.avail(MEMF_LARGEST) == before);
    p = f.alloc(before);
    assert(p && f.avail() == 0 && f.avail(MEMF_LARGEST) == 0);
    assert(f.avail(MEMF_TOTAL) == before);
    assert(f.avail(MEMF_TOTAL | MEMF_LARGEST) == 0);
    f.unchanged([&] { assert(f.alloc(1) == 0); });
    f.release(p, before);
    assert(f.avail() == before);
}

static void test_sizes()
{
    Fixture f;
    for (bool vec : { false, true })
        for (ULONG bytes : { 0U, 0xFFFFFFFFU, 0xFFFFFFFEU, 0xFFFFFFF9U,
                             0xFFFFFFF8U, 0xFFFFFFF1U, 0xFFFFFFF0U })
            f.unchanged([&] { assert(f.alloc(bytes, MEMF_CLEAR, vec) == 0); });
    for (ULONG addr : { 0U, 1U, 0xFFFFFFFFU, 0xFFFFFFFEU, f.ctx->mem_size,
                        f.ctx->mem_size - 1, f.ctx->heap_start, f.ctx->heap_end })
        f.unchanged([&] { f.release(addr, 0xFFFFFFFFU); f.release(addr, 0, true); });
    ULONG p = f.alloc(64);
    f.unchanged([&] { f.release(p + 8, 56); });
    f.release(p, 64);
    f.unchanged([&] { f.release(p, 64); });
    /* Internal loader calls retain the heap's zero-payload allocation;
       the Exec entry points alone guarantee NULL for a zero request. */
    const ULONG before = f.avail();
    p = Kaffeine_HeapAlloc(f.ctx, 0, 0);
    assert(p && f.avail() == before - 8);
    f.release(p);
    assert(f.avail() == before);
}

static void test_pools()
{
    Fixture f;
    const ULONG before = f.avail();
    for (ULONG flags : { ULONG(MEMF_ANY), ULONG(MEMF_CLEAR | MEMF_PUBLIC),
                         ULONG(MEMF_CLEAR | MEMF_CHIP), ULONG(MEMF_CLEAR | MEMF_FAST) })
    {
        ULONG pool = f.pool(flags, 4096, 1024);
        assert(pool && !(pool & 7) && pool >= f.ctx->heap_start && pool < f.ctx->heap_end);
        assert(m68k_read32(f.ctx, pool + 4) == flags);
        assert(m68k_read32(f.ctx, pool + 8) == 4096);
        assert(m68k_read32(f.ctx, pool + 12) == 1024);
        for (ULONG bytes : { 1U, 7U, 8U, 9U, 100U, 1024U, 4097U })
        {
            ULONG p = f.pooled(pool, bytes);
            assert(p && !(p & 7) && p != pool);
            if (flags & MEMF_CLEAR)
                f.expect_bytes(p, bytes, 0);
            f.fill(p, bytes, 0x7B);
            f.free_pooled(pool, p, bytes);
            ULONG q = f.pooled(pool, bytes);
            assert(q == p);
            f.expect_bytes(q, bytes, (flags & MEMF_CLEAR) ? 0 : 0x7B);
            f.free_pooled(pool, q, bytes);
        }
        f.unchanged([&] { assert(f.pooled(pool, 0) == 0); });
        f.unchanged([&] { assert(f.pooled(pool, 0xFFFFFFFFU) == 0); });
        f.delete_pool(pool);
        assert(f.avail() == before);
    }
    /* Actual AROS CreatePool does not enforce the historical threshold <=
       puddle restriction; both are non-reserving hints in this bridge. */
    for (auto hints : { std::array<ULONG, 2>{0, 0}, {1, 4096}, {0xFFFFFFFFU, 0xFFFFFFFFU} })
    {
        ULONG pool = f.pool(MEMF_CLEAR, hints[0], hints[1]);
        assert(pool);
        ULONG p = f.pooled(pool, 37);
        f.expect_bytes(p, 37, 0);
        f.delete_pool(pool);
        assert(f.avail() == before);
    }
}

static void test_delete()
{
    Fixture f;
    const ULONG original = f.avail();
    ULONG other = f.pool(), other_block = f.pooled(other, 73), plain = f.alloc(88);
    assert(other && other_block && plain);
    f.fill(other_block, 73, 0x92);
    f.fill(plain, 88, 0x93);
    const ULONG before = f.avail();
    ULONG pool = f.pool(MEMF_CLEAR), a = f.pooled(pool, 17), b = f.pooled(pool, 1500);
    ULONG c = f.pooled(pool, 4100);
    assert(pool && pool != other && a && b && c);
    f.free_pooled(pool, b, 1500);
    f.delete_pool(pool); /* a and c intentionally remain outstanding. */
    assert(f.avail() == before);
    f.expect_bytes(other_block, 73, 0x92);
    f.expect_bytes(plain, 88, 0x93);
    f.unchanged([&] { f.delete_pool(pool); f.free_pooled(pool, a, 17); });
    f.delete_pool(other);
    f.release(plain, 88);
    assert(f.avail() == original);
}

static void test_pool_clear_reuse()
{
    Fixture f;
    ULONG pool = f.pool(MEMF_CLEAR);
    assert(pool);
    ULONG p = f.pooled(pool, 100);
    assert(p);
    f.fill(p, 100, 0xA7);
    f.free_pooled(pool, p, 100);
    ULONG q = f.pooled(pool, 100);
    assert(q == p);
    f.expect_bytes(q, 100, 0);
    f.delete_pool(pool);
}

static void test_bad_addresses()
{
    Fixture f;
    ULONG pool = f.pool(MEMF_CLEAR), other = f.pool();
    ULONG p = f.pooled(pool, 33), q = f.pooled(other, 33), plain = f.alloc(64);
    assert(pool && other && p && q && plain);
    f.fill(p, 33, 0x11);
    f.fill(q, 33, 0x22);
    f.fill(plain, 64, 0x33);
    const ULONG bad_pools[] =
    {
        0, 1, 0x00CAFE00, 0xFFFFFFFFU, 0xFFFFFFFEU,
        f.ctx->mem_size, f.ctx->mem_size - 1, f.ctx->heap_start,
        f.ctx->heap_end, pool + 8, p, q, plain
    };
    for (ULONG bad : bad_pools)
        f.unchanged([&] {
            assert(f.pooled(bad, 33) == 0);
            f.free_pooled(bad, p, 33);
            f.delete_pool(bad);
        });
    const ULONG bad_blocks[] =
    {
        0, 1, 0xFFFFFFFFU, 0xFFFFFFFEU, f.ctx->mem_size, f.ctx->mem_size - 1,
        f.ctx->heap_start + 8, f.ctx->heap_end, p + 1, p + 8, p - 8,
        p + 33, q, plain, pool, other
    };
    for (ULONG bad : bad_blocks)
        f.unchanged([&] { f.free_pooled(pool, bad, 33); });
    for (ULONG bytes : { 0U, 32U, 34U, 40U, 0xFFFFFFFFU })
        f.unchanged([&] { f.free_pooled(pool, p, bytes); });
    f.unchanged([&] { f.release(p, 33); f.release(pool, 0, true); });
    f.ctx->num_shadows = 1;
    f.ctx->shadow_map[0].m68k_addr = plain;
    f.ctx->shadow_map[0].native_ptr = reinterpret_cast<void *>(uintptr_t(1));
    f.unchanged([&] { assert(f.pooled(plain, 33) == 0); f.delete_pool(plain); });
    f.free_pooled(pool, p, 33);
    f.unchanged([&] { f.free_pooled(pool, p, 33); });
    f.expect_bytes(q, 33, 0x22);
    f.expect_bytes(plain, 64, 0x33);
    f.delete_pool(pool);
    f.delete_pool(other);
    f.release(plain, 64);
}

/* The AChain copies of MatchFirst() and the locale blocks belong to the
   bridge: guest FreeMem() and FreeVec() leave them alone, and each owner
   releases only blocks of its own kind, by their exact base. */
static void test_owned_blocks()
{
    Fixture f(0x1000);
    const ULONG full = f.avail();
    ULONG achain = Kaffeine_AChainAlloc(f.ctx, 274), locale = Kaffeine_LocaleAlloc(f.ctx, 64);
    assert(achain && locale && !(achain & 7) && f.avail() < full);
    f.expect_bytes(achain, 280, 0);
    f.fill(achain, 274, 0x5A);
    f.unchanged([&] { f.release(achain, 274); f.release(achain, 0, true); });
    f.unchanged([&] { Kaffeine_LocaleFree(f.ctx, achain); });
    f.unchanged([&] { Kaffeine_AChainFree(f.ctx, locale); });
    f.unchanged([&] { Kaffeine_AChainFree(f.ctx, achain + 8); });
    f.unchanged([&] { Kaffeine_AChainFree(f.ctx, 0); });
    f.expect_bytes(achain, 274, 0x5A);
    Kaffeine_AChainFree(f.ctx, achain);
    Kaffeine_LocaleFree(f.ctx, locale);
    assert(f.avail() == full);
    f.unchanged([&] { Kaffeine_AChainFree(f.ctx, achain); });
    ULONG again = Kaffeine_AChainAlloc(f.ctx, 274);
    assert(again);
    f.expect_bytes(again, 280, 0);
    Kaffeine_AChainFree(f.ctx, again);
    assert(!Kaffeine_AChainAlloc(f.ctx, 0) && f.avail() == full);
}

static void test_guest_metadata()
{
    Fixture f;
    const ULONG before = f.avail();
    ULONG pool = f.pool(), p = f.pooled(pool, 17), plain = f.alloc(32);
    assert(pool && p && plain);
    /* A copy of the entire opaque pool header is still not a pool. */
    std::memcpy(f.ctx->mem + plain, f.ctx->mem + pool, 16);
    f.unchanged([&] { assert(f.pooled(plain, 17) == 0); f.delete_pool(plain); });
    /* Neither allocation boundaries nor ownership come from guest headers. */
    m68k_write32(f.ctx, p - 8, 0xFFFFFFFFU);
    m68k_write32(f.ctx, p - 4, plain);
    m68k_write32(f.ctx, pool - 8, 0xFFFFFFFFU);
    m68k_write32(f.ctx, pool, 0);
    f.fill(plain, 32, 0x31);
    f.delete_pool(pool);
    f.expect_bytes(plain, 32, 0x31);
    f.release(plain, 32);
    assert(f.avail() == before);

    /* A usable first fit followed by corrupt free metadata must fail before
       writing anything, rather than reading through a wrapping address. */
    ULONG a = f.alloc(64), b = f.alloc(64);
    f.release(a, 64);
    ULONG head = f.ctx->heap_free;
    ULONG next = m68k_read32(f.ctx, head + 4);
    for (ULONG corrupt : { head, head + 1, f.ctx->mem_size - 1, 0xFFFFFFFFU, 0xFFFFFFFEU })
    {
        m68k_write32(f.ctx, head + 4, corrupt);
        f.unchanged([&] {
            assert(f.alloc(1, MEMF_CLEAR) == 0);
            assert(f.avail() == 0 && f.avail(MEMF_LARGEST) == 0 && f.avail(MEMF_TOTAL) == 0);
            assert(f.pool() == 0);
            f.release(b, 64);
        });
    }
    m68k_write32(f.ctx, head + 4, next);
    ULONG free_bytes = m68k_read32(f.ctx, head);
    for (ULONG corrupt : { 0U, 1U, 7U, 9U, free_bytes + 8, 0xFFFFFFFFU })
    {
        m68k_write32(f.ctx, head, corrupt);
        f.unchanged([&] {
            assert(f.alloc(1) == 0 && f.avail() == 0 && f.pool() == 0);
            f.release(b, 64);
        });
    }
    m68k_write32(f.ctx, head, free_bytes);
    f.release(b, 64);
    assert(f.avail() == before);
}

static void test_native_failure()
{
    Fixture f;
    const ULONG before = f.avail();
    f.unchanged([&] {
        fail_native_allocation = true;
        assert(f.alloc(32, MEMF_CLEAR) == 0 && !fail_native_allocation);
    });
    f.unchanged([&] {
        fail_native_allocation = true;
        assert(f.pool() == 0 && !fail_native_allocation);
    });
    ULONG pool = f.pool(MEMF_CLEAR);
    assert(pool);
    f.unchanged([&] {
        fail_native_allocation = true;
        assert(f.pooled(pool, 32) == 0 && !fail_native_allocation);
    });
    f.delete_pool(pool);
    ULONG all = f.alloc(f.avail(MEMF_LARGEST));
    assert(all);
    f.unchanged([&] { assert(f.pool() == 0); });
    f.release(all);
    assert(f.avail() == before);
    /* Context destruction also releases the native records of pools and
       allocations that the guest never returned. */
    pool = f.pool();
    assert(pool && f.pooled(pool, 100) && f.alloc(100));
}

static void test_sequences()
{
    Fixture f(0x10000);
    const ULONG before = f.avail();
    const ULONG pools[] = { f.pool(), f.pool(MEMF_CLEAR) };
    struct Live { ULONG addr = 0, bytes = 0, owner = 0; UBYTE value = 0; };
    std::array<Live, 24> live;
    uint32_t seed = 0x51A71E;
    for (unsigned step = 0; step < 500; step++)
    {
        seed = seed * 1664525U + 1013904223U;
        auto &slot = live[(seed >> 8) % live.size()];
        if (slot.addr)
            f.free_pooled(slot.owner, slot.addr, slot.bytes);
        slot.bytes = 1 + ((seed >> 16) % 1500);
        slot.owner = pools[seed & 1];
        slot.addr = f.pooled(slot.owner, slot.bytes);
        assert(slot.addr);
        if (slot.owner == pools[1])
            f.expect_bytes(slot.addr, slot.bytes, 0);
        slot.value = 1 + step % 254;
        f.fill(slot.addr, slot.bytes, slot.value);
        for (const auto &other : live)
            if (other.addr)
                f.expect_bytes(other.addr, other.bytes, other.value);
    }
    f.delete_pool(pools[0]);
    for (const auto &slot : live)
        if (slot.addr && slot.owner == pools[1])
            f.expect_bytes(slot.addr, slot.bytes, slot.value);
    f.delete_pool(pools[1]);
    assert(f.avail() == before && f.avail(MEMF_LARGEST) == before);
}

/* -624 CopyMem() and -630 CopyMemQuick(): A0 source, A1 dest, D0 size. */
static void test_copy()
{
    Fixture f;
    const ULONG a = f.alloc(64), b = f.alloc(64), end = f.ctx->mem_size;
    assert(a && b);
    for (ULONG lvo : { 624U, 630U })
    {
        for (ULONG i = 0; i < 64; i++)
            f.ctx->mem[a + i] = (UBYTE)(i + 1);
        f.fill(b, 64, 0);
        assert(f.call(lvo, 64, 0, 0, a, b) == 0);
        assert(std::memcmp(f.ctx->mem + a, f.ctx->mem + b, 64) == 0);

        /* Overlap in both directions keeps the source bytes. */
        for (ULONG i = 0; i < 64; i++)
            f.ctx->mem[a + i] = (UBYTE)i;
        assert(f.call(lvo, 32, 0, 0, a, a + 8) == 0);
        for (ULONG i = 0; i < 32; i++)
            assert(f.ctx->mem[a + 8 + i] == (UBYTE)i);
        for (ULONG i = 0; i < 64; i++)
            f.ctx->mem[a + i] = (UBYTE)i;
        assert(f.call(lvo, 32, 0, 0, a + 8, a) == 0);
        for (ULONG i = 0; i < 32; i++)
            assert(f.ctx->mem[a + i] == (UBYTE)(i + 8));

        /* A range that ends exactly at the end of guest memory is copied. */
        std::memset(f.ctx->mem + end - 4, 0x5A, 4);
        assert(f.call(lvo, 4, 0, 0, end - 4, b) == 0);
        f.expect_bytes(b, 4, 0x5A);
        f.fill(b, 4, 0xC3);
        assert(f.call(lvo, 4, 0, 0, b, end - 4) == 0);
        for (ULONG i = 0; i < 4; i++)
            assert(f.ctx->mem[end - 4 + i] == 0xC3);

        /* Size 0, address 0 and ranges that leave guest memory copy nothing. */
        const ULONG cases[][3] = {
            { a, b, 0 }, { 0, b, 4 }, { a, 0, 4 }, { end - 2, b, 4 }, { a, end - 2, 4 },
            { end, b, 1 }, { a, end, 1 }, { 0xFFFFFFFCU, b, 8 }, { a, 0xFFFFFFFCU, 8 },
            { a, b, 0xFFFFFFFFU }, { a, b, end }
        };
        for (const auto &c : cases)
        {
            const UBYTE tail[2] = { f.ctx->mem[end - 2], f.ctx->mem[end - 1] };
            f.unchanged([&] { assert(f.call(lvo, c[2], 0, 0, c[0], c[1]) == 0); });
            assert(f.ctx->mem[end - 2] == tail[0] && f.ctx->mem[end - 1] == tail[1]);
        }

        /* The guest copy of a shadowed structure is copied as guest bytes;
           the native structure behind it is never read or written. */
        std::array<UBYTE, 64> native;
        native.fill(0x77);
        f.ctx->shadow_map[0].m68k_addr = b;
        f.ctx->shadow_map[0].native_ptr = native.data();
        f.ctx->shadow_map[0].type = 1;
        f.ctx->num_shadows = 1;
        f.fill(a, 16, 0x11);
        assert(f.call(lvo, 16, 0, 0, a, b) == 0);
        f.expect_bytes(b, 16, 0x11);
        f.fill(b, 16, 0x22);
        assert(f.call(lvo, 16, 0, 0, b, a) == 0);
        f.expect_bytes(a, 16, 0x22);
        for (UBYTE value : native)
            assert(value == 0x77);
        f.ctx->num_shadows = 0;
    }
    f.release(a);
    f.release(b);
}

static void test_absolute()
{
    Fixture f;
    const ULONG start = f.ctx->heap_start, end = f.ctx->heap_end, before = f.avail();
    f.fill(start + 0x100, 24, 0x61);
    const ULONG abs = f.call(204, 17, 0xD1BAD, 0, 0xA0BAD, start + 0x103);
    assert(abs == start + 0x100);
    f.expect_bytes(abs, 24, 0x61);
    f.unchanged([&] {
        assert(!f.call(204, 1, 0, 0, 0, abs + 1));
        f.release(abs + 1);
    });
    f.release(abs, 17);
    assert(f.avail() == before);
    f.unchanged([&] { f.release(abs); });

    const ULONG used = f.alloc(64);
    f.fill(used, 64, 0x72);
    f.unchanged([&] {
        assert(!f.call(204, 32, 0, 0, 0, used - 16));
        assert(!f.call(204, 1, 0, 0, 0, used));
        assert(!f.call(204, 1, 0, 0, 0, used - 1));
    });
    f.expect_bytes(used, 64, 0x72);
    f.release(used);
    const ULONG invalid[][2] = { {start, 0}, {start - 1, 1}, {end, 1}, {end - 1, 2},
        {0, 8}, {f.ctx->mem_size - 1, 2}, {0xFFFFFFFEu, 8}, {start, 0xFFFFFFFFu} };
    for (const auto &c : invalid)
        f.unchanged([&] { assert(!f.call(204, c[1], 0, 0, 0, c[0])); });

    /* Both eight-byte free fragments and a headerless whole-heap claim. */
    assert(f.call(204, end - start - 16, 0, 0, 0, start + 8) == start + 8);
    assert(f.avail() == 0);
    f.release(start + 8);
    assert(f.avail() == before);
    assert(f.call(204, end - start, 0, 0, 0, start) == start);
    assert(!f.alloc(1));
    f.release(start);
    assert(f.avail() == before);
    assert(f.call(204, 1, 0, 0, 0, start) == start);
    assert(f.call(204, 1, 0, 0, 0, end - 1) == end - 8);
    f.release(start);
    f.release(end - 8);
    assert(f.avail() == before);

    const ULONG freed = f.alloc(512);
    f.release(freed, 512);
    assert(f.call(204, 120, 0, 0, 0, freed + 64) == freed + 64);
    f.release(freed + 64, 120);
    assert(f.avail() == before);
    for (int fail_after : {0, 1})
    {
        native_allocations_before_failure = fail_after;
        f.unchanged([&] { assert(!f.call(204, 8, 0, 0, 0, start + 16)); });
        assert(native_allocations_before_failure == -1 && f.avail() == before);
    }
    m68k_write32(f.ctx, start + 4, 0xFFFFFFFE);
    f.unchanged([&] { assert(!f.call(204, 8, 0, 0, 0, start + 16)); });
    m68k_write32(f.ctx, start + 4, 0);
}

static void test_absolute_sequences()
{
    Fixture f;
    const ULONG before = f.avail();
    std::array<ULONG, 64> absolute = {};
    std::array<ULONG, 8> ordinary = {};
    ULONG seed = 0xB14826;
    for (unsigned step = 0; step < 800; ++step)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const unsigned index = seed % absolute.size();
        const ULONG location = f.ctx->heap_start + 8 + index * 64;
        if (absolute[index])
        {
            assert(!f.call(204, 1, 0, 0, 0, absolute[index]));
            f.release(absolute[index]);
            absolute[index] = 0;
        }
        else
        {
            absolute[index] = f.call(204, 13, 0, 0, 0, location + 3);
            if (absolute[index])
            {
                assert(absolute[index] == location);
                f.fill(location, 16, static_cast<UBYTE>(index + 1));
            }
        }
        if (!(step % 7))
        {
            const unsigned slot = (seed >> 12) % ordinary.size();
            if (ordinary[slot]) { f.release(ordinary[slot]); ordinary[slot] = 0; }
            else
            {
                ordinary[slot] = f.alloc(24);
                if (ordinary[slot]) f.fill(ordinary[slot], 24, 0x91);
            }
        }
        for (unsigned i = 0; i < absolute.size(); ++i)
            if (absolute[i]) f.expect_bytes(absolute[i], 16, static_cast<UBYTE>(i + 1));
        for (ULONG addr : ordinary) if (addr) f.expect_bytes(addr, 24, 0x91);
    }
    for (ULONG addr : absolute) if (addr) f.release(addr);
    for (ULONG addr : ordinary) if (addr) f.release(addr);
    assert(f.avail() == before);
}

static void entry_spec(Fixture &f, ULONG at, UWORD count)
{
    std::memset(f.ctx->mem + at, 0xC3, 16 + 8 * count);
    m68k_write16(f.ctx, at + 14, count);
    for (ULONG i = 0; i < count; ++i)
    {
        m68k_write32(f.ctx, at + 16 + 8 * i, i == 1 ? MEMF_CLEAR | MEMF_PUBLIC : MEMF_PUBLIC);
        m68k_write32(f.ctx, at + 20 + 8 * i, i == 2 ? 0 : 17 + i * 8);
    }
}

static void test_entries()
{
    Fixture f;
    const ULONG input = 0x800, before = f.avail();
    entry_spec(f, input, 3);
    const ULONG list = f.call(222, 0xD0BAD, 0xD1BAD, 0, input, 0xFFFFFFFE);
    assert(list && !(list & 0x80000000u) && list != input && !(list & 7));
    assert(m68k_read16(f.ctx, list + 14) == 3);
    for (ULONG i = 0; i < 14; ++i) assert(f.ctx->mem[list + i] == 0);
    const ULONG a = m68k_read32(f.ctx, list + 16), b = m68k_read32(f.ctx, list + 24);
    assert(a && b && a != b && !(a & 7) && !(b & 7));
    assert(m68k_read32(f.ctx, list + 20) == 17 && m68k_read32(f.ctx, list + 28) == 25 &&
           !m68k_read32(f.ctx, list + 32) && !m68k_read32(f.ctx, list + 36));
    f.expect_bytes(b, 25, 0);
    assert(f.avail() < before);
    /* The native group owns its entries. Guest metadata cannot free others. */
    f.unchanged([&] { f.release(list); f.release(a); });
    for (ULONG addr : {0u, input, list + 8, 0xFFFFFFFEu, f.ctx->mem_size - 15})
        f.unchanged([&] { assert(f.call(228, 0xD01234, 0, 0, addr) == 0xD01234); });
    const ULONG foreign = f.alloc(32);
    for (unsigned damage = 0; damage < 4; ++damage)
    {
        const ULONG offset = damage == 0 ? 14 : damage == 1 ? 16 : damage == 2 ? 20 : 24;
        const ULONG saved = m68k_read32(f.ctx, list + offset);
        if (damage == 0) m68k_write16(f.ctx, list + 14, 0xFFFF);
        if (damage == 1) m68k_write32(f.ctx, list + 16, foreign);
        if (damage == 2) m68k_write32(f.ctx, list + 20, 99);
        if (damage == 3) m68k_write32(f.ctx, list + 24, a);
        f.unchanged([&] { assert(f.call(228, 0xD01234, 0, 0, list) == 0xD01234); });
        m68k_write32(f.ctx, list + offset, saved);
    }
    /* Linking the MemList into a guest task's list is legitimate. */
    m68k_write32(f.ctx, list, 0xBAD1); m68k_write32(f.ctx, list + 4, 0xBAD2);
    assert(f.call(228, 0xD01234, 0, 0, list) == 0xD01234);
    f.release(foreign);
    assert(f.avail() == before);
    f.unchanged([&] { assert(f.call(228, 0xD01234, 0, 0, list) == 0xD01234); });
    entry_spec(f, input, 0);
    const ULONG empty = f.call(222, 0, 0, 0, input);
    assert(empty && !(empty & 0x80000000u) && !m68k_read16(f.ctx, empty + 14));
    f.call(228, 0, 0, 0, empty);
    assert(f.avail() == before);
}

static void test_entry_failures()
{
    Fixture f;
    const ULONG input = 0x800, before = f.avail(), public_failure = 0x80000001u;
    for (ULONG addr : {0u, 0xFFFFFFFEu, f.ctx->mem_size, f.ctx->mem_size - 15})
        f.unchanged([&] { assert(f.call(222, 0, 0, 0, addr) == public_failure); });
    const ULONG end_input = f.ctx->mem_size - 24;
    entry_spec(f, end_input, 1);
    const ULONG list = f.call(222, 0, 0, 0, end_input);
    assert(list && !(list & 0x80000000u));
    f.call(228, 0, 0, 0, list);
    m68k_write16(f.ctx, end_input + 14, 2);
    f.unchanged([&] { assert(f.call(222, 0, 0, 0, end_input) == public_failure); });
    m68k_write16(f.ctx, end_input + 14, 0xFFFF);
    f.unchanged([&] { assert(f.call(222, 0, 0, 0, end_input) == public_failure); });
    entry_spec(f, input, 3);
    const ULONG source_before = m68k_read32(f.ctx, input + 24);
    m68k_write32(f.ctx, input + 24, MEMF_CLEAR | 4);
    m68k_write32(f.ctx, input + 28, 0xFFFFFFFFu);
    assert(f.call(222, 0, 0, 0, input) == (0x80000000u | MEMF_CLEAR | 4));
    assert(f.avail() == before && native_allocations_before_failure == -1);
    assert(m68k_read32(f.ctx, input + 24) == (MEMF_CLEAR | 4));
    /* A representable request that exhausts the heap after the first member. */
    m68k_write32(f.ctx, input + 28, before);
    assert(f.call(222, 0, 0, 0, input) == (0x80000000u | MEMF_CLEAR | 4));
    assert(f.avail() == before);
    m68k_write32(f.ctx, input + 24, source_before);
    m68k_write32(f.ctx, input + 28, 25);
    /* Scratch, list record, first member, then second member. */
    for (int fail_after : {0, 1, 2, 3})
    {
        const auto source = std::vector<UBYTE>(f.ctx->mem + input, f.ctx->mem + input + 40);
        const auto native_count = native_blocks.size();
        native_allocations_before_failure = fail_after;
        const ULONG expected = fail_after < 3 ? public_failure : (0x80000000u | MEMF_CLEAR | 1);
        assert(f.call(222, 0, 0, 0, input) == expected);
        assert(native_allocations_before_failure == -1 && f.avail() == before &&
               native_blocks.size() == native_count);
        assert(!std::memcmp(source.data(), f.ctx->mem + input, source.size()));
    }
    Fixture small(32);
    entry_spec(small, input, 3);
    small.unchanged([&] { assert(small.call(222, 0, 0, 0, input) == public_failure); });
}

static void test_capacity_storage()
{
    Fixture f;
    const ULONG address = f.call(198, KAFFEINE_MEMINFO_SIZE, MEMF_CLEAR);
    assert(address);
    f.ctx->memory_info = address;
    const auto before = f.snapshot();
    const ULONG available = f.call(216);
    f.call(210, KAFFEINE_MEMINFO_SIZE, 0, 0, 0, address);
    f.call(690, 0, 0, 0, 0, address);
    assert(f.snapshot() == before && f.call(216) == available);
}

static void test_fallbacks()
{
    Fixture f;
    f.ctx->libs[0].thunks = nullptr;
    f.ctx->libs[0].num_thunks = 0;
    for (const auto &entry : generated_table)
        f.unchanged([&] {
            assert(f.call(entry.lvo, 33, MEMF_CLEAR, 1024, 0xFFFFFFFFU,
                          0xFFFFFFFEU, true) == 0xBADCA11);
        });
}

static void test_context_guard()
{
    static_assert(KAFFEINE_MEM_GUARD == 0x8000);
    const auto before = native_blocks;
    KaffeineContext *ctx = Kaffeine_CreateContext(nullptr);
    assert(ctx && ctx->mem_size == KAFFEINE_MEM_SIZE && ctx->stack_top == ctx->mem_size &&
           ctx->heap_end <= ctx->mem_size &&
           native_blocks.at(ctx->mem) == ctx->mem_size + KAFFEINE_MEM_GUARD);
    UBYTE *mem = ctx->mem;
    assert(std::all_of(mem + ctx->mem_size, mem + ctx->mem_size + KAFFEINE_MEM_GUARD,
                       [](UBYTE b) { return b == 0; }));
    // Native rendering may write in the guard. Destruction still frees
    // the complete allocation, with the guest-visible size unchanged.
    std::memset(mem + ctx->mem_size, 0xA5, KAFFEINE_MEM_GUARD);
    const size_t freed = native_frees.size();
    Kaffeine_DestroyContext(ctx);
    const auto begin = native_frees.begin() + freed;
    assert(native_blocks == before &&
           std::count(begin, native_frees.end(), std::make_pair((APTR)mem, (ULONG)(KAFFEINE_MEM_SIZE + KAFFEINE_MEM_GUARD))) == 1 &&
           std::count(begin, native_frees.end(), std::make_pair((APTR)ctx, (ULONG)sizeof(*ctx))) == 1);

    // Fail the arena, context, or initial heap record. The counted FreeMem
    // stub also verifies exact allocation sizes on every rollback path.
    for (int fail : { 0, 1, 2 })
    {
        const size_t mark = native_frees.size();
        native_allocations_before_failure = fail;
        assert(!Kaffeine_CreateContext(nullptr) && native_blocks == before &&
               native_allocations_before_failure == -1 && native_frees.size() == mark + fail);
        const auto first = native_frees.begin() + mark;
        assert(std::count_if(first, native_frees.end(), [](const auto &f) {
                   return f.second == KAFFEINE_MEM_SIZE + KAFFEINE_MEM_GUARD;
               }) == (fail ? 1 : 0));
    }
    native_frees.clear();
}

int main(int argc, char **argv)
{
    const struct { const char *name; void (*run)(); } tests[] =
    {
        { "context-guard", test_context_guard },
        { "clear", test_clear }, { "whole-block", test_whole_block },
        { "avail", test_avail }, { "sizes", test_sizes },
        { "pools", test_pools }, { "pool-clear", test_pool_clear_reuse }, { "delete", test_delete },
        { "bad-addresses", test_bad_addresses }, { "owned-blocks", test_owned_blocks },
        { "guest-metadata", test_guest_metadata },
        { "native-failure", test_native_failure }, { "sequences", test_sequences },
        { "copy", test_copy }, { "absolute", test_absolute },
        { "absolute-sequences", test_absolute_sequences },
        { "entries", test_entries }, { "entry-failures", test_entry_failures },
        { "capacity-storage", test_capacity_storage },
        { "fallbacks", test_fallbacks }
    };
    unsigned groups = 0;
    for (const auto &test : tests)
        if (argc == 1 || std::strcmp(argv[1], test.name) == 0)
        {
            test.run();
            assert(native_blocks.empty());
            std::printf("PASS: %s\n", test.name);
            groups++;
        }
    assert(groups);
    std::printf("Exec guest memory: %u groups passed\n", groups);
    return 0;
}

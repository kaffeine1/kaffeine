/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Bound production heap work through literal Exec LVOs. Only the C heap
    object uses trace-pc; native allocator, dispatcher and test work is excluded.
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

static uint64_t work;
static std::map<APTR, ULONG> native_blocks;
static int fail_after = -1;

extern "C" void __sanitizer_cov_trace_pc(void) { ++work; }
extern "C" unsigned long test_heap_record_size(void);
extern "C" BOOL test_heap_invariants(KaffeineContext *ctx);

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    if (fail_after == 0) { fail_after = -1; return nullptr; }
    if (fail_after > 0) --fail_after;
    APTR block = std::malloc(size);
    if (block)
    {
        std::memset(block, flags & MEMF_CLEAR ? 0 : 0xA5, size);
        assert(native_blocks.emplace(block, size).second);
    }
    return block;
}
extern "C" void FreeMem(APTR block, ULONG size)
{
    auto found = native_blocks.find(block);
    assert(found != native_blocks.end() && found->second == size);
    native_blocks.erase(found);
    std::free(block);
}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }

struct State { ULONG d[8] = {}, a[8] = {}; UWORD sr = 0xA715; ULONG pc = 0x43210; };
static ULONG get_d(void *s, int n) { return static_cast<State *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<State *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<State *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<State *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<State *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<State *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<State *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<State *>(s)->pc = v; }
static const KaffeineCPUOps ops = { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, nullptr };
static unsigned generated_calls;
static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBADCA11; }
static const M68KThunkEntry manual[] = { KAFFEINE_EXEC_MEMORY_THUNKS { 702, Kaffeine_DeletePool } };
static const M68KThunkEntry generated[] =
{
    {198, poison}, {210, poison}, {216, poison}, {684, poison}, {690, poison},
    {696, poison}, {702, poison}, {708, poison}, {714, poison}
};

struct Fixture
{
    KaffeineContext *ctx = Kaffeine_CreateContext(nullptr);
    State state;
    KaffeineCPU cpu = { &ops, &state };
    uint64_t last = 0;

    Fixture()
    {
        assert(ctx);
        ctx->num_libs = 1;
        auto &lib = ctx->libs[0];
        lib.m68k_addr = 0x10000;
        lib.jt_start = 0x10000 - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual;
        lib.num_thunks = std::size(manual);
        lib.gen_thunks = generated;
        lib.num_gen_thunks = std::size(generated);
        m68k_write32(ctx, 0x200, 0x76543210);
    }
    ~Fixture() { Kaffeine_DestroyContext(ctx); assert(native_blocks.empty()); }

    ULONG call(ULONG lvo, ULONG d0 = 0, ULONG d1 = 0, ULONG a0 = 0, ULONG a1 = 0)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = 0xD0000000U + i;
            state.a[i] = 0xA0000000U + i;
        }
        state.d[0] = d0;
        state.d[1] = d1;
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[7] = 0x200;
        const auto before = state;
        KaffeineDispatchResult result = {};
        generated_calls = 0;
        work = 0;
        assert(Kaffeine_DispatchLibraryCall(ctx, &cpu, 0x10000 - lvo, &result));
        last = work;
        assert(!generated_calls && !result.supervisor && result.return_pc == 0x76543210);
        assert(state.a[7] == 0x204 && m68k_read32(ctx, 0x200) == 0x76543210);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.pc == before.pc && state.sr == before.sr);
        return state.d[0];
    }
    void bounded(ULONG n, const char *operation, uint64_t multiplier = 1)
    {
        unsigned bits = 1;
        for (ULONG count = n + 16; count >>= 1;) ++bits;
        const uint64_t budget = multiplier * (512 * bits + 1024);
        if (last > budget)
        {
            std::fprintf(stderr, "%s with %u live blocks: %llu heap basic blocks, budget %llu\n",
                         operation, n, (unsigned long long)last, (unsigned long long)budget);
            std::abort();
        }
    }
    ULONG alloc(ULONG bytes, bool vec = false) { return call(vec ? 684 : 198, bytes); }
    void release(ULONG addr, bool vec = false) { assert(call(vec ? 690 : 210, 0, 0, 0, addr) == 0); }
    ULONG pool() { return call(696, MEMF_CLEAR, 4096); }
    ULONG pooled(ULONG owner, ULONG bytes) { return call(708, bytes, 0, owner); }
    void free_pooled(ULONG owner, ULONG addr, ULONG bytes) { assert(call(714, bytes, 0, owner, addr) == 0); }
    void delete_pool(ULONG owner) { assert(call(702, 0, 0, owner) == 0); }
    void valid() { assert(test_heap_invariants(ctx)); }
};

static void scaling()
{
    std::printf("Native interval record: %lu bytes\n", test_heap_record_size());
    for (ULONG n : { 64U, 1024U, 16384U })
    {
        Fixture f;
        const ULONG before = f.call(216);
        const ULONG owner = f.pool(), foreign = f.pool();
        std::vector<ULONG> live(n), owned;
        for (ULONG i = 0; i < n; ++i)
        {
            live[i] = i & 1 ? f.pooled(foreign, 24 + i % 5 * 8) : f.alloc(24 + i % 5 * 8);
            assert(live[i]);
        }
        f.valid();
        uint64_t maximum_alloc = 0, maximum_free = 0, maximum_pool = 0;
        for (bool vec : { false, true })
        {
            ULONG p = f.alloc(24, vec);
            f.bounded(n, "AllocMem/AllocVec");
            maximum_alloc = std::max(maximum_alloc, f.last);
            assert(p);
            f.release(p, vec);
            f.bounded(n, "FreeMem/FreeVec");
            maximum_free = std::max(maximum_free, f.last);
        }
        for (ULONG i = 0; i < 3; ++i)
        {
            owned.push_back(f.pooled(owner, 24));
            assert(owned.back());
            f.bounded(n, "AllocPooled");
            maximum_pool = std::max(maximum_pool, f.last);
        }
        f.free_pooled(owner, owned.back(), 24);
        f.bounded(n, "FreePooled");
        maximum_pool = std::max(maximum_pool, f.last);
        f.delete_pool(owner); /* Two members, independently of foreign blocks. */
        f.bounded(n, "DeletePool", 3);
        uint64_t delete_work = f.last;
        assert(f.call(216));
        f.bounded(n, "AvailMem with one free interval");
        f.valid();
        for (ULONG i = 0; i < n; i += 2)
        {
            f.release(live[i]);
            f.bounded(n, "fragmented FreeMem");
            maximum_free = std::max(maximum_free, f.last);
        }
        f.valid();
        const ULONG fragmented_owner = f.pool();
        assert(fragmented_owner && f.pooled(fragmented_owner, 24) && f.pooled(fragmented_owner, 24));
        f.delete_pool(fragmented_owner);
        f.bounded(n, "DeletePool among foreign free intervals", 3);
        delete_work = std::max(delete_work, f.last);
        f.valid();
        for (ULONG i = 0; i < n; i += 2)
        {
            live[i] = f.alloc(24);
            assert(live[i]);
            f.bounded(n, "first-fit AllocMem");
            maximum_alloc = std::max(maximum_alloc, f.last);
        }
        f.valid();
        for (ULONG i = 0; i < n; ++i)
        {
            if (i & 1) f.free_pooled(foreign, live[i], 24 + i % 5 * 8);
            else f.release(live[i]);
            f.bounded(n, "free all");
            maximum_free = std::max(maximum_free, f.last);
        }
        f.delete_pool(foreign);
        assert(f.call(216) == before);
        f.valid();
        std::printf("%5u live: alloc %llu, free %llu, pooled %llu, delete(2) %llu heap basic blocks\n",
                    n, (unsigned long long)maximum_alloc, (unsigned long long)maximum_free,
                    (unsigned long long)maximum_pool, (unsigned long long)delete_work);
    }
}

static void corruption()
{
    Fixture f;
    std::array<ULONG, 4> live;
    for (auto &addr : live)
    {
        addr = f.alloc(64);
        assert(addr);
        std::memset(f.ctx->mem + addr, 0x37, 64);
    }
    f.release(live[0]);
    f.release(live[2]);
    const ULONG hole = m68k_read32(f.ctx, f.ctx->heap_free + 4);
    const ULONG size = m68k_read32(f.ctx, hole), next = m68k_read32(f.ctx, hole + 4);
    assert(hole && hole != f.ctx->heap_free);
    for (ULONG corrupt : { hole, live[1], 0xFFFFFFFEU, 0xFFFFFFFFU })
    {
        m68k_write32(f.ctx, hole + 4, corrupt);
        assert(f.call(216) == 0 && f.call(216, 0, MEMF_TOTAL) == 0);
        /* An untouched corrupt mirror cannot steer the native first fit. */
        ULONG p = f.alloc(24);
        assert(p && p < live[3] && p + 24 <= live[3] - 8);
        const std::vector<UBYTE> snapshot(f.ctx->mem, f.ctx->mem + f.ctx->mem_size);
        const auto native = native_blocks;
        f.release(live[1]); /* Its free neighbours include the corrupt hole. */
        assert(std::memcmp(snapshot.data(), f.ctx->mem, snapshot.size()) == 0);
        assert(native == native_blocks);
        for (ULONG addr : { live[1], live[3] })
            for (unsigned i = 0; i < 64; ++i) assert(f.ctx->mem[addr + i] == 0x37);
        m68k_write32(f.ctx, hole + 4, next);
        f.release(p);
        f.valid();
    }
    m68k_write32(f.ctx, hole, size + 8); /* Enlarging a free span into live memory. */
    assert(f.call(216) == 0);
    const auto snapshot = std::vector<UBYTE>(f.ctx->mem, f.ctx->mem + f.ctx->mem_size);
    f.release(live[1]);
    assert(std::memcmp(snapshot.data(), f.ctx->mem, snapshot.size()) == 0);
    m68k_write32(f.ctx, hole, size);
    f.release(live[1]);
    f.release(live[3]);
    f.valid();
}

/* Allocation order alone only ever inserts below the previous block. Mixed
   operations insert anywhere, so every AVL rotation case occurs; the
   invariants and every block's contents are checked after each step. */
static void random_operations()
{
    struct Block { ULONG addr, size, pool; bool vec; UBYTE fill; };
    Fixture f;
    const ULONG before = f.call(216);
    std::vector<Block> blocks;
    std::vector<ULONG> pools;
    uint32_t seed = 0x2545F491U;
    auto next = [&seed](uint32_t range) {
        seed = seed * 1664525U + 1013904223U;
        return (seed >> 8) % range;
    };
    auto intact = [&f](const Block &b) {
        for (ULONG i = 0; i < b.size; ++i)
            if (f.ctx->mem[b.addr + i] != b.fill) return false;
        return true;
    };

    for (unsigned step = 0; step < 6000; ++step)
    {
        const uint32_t op = next(10);
        if (op < 4 || blocks.empty())
        {
            Block b = { 0, 1 + next(700), 0, false, static_cast<UBYTE>(step | 1) };
            if (op == 3 && !pools.empty())
            {
                b.pool = pools[next(pools.size())];
                b.addr = f.pooled(b.pool, b.size);
            }
            else
            {
                b.vec = op & 1;
                b.addr = f.alloc(b.size, b.vec);
            }
            assert(b.addr);
            std::memset(f.ctx->mem + b.addr, b.fill, b.size);
            blocks.push_back(b);
        }
        else if (op < 8)
        {
            const size_t i = next(blocks.size());
            const Block b = blocks[i];
            assert(intact(b));
            if (b.pool) f.free_pooled(b.pool, b.addr, b.size);
            else f.release(b.addr, b.vec);
            blocks[i] = blocks.back();
            blocks.pop_back();
        }
        else if (op == 8 && pools.size() < 8)
        {
            pools.push_back(f.pool());
            assert(pools.back());
        }
        else if (!pools.empty())
        {
            const size_t i = next(pools.size());
            const ULONG owner = pools[i];
            f.delete_pool(owner);
            blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                        [owner](const Block &b) { return b.pool == owner; }),
                         blocks.end());
            pools[i] = pools.back();
            pools.pop_back();
        }
        f.valid();
    }
    for (const Block &b : blocks)
    {
        assert(intact(b));
        if (!b.pool) f.release(b.addr, b.vec);
    }
    for (ULONG owner : pools) f.delete_pool(owner);
    assert(f.call(216) == before);
    f.valid();
}

/* DeletePool() checks the free neighbours of every member before it frees
   any: a corrupt mirror next to a later member leaves the pool whole. */
static void pool_delete_atomicity()
{
    Fixture f;
    const ULONG before = f.call(216);
    /* Each allocation lies below the previous one. */
    const ULONG owner = f.pool(), w = f.alloc(64), a = f.pooled(owner, 64), y = f.alloc(64);
    const ULONG z = f.alloc(64), v = f.alloc(64), b = f.pooled(owner, 64), guard = f.alloc(64);
    assert(owner && w && a && y && z && v && b && guard);
    assert(owner > w && w > a && a > y && y > z && z > v && v > b && b > guard);
    f.release(w);
    f.release(y);
    f.release(v);
    f.valid();
    const ULONG hole = y - 8, size = m68k_read32(f.ctx, hole);
    m68k_write32(f.ctx, hole, size + 8);
    const std::vector<UBYTE> snapshot(f.ctx->mem, f.ctx->mem + f.ctx->mem_size);
    const auto native = native_blocks;
    /* b is released first and has valid neighbours; a borders the bad mirror. */
    f.delete_pool(owner);
    assert(std::memcmp(snapshot.data(), f.ctx->mem, snapshot.size()) == 0);
    assert(native == native_blocks);
    m68k_write32(f.ctx, hole, size);
    f.valid();
    f.delete_pool(owner);
    f.valid();
    f.release(z);
    f.release(guard);
    assert(f.call(216) == before);
    f.valid();
}

static void initialization_failure()
{
    fail_after = 2; /* Arena and context succeed, initial native interval fails. */
    assert(!Kaffeine_CreateContext(nullptr) && native_blocks.empty());
}

int main(int argc, char **argv)
{
    if (argc == 1 || std::strcmp(argv[1], "scaling") == 0) scaling();
    if (argc == 1 || std::strcmp(argv[1], "corruption") == 0) corruption();
    if (argc == 1 || std::strcmp(argv[1], "random") == 0) random_operations();
    if (argc == 1 || std::strcmp(argv[1], "pool-atomicity") == 0) pool_delete_atomicity();
    if (argc == 1 || std::strcmp(argv[1], "initialization") == 0) initialization_failure();
    assert(native_blocks.empty());
    std::puts("PASS: heap scaling, native invariants and mirror safety");
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Guest semaphore ABI regressions through the production C dispatcher.
*/

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
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

static unsigned checks, failures, generated_calls;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x12345678;
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

static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }
static const M68KThunkEntry manual[] = { KAFFEINE_EXEC_COMPAT_THUNKS };
/* Literal offsets and registers are independent of the production macro. */
static const M68KThunkEntry generated[] =
{
    {540, poison}, {546, poison}, {558, poison}, {564, poison}, {570, poison},
    {576, poison}, {582, poison}, {588, poison}, {594, poison}, {600, poison},
    {606, poison}, {678, poison}, {720, poison}
};

struct Fixture
{
    static constexpr ULONG base = 0x1000, stack = 0xF00, task = 0x1400;
    static constexpr ULONG sem = 0x202, second = 0x302, list = 0x400;
    static constexpr ULONG initial_d0 = 0xD0123456;
    KaffeineContext ctx = {};
    std::array<UBYTE, 0x8000> memory;
    std::array<UBYTE, 128> native;
    TestCPU state;
    KaffeineCPU cpu = { &ops, &state };

    Fixture()
    {
        memory.fill(0xA5);
        native.fill(0xC7);
        ctx.mem = memory.data();
        ctx.mem_size = memory.size();
        ctx.m68k_sysbase = base;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = base;
        lib.jt_start = base - 6 * 512;
        std::strcpy(lib.name, "exec.library");
        lib.thunks = manual;
        lib.num_thunks = sizeof(manual) / sizeof(manual[0]);
        lib.gen_thunks = generated;
        lib.num_gen_thunks = sizeof(generated) / sizeof(generated[0]);
        m68k_write32(&ctx, base + 276, task);
        m68k_write32(&ctx, stack, 0x76543210);
        empty(base + 532);
        generated_calls = 0;
    }

    void empty(ULONG addr)
    {
        m68k_write32(&ctx, addr, addr + 4);
        m68k_write32(&ctx, addr + 4, 0);
        m68k_write32(&ctx, addr + 8, addr);
    }

    void initialized(ULONG addr)
    {
        /* An independent 68k SignalSemaphore, also used before testing Init. */
        memory[addr + 8] = 15;
        m68k_write16(&ctx, addr + 14, 0);
        empty(addr + 16);
        m68k_write32(&ctx, addr + 40, 0);
        m68k_write16(&ctx, addr + 44, 0xFFFF);
    }

    ULONG call(ULONG lvo, ULONG a0, ULONG a1 = 0xB1111111)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = initial_d0 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[7] = stack;
        m68k_write32(&ctx, stack, 0x76543210);
        const auto before = state;
        const auto native_before = native;
        ctx.running = TRUE;
        ctx.exit_code = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(native == native_before);
        return state.d[0];
    }

    bool counts(ULONG s, UWORD nest, ULONG owner)
    {
        return m68k_read16(&ctx, s + 14) == nest &&
               m68k_read32(&ctx, s + 40) == owner &&
               m68k_read16(&ctx, s + 44) == static_cast<UWORD>(nest - 1);
    }

    void two_nodes(ULONG header = list)
    {
        initialized(sem); initialized(second);
        m68k_write32(&ctx, header, sem);
        m68k_write32(&ctx, header + 4, 0);
        m68k_write32(&ctx, header + 8, second);
        m68k_write32(&ctx, sem, second);
        m68k_write32(&ctx, sem + 4, header);
        m68k_write32(&ctx, second, header + 4);
        m68k_write32(&ctx, second + 4, sem);
    }
};

static void test_shared_regression()
{
    Fixture f;
    f.initialized(f.sem);
    check(f.call(720, f.sem) == 1 && f.counts(f.sem, 1, 0),
          "-720 obtains a free shared semaphore and returns AROS TRUE (1)");
}

static void test_init_and_exclusive()
{
    Fixture f;
    auto expected = f.memory;
    f.initialized(f.sem);
    expected.swap(f.memory);
    check(f.call(558, f.sem) == f.initial_d0 && f.memory == expected,
          "-558 initializes only the AROS fields, preserving D0 and dirty reserved bytes");
    check(f.call(564, f.sem) == f.initial_d0 && f.counts(f.sem, 1, f.task),
          "-564 owns the semaphore as the ExecBase ThisTask shadow");
    check(f.call(576, f.sem) == 1 && f.counts(f.sem, 2, f.task),
          "-576 nests an exclusive acquisition and returns 1");
    check(f.call(678, f.sem) == f.initial_d0 && f.counts(f.sem, 3, f.task),
          "-678 on our exclusive lock preserves its owner");
    check(f.call(720, f.sem) == 1 && f.counts(f.sem, 4, f.task),
          "-720 nests on our exclusive lock without converting it to shared");
    for (UWORD nest = 3; nest != 0xFFFF; --nest)
        check(f.call(570, f.sem) == f.initial_d0 &&
              f.counts(f.sem, nest, nest ? f.task : 0),
              "-570 decrements nesting/queue counts and clears owner at zero");
    check(f.call(576, f.sem) == 1 && f.counts(f.sem, 1, f.task),
          "exclusive attempt also obtains a free semaphore");
    check(generated_calls == 0, "exclusive lifecycle never uses a generated fallback");
}

static void test_shared_and_busy()
{
    Fixture f;
    f.initialized(f.sem);
    check(f.call(678, f.sem) == f.initial_d0 && f.counts(f.sem, 1, 0),
          "shared obtain of a free semaphore has NULL owner");
    check(f.call(678, f.sem) == f.initial_d0 && f.counts(f.sem, 2, 0),
          "shared obtain increases the shared count");
    check(f.call(720, f.sem) == 1 && f.counts(f.sem, 3, 0),
          "shared attempt increases the shared count");
    const auto busy = f.memory;
    check(f.call(576, f.sem) == 0 && f.ctx.running && f.memory == busy,
          "exclusive attempt of a shared semaphore returns FALSE without writes");
    check(f.call(564, f.sem) == 20 && !f.ctx.running &&
          f.ctx.exit_code == 20 && f.memory == busy,
          "blocking shared-to-exclusive upgrade stops with rc 20 instead of waiting");
    for (UWORD nest = 2; nest != 0xFFFF; --nest)
        check(f.call(570, f.sem) == f.initial_d0 && f.counts(f.sem, nest, 0),
              "releasing shared locks returns counts to zero/-1");

    f.initialized(f.sem);
    m68k_write16(&f.ctx, f.sem + 14, 1);
    m68k_write16(&f.ctx, f.sem + 44, 0);
    m68k_write32(&f.ctx, f.sem + 40, 0xDEADBEEF);
    for (ULONG lvo : {576u, 720u, 564u, 678u, 570u})
    {
        const auto before = f.memory;
        bool attempt = lvo == 576 || lvo == 720;
        check(f.call(lvo, f.sem) == (attempt ? 0u : 20u) &&
              bool(f.ctx.running) == attempt && f.memory == before,
              "foreign owner fails attempts or stops void operations without writes");
    }
    check(generated_calls == 0, "shared and busy paths never use native fallbacks");
}

static void test_lists()
{
    Fixture f;
    f.two_nodes();
    const auto before = f.memory;
    check(f.call(582, f.list) == f.initial_d0 &&
          f.counts(f.sem, 1, f.task) && f.counts(f.second, 1, f.task),
          "-582 obtains every member of a guest List");
    check(f.call(582, f.list) == f.initial_d0 &&
          f.counts(f.sem, 2, f.task) && f.counts(f.second, 2, f.task),
          "list obtain nests locks already owned by ThisTask");
    check(f.call(588, f.list) == f.initial_d0 &&
          f.counts(f.sem, 1, f.task) && f.counts(f.second, 1, f.task),
          "-588 releases one acquisition from every member");
    check(f.call(588, f.list) == f.initial_d0 && f.memory == before,
          "list release to zero preserves links, reserved fields and all other memory");
    f.call(582, f.list);
    f.initialized(f.second);
    const auto partially_owned = f.memory;
    check(f.call(588, f.list) == 20 && !f.ctx.running && f.memory == partially_owned,
          "a bad second release leaves the already owned first member unchanged");
    f.empty(f.list);
    const auto empty = f.memory;
    check(f.call(582, f.list) == f.initial_d0 &&
          f.call(588, f.list) == f.initial_d0 && f.memory == empty,
          "both list operations accept an empty list");

    for (unsigned damage = 0; damage < 7; ++damage)
    {
        f.two_nodes();
        if (damage == 0) m68k_write32(&f.ctx, f.second, 0xFFFFFFFE);
        if (damage == 1) m68k_write32(&f.ctx, f.second, f.sem);
        if (damage == 2) m68k_write32(&f.ctx, f.second + 4, f.list);
        if (damage == 3) m68k_write32(&f.ctx, f.list + 8, f.sem);
        if (damage == 4) m68k_write16(&f.ctx, f.second + 14, 1);
        if (damage == 5)
        {
            m68k_write16(&f.ctx, f.second + 14, 1);
            m68k_write16(&f.ctx, f.second + 44, 0);
        }
        if (damage == 6) m68k_write32(&f.ctx, f.second + 16, f.sem);
        const auto snapshot = f.memory;
        check(f.call(582, f.list) == 20 && !f.ctx.running && f.memory == snapshot,
              "list preflight rejects malformed links, counts, waiters or upgrade atomically");
        check(f.call(588, f.list) == 20 && !f.ctx.running && f.memory == snapshot,
              "failed list release is atomic, including a free first member");
    }
    check(generated_calls == 0, "list calls never use generated native handlers");
}

static void test_public_list()
{
    Fixture f;
    const ULONG pub = f.base + 532, third = 0x502;
    const char *names[] = {"low", "same", "high"};
    const ULONG sems[] = {f.sem, f.second, third};
    const UBYTE priorities[] = {0x80, 0x80, 127};
    for (unsigned i = 0; i < 3; ++i)
    {
        std::strcpy(reinterpret_cast<char *>(f.memory.data() + 0x700 + 16*i), names[i]);
        m68k_write32(&f.ctx, sems[i] + 10, 0x700 + 16*i);
        f.memory[sems[i] + 9] = priorities[i];
        check(f.call(600, 0xFFFFFFFE, sems[i]) == f.initial_d0 && f.counts(sems[i], 0, 0),
              "-600 reads A1, initializes the semaphore and publishes it locally");
    }
    check(m68k_read32(&f.ctx, pub) == third &&
          m68k_read32(&f.ctx, third) == f.sem &&
          m68k_read32(&f.ctx, f.sem) == f.second &&
          m68k_read32(&f.ctx, f.second) == pub + 4 &&
          m68k_read32(&f.ctx, pub + 8) == f.second,
          "public list uses signed priorities and stable order for equal priorities");
    for (unsigned i = 0; i < 3; ++i)
        check(f.call(594, 0xFFFFFFFE, 0x700 + 16*i) == sems[i] &&
              f.call(594, 0xFFFFFFFE, 0x700 + 16*i) == sems[i],
              "-594 reads A1 and preserves guest semaphore identity on repeated lookup");
    const ULONG dup = 0x602;
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + 0x730), "same");
    m68k_write32(&f.ctx, dup + 10, 0x730);
    f.memory[dup + 9] = 0x80;
    check(f.call(600, 0xFFFFFFFE, dup) == f.initial_d0 &&
          f.call(594, 0xFFFFFFFE, 0x730) == f.second &&
          f.call(606, 0xFFFFFFFE, dup) == f.initial_d0,
          "-594 returns the first of two public semaphores with the same name, as FindName() does");
    check(f.call(582, pub) == f.initial_d0 && f.call(588, pub) == f.initial_d0,
          "public semaphore list is also a normal guest List for list acquisition");
    const auto duplicate = f.memory;
    check(f.call(600, 0, f.sem) == 20 && !f.ctx.running && f.memory == duplicate,
          "duplicate AddSemaphore is rejected before reinitializing a published node");
    for (ULONG s : {f.sem, third, f.second})
        check(f.call(606, 0xFFFFFFFE, s) == f.initial_d0,
              "-606 reads A1 and removes a middle, head or final public semaphore");
    check(m68k_read32(&f.ctx, pub) == pub + 4 &&
          m68k_read32(&f.ctx, pub + 8) == pub && f.call(594, 0, 0x700) == 0,
          "removing every semaphore restores the public list sentinels");
    check(generated_calls == 0, "public list calls never reach native Exec");
}

static void test_bad_inputs_and_async()
{
    for (ULONG lvo : {540u,546u,558u,564u,570u,576u,582u,588u,594u,600u,606u,678u,720u})
    {
        for (ULONG bad : {0u, 0x8000u, 0x7FFFu, 0xFFFFFFFEu, 0xFFFFFFFFu})
        {
            Fixture f;
            m68k_write32(&f.ctx, f.stack, 0x76543210);
            const auto snapshot = f.memory;
            const bool attempt = lvo == 576 || lvo == 720 || lvo == 594;
            check(f.call(lvo, bad, bad) == (attempt ? 0u : 20u) &&
                  bool(f.ctx.running) == attempt && f.memory == snapshot,
                  "every vector bounds-checks bad addresses or refuses asynchronous calls");
            check(generated_calls == 0, "bad input cannot reach a generated fallback");
        }
    }
    for (ULONG lvo : {540u,546u})
    {
        Fixture f;
        f.initialized(f.sem);
        m68k_write32(&f.ctx, f.stack, 0x76543210);
        const auto snapshot = f.memory;
        check(f.call(lvo, f.sem, f.second) == 20 && !f.ctx.running &&
              f.ctx.exit_code == 20 && f.memory == snapshot,
              "Procure/Vacate explicitly stop even with in-range semaphore/message bytes");
    }
    for (ULONG lvo : {558u,564u,570u,576u,678u,720u,600u,606u})
    {
        Fixture f;
        f.initialized(f.sem);
        f.ctx.num_shadows = 1;
        f.ctx.shadow_map[0] = {f.sem, reinterpret_cast<void *>(1), 0, 0 };
        m68k_write32(&f.ctx, f.stack, 0x76543210);
        const auto before = f.memory;
        const bool attempt = lvo == 576 || lvo == 720;
        check(f.call(lvo, f.sem, f.sem) == (attempt ? 0u : 20u) && f.memory == before,
              "registered native semaphore shadows are rejected without dereferencing native pointers");
    }
}

static void test_boundaries_and_corruption()
{
    Fixture f;
    const ULONG edge = f.memory.size() - 46;
    check(f.call(558, edge) == f.initial_d0 && f.call(576, edge) == 1 &&
          f.call(570, edge) == f.initial_d0 && f.counts(edge, 0, 0),
          "a complete word-aligned semaphore at the arena's exact end is usable");
    for (ULONG lvo : {558u,564u,570u,576u,678u,720u})
    {
        const auto snapshot = f.memory;
        bool attempt = lvo == 576 || lvo == 720;
        check(f.call(lvo, edge + 1) == (attempt ? 0u : 20u) && f.memory == snapshot,
              "a semaphore one byte beyond the arena is rejected before any field access");
    }
    for (unsigned damage = 0; damage < 6; ++damage)
    {
        f.initialized(f.sem);
        if (damage == 0) f.memory[f.sem + 8] = 0;
        if (damage == 1) m68k_write32(&f.ctx, f.sem + 20, 1);
        if (damage == 2) m68k_write32(&f.ctx, f.sem + 24, 0xFFFFFFFF);
        if (damage == 3) m68k_write16(&f.ctx, f.sem + 44, 0);
        if (damage == 4) m68k_write16(&f.ctx, f.sem + 14, 0x8000);
        if (damage == 5)
        {
            m68k_write16(&f.ctx, f.sem + 14, 32767);
            m68k_write16(&f.ctx, f.sem + 44, 32766);
            m68k_write32(&f.ctx, f.sem + 40, f.task);
        }
        const auto snapshot = f.memory;
        check(f.call(576, f.sem) == 0 && f.call(720, f.sem) == 0 &&
              f.memory == snapshot,
              "attempts reject corrupt state and count overflow without writes");
        check(f.call(564, f.sem) == 20 && !f.ctx.running && f.memory == snapshot,
              "blocking obtain rejects corrupt state or count overflow without writes");
    }
    f.initialized(f.sem);
    const auto free = f.memory;
    check(f.call(570, f.sem) == 20 && !f.ctx.running && f.memory == free,
          "release of a free semaphore fails without underflowing counts");

    for (ULONG bad_base : {0u, 0x8000u, 0xFFFFFFFEu})
    {
        f.ctx.m68k_sysbase = bad_base;
        check(f.call(576, f.sem) == 0 && f.call(720, f.sem) == 0,
              "attempts require an in-range ExecBase ThisTask field");
    }
}

static void test_list_limits_and_overlap()
{
    Fixture f;
    /* More than 256 nodes: exercise both the accepted boundary and the defined
       bounded failure, without a ring or malformed links. */
    constexpr ULONG first = 0x2000;
    auto chain = [&](unsigned count)
    {
        f.empty(f.list);
        for (unsigned i = 0; i < count; ++i)
        {
            ULONG s = first + i * 48;
            f.initialized(s);
            m68k_write32(&f.ctx, s, i + 1 < count ? s + 48 : f.list + 4);
            m68k_write32(&f.ctx, s + 4, i ? s - 48 : f.list);
        }
        m68k_write32(&f.ctx, f.list, first);
        m68k_write32(&f.ctx, f.list + 8, first + (count - 1) * 48);
    };
    chain(256);
    check(f.call(582, f.list) == f.initial_d0 && f.call(588, f.list) == f.initial_d0,
          "a list of exactly 256 semaphores can be obtained and released");
    chain(257);
    const auto over_limit = f.memory;
    check(f.call(582, f.list) == 20 && !f.ctx.running && f.memory == over_limit,
          "a longer valid list fails atomically with rc 20");
    f.two_nodes();
    /* A full semaphore cannot overlap the header, even when the links fit. */
    m68k_write32(&f.ctx, f.list, f.list + 8);
    m68k_write32(&f.ctx, f.list + 8, f.list + 4);
    m68k_write32(&f.ctx, f.list + 12, f.list);
    const auto overlap = f.memory;
    check(f.call(582, f.list) == 20 && f.memory == overlap,
          "a semaphore overlapping its list header is rejected without writes");

    f.initialized(first);
    f.initialized(first + 32);
    m68k_write32(&f.ctx, f.list, first);
    m68k_write32(&f.ctx, f.list + 8, first + 32);
    m68k_write32(&f.ctx, first, first + 32);
    m68k_write32(&f.ctx, first + 4, f.list);
    m68k_write32(&f.ctx, first + 32, f.list + 4);
    m68k_write32(&f.ctx, first + 36, first);
    const auto overlapping_members = f.memory;
    check(f.call(582, f.list) == 20 && f.call(588, f.list) == 20 &&
          f.memory == overlapping_members,
          "overlapping semaphore members with valid node links are rejected atomically");
}

static void test_public_invalid_and_isolation()
{
    Fixture f;
    std::strcpy(reinterpret_cast<char *>(f.memory.data() + 0x700), "private");
    m68k_write32(&f.ctx, f.sem + 10, 0x700);
    check(f.call(600, 0, f.sem) == f.initial_d0, "publish one context-local semaphore");
    Fixture other;
    std::strcpy(reinterpret_cast<char *>(other.memory.data() + 0x700), "private");
    check(other.call(594, 0, 0x700) == 0 && f.call(594, 0, 0x700) == f.sem,
          "public guest semaphores are isolated between contexts");
    m68k_write32(&f.ctx, f.sem + 10, f.memory.size() - 1);
    const auto invalid_name = f.memory;
    check(f.call(594, 0, 0x700) == 0 && f.memory == invalid_name,
          "an unterminated published name never escapes the arena during lookup");
    check(f.call(600, 0, f.second) == 20 && f.memory == invalid_name,
          "AddSemaphore rejects an invalid name before initialization");
    check(f.call(606, 0, f.second) == 20 && f.memory == invalid_name,
          "RemSemaphore rejects a node absent from the public list atomically");
    f.ctx.m68k_sysbase = 0xFFFFFFFE;
    check(f.call(594, 0, 0x700) == 0 && f.call(600, 0, f.sem) == 20 &&
          f.call(606, 0, f.sem) == 20 && f.memory == invalid_name,
          "public list calls check the ExecBase field range before deriving its address");
}

static Fixture *executing;
static unsigned slices;
static void stop_slice(void *)
{
    assert(++slices == 1);
    executing->call(540, executing->sem, executing->second);
}

static void test_failure_reaches_run_loop()
{
    Fixture f;
    const KaffeineCPUOps run_ops =
        {get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, stop_slice, nullptr};
    f.cpu.ops = &run_ops;
    executing = &f;
    slices = 0;
    check(Kaffeine_RunCPU(&f.ctx, &f.cpu) == 20 && slices == 1 && !f.ctx.running &&
          f.ctx.exit_code == 20,
          "unsupported semaphore call stops the real C run loop after one slice with rc 20");
}

static void test_fallback_controls()
{
    Fixture f;
    f.ctx.libs[0].thunks = nullptr;
    f.ctx.libs[0].num_thunks = 0;
    for (const auto &entry : generated)
        check(f.call(entry.lvo, f.sem, f.second) == 0xBAD0F00D,
              "poisoned generated fallback is reachable without manual overrides");
    check(generated_calls == 13, "all thirteen poison controls were exercised");
}

int main()
{
    test_shared_regression();
    test_init_and_exclusive();
    test_shared_and_busy();
    test_lists();
    test_public_list();
    test_bad_inputs_and_async();
    test_boundaries_and_corruption();
    test_list_limits_and_overlap();
    test_public_invalid_and_isolation();
    test_failure_reaches_run_loop();
    test_fallback_controls();
    std::printf("Guest semaphore regressions: %u/%u checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}

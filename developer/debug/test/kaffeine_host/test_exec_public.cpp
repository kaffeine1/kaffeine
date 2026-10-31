/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Public guest ports and refused native objects through literal Exec LVOs.
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
#include "kaffeine_rastport.h"

extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    assert(false);
    return 0;
}

static unsigned putmsg_calls;
static void *putmsg_port, *putmsg_message;
extern "C" void PutMsg(struct MsgPort *port, struct Message *message)
{
    ++putmsg_calls;
    putmsg_port = port;
    putmsg_message = message;
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

/* A backend that keeps translations, and what the cache thunks asked it. */
static unsigned flushes;
static ULONG flush_addr, flush_size;
static void flush_code(void *, ULONG addr, ULONG size)
{
    ++flushes;
    flush_addr = addr;
    flush_size = size;
}
static const KaffeineCPUOps flushing_ops =
    { get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, nullptr, flush_code };

static IPTR poison(KaffeineContext *, void *) { ++generated_calls; return 0xBAD0F00D; }
static const M68KThunkEntry manual[] = { KAFFEINE_EXEC_COMPAT_THUNKS };
static const M68KThunkEntry generated[] =
{
    {96, poison}, {282, poison}, {288, poison}, {354, poison}, {360, poison}, {366, poison}, {390, poison},
    {486, poison}, {492, poison}, {498, poison}, {636, poison}, {642, poison}
};

struct Fixture
{
    static constexpr ULONG base = 0x1000, stack = 0xF00, list = base + 392;
    static constexpr ULONG port = 0x202, second = 0x302, third = 0x402;
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
        lib.num_thunks = std::size(manual);
        lib.gen_thunks = generated;
        lib.num_gen_thunks = std::size(generated);
        m68k_write32(&ctx, stack, 0x76543210);
        empty(list);
        named(port, "same", -100);
        named(second, "same", 100);
        named(third, "third", 100);
    }

    void empty(ULONG addr)
    {
        m68k_write32(&ctx, addr, addr + 4);
        m68k_write32(&ctx, addr + 4, 0);
        m68k_write32(&ctx, addr + 8, addr);
    }
    void named(ULONG addr, const char *name, int priority)
    {
        const ULONG text = addr + 40;
        std::strcpy(reinterpret_cast<char *>(memory.data() + text), name);
        m68k_write32(&ctx, addr + 10, text);
        memory[addr + 9] = static_cast<UBYTE>(priority);
    }
    ULONG call(ULONG lvo, ULONG a1, ULONG a0 = 0xFFFFFFFE,
               ULONG d0 = initial_d0, ULONG d1 = 0xD1CACE, bool fallback = false)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            state.d[i] = initial_d0 + i;
            state.a[i] = 0xA0000000 + i;
        }
        state.d[0] = d0; state.d[1] = d1;
        state.a[0] = a0; state.a[1] = a1; state.a[7] = stack;
        const auto before = state;
        const auto native_before = native;
        ctx.running = TRUE; ctx.exit_code = 0;
        generated_calls = 0;
        KaffeineDispatchResult result = {};
        assert(Kaffeine_DispatchLibraryCall(&ctx, &cpu, base - lvo, &result));
        assert(result.return_pc == 0x76543210 && !result.supervisor);
        assert(state.a[7] == stack + 4);
        for (unsigned i = 1; i < 8; ++i) assert(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) assert(state.a[i] == before.a[i]);
        assert(state.sr == before.sr && state.pc == before.pc);
        assert(native == native_before);
        assert(generated_calls == (fallback ? 1U : 0U));
        return state.d[0];
    }
    ULONG find(const char *name)
    {
        std::strcpy(reinterpret_cast<char *>(memory.data() + 0x700), name);
        return call(390, 0x700);
    }
};

static void test_lifecycle()
{
    Fixture f;
    auto expected = f.memory;
    expected[f.port + 8] = 4;
    auto put = [&](ULONG addr, ULONG value) {
        for (unsigned i = 0; i < 4; ++i) expected[addr + i] = value >> (24 - 8 * i);
    };
    put(f.port + 20, f.port + 24); put(f.port + 24, 0); put(f.port + 28, f.port + 20);
    put(f.list, f.port); put(f.list + 8, f.port);
    put(f.port, f.list + 4); put(f.port + 4, f.list);
    check(f.call(354, f.port) == f.initial_d0 && f.memory == expected,
          "AddPort initializes only the node type, message links and public links");
    check(f.call(354, f.second) == f.initial_d0 && f.call(354, f.third) == f.initial_d0 &&
          m68k_read32(&f.ctx, f.list) == f.second &&
          m68k_read32(&f.ctx, f.second) == f.third &&
          m68k_read32(&f.ctx, f.third) == f.port && f.find("same") == f.second,
          "signed priority and stable equal priority select the first same-name port");
    const auto before_remove = f.memory;
    check(f.call(360, f.third) == f.initial_d0 &&
          m68k_read32(&f.ctx, f.second) == f.port &&
          !std::memcmp(before_remove.data() + f.third, f.memory.data() + f.third, 34),
          "RemPort unlinks a member and preserves its own fields and stale links");
    check(f.call(360, f.second) == f.initial_d0 && f.find("same") == f.port &&
          f.call(360, f.port) == f.initial_d0 && f.find("same") == 0 &&
          m68k_read32(&f.ctx, f.list) == f.list + 4 &&
          m68k_read32(&f.ctx, f.list + 8) == f.list,
          "head and final removals restore the empty guest public list");
    check(f.call(354, f.port) == f.initial_d0 && f.find("same") == f.port,
          "a removed port can be added again");
    Fixture other;
    check(other.find("same") == 0, "public ports are isolated between guest contexts");
    f.ctx.shadow_map[0].m68k_addr = f.second;
    f.ctx.shadow_map[0].native_ptr = f.native.data(); f.ctx.num_shadows = 1;
    check(f.find("third") == 0 && f.find("REXX") == 0,
          "native-only port names and shadow objects are invisible to FindPort");
}

static void test_bad_lists()
{
    for (unsigned damage = 0; damage < 12; ++damage)
    {
        Fixture f;
        assert(f.call(354, f.port) == f.initial_d0);
        assert(f.call(354, f.second) == f.initial_d0);
        if (damage == 0) m68k_write32(&f.ctx, f.list + 4, 1);
        if (damage == 1) m68k_write32(&f.ctx, f.second + 4, f.port);
        if (damage == 2) m68k_write32(&f.ctx, f.port, f.second);
        if (damage == 3) m68k_write32(&f.ctx, f.port, 0xFFFFFFFE);
        if (damage == 4) m68k_write32(&f.ctx, f.list + 8, f.second);
        if (damage == 5) m68k_write32(&f.ctx, f.list, f.memory.size() - 33);
        if (damage == 6) f.ctx.m68k_sysbase = 0xFFFFFFFE;
        if (damage == 7) f.ctx.m68k_sysbase = 0;
        if (damage == 8)
        {
            m68k_write32(&f.ctx, f.list, f.list + 8);
            m68k_write32(&f.ctx, f.list + 8, f.list + 4);
            m68k_write32(&f.ctx, f.list + 12, f.list);
        }
        if (damage == 9)
        {
            f.ctx.shadow_map[0].m68k_addr = f.second;
            f.ctx.shadow_map[0].native_ptr = f.native.data();
            f.ctx.num_shadows = 1;
        }
        if (damage == 10) f.ctx.m68k_sysbase = f.ctx.mem_size - 392 - 13;
        if (damage == 11)
        {
            const ULONG node = 0x2000;
            m68k_write32(&f.ctx, f.list, node);
            m68k_write32(&f.ctx, f.list + 8, node + 16);
            m68k_write32(&f.ctx, node, node + 16);
            m68k_write32(&f.ctx, node + 4, f.list);
            m68k_write32(&f.ctx, node + 16, f.list + 4);
            m68k_write32(&f.ctx, node + 20, node);
        }
        const auto snapshot = f.memory;
        check(f.call(390, f.port + 40) == 0 && f.memory == snapshot,
              "FindPort rejects the entire malformed list, even with a matching first name");
        check(f.call(354, f.third) == 20 && !f.ctx.running && f.ctx.exit_code == 20 &&
              f.memory == snapshot, "AddPort stops on a malformed list before writing");
        check(f.call(360, f.port) == 20 && !f.ctx.running && f.memory == snapshot,
              "RemPort stops on a malformed list before writing");
    }
}

static void test_bad_objects_and_names()
{
    Fixture f;
    assert(f.call(354, f.port) == f.initial_d0);
    for (ULONG addr : { 0u, 0xFFFFFFFEu, f.ctx.mem_size, f.ctx.mem_size - 33,
                        f.port, f.port + 16, f.list })
    {
        const auto snapshot = f.memory;
        check(f.call(354, addr) == 20 && !f.ctx.running && f.memory == snapshot,
              "AddPort refuses NULL, outside, duplicate and overlapping objects atomically");
    }
    for (ULONG addr : { 0u, 0xFFFFFFFEu, f.second })
    {
        const auto snapshot = f.memory;
        check(f.call(360, addr) == 20 && !f.ctx.running && f.memory == snapshot,
              "RemPort refuses foreign nodes without following their links");
    }
    assert(f.call(360, f.port) == f.initial_d0);
    const auto removed = f.memory;
    check(f.call(360, f.port) == 20 && f.memory == removed,
          "repeated removal leaves stale links untouched");
    f.ctx.shadow_map[0].m68k_addr = f.port;
    f.ctx.shadow_map[0].native_ptr = f.native.data(); f.ctx.num_shadows = 1;
    check(f.call(354, f.port) == 20 && f.memory == removed,
          "native port shadows cannot be published as guest ports");
    f.ctx.num_shadows = 0;
    const ULONG end = f.ctx.mem_size;
    f.memory[end - 2] = 'x'; f.memory[end - 1] = 0;
    m68k_write32(&f.ctx, f.port + 10, end - 2);
    check(f.call(354, f.port) == f.initial_d0 && f.call(390, end - 2) == f.port,
          "port names may terminate in the final byte of guest memory");
    f.memory[end - 1] = 'y';
    const auto unterminated = f.memory;
    check(f.call(390, end - 2) == 0 && f.memory == unterminated,
          "lookup rejects a name whose terminator would be one byte past memory");
    check(f.call(390, f.second + 40) == 0 && f.memory == unterminated,
          "lookup also validates every published name");
    check(f.call(354, f.second) == 20 && f.memory == unterminated,
          "insertion validates existing names before changing the list");
    assert(f.call(360, f.port) == f.initial_d0);
    check(f.call(354, f.port) == 20, "an unterminated new port name is refused");
    for (ULONG name : { 0u, end, 0xFFFFFFFFu })
    {
        m68k_write32(&f.ctx, f.port + 10, name);
        const auto snapshot = f.memory;
        check(f.call(354, f.port) == 20 && f.call(390, name) == 0 && f.memory == snapshot,
              "NULL, end and wrapping string pointers never reach native Exec");
    }
    Fixture boundary;
    const ULONG last_port = boundary.ctx.mem_size - 34;
    m68k_write32(&boundary.ctx, last_port + 10, boundary.second + 40);
    check(boundary.call(354, last_port) == boundary.initial_d0 &&
          boundary.call(390, boundary.second + 40) == last_port &&
          boundary.call(360, last_port) == boundary.initial_d0,
          "a whole MsgPort ending exactly at the arena boundary is accepted");
}

static void test_list_limit()
{
    Fixture f;
    const ULONG first = 0x2000;
    for (unsigned i = 0; i < 257; ++i)
    {
        const ULONG node = first + i * 48;
        m68k_write32(&f.ctx, node, i == 256 ? f.list + 4 : node + 48);
        m68k_write32(&f.ctx, node + 4, i ? node - 48 : f.list);
        m68k_write32(&f.ctx, node + 10, f.port + 40);
    }
    m68k_write32(&f.ctx, f.list, first);
    m68k_write32(&f.ctx, f.list + 8, first + 256 * 48);
    const auto snapshot = f.memory;
    check(f.call(390, f.port + 40) == 0 && f.call(354, f.port) == 20 &&
          f.call(360, first) == 20 && f.memory == snapshot,
          "a valid 257-node list is refused atomically at the bounded walk limit");
    m68k_write32(&f.ctx, first + 255 * 48, f.list + 4);
    m68k_write32(&f.ctx, f.list + 8, first + 255 * 48);
    const auto full = f.memory;
    check(f.call(354, f.port) == 20 && f.memory == full,
          "insertion refuses an already full 256-node list without initialization");
    check(f.call(390, f.port + 40) == first && f.call(360, first) == f.initial_d0 &&
          f.call(354, f.port) == f.initial_d0,
          "exactly 256 public ports are accepted");
}

static void test_refused_objects_and_cache()
{
    Fixture f;
    const ULONG end = f.ctx.mem_size;
    f.memory[end - 2] = 'x'; f.memory[end - 1] = 0;
    for (ULONG name : { f.port + 40, end - 2, end - 1, 0u, end, 0xFFFFFFFFu })
    {
        const auto snapshot = f.memory;
        check(f.call(96, name) == 0 && f.call(498, name) == 0 &&
              f.memory == snapshot && f.ctx.running,
              "FindResident and OpenResource refuse native objects for bounded or invalid names");
    }
    f.memory[end - 1] = 'y';
    check(f.call(96, end - 2) == 0 && f.call(498, end - 2) == 0,
          "resident and resource names cannot terminate one byte beyond the arena");
    for (ULONG addr : { 0u, f.port, end, 0xFFFFFFFEu })
    {
        const auto snapshot = f.memory;
        check(f.call(486, addr) == f.initial_d0 && f.call(492, addr) == f.initial_d0 &&
              f.memory == snapshot, "AddResource and RemResource ignore all pointers and preserve D0");
    }
    const ULONG ranges[][2] = { {f.port, 16}, {end - 4, 4}, {end - 4, 5},
        {0, 0}, {end, 0}, {0xFFFFFFFEu, 8}, {f.port, 0xFFFFFFFFu} };
    for (const auto &range : ranges)
    {
        const auto snapshot = f.memory;
        check(f.call(642, 0xA1BAD, range[0], range[1], 0xFFFFFFFFu) == range[1] &&
              f.ctx.running && f.memory == snapshot,
              "CacheClearE checks its A0/D0 range and preserves all registers without a native call");
    }
}

/* CacheClearU() and CacheClearE() with CACRF_ClearI tell the backend which
   guest code may have changed; CacheClearE() names its range unless that is
   all addresses (length ~0) or outside guest memory. */
static void test_cache_flush()
{
    const ULONG clear_i = 0x8, clear_d = 0x800;
    Fixture f;
    const ULONG end = f.ctx.mem_size;
    f.cpu.ops = &flushing_ops;

    const auto snapshot = f.memory;
    flushes = 0;
    check(f.call(636, 0xA1BAD) == Fixture::initial_d0 && flushes == 1 && flush_addr == 0 &&
          flush_size == KAFFEINE_FLUSH_ALL && f.memory == snapshot,
          "CacheClearU flushes all guest code and preserves D0 and every register");
    struct { ULONG addr, size, caches, flush_addr, flush_size; unsigned flushes; } cases[] = {
        { f.port, 16, clear_i, f.port, 16, 1 },
        { f.port, 16, clear_i | clear_d, f.port, 16, 1 },
        { end - 4, 4, 0xFFFFFFFFu, end - 4, 4, 1 },
        { f.port, 16, clear_d, 0, 0, 0 },
        { f.port, 16, 0, 0, 0, 0 },
        { f.port, 0xFFFFFFFFu, clear_i, 0, KAFFEINE_FLUSH_ALL, 1 },
        { 0, 0xFFFFFFFFu, clear_i, 0, KAFFEINE_FLUSH_ALL, 1 },
        { end - 4, 5, clear_i, 0, KAFFEINE_FLUSH_ALL, 1 },
        { 0, 16, clear_i, 0, KAFFEINE_FLUSH_ALL, 1 },
        { 0xFFFFFFFEu, 8, clear_i, 0, KAFFEINE_FLUSH_ALL, 1 },
    };
    for (const auto &c : cases)
    {
        flushes = 0; flush_addr = flush_size = 0x5A5A5A5A;
        check(f.call(642, 0xA1BAD, c.addr, c.size, c.caches) == c.size &&
              flushes == c.flushes && f.memory == snapshot &&
              (!c.flushes || (flush_addr == c.flush_addr && flush_size == c.flush_size)),
              "CacheClearE flushes its range for CACRF_ClearI, all guest code for ~0 or a bad range");
    }
    /* A backend without flush_code, as Moira: the same results. */
    f.cpu.ops = &ops;
    check(f.call(636, 0xA1BAD) == Fixture::initial_d0 &&
          f.call(642, 0xA1BAD, f.port, 16, clear_i) == 16 && f.memory == snapshot,
          "CacheClearU and CacheClearE need no flush_code");
}

static void test_putmsg()
{
    Fixture f;
    const ULONG message = 0x600, native_port = 0x640, native_message = 0x680;
    assert(f.call(354, f.port) == f.initial_d0);
    const ULONG found = f.find("same");
    assert(found == f.port);
    /* A fully initialized soft-interrupt port remains outside the supported
       guest PA_IGNORE/PA_SIGNAL contract. Keep refusal coverage meaningful. */
    f.memory[f.port + 14] = 1;
    m68k_write32(&f.ctx, f.port + 16, 0x740);
    std::memset(f.memory.data() + message, 0, 24);
    f.memory[message + 8] = 5;
    m68k_write16(&f.ctx, message + 18, 24);
    f.ctx.shadow_map[0].m68k_addr = native_port;
    f.ctx.shadow_map[0].native_ptr = f.native.data();
    f.ctx.shadow_map[0].type = KAFFEINE_SHADOW_USERPORT;
    f.ctx.shadow_map[1].m68k_addr = native_message;
    f.ctx.shadow_map[1].native_ptr = f.native.data() + 64;
    f.ctx.shadow_map[1].type = KAFFEINE_SHADOW_INTUIMESSAGE;
    f.ctx.num_shadows = 2;
    const ULONG refused[][2] = { {found, message}, {found, native_message},
        {native_port, message}, {0, native_message}, {native_port, 0},
        {native_port + 2, native_message}, {native_port, native_message + 2} };
    for (const auto &pair : refused)
    {
        const auto snapshot = f.memory;
        putmsg_calls = 0;
        check(f.call(366, pair[1], pair[0]) == 20 && !f.ctx.running &&
              f.ctx.exit_code == 20 && f.memory == snapshot && !putmsg_calls,
              "PutMsg stops before native Exec sees a guest port or message");
    }
    const auto snapshot = f.memory;
    putmsg_calls = 0;
    check(f.call(366, native_message, native_port) == f.initial_d0 && f.ctx.running &&
          putmsg_calls == 1 && putmsg_port == f.native.data() &&
          putmsg_message == f.native.data() + 64 && f.memory == snapshot,
          "PutMsg forwards a native message to a native port and preserves D0");

    const UWORD wrong_types[][2] = {
        {0, KAFFEINE_SHADOW_INTUIMESSAGE},
        {KAFFEINE_SHADOW_WINDOW, KAFFEINE_SHADOW_INTUIMESSAGE},
        {KAFFEINE_SHADOW_SCREEN, KAFFEINE_SHADOW_INTUIMESSAGE},
        {258, KAFFEINE_SHADOW_INTUIMESSAGE},
        {KAFFEINE_SHADOW_USERPORT, 0},
        {KAFFEINE_SHADOW_USERPORT, KAFFEINE_SHADOW_WINDOW},
        {KAFFEINE_SHADOW_USERPORT, 258}
    };
    for (const auto &types : wrong_types)
    {
        f.ctx.shadow_map[0].type = types[0];
        f.ctx.shadow_map[1].type = types[1];
        putmsg_calls = 0;
        check(f.call(366, native_message, native_port) == 20 && !f.ctx.running &&
              f.ctx.exit_code == 20 && putmsg_calls == 0 && f.memory == snapshot &&
              f.ctx.shadow_map[0].type == types[0] && f.ctx.shadow_map[1].type == types[1],
              "PutMsg rejects exact native shadows of the wrong object kinds before native Exec");
    }
    f.ctx.shadow_map[0].type = KAFFEINE_SHADOW_USERPORT;

    /* An IntuiMessage that the program took is in a port again once sent:
       it becomes one that WaitPort() shows, which the program may not send
       again, as it may not send a GadTools copy. */
    f.ctx.shadow_map[1].type = KAFFEINE_SHADOW_INTUIMESSAGE;
    putmsg_calls = 0;
    check(f.call(366, native_message, native_port) == f.initial_d0 && f.ctx.running &&
          putmsg_calls == 1 && f.ctx.shadow_map[1].type == KAFFEINE_SHADOW_WAITMESSAGE,
          "PutMsg of an IntuiMessage that the program took leaves it to the port");
    for (UWORD type : { KAFFEINE_SHADOW_WAITMESSAGE, KAFFEINE_SHADOW_GTMESSAGE })
    {
        f.ctx.shadow_map[1].type = type;
        putmsg_calls = 0;
        check(f.call(366, native_message, native_port) == 20 && !f.ctx.running &&
              f.ctx.exit_code == 20 && !putmsg_calls && f.ctx.shadow_map[1].type == type,
              "PutMsg stops at an IntuiMessage still in a port and at a GadTools copy");
    }
}

static void test_fallbacks()
{
    Fixture f;
    f.ctx.libs[0].thunks = nullptr; f.ctx.libs[0].num_thunks = 0;
    for (const auto &entry : generated)
        check(f.call(entry.lvo, f.port, f.second, 7, 3, true) == 0xBAD0F00D,
              "each poisoned generated fallback is reachable without its manual override");
}

static void test_task_launch_refusal()
{
    for (ULONG lvo : { 282u, 288u })
        for (ULONG address : { 0u, 0x202u, 0xFFFFFFF0u })
        {
            Fixture f;
            auto before = f.memory;
            check(f.call(lvo, address) == 20 && !f.ctx.running && f.ctx.exit_code == 20,
                  "unsupported task operation stops before native scheduling");
            check(f.memory == before && !generated_calls,
                  "refused task operations preserve task bytes and bypass generated fallback");
        }
}

int main()
{
    test_task_launch_refusal();
    test_lifecycle(); test_bad_lists(); test_bad_objects_and_names();
    test_list_limit(); test_refused_objects_and_cache(); test_cache_flush(); test_putmsg();
    test_fallbacks();
    std::printf("Exec public objects: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

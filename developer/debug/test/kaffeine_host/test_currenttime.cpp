/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 *
 * The manual Intuition CurrentTime() thunk, extracted from production by
 * currenttime_extract.py, called through the real dispatcher with the
 * production guest boundary and a recording native CurrentTime().
 */

/* Keep checks and the calls they exercise active with release build flags. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <aros/debug.h>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"

extern "C" {
extern const M68KThunkEntry currenttime_manual[1];
extern const M68KThunkEntry currenttime_generated[1];
IPTR currenttime_generated_thunk(KaffeineContext *ctx, void *cpu);
void currenttime_debug(const char *format, ...);
void CurrentTime(ULONG *seconds, ULONG *micros);
BOOL Kaffeine_LockProtected(KaffeineContext *ctx, ULONG addr, ULONG size);
}

constexpr ULONG MEM = 0x400, BASE = 0x1000, SP = 0x80, RET = 0x00012340;
constexpr UBYTE SENTINEL = 0xA5;
/* Not palindromes, so a byte-swapped or misplaced store cannot pass. */
constexpr ULONG NATIVE_SECONDS = 0x01020304, NATIVE_MICROS = 0x00071A2B;
/* A 20-byte bridge structure (shadow type 0), video and lock ranges. */
constexpr ULONG SHADOW = 0x200, VIDEO = 0x300, VIDEO_SIZE = 16, LOCK = 0x340, LOCK_SIZE = 8;

struct TestCPU
{
    ULONG d[8];
    ULONG a[8];
    UWORD sr;
    ULONG pc;
};

static ULONG test_get_d(void *state, int reg) { return static_cast<TestCPU *>(state)->d[reg]; }
static ULONG test_get_a(void *state, int reg) { return static_cast<TestCPU *>(state)->a[reg]; }
static void test_set_d(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->d[reg] = value; }
static void test_set_a(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->a[reg] = value; }
static UWORD test_get_sr(void *state) { return static_cast<TestCPU *>(state)->sr; }
static void test_set_sr(void *state, UWORD value) { static_cast<TestCPU *>(state)->sr = value; }
static ULONG test_get_pc(void *state) { return static_cast<TestCPU *>(state)->pc; }
static void test_set_pc(void *state, ULONG value) { static_cast<TestCPU *>(state)->pc = value; }

static const KaffeineCPUOps test_ops =
{
    test_get_d, test_get_a, test_set_d, test_set_a,
    test_get_sr, test_set_sr, test_get_pc, test_set_pc, nullptr, nullptr
};

static bool overlaps(ULONG a, ULONG n, ULONG b, ULONG bn)
{
    return std::uint64_t(a) < std::uint64_t(b) + bn && std::uint64_t(b) < std::uint64_t(a) + n;
}

static BOOL video_protected(KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return overlaps(addr, size, VIDEO, VIDEO_SIZE);
}

/* Overrides the weak lock collaborator of message_native_unused.o. */
extern "C" BOOL Kaffeine_LockProtected(KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return overlaps(addr, size, LOCK, LOCK_SIZE);
}

struct Fixture;
static Fixture *live;
static unsigned native_calls, generated_calls, cases;
static char refusal_log[256];

extern "C" void currenttime_debug(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(refusal_log, sizeof(refusal_log), format, args);
    va_end(args);
}

/* Only reachable when the manual row is missing. */
extern "C" IPTR currenttime_generated_thunk(KaffeineContext *ctx, void *cpu)
{
    generated_calls++;
    return 0x5A5A5A5A;
}

struct Fixture
{
    KaffeineContext ctx = {};
    UBYTE memory[MEM] = {};
    UBYTE before[MEM] = {};
    TestCPU state = {};
    KaffeineCPU cpu = { &test_ops, &state };

    explicit Fixture(bool manual = true)
    {
        ctx.mem = memory;
        ctx.mem_size = MEM;
        ctx.running = TRUE;
        ctx.video_protected = video_protected;
        ctx.num_shadows = 1;
        ctx.shadow_map[0].m68k_addr = SHADOW;
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.jt_start = BASE - 0x100;
        lib.m68k_addr = BASE;
        std::strcpy(lib.name, "intuition.library");
        lib.thunks = manual ? currenttime_manual : nullptr;
        lib.num_thunks = manual ? 1 : 0;
        lib.gen_thunks = currenttime_generated;
        lib.num_gen_thunks = 1;
        std::memset(memory, SENTINEL, sizeof(memory));
        m68k_write32(&ctx, SP, RET);
        std::memcpy(before, memory, sizeof(memory));
        for (int i = 0; i < 8; ++i)
        {
            state.d[i] = 0x11110000u + i;
            state.a[i] = 0x22220000u + i;
        }
        state.d[0] = 0xDEADBEEF;
        state.a[7] = SP;
        native_calls = generated_calls = 0;
        refusal_log[0] = 0;
        live = this;
        cases++;
    }
};

static bool in_guest(const void *p)
{
    auto at = reinterpret_cast<std::uintptr_t>(p);
    auto mem = reinterpret_cast<std::uintptr_t>(live->memory);
    return at + sizeof(ULONG) > mem && at < mem + MEM;
}

/* Native CurrentTime() stores host-endian ULONGs. The thunk must give it
   two host temporaries, and no guest byte may change before it returns. */
extern "C" void CurrentTime(ULONG *seconds, ULONG *micros)
{
    native_calls++;
    assert(seconds && micros && seconds != micros);
    assert(!in_guest(seconds) && !in_guest(micros));
    assert(std::memcmp(live->memory, live->before, MEM) == 0);
    *seconds = NATIVE_SECONDS;
    *micros = NATIVE_MICROS;
}

static void put_be(UBYTE *memory, ULONG addr, ULONG value)
{
    for (int i = 0; i < 4; ++i)
        memory[addr + i] = UBYTE(value >> (24 - 8 * i));
}

static void dispatch(Fixture &f, ULONG seconds, ULONG micros)
{
    KaffeineDispatchResult result = {};
    f.state.a[0] = seconds;
    f.state.a[1] = micros;
    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, BASE - 84, &result));
    assert(result.return_pc == RET && !result.supervisor);
    assert(f.state.a[7] == SP + 4);
    assert(f.state.a[0] == seconds && f.state.a[1] == micros);
    for (int i = 1; i < 8; ++i)
        assert(f.state.d[i] == 0x11110000u + i);
    for (int i = 2; i < 7; ++i)
        assert(f.state.a[i] == 0x22220000u + i);
}

static void expect_success(ULONG seconds, ULONG micros)
{
    Fixture f;
    UBYTE expected[MEM];
    std::memcpy(expected, f.memory, MEM);
    /* What a guest call leaves: seconds first, micros over shared bytes. */
    if (seconds) put_be(expected, seconds, NATIVE_SECONDS);
    if (micros) put_be(expected, micros, NATIVE_MICROS);
    dispatch(f, seconds, micros);
    assert(native_calls == 1 && generated_calls == 0);
    assert(f.state.d[0] == 0xDEADBEEF);
    assert(f.ctx.running && f.ctx.exit_code == 0 && !refusal_log[0]);
    assert(std::memcmp(f.memory, expected, MEM) == 0);
}

static void expect_refusal(ULONG seconds, ULONG micros)
{
    Fixture f;
    dispatch(f, seconds, micros);
    assert(native_calls == 0 && generated_calls == 0);
    assert(f.state.d[0] == 20);
    assert(!f.ctx.running && f.ctx.exit_code == 20);
    assert(std::strcmp(refusal_log, "[kaffeine] CurrentTime(): invalid guest output, stopping (20)\n") == 0);
    /* All or nothing: the valid destination is not written either. */
    assert(std::memcmp(f.memory, f.before, MEM) == 0);
}

static void test_registrations()
{
    assert(currenttime_manual[0].lvo == 84 && currenttime_generated[0].lvo == 84);
    assert(currenttime_generated[0].thunk == currenttime_generated_thunk);
    assert(currenttime_manual[0].thunk != currenttime_generated_thunk);

    /* Without the manual row the dispatcher reaches the generated one. */
    Fixture f(false);
    KaffeineDispatchResult result = {};
    f.state.a[0] = 0x100;
    f.state.a[1] = 0x110;
    assert(Kaffeine_DispatchLibraryCall(&f.ctx, &f.cpu, BASE - 84, &result));
    assert(generated_calls == 1 && native_calls == 0 && f.state.d[0] == 0x5A5A5A5A);
}

static void test_outputs()
{
    expect_success(0x100, 0x110);
    expect_success(0x100, 0);
    expect_success(0, 0x110);
    expect_success(0, 0);
    expect_success(0x120, 0x120);
    expect_success(0x130, 0x132);
    expect_success(0x142, 0x140);
    expect_success(MEM - 4, MEM - 8);
    expect_success(SHADOW - 4, SHADOW + 20);
    expect_success(VIDEO - 4, LOCK + LOCK_SIZE);
}

static void test_refusals()
{
    expect_refusal(MEM - 3, 0x110);
    expect_refusal(0x100, MEM - 3);
    expect_refusal(MEM, 0x110);
    expect_refusal(0xFFFFFFFE, 0x110);
    expect_refusal(0x100, 0xFFFFFFFC);
    expect_refusal(SHADOW - 3, 0x110);
    expect_refusal(0x100, SHADOW + 19);
    expect_refusal(0x100, VIDEO + VIDEO_SIZE - 1);
    expect_refusal(LOCK - 3, 0x110);
    expect_refusal(MEM - 1, 0xFFFFFFFF);
}

int main()
{
    test_registrations();
    test_outputs();
    test_refusals();
    std::printf("PASS: manual CurrentTime() guest outputs (%u cases, real dispatcher)\n", cases);
    return 0;
}

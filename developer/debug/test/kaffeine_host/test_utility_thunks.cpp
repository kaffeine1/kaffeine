/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Author: Michele Dipace (@kaffeine1)
 *
 * Call the production utility.library thunks through the real dispatcher
 * with literal LVOs and poisoned generated fallbacks. Check both result
 * registers of the math functions: the quotient and remainder of
 * SDivMod32() and UDivMod32(), and the high and low halves of the 64-bit
 * products of SMult64() and UMult64(). Check that FindTagItem(),
 * GetTagData(), NextTagItem(), boolean packing, mapping, filtering and
 * allocation operate on guest BE32 pairs without converting ti_Data.
 * Compare guest strings with the unsigned byte rules of AROS ToLower.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <utility/date.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_layers.h"
#include "kaffeine_taglist.h"
#include "kaffeine_thunks_utility.h"

extern "C" {
#define UTILITY_INTERN_H
#define AROS_LHA(type, name, reg) type name
#define AROS_LH1(type, fn, a1, bt, bn, o, lib) type fn(a1)
#define AROS_LH2(type, fn, a1, a2, bt, bn, o, lib) type fn(a1, a2)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../../../rom/utility/amiga2date.c"
#pragma GCC diagnostic pop
/* date2amiga.c computes year * 31536000 in int, which overflows for dates
   from 2047 on, and relies on the wrap for its ULONG result. Keep the AROS
   source as it is and leave only that check out of sanitizer builds. */
#undef AROS_LH1
#define AROS_LH1(type, fn, a1, bt, bn, o, lib) \
    __attribute__((no_sanitize("signed-integer-overflow"))) type fn(a1)
#include "../../../../rom/utility/date2amiga.c"
#undef AROS_LH1
#define AROS_LH1(type, fn, a1, bt, bn, o, lib) type fn(a1)
#include "../../../../rom/utility/checkdate.c"
#include "../../../../rom/utility/smult32.c"
#include "../../../../rom/utility/umult32.c"
#undef AROS_LIBFUNC_EXIT
#undef AROS_LIBFUNC_INIT
#undef AROS_LH2
#undef AROS_LH1
#undef AROS_LHA
#undef UTILITY_INTERN_H
}

#include "utility_arithmetic_generated.h"

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *what, uint32_t a = 0, uint32_t b = 0)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::printf("FAIL: %s (0x%08x, 0x%08x)\n", what, (unsigned)a, (unsigned)b);
    }
}

static std::map<APTR, ULONG> native_blocks;
static int fail_native_after = -1;

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    if (fail_native_after == 0)
    {
        fail_native_after = -1;
        return nullptr;
    }
    if (fail_native_after > 0) --fail_native_after;
    APTR block = std::malloc(size);
    if (block)
    {
        std::memset(block, (flags & MEMF_CLEAR) ? 0 : 0xCD, size);
        native_blocks.emplace(block, size);
    }
    return block;
}

extern "C" void FreeMem(APTR block, ULONG size)
{
    auto it = native_blocks.find(block);
    check(it != native_blocks.end() && it->second == size,
          "native bookkeeping is freed exactly once with its own size");
    if (it == native_blocks.end()) std::abort();
    native_blocks.erase(it);
    std::free(block);
}

/* Unused collaborators of the production heap's HUNK/library entry points. */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }

struct TestCPU
{
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0;
    ULONG pc = 0;
};

static ULONG get_d(void *state, int reg) { return static_cast<TestCPU *>(state)->d[reg]; }
static ULONG get_a(void *state, int reg) { return static_cast<TestCPU *>(state)->a[reg]; }
static void set_d(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->d[reg] = value; }
static void set_a(void *state, int reg, ULONG value) { static_cast<TestCPU *>(state)->a[reg] = value; }
static UWORD get_sr(void *state) { return static_cast<TestCPU *>(state)->sr; }
static void set_sr(void *state, UWORD value) { static_cast<TestCPU *>(state)->sr = value; }
static ULONG get_pc(void *state) { return static_cast<TestCPU *>(state)->pc; }
static void set_pc(void *state, ULONG value) { static_cast<TestCPU *>(state)->pc = value; }
static void execute(void *) { std::abort(); }

static const KaffeineCPUOps cpu_ops =
{
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

/* From utility_lib.fd, independent of the production registrations */
enum
{
    LVO_FINDTAGITEM = 30, LVO_GETTAGDATA = 36, LVO_PACKBOOLTAGS = 42,
    LVO_NEXTTAGITEM = 48, LVO_FILTERTAGCHANGES = 54, LVO_MAPTAGS = 60,
    LVO_ALLOCATETAGITEMS = 66, LVO_CLONETAGITEMS = 72, LVO_FREETAGITEMS = 78,
    LVO_REFRESHTAGITEMCLONES = 84, LVO_TAGINARRAY = 90, LVO_FILTERTAGITEMS = 96,
    LVO_AMIGA2DATE = 120, LVO_DATE2AMIGA = 126, LVO_CHECKDATE = 132,
    LVO_SMULT32 = 138, LVO_UMULT32 = 144,
    LVO_SDIVMOD32 = 150, LVO_UDIVMOD32 = 156,
    LVO_STRICMP = 162, LVO_STRNICMP = 168,
    LVO_SMULT64 = 198, LVO_UMULT64 = 204
};

static unsigned generated_calls;

static IPTR unexpected_generated(KaffeineContext *, void *)
{
    ++generated_calls;
    return 0xBAD0BAD0;
}

/* If a manual entry moves or disappears, the call reaches one of these. */
static const M68KThunkEntry generated_thunks[] =
{
    { LVO_SMULT32, thunk_utility_SMult32 }, { LVO_UMULT32, thunk_utility_UMult32 },
    { LVO_FINDTAGITEM, unexpected_generated }, { LVO_GETTAGDATA, unexpected_generated },
    { LVO_PACKBOOLTAGS, unexpected_generated },
    { LVO_NEXTTAGITEM, unexpected_generated }, { LVO_SDIVMOD32, unexpected_generated },
    { LVO_FILTERTAGCHANGES, unexpected_generated }, { LVO_MAPTAGS, unexpected_generated },
    { LVO_ALLOCATETAGITEMS, unexpected_generated }, { LVO_CLONETAGITEMS, unexpected_generated },
    { LVO_FREETAGITEMS, unexpected_generated }, { LVO_REFRESHTAGITEMCLONES, unexpected_generated },
    { LVO_TAGINARRAY, unexpected_generated }, { LVO_FILTERTAGITEMS, unexpected_generated },
    { LVO_STRICMP, unexpected_generated }, { LVO_STRNICMP, unexpected_generated },
    { LVO_UDIVMOD32, unexpected_generated }, { LVO_SMULT64, unexpected_generated },
    { LVO_UMULT64, unexpected_generated },
    { LVO_AMIGA2DATE, unexpected_generated }, { LVO_DATE2AMIGA, unexpected_generated },
    { LVO_CHECKDATE, unexpected_generated }
};

struct Fixture
{
    static constexpr ULONG return_pc = 0x76543210;
    KaffeineContext &ctx = *static_cast<KaffeineContext *>(AllocMem(sizeof(KaffeineContext), MEMF_CLEAR));
    UBYTE memory[4096] = {};
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        ctx.mem = memory;
        ctx.mem_size = sizeof(memory);
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = 0x1000;
        lib.jt_start = 0x800;
        std::strcpy(lib.name, "utility.library");
        lib.thunks = kaffeine_thunks_utility_manual;
        lib.num_thunks = kaffeine_thunks_utility_manual_count;
        lib.gen_thunks = generated_thunks;
        lib.num_gen_thunks = sizeof(generated_thunks) / sizeof(generated_thunks[0]);
    }

    /* Call an LVO with D0, D1 and A0 set; the other registers hold markers. */
    void call(ULONG lvo, uint32_t d0, uint32_t d1, uint32_t a0 = 0xA0A0A000,
              uint32_t a1 = 0xA0A0A001)
    {
        for (int i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0D0D000 + i;
            state.a[i] = 0xA0A0A000 + i;
        }
        state.d[0] = d0;
        state.d[1] = d1;
        state.a[0] = a0;
        state.a[1] = a1;
        state.a[7] = 0x80;
        state.sr = 0xA715;
        state.pc = 0x87654321;
        m68k_write32(&ctx, state.a[7], return_pc);
        generated_calls = 0;

        KaffeineDispatchResult result = {};
        bool dispatched = Kaffeine_DispatchLibraryCall(&ctx, &cpu, 0x1000 - lvo, &result);
        check(dispatched, "utility.library LVO is dispatched", lvo);
        check(state.a[7] == 0x84 && result.return_pc == return_pc && !result.supervisor,
              "dispatch consumes exactly one guest return address", lvo);
        check(generated_calls == 0, "no poisoned generated fallback is called", lvo);
        bool others = state.a[0] == a0 && state.a[1] == a1 &&
                      state.sr == 0xA715 && state.pc == 0x87654321;
        for (int i = 2; i < 8; i++)
            others = others && state.d[i] == 0xD0D0D000 + (ULONG)i;
        for (int i = 2; i < 7; i++)
            others = others && state.a[i] == 0xA0A0A000 + (ULONG)i;
        check(others, "D2-D7, A0-A6, SR and PC are left unchanged", lvo);
    }
};

static Fixture fixture;

static void expect(ULONG lvo, uint32_t d0, uint32_t d1, uint32_t want_d0, uint32_t want_d1,
                   const char *what)
{
    fixture.call(lvo, d0, d1);
    check(fixture.state.d[0] == want_d0, what, fixture.state.d[0], want_d0);
    check(fixture.state.d[1] == want_d1, what, fixture.state.d[1], want_d1);
}

static uint32_t s(int32_t value) { return (uint32_t)value; }

static void test_table()
{
    const ULONG want[] =
    {
        LVO_FINDTAGITEM, LVO_GETTAGDATA, LVO_PACKBOOLTAGS, LVO_NEXTTAGITEM,
        LVO_FILTERTAGCHANGES, LVO_MAPTAGS, LVO_ALLOCATETAGITEMS, LVO_CLONETAGITEMS,
        LVO_FREETAGITEMS, LVO_REFRESHTAGITEMCLONES, LVO_TAGINARRAY, LVO_FILTERTAGITEMS,
        102, LVO_AMIGA2DATE, LVO_DATE2AMIGA, LVO_CHECKDATE,
        LVO_SDIVMOD32, LVO_UDIVMOD32, LVO_STRICMP, LVO_STRNICMP, LVO_SMULT64, LVO_UMULT64,
        312
    };
    const ULONG count = sizeof(want) / sizeof(want[0]);
    bool ok = kaffeine_thunks_utility_manual_count == count;
    for (ULONG i = 0; ok && i < count; i++)
        ok = kaffeine_thunks_utility_manual[i].lvo == want[i] &&
             kaffeine_thunks_utility_manual[i].thunk != nullptr;
    ok = ok && kaffeine_thunks_utility_manual[count].thunk == nullptr;
    check(ok, "manual table registers every literal utility LVO, with one sentinel");
}

static void test_sdivmod32()
{
    expect(LVO_SDIVMOD32, 100, 7, 14, 2, "SDivMod32(100, 7)");
    expect(LVO_SDIVMOD32, s(-100), 7, s(-14), s(-2), "SDivMod32(-100, 7)");
    expect(LVO_SDIVMOD32, 100, s(-7), s(-14), 2, "SDivMod32(100, -7)");
    expect(LVO_SDIVMOD32, s(-100), s(-7), 14, s(-2), "SDivMod32(-100, -7)");
    expect(LVO_SDIVMOD32, s(-123456789), 1000, s(-123456), s(-789),
           "SDivMod32(-123456789, 1000), the c_compute case");
    expect(LVO_SDIVMOD32, 0x80000000u, s(-1), 0x80000000u, 0, "SDivMod32(-2^31, -1) wraps");
    expect(LVO_SDIVMOD32, 0x80000000u, 1, 0x80000000u, 0, "SDivMod32(-2^31, 1)");
    expect(LVO_SDIVMOD32, 5, 0, 0, 5, "SDivMod32(5, 0) keeps the dividend as remainder");
    expect(LVO_SDIVMOD32, 0, 5, 0, 0, "SDivMod32(0, 5)");
}

static void test_udivmod32()
{
    expect(LVO_UDIVMOD32, 42, 10, 4, 2, "UDivMod32(42, 10), a printf digit step");
    expect(LVO_UDIVMOD32, 3000000000u, 7, 428571428u, 4, "UDivMod32(3000000000, 7)");
    expect(LVO_UDIVMOD32, 0xFFFFFFFFu, 0x10000u, 0xFFFFu, 0xFFFFu, "UDivMod32(2^32-1, 2^16)");
    expect(LVO_UDIVMOD32, 0x80000000u, 2, 0x40000000u, 0, "UDivMod32 treats D0 as unsigned");
    expect(LVO_UDIVMOD32, 5, 0, 0, 5, "UDivMod32(5, 0) keeps the dividend as remainder");
}

static void test_mult64()
{
    expect(LVO_SMULT64, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x3FFFFFFFu, 1, "SMult64(2^31-1, 2^31-1)");
    expect(LVO_SMULT64, s(-2), 3, 0xFFFFFFFFu, 0xFFFFFFFAu, "SMult64(-2, 3)");
    expect(LVO_SMULT64, 0x80000000u, 0x80000000u, 0x40000000u, 0, "SMult64(-2^31, -2^31)");
    expect(LVO_SMULT64, 0x80000000u, s(-1), 0, 0x80000000u, "SMult64(-2^31, -1)");
    expect(LVO_UMULT64, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu, 1, "UMult64(2^32-1, 2^32-1)");
    expect(LVO_UMULT64, 0x10000u, 0x10000u, 1, 0, "UMult64(2^16, 2^16)");
    expect(LVO_UMULT64, 0x80000000u, 2, 1, 0, "UMult64 treats D0 as unsigned");
}

static unsigned forbidden_info_calls;
static IPTR forbidden_info_thunk(KaffeineContext *ctx, void *)
{
    ++forbidden_info_calls;
    ctx->mem[0x700] ^= 0xFF;
    return 0xBAD0BAD0;
}
static void forbidden_info_after(KaffeineContext *ctx)
{
    ++forbidden_info_calls;
    ctx->mem[0x701] ^= 0xFF;
}

static void test_layer_info_arithmetic()
{
    struct Arithmetic { const char *name; ULONG lvo, a, b, d0, d1; bool manual; };
    const Arithmetic allowed[] = {
        {"SMult32", 138, s(-7), 6, s(-42), 6, false},
        {"UMult32", 144, 0xFFFFFFFF, 2, 0xFFFFFFFE, 2, false},
        {"SDivMod32", 150, s(-100), 7, s(-14), s(-2), true},
        {"UDivMod32", 156, 3000000000u, 7, 428571428u, 4, true},
        {"SMult64", 198, s(-2), 3, 0xFFFFFFFF, 0xFFFFFFFA, true},
        {"UMult64", 204, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 1, true}
    };
    auto &ctx = fixture.ctx;
    auto saved_lib = ctx.libs[0];
    auto saved_layers = ctx.layer_ops;
    BOOL saved_running = ctx.running, saved_failed = ctx.callback_failed;
    LONG saved_exit = ctx.exit_code;
    KaffeineLayerOps held = {};
    held.info_locked = [](KaffeineContext *) { return TRUE; };
    ctx.layer_ops = &held;
    m68k_write32(&ctx, 0x80, Fixture::return_pc);
    const auto before = std::vector<UBYTE>(fixture.memory, fixture.memory + sizeof(fixture.memory));
    const size_t allocations = native_blocks.size();
    auto reset_stop = [&] { ctx.running = TRUE; ctx.callback_failed = FALSE; ctx.exit_code = 0; };
    auto unchanged = [&] {
        return std::memcmp(fixture.memory, before.data(), before.size()) == 0 &&
               native_blocks.size() == allocations && ctx.layer_ops == &held;
    };
    auto rejected = [&](ULONG lvo) {
        reset_stop(); forbidden_info_calls = 0; ctx.after_thunk = forbidden_info_after;
        fixture.call(lvo, 4, 9, 0x400, 0x500);
        check(!ctx.running && ctx.callback_failed && ctx.exit_code == 20 &&
              fixture.state.d[0] == 20 && !forbidden_info_calls && unchanged() &&
              ctx.after_thunk == forbidden_info_after,
              "Info-held refusal precedes thunk, callback, memory and allocation effects", lvo);
        ctx.after_thunk = nullptr;
    };
    for (const auto &a : allowed)
    {
        bool found = false;
        for (const auto &v : utility_conf_vectors)
            if (!std::strcmp(v.name, a.name)) found = v.lvo == a.lvo;
        check(found, "arithmetic literal LVO agrees with the independent public utility.conf", a.lvo);
        for (UWORD depth : { 0, 1 })
        {
            reset_stop(); ctx.callback_depth = depth;
            fixture.call(a.lvo, a.a, a.b);
            check(ctx.running && !ctx.callback_failed && !ctx.exit_code && ctx.callback_depth == depth &&
                  fixture.state.d[0] == a.d0 && fixture.state.d[1] == a.d1 && unchanged(),
                  "real generated/manual arithmetic preserves scope, both results and guest/native storage", a.lvo);
        }
    }
    ctx.callback_depth = 0;
    /* Existing edge-case tests execute the real manual D0/D1 implementations
       inside the held scope, including zero divisors and signed overflow. */
    reset_stop(); test_sdivmod32(); test_udivmod32(); test_mult64();
    check(ctx.running && !ctx.callback_failed && unchanged(),
          "manual arithmetic edge cases stay pure inside the Info scope");
    for (const auto &v : utility_conf_vectors)
    {
        bool arithmetic = false;
        for (const auto &a : allowed) arithmetic |= !std::strcmp(v.name,a.name);
        if (arithmetic) continue;
        /* Exhaustive public-vector oracle: callbacks, tag walks and mutations,
           allocation, strings, dates and every other Utility API stay blocked
           before either a manual or generated entry can run. */
        M68KThunkEntry poison = {v.lvo,forbidden_info_thunk};
        for (bool manual : { false, true })
        {
            ctx.libs[0] = saved_lib;
            ctx.libs[0].thunks = manual ? &poison : nullptr;
            ctx.libs[0].num_thunks = manual ? 1 : 0;
            ctx.libs[0].gen_thunks = &poison; ctx.libs[0].num_gen_thunks = 1;
            rejected(v.lvo);
        }
    }
    for (const auto &a : allowed)
    {
        ctx.libs[0] = saved_lib;
        ctx.libs[0].num_thunks = 0;
        if (!a.manual) ctx.libs[0].num_gen_thunks = 0;
        rejected(a.lvo); // no pair-result generated fallback or missing-thunk success
    }
    for (ULONG lvo : { 48u, 54u, 66u, 72u, 78u, 102u })
    {
        ctx.libs[0] = saved_lib;
        rejected(lvo); // actual manual tag/allocation/Hook entries remain gated
    }
    ctx.libs[0] = saved_lib; std::strcpy(ctx.libs[0].name,"other.library"); rejected(138);
    ctx.libs[0] = saved_lib; ctx.libs[0].dos = TRUE; rejected(138);
    ctx.libs[0] = saved_lib; ctx.layer_ops = saved_layers;
    ctx.running = saved_running; ctx.callback_failed = saved_failed; ctx.exit_code = saved_exit;
}

/* Compare with 64-bit reference arithmetic on pseudo-random operands. */
static void test_reference()
{
    uint32_t seed = 0x12345678;
    bool sdiv = true, udiv = true, smul = true, umul = true;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

    for (int i = 0; i < 20000; i++)
    {
        uint32_t a = next(), b = next();
        if (i % 4 == 0)
            b >>= next() % 32;      /* small divisors as well as large ones */
        if (!b)
            b = 1;

        int64_t sa = (int32_t)a, sb = (int32_t)b;
        fixture.call(LVO_SDIVMOD32, a, b);
        if (!(sa == INT32_MIN && sb == -1))
            sdiv = sdiv && (int32_t)fixture.state.d[0] == sa / sb &&
                   (int32_t)fixture.state.d[1] == sa % sb;

        fixture.call(LVO_UDIVMOD32, a, b);
        udiv = udiv && fixture.state.d[0] == a / b && fixture.state.d[1] == a % b;

        int64_t sp = sa * (int32_t)b;
        fixture.call(LVO_SMULT64, a, b);
        smul = smul && fixture.state.d[0] == (uint32_t)((uint64_t)sp >> 32) &&
               fixture.state.d[1] == (uint32_t)sp;

        uint64_t up = (uint64_t)a * b;
        fixture.call(LVO_UMULT64, a, b);
        umul = umul && fixture.state.d[0] == (uint32_t)(up >> 32) &&
               fixture.state.d[1] == (uint32_t)up;
    }
    check(sdiv, "SDivMod32 matches the reference on 20000 operand pairs");
    check(udiv, "UDivMod32 matches the reference on 20000 operand pairs");
    check(smul, "SMult64 matches the reference on 20000 operand pairs");
    check(umul, "UMult64 matches the reference on 20000 operand pairs");
}

/* FindTagItem() */

static const ULONG U1 = TAG_USER | 1, U2 = TAG_USER | 2, U3 = TAG_USER | 3, U4 = TAG_USER | 4;

static void clear_tags()
{
    std::memset(fixture.memory + 0x100, 0, sizeof(fixture.memory) - 0x100);
}

static void put_tags(ULONG addr, std::initializer_list<std::pair<ULONG, ULONG>> items)
{
    for (const auto &item : items)
    {
        m68k_write32(&fixture.ctx, addr, item.first);
        m68k_write32(&fixture.ctx, addr + 4, item.second);
        addr += 8;
    }
}

/* D0 is the tag and A0 the list. D1 holds another tag and A1 an address
   outside guest memory, so reading the wrong register finds nothing. */
static ULONG find(ULONG tag, ULONG list)
{
    fixture.call(LVO_FINDTAGITEM, tag, U4, list);
    check(fixture.state.d[1] == U4, "FindTagItem leaves D1 alone", fixture.state.d[1], U4);
    return fixture.state.d[0];
}

static void expect_find(ULONG tag, ULONG list, ULONG want, const char *what)
{
    UBYTE before[sizeof(fixture.memory)];
    std::memcpy(before, fixture.memory, sizeof(before));
    ULONG got = find(tag, list);
    check(got == want, what, got, want);
    /* The stack slot of the return address is rewritten by every call. */
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "FindTagItem writes no guest memory and makes no shadow", got);
}

static void test_findtagitem_lists()
{
    clear_tags();
    /* 8192 is in the range of guest addresses but is a size here. */
    put_tags(0x400, { { U1, 10 }, { U2, 8192 }, { U3, 0x450 }, { TAG_DONE, 0 } });
    expect_find(U1, 0x400, 0x400, "FindTagItem returns the first item of the guest list");
    expect_find(U2, 0x400, 0x408, "FindTagItem returns the guest address of the item");
    expect_find(U3, 0x400, 0x410, "FindTagItem finds the item before TAG_DONE");
    check(m68k_read32(&fixture.ctx, 0x40C) == 8192,
          "the item keeps its ti_Data, a scalar in the address range", m68k_read32(&fixture.ctx, 0x40C));
    check(fixture.state.d[0] != (ULONG)(uintptr_t)(fixture.memory + 0x410),
          "FindTagItem does not return the low half of a host pointer");
    expect_find(U4, 0x400, 0, "FindTagItem returns NULL for a tag that is not there");
    expect_find(U1, 0, 0, "FindTagItem returns NULL for a NULL list");
    put_tags(0x600, { { TAG_DONE, 0 } });
    expect_find(U1, 0x600, 0, "FindTagItem returns NULL for an empty list");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x600 }, { U3, 3 }, { TAG_DONE, 0 } });
    put_tags(0x600, { { U2, 2 }, { U3, 33 }, { TAG_DONE, 0 } });
    expect_find(U3, 0x400, 0x608, "FindTagItem follows TAG_MORE and returns the item in the next list");
    expect_find(U4, 0x400, 0, "FindTagItem stops at the TAG_DONE behind TAG_MORE");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0 }, { U2, 2 }, { TAG_DONE, 0 } });
    expect_find(U2, 0x400, 0, "TAG_MORE with a NULL ti_Data ends the list");
    put_tags(0x500, { { U1, 1 }, { TAG_DONE, 0 }, { U2, 2 }, { TAG_DONE, 0 } });
    expect_find(U2, 0x500, 0, "an item after TAG_DONE is not found");

    clear_tags();
    put_tags(0x400, { { TAG_IGNORE, U2 }, { U2, 7 }, { TAG_DONE, 0 } });
    expect_find(U2, 0x400, 0x408, "FindTagItem passes over TAG_IGNORE");

    clear_tags();
    put_tags(0x400, { { TAG_SKIP, 1 }, { U2, 1 }, { U2, 2 }, { TAG_DONE, 0 } });
    expect_find(U2, 0x400, 0x410, "TAG_SKIP 1 passes over itself and one item");
    put_tags(0x500, { { TAG_SKIP, 0 }, { U2, 1 }, { TAG_DONE, 0 } });
    expect_find(U2, 0x500, 0x508, "TAG_SKIP 0 passes over itself only");

    clear_tags();
    put_tags(0x400, { { TAG_IGNORE, 0 }, { TAG_SKIP, 0 }, { TAG_MORE, 0x500 } });
    put_tags(0x500, { { U1, 0 }, { TAG_DONE, 0 } });
    expect_find(TAG_IGNORE, 0x400, 0, "FindTagItem never returns TAG_IGNORE");
    expect_find(TAG_SKIP, 0x400, 0, "FindTagItem never returns TAG_SKIP");
    expect_find(TAG_MORE, 0x400, 0, "FindTagItem never returns TAG_MORE");
    expect_find(TAG_DONE, 0x400, 0, "FindTagItem never returns TAG_DONE");
    expect_find(U1, 0x400, 0x500, "the control tags lead to the user tag");
}

static void test_findtagitem_bounds()
{
    const ULONG end = sizeof(fixture.memory);

    clear_tags();
    put_tags(end - 16, { { U1, 1 }, { U2, 2 } });
    expect_find(U2, end - 16, end - 8, "FindTagItem finds an item in the last 8 bytes of guest memory");
    expect_find(U3, end - 16, 0, "a list that runs off the end of guest memory returns NULL");
    expect_find(U1, end, 0, "a list at the end of guest memory returns NULL");
    expect_find(U1, end - 4, 0, "an item that crosses the end of guest memory returns NULL");
    m68k_write32(&fixture.ctx, end - 4, U1);
    expect_find(U1, end - 4, 0, "a user tag in the last 4 bytes of guest memory is not an item");
    expect_find(U1, 0xFFFFFFF8, 0, "a list address near 2^32 returns NULL");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x5000 } });
    expect_find(U1, 0x400, 0x400, "an item before a bad TAG_MORE is still found");
    expect_find(U2, 0x400, 0, "TAG_MORE outside guest memory returns NULL");
    /* The cycle check runs ahead and reaches the bad item first. */
    put_tags(0x500, { { U2, 0 }, { U2, 0 }, { U2, 0 }, { U2, 0 }, { U2, 0 }, { U2, 0 },
                      { U2, 0 }, { U2, 0 }, { U1, 1 }, { TAG_MORE, 0x5000 } });
    expect_find(U1, 0x500, 0x540, "an item far before a bad TAG_MORE is still found");

    clear_tags();
    put_tags(0x400, { { TAG_SKIP, 0xFFFFFFFF }, { U1, 1 }, { TAG_DONE, 0 } });
    expect_find(U1, 0x400, 0, "TAG_SKIP past 2^32 returns NULL");
    /* (0x20000000 + 1) * 8 wraps to 8: the next item, if nothing checked it */
    put_tags(0x500, { { TAG_SKIP, 0x20000000 }, { U1, 1 }, { TAG_DONE, 0 } });
    expect_find(U1, 0x500, 0, "a TAG_SKIP whose byte count wraps around 2^32 returns NULL");
    put_tags(end - 16, { { TAG_SKIP, 1 }, { U1, 1 } });
    expect_find(U1, end - 16, 0, "TAG_SKIP to the end of guest memory returns NULL");
}

/* The items Kaffeine_TagWalkNext() returns from list, and how the walk ends;
   a walk that does not end is cut off and ends with an item */
static std::vector<ULONG> walk_items(ULONG list, int *end)
{
    std::vector<ULONG> items;
    KaffeineTagWalk walk;
    ULONG item = 0;
    int kind;

    Kaffeine_TagWalkInit(&walk, list);
    while ((kind = Kaffeine_TagWalkNext(&fixture.ctx, &walk, &item)) == KAFFEINE_TAG_ITEM &&
           items.size() < 10000)
        items.push_back(item);
    *end = kind;
    return items;
}

/* AROS NextTagItem() over guest memory, stopped where an address repeats:
   the user items in order, and how the list ends */
static std::vector<ULONG> reference_items(ULONG list, int *end)
{
    const uint64_t size = sizeof(fixture.memory);
    std::vector<ULONG> items;
    std::set<ULONG> seen;
    uint64_t pos = list;

    *end = KAFFEINE_TAG_END;
    while (pos)
    {
        if (pos + 8 > size || !seen.insert((ULONG)pos).second)
        {
            *end = KAFFEINE_TAG_BAD;
            break;
        }
        ULONG item_tag = m68k_read32(&fixture.ctx, (ULONG)pos);
        ULONG item_data = m68k_read32(&fixture.ctx, (ULONG)pos + 4);

        if (item_tag == TAG_DONE)
            break;
        if (item_tag == TAG_MORE)
            pos = item_data;
        else if (item_tag == TAG_SKIP)
            pos += ((uint64_t)item_data + 1) * 8;
        else
        {
            if (item_tag != TAG_IGNORE)
                items.push_back((ULONG)pos);
            pos += 8;
        }
    }
    return items;
}

static void test_findtagitem_cycles()
{
    clear_tags();
    put_tags(0x400, { { TAG_MORE, 0x400 } });
    expect_find(U1, 0x400, 0, "a TAG_MORE to itself ends the search");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x600 } });
    put_tags(0x600, { { U2, 2 }, { TAG_MORE, 0x400 } });
    expect_find(U2, 0x400, 0x600, "an item before a cycle is found");
    expect_find(U3, 0x400, 0, "two lists that point at each other end the search");

    /* A ring of 40 lists, each with an item and a TAG_SKIP to a TAG_MORE */
    clear_tags();
    for (ULONG i = 0; i < 40; i++)
    {
        ULONG at = 0x200 + i * 0x40, next = 0x200 + ((i + 1) % 40) * 0x40;
        put_tags(at, { { TAG_USER | (0x100 + i), i }, { TAG_SKIP, 1 }, { U1, 0 },
                       { TAG_MORE, next } });
    }
    expect_find(TAG_USER | 0x127, 0x200, 0x200 + 39 * 0x40, "the last list of a ring is reached");
    expect_find(U1, 0x200, 0, "a ring of 40 lists without the tag ends the search");

    /* The same chain without the ring is long but ends, and is searched to the end */
    m68k_write32(&fixture.ctx, 0x200 + 39 * 0x40 + 28, 0);
    expect_find(U4, 0x200, 0, "a chain of 40 lists ends at its NULL TAG_MORE");

    /* Two items before a ring of four items and a TAG_MORE: the hare meets
       the walk when it has passed two items of the ring */
    clear_tags();
    put_tags(0x400, { { U1, 1 }, { U2, 2 }, { TAG_USER | 0x10, 0 }, { TAG_USER | 0x11, 0 },
                      { TAG_USER | 0x12, 0 }, { TAG_USER | 0x13, 0 }, { TAG_MORE, 0x410 } });
    expect_find(TAG_USER | 0x12, 0x400, 0x420,
                "an item of a ring after the point where the cycle is found is found");
    expect_find(TAG_USER | 0x13, 0x400, 0x428, "the last item of a ring behind two items is found");
    expect_find(U3, 0x400, 0, "a ring behind two items without the tag ends the search");

    /* The last item of this ring leads into its first one without a TAG_MORE */
    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x610 } });
    put_tags(0x600, { { U2, 2 }, { U3, 3 }, { TAG_USER | 0x20, 0 }, { TAG_MORE, 0x600 } });
    expect_find(U2, 0x400, 0x600, "an item behind the TAG_MORE of a ring is found");
    expect_find(U3, 0x400, 0x608, "an item that leads back to the start of its ring is found");
    expect_find(U4, 0x400, 0, "that ring without the tag ends the search");

    /* Rings of 1 to 8 items and a TAG_MORE behind 0 to 6 items */
    bool all_found = true;
    ULONG bad_tail = 0, bad_ring = 0;
    for (ULONG tail = 0; tail <= 6; tail++)
    {
        for (ULONG ring = 1; ring <= 8; ring++)
        {
            const ULONG start = 0x400 + tail * 8;
            bool found = true;

            std::vector<ULONG> want;
            int end;

            clear_tags();
            for (ULONG i = 0; i < tail + ring; i++)
            {
                put_tags(0x400 + i * 8, { { TAG_USER | (0x100 + i), i } });
                want.push_back(0x400 + i * 8);
            }
            put_tags(start + ring * 8, { { TAG_MORE, start } });
            found = walk_items(0x400, &end) == want && end == KAFFEINE_TAG_BAD;
            for (ULONG i = 0; i < tail + ring; i++)
                found = found && find(TAG_USER | (0x100 + i), 0x400) == 0x400 + i * 8;
            found = found && find(U1, 0x400) == 0;
            if (!found && all_found)
            {
                bad_tail = tail;
                bad_ring = ring;
            }
            all_found = all_found && found;
        }
    }
    check(all_found, "every item of rings of 1 to 8 items behind 0 to 6 items is walked once and found",
          bad_tail, bad_ring);
}

/* GetTagData() */

static ULONG get_data(ULONG tag, ULONG def, ULONG list)
{
    fixture.call(LVO_GETTAGDATA, tag, def, list);
    check(fixture.state.d[1] == def, "GetTagData leaves D1 alone", fixture.state.d[1], def);
    return fixture.state.d[0];
}

static void expect_data(ULONG tag, ULONG def, ULONG list, ULONG want, const char *what)
{
    UBYTE before[sizeof(fixture.memory)];
    std::memcpy(before, fixture.memory, sizeof(before));
    ULONG got = get_data(tag, def, list);
    check(got == want, what, got, want);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "GetTagData writes no guest memory and makes no shadow", got);
}

static void test_gettagdata()
{
    const ULONG end = sizeof(fixture.memory);

    clear_tags();
    /* 8192 is a size in the guest address range; 0x450 looks like a guest
       address; U4 has ti_Data 0 and must return 0 rather than defaultVal. */
    put_tags(0x400, { { U1, 10 }, { U2, 8192 }, { U3, 0x450 }, { U4, 0 },
                      { U1, 99 }, { TAG_DONE, 0 } });
    expect_data(U1, 777, 0x400, 10, "GetTagData returns ti_Data of the first matching user item");
    expect_data(U2, 777, 0x400, 8192, "GetTagData keeps a size such as 8192 unchanged");
    expect_data(U3, 777, 0x400, 0x450, "GetTagData keeps a value that looks like a guest address unchanged");
    expect_data(U4, 0xDEADBEEFu, 0x400, 0, "GetTagData returns a zero ti_Data instead of defaultVal");

    expect_data(TAG_USER | 9, 0xCAFEBABEu, 0x400, 0xCAFEBABEu,
                "GetTagData returns defaultVal for a missing tag");
    expect_data(TAG_USER | 9, 0, 0x400, 0, "GetTagData returns a zero defaultVal for a missing tag");
    expect_data(U1, 0x12345678u, 0, 0x12345678u, "GetTagData returns defaultVal for a NULL list");
    put_tags(0x600, { { TAG_DONE, 0 } });
    expect_data(U1, 0x87654321u, 0x600, 0x87654321u, "GetTagData returns defaultVal for an empty list");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x600 }, { U3, 3 }, { TAG_DONE, 0 } });
    put_tags(0x600, { { U2, 8192 }, { U3, 33 }, { TAG_DONE, 0 } });
    expect_data(U2, 777, 0x400, 8192, "GetTagData follows TAG_MORE and keeps 8192 unchanged");
    expect_data(U3, 777, 0x400, 33, "GetTagData returns the item behind TAG_MORE, not after it");
    expect_data(U4, 555, 0x400, 555, "GetTagData returns defaultVal after TAG_DONE behind TAG_MORE");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0 }, { U2, 2 }, { TAG_DONE, 0 } });
    expect_data(U2, 444, 0x400, 444, "GetTagData stops at a NULL TAG_MORE and returns defaultVal");

    clear_tags();
    put_tags(0x400, { { TAG_IGNORE, U2 }, { U2, 7 }, { TAG_DONE, 0 } });
    expect_data(U2, 999, 0x400, 7, "GetTagData passes over TAG_IGNORE");
    expect_data(TAG_IGNORE, 999, 0x400, 999, "GetTagData never matches TAG_IGNORE");

    clear_tags();
    put_tags(0x400, { { TAG_SKIP, 1 }, { U2, 111 }, { U2, 222 }, { TAG_DONE, 0 } });
    expect_data(U2, 999, 0x400, 222, "GetTagData passes over TAG_SKIP 1 and one item");
    put_tags(0x500, { { TAG_SKIP, 0 }, { U2, 333 }, { TAG_DONE, 0 } });
    expect_data(U2, 999, 0x500, 333, "GetTagData passes over TAG_SKIP 0 only");
    expect_data(TAG_SKIP, 999, 0x400, 999, "GetTagData never matches TAG_SKIP");
    expect_data(TAG_MORE, 999, 0x400, 999, "GetTagData never matches TAG_MORE");
    expect_data(TAG_DONE, 999, 0x400, 999, "GetTagData never matches TAG_DONE");

    clear_tags();
    put_tags(end - 16, { { U1, 11 }, { U2, 8192 } });
    expect_data(U2, 777, end - 16, 8192, "GetTagData reads an item in the last 8 bytes of guest memory");
    expect_data(U3, 777, end - 16, 777, "GetTagData returns defaultVal when the list runs off guest memory");
    expect_data(U1, 777, end, 777, "GetTagData returns defaultVal for a list at the end of guest memory");
    expect_data(U1, 777, end - 4, 777, "GetTagData returns defaultVal when an item crosses the end of guest memory");
    expect_data(U1, 777, 0xFFFFFFF8u, 777, "GetTagData returns defaultVal for a list address near 2^32");

    clear_tags();
    put_tags(0x400, { { U1, 11 }, { U2, 22 }, { TAG_USER | 0x10, 100 }, { TAG_USER | 0x11, 101 },
                      { TAG_USER | 0x12, 102 }, { TAG_USER | 0x13, 8192 }, { TAG_MORE, 0x410 } });
    expect_data(TAG_USER | 0x13, 777, 0x400, 8192,
                "GetTagData returns ti_Data of the last item of a ring behind two items");
    expect_data(U3, 777, 0x400, 777,
                "GetTagData returns defaultVal when a ring does not contain the tag");
}

/* NextTagItem() */

static ULONG next_tag(ULONG ptr)
{
    fixture.call(LVO_NEXTTAGITEM, U4, 0xD1D1D1D1u, ptr);
    check(fixture.state.d[1] == 0xD1D1D1D1u, "NextTagItem leaves D1 alone",
          fixture.state.d[1], 0xD1D1D1D1u);
    return fixture.state.d[0];
}

static void expect_next(ULONG ptr, ULONG want_item, ULONG want_state, const char *what)
{
    UBYTE before[sizeof(fixture.memory)];
    std::memcpy(before, fixture.memory, sizeof(before));
    ULONG got = next_tag(ptr);
    ULONG state = m68k_read32(&fixture.ctx, ptr);
    check(got == want_item, what, got, want_item);
    check(state == want_state, "NextTagItem updates the guest pointer variable", state, want_state);
    std::memcpy(before + ptr, fixture.memory + ptr, 4);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "NextTagItem changes only the guest pointer variable and makes no shadow", got);
}

static void expect_next_bad_ptr(ULONG ptr, const char *what)
{
    UBYTE before[sizeof(fixture.memory)];
    std::memcpy(before, fixture.memory, sizeof(before));
    ULONG got = next_tag(ptr);
    check(got == 0, what, got, 0);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "NextTagItem with a bad variable address writes no guest memory", ptr);
}

static void test_nexttagitem()
{
    const ULONG end = sizeof(fixture.memory);

    /* Full iteration: control tags skipped, TAG_MORE followed, variable updated
       after every call and cleared to 0 at TAG_DONE. */
    clear_tags();
    put_tags(0x400, { { U1, 10 }, { TAG_IGNORE, U3 }, { TAG_SKIP, 1 }, { U3, 99 },
                      { U2, 8192 }, { TAG_MORE, 0x600 }, { U4, 44 } });
    put_tags(0x600, { { U3, 0x450 }, { U4, 8192 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x300, 0x400);
    expect_next(0x300, 0x400, 0x408, "NextTagItem returns the first item and advances the variable");
    expect_next(0x300, 0x420, 0x428, "NextTagItem skips TAG_IGNORE and TAG_SKIP 1 and advances the variable");
    check(m68k_read32(&fixture.ctx, 0x424) == 8192,
          "NextTagItem leaves ti_Data 8192 untouched in the guest item");
    expect_next(0x300, 0x600, 0x608, "NextTagItem follows TAG_MORE into the next list");
    expect_next(0x300, 0x608, 0x610, "NextTagItem returns the second item behind TAG_MORE");
    expect_next(0x300, 0, 0, "NextTagItem returns NULL and sets the variable to 0 at TAG_DONE");
    expect_next(0x300, 0, 0, "NextTagItem on a variable that is already 0 stays 0 and returns NULL");

    /* Word-aligned variable and list addresses, and TAG_SKIP 0 */
    clear_tags();
    put_tags(0x402, { { TAG_SKIP, 0 }, { U1, 1 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x302, 0x402);
    expect_next(0x302, 0x40A, 0x412, "NextTagItem handles a word-aligned variable and TAG_SKIP 0");
    expect_next(0x302, 0, 0, "NextTagItem ends at TAG_DONE with a word-aligned variable");

    /* TAG_MORE with NULL ti_Data and an empty list both clear the variable to 0 */
    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0 }, { U2, 2 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x300, 0x400);
    expect_next(0x300, 0x400, 0x408, "NextTagItem returns the item before a NULL TAG_MORE");
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 and returns NULL at a NULL TAG_MORE");
    put_tags(0x500, { { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x300, 0x500);
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 for an empty list");

    /* Variable at the end of guest memory or outside it */
    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, end - 4, 0x400);
    expect_next(end - 4, 0x400, 0x408, "NextTagItem accepts a variable in the last 4 bytes of guest memory");
    expect_next(end - 4, 0, 0, "NextTagItem clears a variable in the last 4 bytes at TAG_DONE");
    m68k_write16(&fixture.ctx, end - 2, 0x0400);
    /* Guest address 0 is memory here: with a list pointer stored there, a
       NULL variable address must still return NULL and change nothing */
    put_tags(0x700, { { U1, 1 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0, 0x700);
    expect_next_bad_ptr(0, "NextTagItem with a NULL variable address returns NULL");
    check(m68k_read32(&fixture.ctx, 0) == 0x700,
          "NextTagItem with a NULL variable address leaves guest address 0 alone",
          m68k_read32(&fixture.ctx, 0));
    m68k_write32(&fixture.ctx, 0, 0);
    expect_next_bad_ptr(end, "NextTagItem with a variable at the end of guest memory returns NULL");
    expect_next_bad_ptr(end - 2, "NextTagItem with a variable crossing the end of guest memory returns NULL");
    expect_next_bad_ptr(end - 1, "NextTagItem with 1 byte left in guest memory returns NULL");
    expect_next_bad_ptr(0xFFFFFFFCu, "NextTagItem with a variable address near 2^32 returns NULL");

    /* Lists outside guest memory or running off the end set the variable to 0 */
    clear_tags();
    put_tags(end - 16, { { U1, 1 }, { U2, 2 } });
    m68k_write32(&fixture.ctx, 0x300, end - 16);
    expect_next(0x300, end - 16, end - 8, "NextTagItem returns the item before the last 8 bytes");
    expect_next(0x300, end - 8, end, "NextTagItem returns the item in the last 8 bytes of guest memory");
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 when the list runs off guest memory");

    m68k_write32(&fixture.ctx, 0x300, end - 4);
    m68k_write32(&fixture.ctx, end - 4, U1);
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 when an item crosses the end of guest memory");
    m68k_write32(&fixture.ctx, 0x300, 0xFFFFFFF8u);
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 for a list address near 2^32");

    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_MORE, 0x5000 } });
    m68k_write32(&fixture.ctx, 0x300, 0x400);
    expect_next(0x300, 0x400, 0x408, "NextTagItem returns the item before a bad TAG_MORE");
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 at a TAG_MORE outside guest memory");

    put_tags(0x500, { { TAG_SKIP, 0xFFFFFFFFu }, { U1, 1 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x300, 0x500);
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 at a TAG_SKIP past 2^32");
    put_tags(0x600, { { TAG_SKIP, 0x20000000u }, { U1, 1 }, { TAG_DONE, 0 } });
    m68k_write32(&fixture.ctx, 0x300, 0x600);
    expect_next(0x300, 0, 0, "NextTagItem sets the variable to 0 when TAG_SKIP byte count wraps");

    /* Rings: a loop of control tags has no user item, so the walker stops
       within one call, sets the variable to 0 and returns NULL. A ring that
       contains user items returns the next user item on each call and writes
       item + 8 to the 4-byte guest variable; because that variable holds no
       walker state across calls, successive calls walk round the ring just as
       AROS LibNextTagItem() does, while each single call terminates. */
    clear_tags();
    put_tags(0x400, { { TAG_MORE, 0x400 } });
    m68k_write32(&fixture.ctx, 0x300, 0x400);
    expect_next(0x300, 0, 0, "NextTagItem ends a TAG_MORE to itself in one call and zeroes the variable");

    put_tags(0x500, { { TAG_IGNORE, 0 }, { TAG_SKIP, 1 }, { U1, 1 }, { TAG_MORE, 0x500 } });
    m68k_write32(&fixture.ctx, 0x300, 0x500);
    expect_next(0x300, 0, 0, "NextTagItem ends a control-tag ring in one call and zeroes the variable");

    clear_tags();
    put_tags(0x400, { { U1, 11 }, { U2, 22 }, { TAG_MORE, 0x400 } });
    m68k_write32(&fixture.ctx, 0x300, 0x400);
    expect_next(0x300, 0x400, 0x408, "NextTagItem on a user-item ring returns the first item");
    expect_next(0x300, 0x408, 0x410, "NextTagItem on a user-item ring returns the second item");
    expect_next(0x300, 0x400, 0x408,
                "NextTagItem on a user-item ring follows TAG_MORE back to the first item");
}

/* The first user item with the tag in the reference walk, or 0 */
static ULONG reference_find(ULONG tag, const std::vector<ULONG> &items)
{
    for (ULONG item : items)
        if (m68k_read32(&fixture.ctx, item) == tag)
            return item;
    return 0;
}

/* Pseudo-random lists of user tags, control tags and TAG_MORE links,
   with and without cycles, against the reference */
static void test_findtagitem_reference()
{
    uint32_t seed = 0x2468ACE1;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    bool same = true, same_data = true, walked = true;
    ULONG bad_seed = 0, bad_got = 0, bad_want = 0, bad_walk = 0;

    for (int i = 0; i < 4000; i++)
    {
        const uint32_t list_seed = seed;
        const ULONG slots = 2 + next() % 30;

        clear_tags();
        for (ULONG slot = 0; slot < slots; slot++)
        {
            ULONG kind = next() % 20, tag, data;

            if (kind < 10)
            {
                tag = TAG_USER | (next() % 6);
                data = next();
            }
            else if (kind < 12)
            {
                tag = TAG_IGNORE;
                data = next();
            }
            else if (kind < 14)
            {
                tag = TAG_SKIP;
                data = next() % 4;
            }
            else if (kind < 19)
            {
                ULONG where = next() % 20;

                tag = TAG_MORE;
                data = where < 17 ? 0x400 + (next() % slots) * 8 :
                       where == 17 ? 0 : where == 18 ? 0x5000 : 0x404 + (next() % slots) * 8;
            }
            else
            {
                tag = TAG_DONE;
                data = 0;
            }
            put_tags(0x400 + slot * 8, { { tag, data } });
        }

        const ULONG list = 0x400 + (next() % slots) * 8;
        int want_end, got_end;
        const std::vector<ULONG> want_items = reference_items(list, &want_end);

        walked = walked && walk_items(list, &got_end) == want_items && got_end == want_end;
        if (!walked && !bad_walk)
            bad_walk = list_seed;
        for (ULONG t = 0; t < 7; t++)
        {
            const ULONG def = 0xA5A50000u | t;
            ULONG got = find(TAG_USER | t, list), want = reference_find(TAG_USER | t, want_items);
            ULONG got_data = get_data(TAG_USER | t, def, list);
            ULONG want_data = want ? m68k_read32(&fixture.ctx, want + 4) : def;

            if (got != want && same)
            {
                bad_seed = list_seed;
                bad_got = got;
                bad_want = want;
                std::printf("random list from seed 0x%08x, tag 0x%08x: got 0x%08x, want 0x%08x\n",
                            (unsigned)bad_seed, (unsigned)(TAG_USER | t), (unsigned)bad_got,
                            (unsigned)bad_want);
            }
            same = same && got == want;
            same_data = same_data && got_data == want_data;
        }
    }
    check(walked, "the walk matches NextTagItem on 4000 random lists", bad_walk);
    check(same, "FindTagItem matches NextTagItem on 4000 random lists", bad_got, bad_want);
    check(same_data, "GetTagData matches NextTagItem on 4000 random lists");
}

using GuestBytes = std::vector<UBYTE>;

static GuestBytes guest_bytes()
{
    return GuestBytes(fixture.memory + 0x100, fixture.memory + sizeof(fixture.memory));
}

static void expected32(GuestBytes &bytes, ULONG addr, ULONG value)
{
    for (unsigned i = 0; i < 4; i++)
        bytes.at(addr - 0x100 + i) = (UBYTE)(value >> (24 - i * 8));
}

static void expect_bytes(const GuestBytes &want, const char *what)
{
    check(guest_bytes() == want, what);
    check(fixture.ctx.num_shadows == 0, "guest utility functions create no shadows");
}

static ULONG utility_call(ULONG lvo, ULONG d0, ULONG a0, ULONG a1 = 0xA0A0A001)
{
    fixture.call(lvo, d0, U4, a0, a1);
    check(fixture.state.d[1] == U4, "guest utility call preserves D1", lvo);
    if (lvo == LVO_MAPTAGS || lvo == LVO_FILTERTAGCHANGES ||
        lvo == LVO_FREETAGITEMS || lvo == LVO_REFRESHTAGITEMCLONES)
        check(fixture.state.d[0] == d0, "void utility call preserves D0", lvo);
    return fixture.state.d[0];
}

static void test_empty_utility_calls()
{
    const ULONG void_lvos[] = { 54, 60, 78, 84 };
    const ULONG zero_lvos[] = { 66, 72, 90, 96, 162, 168 };
    auto before = guest_bytes();
    check(utility_call(42, 0x12345678, 0, 0) == 0x12345678,
          "PackBoolTags empty-call dispatch");
    for (ULONG lvo : void_lvos) utility_call(lvo, 0x12345678, 0, 0);
    for (ULONG lvo : zero_lvos)
        check(utility_call(lvo, 0, lvo == 72 ? 0xFFFFFFFF : 0, 0) == 0,
              "empty or invalid utility input still uses the manual handler", lvo);
    expect_bytes(before, "empty utility calls never change guest memory");
}

static void put_array(ULONG addr, std::initializer_list<ULONG> values)
{
    for (ULONG value : values)
    {
        m68k_write32(&fixture.ctx, addr, value);
        addr += 4;
    }
}

static const ULONG chain_items[] = { 0x400, 0x420, 0x500, 0x508, 0x510 };

static void mutable_chain()
{
    clear_tags();
    put_tags(0x400, { { U1, 1 }, { TAG_IGNORE, U3 }, { TAG_SKIP, 1 },
                     { U4, 123 }, { U4, 44 }, { TAG_MORE, 0x500 }, { U3, 999 } });
    put_tags(0x500, { { U2, 0 }, { U1, 0 }, { U3, 8192 }, { TAG_DONE, 0xFAFA } });
}

static void test_packbooltags()
{
    mutable_chain();
    put_tags(0x600, { { U1, 3 }, { TAG_IGNORE, 0 }, { TAG_SKIP, 1 },
                     { U3, 0xFF }, { TAG_MORE, 0x680 } });
    put_tags(0x680, { { U2, 0x80000004 }, { U3, 0x10 }, { U1, 0xFF }, { TAG_DONE, 0 } });
    auto before = guest_bytes();
    check(utility_call(LVO_PACKBOOLTAGS, 0x80000000, 0x400, 0x600) == 0x10,
          "PackBoolTags follows both chains, first map match and last boolean duplicate");
    check(utility_call(LVO_PACKBOOLTAGS, 0xAA, 0, 0x600) == 0xAA,
          "PackBoolTags keeps initial flags for a NULL tag list");
    check(utility_call(LVO_PACKBOOLTAGS, 0xAA, 0x400, 0) == 0xAA,
          "PackBoolTags keeps initial flags for a NULL boolean map");
    expect_bytes(before, "PackBoolTags is read only");
    put_tags(0x700, { { U1, 1 }, { U2, 0 }, { TAG_MORE, 0x700 } });
    check(utility_call(LVO_PACKBOOLTAGS, 0x80000000, 0x700, 0x600) == 3,
          "PackBoolTags visits a ring once");
    put_tags(0x680, { { U2, 0x80000004 }, { TAG_MORE, 0x600 } });
    check(utility_call(LVO_PACKBOOLTAGS, 0, 0x400, 0x600) == 0,
          "PackBoolTags terminates a cyclic boolean map with a missing tag");
    put_tags(0xFF8, { { U1, 1 } });
    check(utility_call(LVO_PACKBOOLTAGS, 0, 0xFF8, 0x600) == 3,
          "PackBoolTags includes a complete last item before an invalid continuation");
    for (ULONG bad : { 0x1000u, 0xFF9u, 0xFFFFFFFCu })
        check(utility_call(LVO_PACKBOOLTAGS, 0x55, bad, bad) == 0x55,
              "PackBoolTags refuses invalid item addresses");
}

static void test_filtertagchanges()
{
    for (ULONG apply : { 0u, 1u, 0x80000000u })
    {
        mutable_chain();
        put_tags(0x600, { { U1, 1 }, { TAG_MORE, 0x680 } });
        put_tags(0x680, { { U2, 55 }, { U3, 8192 }, { TAG_DONE, 0 } });
        auto want = guest_bytes();
        expected32(want, 0x400, TAG_IGNORE);
        expected32(want, 0x510, TAG_IGNORE);
        if (apply)
        {
            expected32(want, 0x604, 0);
            expected32(want, 0x684, 0);
        }
        utility_call(LVO_FILTERTAGCHANGES, apply, 0x400, 0x600);
        expect_bytes(want, "FilterTagChanges ignores equal tags and optionally applies only data");
    }
    clear_tags();
    put_tags(0x400, { { U1, 2 }, { U1, 2 }, { TAG_MORE, 0x400 } });
    put_tags(0x600, { { U1, 1 }, { TAG_MORE, 0x600 } });
    auto want = guest_bytes();
    expected32(want, 0x604, 2);
    expected32(want, 0x408, TAG_IGNORE);
    utility_call(LVO_FILTERTAGCHANGES, 1, 0x400, 0x600);
    expect_bytes(want, "FilterTagChanges sees earlier applied duplicates and terminates both rings");
    put_tags(0xFF8, { { U1, 2 } });
    want = guest_bytes();
    expected32(want, 0xFF8, TAG_IGNORE);
    utility_call(LVO_FILTERTAGCHANGES, 0, 0xFF8, 0x600);
    expect_bytes(want, "FilterTagChanges can change the complete last item of the arena");
    for (ULONG bad : { 0u, 0x1000u, 0xFF9u, 0xFFFFFFFCu })
    {
        auto before = guest_bytes();
        utility_call(LVO_FILTERTAGCHANGES, 1, bad, 0x600);
        utility_call(LVO_FILTERTAGCHANGES, 1, 0x400, bad);
        expect_bytes(before, "FilterTagChanges makes no write for empty or invalid input");
    }
    mutable_chain();
    want = guest_bytes();
    for (ULONG item : chain_items) expected32(want, item, TAG_IGNORE);
    utility_call(LVO_FILTERTAGCHANGES, 1, 0x400, 0x400);
    expect_bytes(want, "FilterTagChanges supports identical change and original lists");
}

static void test_maptags()
{
    for (ULONG type : { 0u, 1u, 2u, 0xFFFFFFFFu })
    {
        mutable_chain();
        put_tags(0x600, { { U1, U3 }, { TAG_MORE, 0x680 } });
        put_tags(0x680, { { U2, TAG_DONE }, { U3, U4 }, { U1, U2 }, { TAG_DONE, 0 } });
        auto want = guest_bytes();
        expected32(want, 0x400, U3);
        expected32(want, 0x508, U3);
        expected32(want, 0x500, TAG_IGNORE);
        expected32(want, 0x510, U4);
        if (type == MAP_REMOVE_NOT_FOUND) expected32(want, 0x420, TAG_IGNORE);
        utility_call(LVO_MAPTAGS, type, 0x400, 0x600);
        expect_bytes(want, "MapTags changes only user tag words with AROS MAP type rules");
    }
    for (ULONG control : { (ULONG)TAG_IGNORE, (ULONG)TAG_MORE, (ULONG)TAG_SKIP })
    {
        clear_tags();
        put_tags(0x400, { { U1, 0x400 }, { U2, 22 }, { U3, 33 }, { TAG_MORE, 0x400 } });
        put_tags(0x600, { { U1, control }, { U2, U4 }, { TAG_MORE, 0x600 } });
        auto want = guest_bytes();
        expected32(want, 0x400, control);
        expected32(want, 0x408, U4);
        utility_call(LVO_MAPTAGS, MAP_KEEP_NOT_FOUND, 0x400, 0x600);
        expect_bytes(want, "MapTags snapshots a ring before mapping user tags to controls");
    }
    mutable_chain();
    auto want = guest_bytes();
    utility_call(LVO_MAPTAGS, MAP_KEEP_NOT_FOUND, 0x400, 0);
    expect_bytes(want, "MapTags keeps all tags for a NULL map and KEEP");
    for (ULONG item : chain_items) expected32(want, item, TAG_IGNORE);
    utility_call(LVO_MAPTAGS, MAP_REMOVE_NOT_FOUND, 0x400, 0xFFFFFFFC);
    expect_bytes(want, "MapTags treats an invalid map as no matches");
    put_tags(0xFF8, { { U1, 1 } });
    want = guest_bytes();
    expected32(want, 0xFF8, TAG_IGNORE);
    utility_call(LVO_MAPTAGS, MAP_REMOVE_NOT_FOUND, 0xFF8, 0);
    expect_bytes(want, "MapTags processes a complete last item then stops at the bad continuation");
    for (ULONG bad : { 0u, 0x1000u, 0xFF9u, 0xFFFFFFFCu })
    {
        auto before = guest_bytes();
        utility_call(LVO_MAPTAGS, MAP_REMOVE_NOT_FOUND, bad, 0x600);
        expect_bytes(before, "MapTags never writes an invalid first item");
    }
    mutable_chain();
    auto before = guest_bytes();
    const size_t live = native_blocks.size();
    fail_native_after = 0;
    utility_call(LVO_MAPTAGS, MAP_REMOVE_NOT_FOUND, 0x400, 0);
    expect_bytes(before, "MapTags leaves the list whole if its native snapshot fails");
    check(native_blocks.size() == live, "MapTags failure leaks no snapshot");
}

static void test_tag_arrays()
{
    clear_tags();
    put_array(0x700, { U1, TAG_IGNORE, TAG_MORE, TAG_SKIP, U3, TAG_DONE, U4 });
    auto before = guest_bytes();
    for (ULONG value : { U1, (ULONG)TAG_IGNORE, (ULONG)TAG_MORE, (ULONG)TAG_SKIP, U3 })
        check(utility_call(LVO_TAGINARRAY, value, 0x700) == 1,
              "TagInArray reads contiguous BE32 values, including control tag values");
    for (ULONG value : { (ULONG)TAG_DONE, U2, U4 })
        check(utility_call(LVO_TAGINARRAY, value, 0x700) == 0,
              "TagInArray excludes TAG_DONE and values beyond the terminator");
    expect_bytes(before, "TagInArray is read only");
    put_array(0xFF8, { U1, TAG_DONE });
    check(utility_call(LVO_TAGINARRAY, U1, 0xFF8) == 1,
          "TagInArray accepts TAG_DONE ending exactly at the arena boundary");
    check(utility_call(LVO_TAGINARRAY, U1, 0xFFC) == 0,
          "TagInArray accepts an empty array at the last longword");
    m68k_write32(&fixture.ctx, 0xFFC, U2);
    for (ULONG bad : { 0u, 0xFF8u, 0xFFDu, 0x1000u, 0xFFFFFFFCu })
        check(utility_call(LVO_TAGINARRAY, U1, bad) == 0,
              "TagInArray requires an in-arena terminator even after an early match");
}

static void test_filtertagitems()
{
    for (ULONG logic : { 0u, 1u, 2u, 0xFFFFFFFFu })
    {
        mutable_chain();
        put_array(0x700, { U1, U3, TAG_DONE });
        auto want = guest_bytes();
        if (logic == TAGFILTER_AND)
        {
            expected32(want, 0x420, TAG_IGNORE);
            expected32(want, 0x500, TAG_IGNORE);
        }
        else if (logic == TAGFILTER_NOT)
            for (ULONG item : { 0x400u, 0x508u, 0x510u }) expected32(want, item, TAG_IGNORE);
        const ULONG valid = utility_call(LVO_FILTERTAGITEMS, logic, 0x400, 0x700);
        check(valid == (logic == TAGFILTER_AND ? 3u : logic == TAGFILTER_NOT ? 2u : 0u),
              "FilterTagItems returns surviving user items, or zero for an unknown logic");
        expect_bytes(want, "FilterTagItems changes only the selected ti_Tag words");
    }
    for (ULONG logic : { 0u, 1u })
    {
        mutable_chain();
        put_array(0x700, { TAG_DONE });
        auto want = guest_bytes();
        if (logic == TAGFILTER_AND)
            for (ULONG item : chain_items) expected32(want, item, TAG_IGNORE);
        check(utility_call(LVO_FILTERTAGITEMS, logic, 0x400, 0x700) == (logic ? 5u : 0u),
              "FilterTagItems supports both logics with an empty array");
        expect_bytes(want, "an empty filter array preserves data and control tags");
    }
    mutable_chain();
    put_array(0xFF8, { U1, U2 });
    for (ULONG bad : { 0u, 0xFF8u, 0xFFDu, 0x1000u, 0xFFFFFFFCu })
    {
        auto before = guest_bytes();
        check(utility_call(LVO_FILTERTAGITEMS, TAGFILTER_AND, 0x400, bad) == 0,
              "FilterTagItems rejects a bad or unterminated filter array");
        expect_bytes(before, "invalid filter arrays cause no partial change");
    }
    put_array(0x700, { U1, TAG_DONE });
    clear_tags();
    put_array(0x700, { U1, TAG_DONE });
    put_tags(0x400, { { U1, 1 }, { U2, 2 }, { TAG_MORE, 0x400 } });
    auto want = guest_bytes();
    expected32(want, 0x408, TAG_IGNORE);
    check(utility_call(LVO_FILTERTAGITEMS, TAGFILTER_AND, 0x400, 0x700) == 1,
          "FilterTagItems counts a user ring once");
    expect_bytes(want, "FilterTagItems leaves the ring's control link alone");
    put_tags(0xFF8, { { U2, 2 } });
    want = guest_bytes();
    expected32(want, 0xFF8, TAG_IGNORE);
    check(utility_call(LVO_FILTERTAGITEMS, TAGFILTER_AND, 0xFF8, 0x700) == 0,
          "FilterTagItems processes the complete last item before an invalid continuation");
    expect_bytes(want, "FilterTagItems writes no partial item past the arena");
    for (ULONG bad : { 0u, 0xFF9u, 0x1000u, 0xFFFFFFFCu })
    {
        auto before = guest_bytes();
        check(utility_call(LVO_FILTERTAGITEMS, TAGFILTER_NOT, bad, 0x700) == 0,
              "FilterTagItems rejects an invalid list start");
        expect_bytes(before, "an invalid list start causes no filter write");
    }
}

static void put_string(ULONG addr, const char *s)
{
    std::memcpy(fixture.memory + addr, s, std::strlen(s) + 1);
}

static void test_string_comparisons()
{
    clear_tags();
    put_string(0x400, "AmIgA");
    put_string(0x500, "aMiGa");
    auto before = guest_bytes();
    check(utility_call(LVO_STRICMP, 0xAABBCCDD, 0x400, 0x500) == 0,
          "Stricmp folds ASCII case using A0 and A1");
    check(utility_call(LVO_STRNICMP, 3, 0x400, 0x500) == 0,
          "Strnicmp folds ASCII case");
    expect_bytes(before, "string comparisons write no guest bytes");
    put_string(0x400, "ABx");
    put_string(0x500, "aby");
    check(utility_call(LVO_STRNICMP, 2, 0x400, 0x500) == 0,
          "Strnicmp stops at the length in D0");
    for (ULONG length : { 3u, 100u, 0xFFFFFFFFu, 0x80000000u })
        check(utility_call(LVO_STRNICMP, length, 0x400, 0x500) == 0xFFFFFFFF,
              "Strnicmp returns the folded byte difference, including negative LONG lengths");
    check(utility_call(LVO_STRICMP, 0, 0x500, 0x400) == 1,
          "Stricmp returns a positive byte difference in the reverse order");
    put_string(0x400, "");
    put_string(0x500, "Z");
    check(utility_call(LVO_STRICMP, 0, 0x400, 0x500) == s(-122),
          "Stricmp compares NUL to the lowercased next byte");
    fixture.memory[0xFFF] = 0;
    check(utility_call(LVO_STRICMP, 0, 0xFFF, 0x400) == 0,
          "Stricmp accepts a NUL at the very last arena byte");
    put_string(0xFFD, "Az");
    put_string(0x500, "aZ");
    check(utility_call(LVO_STRICMP, 0, 0xFFD, 0x500) == 0,
          "Stricmp accepts a string ending exactly at the arena boundary");
    check(utility_call(LVO_STRNICMP, 8, 0xFFD, 0x500) == 0,
          "Strnicmp stops at NUL before a longer length");

    /* Independent byte-domain oracle for rom/utility/tolower.c. This also
       covers D7, DF, E7 and FF, whose rules differ from common ctype maps. */
    bool case_rules = true;
    for (ULONG c = 1; c < 256; c++)
    {
        const ULONG folded = ((c >= 0x41 && c <= 0x5A) ||
                              (c >= 0xC0 && c <= 0xDE && c != 0xD7)) ? c + 32 : c;
        fixture.memory[0x400] = (UBYTE)c;
        fixture.memory[0x401] = 0;
        fixture.memory[0x500] = (UBYTE)folded;
        fixture.memory[0x501] = 0;
        case_rules = (utility_call(LVO_STRICMP, 0, 0x400, 0x500) == 0) && case_rules;
        case_rules = (utility_call(LVO_STRNICMP, 1, 0x400, 0x500) == 0) && case_rules;
        fixture.memory[0x500] = '!';
        case_rules = (utility_call(LVO_STRICMP, 0, 0x400, 0x500) == (ULONG)((LONG)folded - '!')) && case_rules;
    }
    check(case_rules, "Stricmp and Strnicmp match all 255 non-NUL unsigned byte case rules");
    put_string(0x400, "abc");
    std::memset(fixture.memory + 0xFF0, 'x', 16);
    for (ULONG bad : { 0u, 0xFF0u, 0xFFFu, 0x1000u, 0xFFFFFFFFu })
    {
        before = guest_bytes();
        check(utility_call(LVO_STRICMP, 0, bad, 0x400) == 0 &&
              utility_call(LVO_STRICMP, 0, 0x400, bad) == 0,
              "Stricmp returns zero for either invalid or unterminated string");
        check(utility_call(LVO_STRNICMP, 0, bad, bad) == 0,
              "Strnicmp with zero length does not read either pointer");
        expect_bytes(before, "invalid string comparisons leave memory unchanged");
    }
    for (ULONG bad : { 0u, 0x1000u, 0xFFFFFFFFu })
        check(utility_call(LVO_STRNICMP, 1, bad, 0x400) == 0 &&
              utility_call(LVO_STRNICMP, 1, 0x400, bad) == 0,
              "Strnicmp returns zero when its first byte lies outside guest memory");

    /* rom/utility/strnicmp.c reads only the bytes it compares, so a field
       without NUL works up to the end of guest memory. */
    put_string(0x500, "B");
    put_string(0x510, "C");
    fixture.memory[0xFFF] = 'b';
    before = guest_bytes();
    check(utility_call(LVO_STRNICMP, 1, 0xFFF, 0x500) == 0 &&
          utility_call(LVO_STRNICMP, 1, 0x500, 0xFFF) == 0,
          "Strnicmp compares a last guest byte without NUL within its length");
    check(utility_call(LVO_STRNICMP, 1, 0xFFF, 0x510) == s(-1) &&
          utility_call(LVO_STRNICMP, 1, 0x510, 0xFFF) == 1,
          "Strnicmp returns the difference of a last guest byte without NUL");
    check(utility_call(LVO_STRNICMP, 100, 0xFF0, 0x400) == 'x' - 'a' &&
          utility_call(LVO_STRNICMP, 0xFFFFFFFF, 0x400, 0xFF0) == s('a' - 'x'),
          "Strnicmp returns an early difference before an unterminated tail");
    check(utility_call(LVO_STRNICMP, 2, 0xFFF, 0x500) == 0 &&
          utility_call(LVO_STRNICMP, 2, 0x500, 0xFFF) == 0,
          "Strnicmp returns zero when its compared bytes reach the end of guest memory");
    expect_bytes(before, "string comparisons at the end of guest memory write no guest bytes");
}

static void test_overlapping_tags()
{
    for (ULONG lvo : { (ULONG)LVO_MAPTAGS, (ULONG)LVO_FILTERTAGITEMS, (ULONG)LVO_FILTERTAGCHANGES })
    {
        clear_tags();
        /* The TAG_MORE at 0x404 overlaps two user items. Changing the tag
           at 0x408 changes that link, so the original walk must be saved. */
        put_tags(0x400, { { U1, TAG_MORE }, { 0x500, U2 }, { TAG_MORE, 0x404 } });
        put_tags(0x500, { { U3, 0 }, { TAG_MORE, 0x400 } });
        put_tags(0x600, { { U1, TAG_MORE }, { 0x500, U2 }, { U3, 0 }, { TAG_DONE, 0 } });
        put_array(0x700, { TAG_DONE });
        auto want = guest_bytes();
        for (ULONG item : { 0x400u, 0x408u, 0x500u }) expected32(want, item, TAG_IGNORE);
        utility_call(lvo, 0, 0x400,
                     lvo == LVO_FILTERTAGCHANGES ? 0x600 : lvo == LVO_FILTERTAGITEMS ? 0x700 : 0);
        expect_bytes(want, "mutating utility walks visit every original user item once despite overlapping control links");
    }
    mutable_chain();
    put_array(0x700, { U1, TAG_DONE });
    for (ULONG lvo : { (ULONG)LVO_MAPTAGS, (ULONG)LVO_FILTERTAGITEMS, (ULONG)LVO_FILTERTAGCHANGES })
    {
        auto before = guest_bytes();
        const size_t live = native_blocks.size();
        fail_native_after = 0;
        utility_call(lvo, 0, 0x400, 0x700);
        expect_bytes(before, "a failed mutation snapshot changes no guest memory");
        check(native_blocks.size() == live && fail_native_after == -1,
              "mutation snapshot failure is consumed and leaks no native memory");
    }
}

static bool guest_block(ULONG addr, ULONG count)
{
    const bool valid = addr && addr < fixture.ctx.mem_size &&
                       fixture.ctx.mem_size - addr >= count * 8;
    check(valid, "utility allocation returns a complete guest range", addr, count);
    return valid;
}

static void free_tags(ULONG addr)
{
    utility_call(LVO_FREETAGITEMS, 0xAABBCCDD, addr);
}

static void test_allocated_tags()
{
    mutable_chain();
    fail_native_after = -1;
    fixture.ctx.heap_start = 0x800;
    fixture.ctx.heap_end = 0xF00;
    check(Kaffeine_HeapInit(&fixture.ctx), "initialize the production guest heap for utility allocations");
    const ULONG available = Kaffeine_HeapAvail(&fixture.ctx, 0);
    const size_t live = native_blocks.size();
    ULONG block = utility_call(LVO_ALLOCATETAGITEMS, 4, 0xFFFFFFFC);
    if (!guest_block(block, 4)) return;
    check(Kaffeine_TagItemsCapacity(&fixture.ctx, block) == 4,
          "AllocateTagItems allocates exactly numTags, with no hidden count or terminator");
    bool cleared = true;
    for (ULONG i = 0; i < 32; i++) cleared = cleared && fixture.memory[block + i] == 0;
    check(cleared, "AllocateTagItems clears every guest tag and data longword");
    std::memset(fixture.memory + block, 0xA5, 32);
    free_tags(block);
    ULONG reused = utility_call(LVO_ALLOCATETAGITEMS, 4, 0);
    check(reused == block, "AllocateTagItems reuses the freed guest block");
    cleared = true;
    for (ULONG i = 0; i < 32; i++) cleared = cleared && fixture.memory[reused + i] == 0;
    check(cleared, "AllocateTagItems clears a reused block");
    free_tags(reused);
    for (ULONG count : { 0u, 0x20000000u, 0xFFFFFFFFu, 0x1FFFFFFFu })
    {
        auto before = guest_bytes();
        check(utility_call(LVO_ALLOCATETAGITEMS, count, 0) == 0,
              "AllocateTagItems returns NULL for zero, overflow or an impossible count");
        expect_bytes(before, "failed tag allocation leaves the guest heap whole");
    }
    ULONG clone = utility_call(LVO_CLONETAGITEMS, 0, 0x400);
    if (!guest_block(clone, 6)) return;
    check(Kaffeine_TagItemsCapacity(&fixture.ctx, clone) == 6,
          "CloneTagItems flattens five user items plus one cleared TAG_DONE");
    bool copied = true;
    for (unsigned i = 0; i < 5; i++)
        copied = copied && m68k_read32(&fixture.ctx, clone + i * 8) == m68k_read32(&fixture.ctx, chain_items[i]) &&
                 m68k_read32(&fixture.ctx, clone + i * 8 + 4) == m68k_read32(&fixture.ctx, chain_items[i] + 4);
    check(copied && m68k_read32(&fixture.ctx, clone + 40) == TAG_DONE &&
          m68k_read32(&fixture.ctx, clone + 44) == 0,
          "CloneTagItems copies both BE32 words, preserves scalar data and appends TAG_DONE");
    for (ULONG addr : chain_items)
        check(m68k_read32(&fixture.ctx, addr) != TAG_IGNORE,
              "CloneTagItems leaves the source items unchanged");

    /* AROS Refresh copies user pairs only. Even a modified terminator and
       its data are left alone for a non-NULL original. */
    for (ULONG i = 0; i < 6; i++) put_tags(clone + i * 8, { { TAG_IGNORE, 0xDEADBEEF } });
    auto want = guest_bytes();
    for (ULONG i = 0; i < 5; i++)
    {
        expected32(want, clone + i * 8, m68k_read32(&fixture.ctx, chain_items[i]));
        expected32(want, clone + i * 8 + 4, m68k_read32(&fixture.ctx, chain_items[i] + 4));
    }
    utility_call(LVO_REFRESHTAGITEMCLONES, 0x12345678, clone, 0x400);
    expect_bytes(want, "RefreshTagItemClones restores tag and data pairs, preserving the entire tail");
    want = guest_bytes();
    expected32(want, clone, TAG_DONE);
    utility_call(LVO_REFRESHTAGITEMCLONES, 0x12345678, clone, 0);
    expect_bytes(want, "RefreshTagItemClones with NULL original writes only the first tag");
    put_tags(0x600, { { TAG_DONE, 0 } });
    auto before = guest_bytes();
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, 0x600);
    expect_bytes(before, "an empty non-NULL original copies nothing, including its terminator");
    for (ULONG i = 0; i < 7; i++) put_tags(0x600 + i * 8, { { U1, i } });
    put_tags(0x638, { { TAG_DONE, 0 } });
    before = guest_bytes();
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, 0x600);
    expect_bytes(before, "RefreshTagItemClones rejects an oversized source before any write");
    before = guest_bytes();
    for (ULONG bad : { 0u, clone + 8, 0x300u, 0x1000u, 0xFFFFFFFCu })
    {
        utility_call(LVO_REFRESHTAGITEMCLONES, 0, bad, 0x400);
        expect_bytes(before, "RefreshTagItemClones ignores foreign, interior and invalid destinations");
    }
    /* One user slot per allocated item leaves no room for TAG_DONE. */
    put_tags(0x630, { { TAG_DONE, 0 } });
    before = guest_bytes();
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, 0x600);
    expect_bytes(before, "RefreshTagItemClones rejects a source filling the terminator slot");
    put_tags(0x600, { { U1, 1 }, { TAG_MORE, 0x600 } });
    for (ULONG bad : { 0x600u, 0xFF9u, 0x1000u, 0xFFFFFFFCu })
    {
        before = guest_bytes();
        utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, bad);
        expect_bytes(before, "RefreshTagItemClones rejects cyclic and invalid sources before copying");
    }
    before = guest_bytes();
    fail_native_after = 0;
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, 0x400);
    expect_bytes(before, "RefreshTagItemClones leaves the destination whole on native allocation failure");
    free_tags(clone);
    before = guest_bytes();
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, clone, 0x400);
    free_tags(clone);
    expect_bytes(before, "refreshing or freeing an already freed clone is harmless");

    for (ULONG list : { 0u, 0x700u })
    {
        put_tags(0x700, { { TAG_MORE, 0 } });
        clone = utility_call(LVO_CLONETAGITEMS, 0, list);
        if (guest_block(clone, 1))
            check(m68k_read32(&fixture.ctx, clone) == TAG_DONE &&
                  m68k_read32(&fixture.ctx, clone + 4) == 0,
                  "CloneTagItems returns an allocated empty list for NULL or a NULL TAG_MORE");
        free_tags(clone);
    }
    put_tags(0x700, { { TAG_MORE, 0x700 } });
    put_tags(0xFF8, { { U1, 1 } });
    put_tags(0x710, { { U1, 11 }, { TAG_MORE, 0xFFFFFFFC } });
    put_tags(0x730, { { U1, 11 }, { TAG_SKIP, 0xFFFFFFFF } });
    for (ULONG bad : { 0x600u, 0x700u, 0x710u, 0x730u, 0xFF8u, 0xFF9u, 0x1000u, 0xFFFFFFFCu })
    {
        before = guest_bytes();
        check(utility_call(LVO_CLONETAGITEMS, 0, bad) == 0,
              "CloneTagItems rejects rings and lists without an in-arena end");
        expect_bytes(before, "a rejected clone allocates no guest memory");
    }
    put_tags(0xFF8, { { TAG_DONE, 0xA5 } });
    clone = utility_call(LVO_CLONETAGITEMS, 0, 0xFF8);
    if (guest_block(clone, 1))
        check(m68k_read32(&fixture.ctx, clone) == TAG_DONE,
              "CloneTagItems accepts a complete terminator at the end of guest memory");
    free_tags(clone);

    for (int fail_after : { 0, 1 })
    {
        before = guest_bytes();
        fail_native_after = fail_after;
        check(utility_call(LVO_CLONETAGITEMS, 0, 0x400) == 0,
              "CloneTagItems handles snapshot and heap record allocation failures");
        expect_bytes(before, "CloneTagItems allocation failure leaves guest memory unchanged");
        check(native_blocks.size() == live, "CloneTagItems allocation failure frees native scratch memory");
    }
    before = guest_bytes();
    fail_native_after = 0;
    check(utility_call(LVO_ALLOCATETAGITEMS, 2, 0) == 0,
          "AllocateTagItems handles native ownership record exhaustion");
    expect_bytes(before, "failed ownership bookkeeping changes no guest bytes");

    ULONG foreign = Kaffeine_HeapAlloc(&fixture.ctx, 32, 0);
    ULONG pool = Kaffeine_PoolCreate(&fixture.ctx, 0, 64, 16);
    ULONG member = Kaffeine_PoolAlloc(&fixture.ctx, pool, 24);
    block = utility_call(LVO_ALLOCATETAGITEMS, 3, 0);
    if (!guest_block(block, 3)) return;
    m68k_write32(&fixture.ctx, foreign - 8, 0xFFFFFFFF);
    m68k_write32(&fixture.ctx, foreign - 4, block);
    before = guest_bytes();
    for (ULONG bad : { 0u, 0x300u, foreign, pool, member, block + 8, block - 8,
                       0x1000u, 0xFFFFFFFCu }) free_tags(bad);
    expect_bytes(before, "FreeTagItems ignores static, foreign heap, pool, interior and invalid pointers");
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, foreign, 0);
    utility_call(LVO_REFRESHTAGITEMCLONES, 0, member, 0x400);
    expect_bytes(before, "refresh cannot claim ordinary or pooled blocks through forged guest headers");
    check(Kaffeine_TagItemsCapacity(&fixture.ctx, block) == 3,
          "foreign frees cannot steal the utility allocation");
    free_tags(block);
    ULONG recycled = Kaffeine_HeapAlloc(&fixture.ctx, 24, 0);
    check(recycled == block, "the ordinary guest heap can reuse a freed tag block");
    before = guest_bytes();
    free_tags(block);
    expect_bytes(before, "a stale tag pointer cannot free a block now owned by ordinary AllocMem");
    Kaffeine_HeapFree(&fixture.ctx, recycled, 24);
    Kaffeine_HeapFree(&fixture.ctx, foreign, 32);
    Kaffeine_PoolDelete(&fixture.ctx, pool);
    check(Kaffeine_HeapAvail(&fixture.ctx, 0) == available && native_blocks.size() == live,
          "freeing guest tag blocks restores heap capacity and native record count");
    block = utility_call(LVO_ALLOCATETAGITEMS, available / 8, 0);
    if (!guest_block(block, available / 8)) return;
    check(Kaffeine_HeapAvail(&fixture.ctx, 0) == 0,
          "AllocateTagItems can consume the exact remaining heap payload");
    check(utility_call(LVO_ALLOCATETAGITEMS, 1, 0) == 0 &&
          utility_call(LVO_CLONETAGITEMS, 0, 0x400) == 0,
          "AllocateTagItems and CloneTagItems return NULL when the guest heap is full");
    free_tags(block);
    check(Kaffeine_HeapAvail(&fixture.ctx, 0) == available,
          "FreeTagItems restores an exactly exhausted heap");
    check(guest_block(utility_call(LVO_ALLOCATETAGITEMS, 2, 0), 2) &&
          guest_block(utility_call(LVO_CLONETAGITEMS, 0, 0x400), 6),
          "leave live tag allocations for the context destruction cleanup check");
}

/* Amiga2Date(), Date2Amiga() and CheckDate() */

static constexpr ULONG CLOCKDATA_SIZE = 14;

static void put_clockdata(ULONG addr, const ClockData &cd)
{
    m68k_write16(&fixture.ctx, addr,      cd.sec);
    m68k_write16(&fixture.ctx, addr + 2,  cd.min);
    m68k_write16(&fixture.ctx, addr + 4,  cd.hour);
    m68k_write16(&fixture.ctx, addr + 6,  cd.mday);
    m68k_write16(&fixture.ctx, addr + 8,  cd.month);
    m68k_write16(&fixture.ctx, addr + 10, cd.year);
    m68k_write16(&fixture.ctx, addr + 12, cd.wday);
}

static ClockData get_clockdata(ULONG addr)
{
    ClockData cd = {};
    cd.sec   = m68k_read16(&fixture.ctx, addr);
    cd.min   = m68k_read16(&fixture.ctx, addr + 2);
    cd.hour  = m68k_read16(&fixture.ctx, addr + 4);
    cd.mday  = m68k_read16(&fixture.ctx, addr + 6);
    cd.month = m68k_read16(&fixture.ctx, addr + 8);
    cd.year  = m68k_read16(&fixture.ctx, addr + 10);
    cd.wday  = m68k_read16(&fixture.ctx, addr + 12);
    return cd;
}

static bool same_clockdata(const ClockData &a, const ClockData &b)
{
    return a.sec == b.sec && a.min == b.min && a.hour == b.hour &&
           a.mday == b.mday && a.month == b.month && a.year == b.year &&
           a.wday == b.wday;
}

static void call_amiga2date(ULONG seconds, ULONG addr)
{
    fixture.call(LVO_AMIGA2DATE, seconds, 0xD1D1D1D1u, addr);
    check(fixture.state.d[1] == 0xD1D1D1D1u, "Amiga2Date leaves D1 alone",
          fixture.state.d[1], 0xD1D1D1D1u);
}

static ULONG call_date2amiga(ULONG addr)
{
    fixture.call(LVO_DATE2AMIGA, 0xD0D0D0D0u, 0xD1D1D1D1u, addr);
    check(fixture.state.d[1] == 0xD1D1D1D1u, "Date2Amiga leaves D1 alone",
          fixture.state.d[1], 0xD1D1D1D1u);
    return fixture.state.d[0];
}

static ULONG call_checkdate(ULONG addr)
{
    fixture.call(LVO_CHECKDATE, 0xD0D0D0D0u, 0xD1D1D1D1u, addr);
    check(fixture.state.d[1] == 0xD1D1D1D1u, "CheckDate leaves D1 alone",
          fixture.state.d[1], 0xD1D1D1D1u);
    return fixture.state.d[0];
}

static void expect_known_date(ULONG seconds, const ClockData &want, const char *what)
{
    const ULONG addr = 0x400;
    UBYTE before[sizeof(fixture.memory)];

    std::memset(fixture.memory + 0x100, 0xAA, sizeof(fixture.memory) - 0x100);
    std::memcpy(before, fixture.memory, sizeof(before));

    call_amiga2date(seconds, addr);
    ClockData got = get_clockdata(addr);
    check(same_clockdata(got, want), what, seconds, got.year);

    std::memcpy(before + addr, fixture.memory + addr, CLOCKDATA_SIZE);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "Amiga2Date writes only the 14-byte guest ClockData and makes no shadow", seconds);

    std::memcpy(before, fixture.memory, sizeof(before));
    ULONG got_d2a = call_date2amiga(addr);
    check(got_d2a == seconds, what, got_d2a, seconds);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "Date2Amiga writes no guest memory and makes no shadow", got_d2a);

    ULONG got_chk = call_checkdate(addr);
    /* rom/utility/checkdate.c returns Date2Amiga(date), which is 0 at the epoch. */
    check(got_chk == seconds, what, got_chk, seconds);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          fixture.ctx.num_shadows == 0,
          "CheckDate writes no guest memory and makes no shadow", got_chk);
}

static void test_dates_known()
{
    /* 1978-01-01 00:00:00 is 0 seconds (Sunday, wday 0) */
    expect_known_date(0u, { 0, 0, 0, 1, 1, 1978, 0 },
                      "1978-01-01 00:00:00 is 0 seconds");
    expect_known_date(1u, { 1, 0, 0, 1, 1, 1978, 0 },
                      "1978-01-01 00:00:01 is 1 second");
    expect_known_date(31535999u, { 59, 59, 23, 31, 12, 1978, 0 },
                      "1978-12-31 23:59:59 is the last second of 1978");
    expect_known_date(94694399u, { 59, 59, 23, 31, 12, 1980, 3 },
                      "1980-12-31 23:59:59 is the last second of leap year 1980");
    expect_known_date(699321600u, { 0, 0, 0, 29, 2, 2000, 2 },
                      "2000-02-29 00:00:00 is a leap day");
    expect_known_date(699366896u, { 56, 34, 12, 29, 2, 2000, 2 },
                      "2000-02-29 12:34:56 on a leap day");
    expect_known_date(725846399u, { 59, 59, 23, 31, 12, 2000, 0 },
                      "2000-12-31 23:59:59 is the last second of 2000");
    expect_known_date(3855081599u, { 59, 59, 23, 28, 2, 2100, 0 },
                      "2100-02-28 23:59:59 before March 1 of non-leap century year 2100");
    expect_known_date(3855081600u, { 0, 0, 0, 1, 3, 2100, 1 },
                      "2100-03-01 00:00:00 in non-leap century year 2100");
    expect_known_date(0xFFFFFFFFu, { 15, 28, 6, 7, 2, 2114, 3 },
                      "0xFFFFFFFF is 2014+100: 2114-02-07 06:28:15");

    /* Check the exact big-endian byte layout in guest memory for 0xFFFFFFFF. */
    const UBYTE want_bytes[CLOCKDATA_SIZE] =
    {
        0x00, 0x0F, /* sec = 15 */
        0x00, 0x1C, /* min = 28 */
        0x00, 0x06, /* hour = 6 */
        0x00, 0x07, /* mday = 7 */
        0x00, 0x02, /* month = 2 */
        0x08, 0x42, /* year = 2114 */
        0x00, 0x03  /* wday = 3 */
    };
    check(std::memcmp(fixture.memory + 0x400, want_bytes, CLOCKDATA_SIZE) == 0,
          "Amiga2Date stores the seven UWORD fields in big-endian byte order");
}

static void test_dates_roundtrip()
{
    const ULONG fixed[] =
    {
        0u, 1u, 59u, 60u, 3599u, 3600u, 86399u, 86400u,
        31535999u, 31536000u, 63071999u, 63072000u,
        68169599u, 68169600u, 68255999u, 68256000u, /* 1980-02-28..1980-03-01 */
        94694399u, 94694400u,
        699235199u, 699235200u, 699321599u, 699321600u, 699407999u, 699408000u,
        725846399u, 725846400u,
        3849983999u, 3849984000u, /* 2099-12-31 23:59:59 .. 2100-01-01 00:00:00 */
        3855081599u, 3855081600u, /* 2100-02-28 23:59:59 .. 2100-03-01 00:00:00 */
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu
    };

    bool fixed_ok = true;
    ULONG bad_fixed = 0, bad_fixed_got = 0;
    for (ULONG s_val : fixed)
    {
        call_amiga2date(s_val, 0x400);
        ULONG d2a = call_date2amiga(0x400);
        ULONG chk = call_checkdate(0x400);
        if (d2a != s_val || chk != s_val)
        {
            if (fixed_ok)
            {
                bad_fixed = s_val;
                bad_fixed_got = d2a != s_val ? d2a : chk;
            }
            fixed_ok = false;
        }
    }
    check(fixed_ok, "Date2Amiga(Amiga2Date(s)) == s and CheckDate(Amiga2Date(s)) == s on boundary timestamps",
          bad_fixed, bad_fixed_got);

    uint32_t seed = 0x9E3779B9u;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };
    bool rand_ok = true;
    ULONG bad_rand = 0, bad_rand_got = 0;
    for (int i = 0; i < 5000; i++)
    {
        ULONG s_val = next();
        ULONG addr = (i & 1) ? 0x402 : 0x400; /* test both 4-byte and word alignment */
        call_amiga2date(s_val, addr);
        ULONG d2a = call_date2amiga(addr);
        ULONG chk = call_checkdate(addr);
        if (d2a != s_val || chk != s_val)
        {
            if (rand_ok)
            {
                bad_rand = s_val;
                bad_rand_got = d2a != s_val ? d2a : chk;
            }
            rand_ok = false;
        }
    }
    check(rand_ok, "Date2Amiga(Amiga2Date(s)) == s and CheckDate(Amiga2Date(s)) == s on 5000 random timestamps",
          bad_rand, bad_rand_got);
}

static void expect_checkdate(const ClockData &cd, ULONG want, const char *what)
{
    put_clockdata(0x400, cd);
    ULONG got = call_checkdate(0x400);
    check(got == want, what, got, want);
}

static void test_checkdate_invalid()
{
    expect_checkdate({ 0, 0, 0, 15, 0, 2000, 0 }, 0, "CheckDate rejects month 0");
    expect_checkdate({ 0, 0, 0, 15, 13, 2000, 0 }, 0, "CheckDate rejects month 13");
    /* 0x0500 is 1280 in big-endian, but 5 if read little-endian without swapping */
    expect_checkdate({ 0, 0, 0, 15, 0x0500, 2000, 0 }, 0,
                     "CheckDate rejects a byte-swapped month 0x0500");
    expect_checkdate({ 0, 0, 0, 0, 1, 2000, 0 }, 0, "CheckDate rejects day 0");
    expect_checkdate({ 0, 0, 0, 31, 4, 2000, 0 }, 0, "CheckDate rejects April 31");
    expect_checkdate({ 0, 0, 0, 31, 6, 2000, 0 }, 0, "CheckDate rejects June 31");
    expect_checkdate({ 0, 0, 0, 31, 9, 2000, 0 }, 0, "CheckDate rejects September 31");
    expect_checkdate({ 0, 0, 0, 31, 11, 2000, 0 }, 0, "CheckDate rejects November 31");
    expect_checkdate({ 0, 0, 0, 32, 1, 2000, 0 }, 0, "CheckDate rejects January 32");
    expect_checkdate({ 0, 0, 0, 30, 2, 2000, 0 }, 0, "CheckDate rejects February 30 in a leap year");
    expect_checkdate({ 0, 0, 0, 30, 2, 1979, 0 }, 0, "CheckDate rejects February 30 in a non-leap year");
    expect_checkdate({ 0, 0, 24, 15, 5, 2000, 0 }, 0, "CheckDate rejects hour 24");
    expect_checkdate({ 0, 60, 12, 15, 5, 2000, 0 }, 0, "CheckDate rejects minute 60");
    expect_checkdate({ 61, 0, 12, 15, 5, 2000, 0 }, 0, "CheckDate rejects second 61");
    expect_checkdate({ 0, 0, 12, 15, 5, 1977, 0 }, 0, "CheckDate rejects year 1977 (< 1978)");
    expect_checkdate({ 0, 0, 12, 15, 5, 2000, 7 }, 0, "CheckDate rejects wday 7 (> 6)");
    expect_checkdate({ 0, 0, 0, 1, 1, 1978, 0 }, 0,
                     "CheckDate returns 0 for 1978-01-01 00:00:00, as AROS checkdate.c notes");

    /* rom/utility/checkdate.c lines 49 and 112-115: CheckDate() checks only
       date->mday > 29 in case 2 and does not check whether the year is a leap
       year, so 1979-02-29 returns Date2Amiga(date) (1979-03-01, 36633600). */
    expect_checkdate({ 0, 0, 0, 29, 2, 1979, 4 }, 36633600u,
                     "CheckDate on 1979-02-29 returns Date2Amiga() as AROS checkdate.c does");

    /* Date2Amiga() refuses out-of-range months instead of indexing dayspermonth[] out of bounds */
    put_clockdata(0x400, { 0, 0, 0, 15, 0, 2000, 0 });
    check(call_date2amiga(0x400) == 0, "Date2Amiga refuses month 0");
    put_clockdata(0x400, { 0, 0, 0, 15, 13, 2000, 0 });
    check(call_date2amiga(0x400) == 0, "Date2Amiga refuses month 13");
}

static void expect_bad_addr(ULONG addr, const char *what)
{
    UBYTE before[sizeof(fixture.memory)];
    std::memcpy(before, fixture.memory, sizeof(before));

    call_amiga2date(699366896u, addr);
    check(std::memcmp(fixture.memory + 0x100, before + 0x100, sizeof(before) - 0x100) == 0 &&
          std::memcmp(fixture.memory, before, 0x80) == 0 &&
          fixture.ctx.num_shadows == 0,
          what, addr);

    ULONG d2a = call_date2amiga(addr);
    check(d2a == 0, what, d2a, 0);
    ULONG chk = call_checkdate(addr);
    check(chk == 0, what, chk, 0);
}

static void test_dates_bounds()
{
    const ULONG end = sizeof(fixture.memory);
    const ClockData leap_day = { 56, 34, 12, 29, 2, 2000, 2 };

    /* A ClockData that ends exactly at the end of guest memory (end - 14) */
    std::memset(fixture.memory + 0x100, 0xCC, sizeof(fixture.memory) - 0x100);
    call_amiga2date(699366896u, end - CLOCKDATA_SIZE);
    check(same_clockdata(get_clockdata(end - CLOCKDATA_SIZE), leap_day),
          "Amiga2Date writes a ClockData that ends at the end of guest memory");
    check(call_date2amiga(end - CLOCKDATA_SIZE) == 699366896u,
          "Date2Amiga reads a ClockData that ends at the end of guest memory");
    check(call_checkdate(end - CLOCKDATA_SIZE) == 699366896u,
          "CheckDate reads a ClockData that ends at the end of guest memory");

    /* A ClockData that crosses the end of guest memory, lies at end, or wraps 2^32 */
    expect_bad_addr(end - CLOCKDATA_SIZE + 1,
                    "a ClockData 13 bytes from the end of guest memory is rejected");
    expect_bad_addr(end - 2,
                    "a ClockData in the last 2 bytes of guest memory is rejected");
    expect_bad_addr(end,
                    "a ClockData at the end of guest memory is rejected");
    expect_bad_addr(0xFFFFFFF2u,
                    "a ClockData address near 2^32 that would wrap is rejected");

    /* Address 0: even if valid ClockData bytes sit at guest offset 0, NULL is rejected */
    put_clockdata(0, leap_day);
    expect_bad_addr(0, "a NULL ClockData address (0) is rejected without touching offset 0");
    check(same_clockdata(get_clockdata(0), leap_day),
          "Amiga2Date(seconds, 0) leaves the bytes at guest offset 0 untouched");
    std::memset(fixture.memory, 0, CLOCKDATA_SIZE);
}

int main(int argc, char **argv)
{
    test_table();
    test_layer_info_arithmetic();
    test_empty_utility_calls();
    test_packbooltags();
    test_filtertagchanges();
    test_maptags();
    test_tag_arrays();
    test_filtertagitems();
    test_string_comparisons();
    test_overlapping_tags();
    if (argc == 1 || std::strcmp(argv[1], "--guest-utility") != 0)
    {
        test_findtagitem_lists();
        test_findtagitem_bounds();
        test_findtagitem_cycles();
        test_gettagdata();
        test_nexttagitem();
        test_findtagitem_reference();
        test_dates_known();
        test_dates_roundtrip();
        test_checkdate_invalid();
        test_dates_bounds();
        test_sdivmod32();
        test_udivmod32();
        test_mult64();
        test_reference();
    }
    test_allocated_tags();
    fixture.ctx.mem = nullptr; /* The arena is the fixture's static byte array. */
    Kaffeine_DestroyContext(&fixture.ctx);
    check(native_blocks.empty(), "context destruction releases every native heap record");
    std::printf("utility.library thunks: %u/%u checks passed\n",
                checks - failures, checks);
    return failures ? 1 : 0;
}

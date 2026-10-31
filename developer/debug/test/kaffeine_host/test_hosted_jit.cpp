/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The hosted AArch64 translation probe (kaffeine_jit.c) with the production
    run loop, dispatcher, callback primitive and thunks. The production
    Moira adapter runs the same programs as the reference.
*/
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include "kaffeine_intern.h"
#include "kaffeine_thunks_utility.h"
#include "kaffeine_exec_compat.h"
#include "kaffeine_format.h"
#include <utility/date.h>
#include "kaffeine_jit.h"
#include "kaffeine_sched.h"

extern "C" APTR AllocMem(ULONG size, ULONG) { return std::calloc(1, size); }
extern "C" void FreeMem(APTR ptr, ULONG) { std::free(ptr); }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_LoadM68KLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" void Amiga2Date(ULONG, ClockData *) { std::abort(); }
extern "C" ULONG Date2Amiga(ClockData *) { std::abort(); }
extern "C" ULONG CheckDate(ClockData *) { std::abort(); }

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

static unsigned failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); failures++; return; \
} } while (0)

enum Backend { MOIRA, JIT, JIT_LIBRARY };

/* Thunks of a probe library: what they saw, through the accessors. */
struct Seen
{
    ULONG d[8], a[8];
    UWORD sr;
};
static Seen echo_seen, snap;
static unsigned echo_calls, snap_calls, count_calls, generated_calls;

/* Field by field: the padding after sr is not part of the state. */
static bool operator==(const Seen &x, const Seen &y)
{
    return std::equal(x.d, x.d + 8, y.d) && std::equal(x.a, x.a + 8, y.a) && x.sr == y.sr;
}

static Seen seen(void *cpu)
{
    Seen s;
    for (int i = 0; i < 8; i++)
    {
        s.d[i] = Kaffeine_GetD(cpu, i);
        s.a[i] = Kaffeine_GetA(cpu, i);
    }
    s.sr = Kaffeine_GetSR(cpu);
    return s;
}

static IPTR poison(KaffeineContext *, void *) { generated_calls++; return 0xBADCA11; }

/* -30: returns D1 + 42, writes D2 and A3, leaves Z and C set */
static IPTR probe_echo(KaffeineContext *, void *cpu)
{
    echo_seen = seen(cpu);
    echo_calls++;
    Kaffeine_SetD(cpu, 2, 0x76543210);
    Kaffeine_SetA(cpu, 3, 0xABCDEF00);
    Kaffeine_SetSR(cpu, (UWORD)((Kaffeine_GetSR(cpu) & 0xFF00) | 0x05));
    return Kaffeine_GetD(cpu, 1) + 42;
}

/* -36: what the program has at its end */
static IPTR probe_snapshot(KaffeineContext *, void *cpu)
{
    snap = seen(cpu);
    snap_calls++;
    return Kaffeine_GetD(cpu, 0);
}

/* -42: D0 + 1 */
static IPTR probe_count(KaffeineContext *, void *cpu)
{
    count_calls++;
    return Kaffeine_GetD(cpu, 0) + 1;
}

/* -48: stops the program with 7 */
static IPTR probe_stop7(KaffeineContext *ctx, void *)
{
    ctx->running = FALSE;
    ctx->exit_code = 7;
    return 7;
}

static IPTR probe_stop_void(KaffeineContext *ctx, void *cpu)
{
    Kaffeine_StopGuest(ctx, cpu, 20);
    return 0;
}

static const M68KThunkEntry probe_thunks[] = {
    { 30, probe_echo }, { 36, probe_snapshot }, { 42, probe_count }, { 48, probe_stop7 },
    { 54, probe_stop_void }
};
/* The production cache thunks reach the probe through its flush_code() */
static const M68KThunkEntry exec_thunks[] = {
    { 522, thunk_exec_RawDoFmt }, { 636, Kaffeine_CacheClearU }, { 642, Kaffeine_CacheClearE }
};
static const M68KThunkEntry utility_generated[] = { { 102, poison }, { 312, poison } };
static const M68KThunkEntry exec_generated[] = { { 522, poison } };

struct Layout
{
    static constexpr ULONG entry = 0x1000, hook_code = 0x3000, hook = 0x3800;
    static constexpr ULONG probe = 0x4400, utility = 0x4C00, exec = 0x5400;
    static constexpr ULONG fmt = 0x5800, args = 0x5C00, buffer = 0x6000, snapshot = 0x6800;
    static constexpr ULONG scratch = 0x8000, scratch_end = 0x10000;
    static constexpr ULONG data = 0x10000, stack = 0x40000;
};

struct Machine
{
    ULONG size;
    std::vector<UBYTE> mem;
    KaffeineContext ctx = {};
    HJBackend *jit = nullptr;
    ULONG pc = Layout::entry;

    explicit Machine(ULONG arena = 1u << 20, UBYTE fill = 0xA5)
        : size(arena), mem(arena + KAFFEINE_MEM_GUARD, 0)
    {
        std::fill(mem.begin(), mem.begin() + arena, fill);
        ctx.mem = mem.data(); ctx.mem_size = arena;
        ctx.entry_point = Layout::entry; ctx.stack_top = Layout::stack;
        ctx.m68k_sysbase = Layout::exec;
        ctx.num_libs = 3;
        library(0, Layout::probe, "probe.library", probe_thunks, 5, nullptr, 0);
        library(1, Layout::utility, "utility.library", kaffeine_thunks_utility_manual,
                kaffeine_thunks_utility_manual_count, utility_generated, 2);
        library(2, Layout::exec, "exec.library", exec_thunks, 3, exec_generated, 1);
    }
    ~Machine() { HJ_Destroy(jit); }
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    void library(unsigned index, ULONG base, const char *name, const M68KThunkEntry *manual,
                 ULONG count, const M68KThunkEntry *gen, ULONG gen_count)
    {
        auto &lib = ctx.libs[index];
        lib.m68k_addr = base; lib.jt_start = base - 768;
        std::strcpy(lib.name, name);
        lib.thunks = manual; lib.num_thunks = count;
        lib.gen_thunks = gen; lib.num_gen_thunks = gen_count;
        for (ULONG addr = lib.jt_start; addr < base; addr += 2)
            m68k_write16(&ctx, addr, 0xA000);
    }
    LONG run(Backend which)
    {
        if (which == MOIRA)
            return Kaffeine_Execute(&ctx);
        if (!jit)
            jit = which == JIT_LIBRARY ? HJ_CreateLibrary(&ctx) : HJ_Create(&ctx);
        return jit ? HJ_Execute(jit) : -999;
    }
    void code(ULONG at, std::initializer_list<UWORD> words)
    { for (UWORD w : words) { m68k_write16(&ctx, at, w); at += 2; } }
    void words(ULONG at, const std::vector<UWORD> &w)
    { for (UWORD x : w) { m68k_write16(&ctx, at, x); at += 2; } }
    void word(UWORD v) { m68k_write16(&ctx, pc, v); pc += 2; }
    void longword(ULONG v) { m68k_write32(&ctx, pc, v); pc += 4; }
    void a(unsigned reg, ULONG v) { word((UWORD)(0x207C + (reg << 9))); longword(v); }
    void d(unsigned reg, ULONG v) { word((UWORD)(0x203C + (reg << 9))); longword(v); }
    void jsr(ULONG base, UWORD lvo) { a(6, base); word(0x4EAE); word((UWORD)-lvo); }
    void hook_call(ULONG hook = Layout::hook)
    { a(0, hook); a(1, Layout::args); a(2, Layout::buffer); jsr(Layout::utility, 102); }
    void hook_at(ULONG hook, ULONG entry) { m68k_write32(&ctx, hook + 8, entry); }
    void text(ULONG at, const std::string &s)
    { std::memcpy(mem.data() + at, s.c_str(), s.size() + 1); }
    std::string string_at(ULONG at) const { return std::string((const char *)mem.data() + at); }
    ULONG read(ULONG at) { return m68k_read32(&ctx, at); }
    void rts() { word(0x4E75); }
    void save()
    {
        word(0x48F9); word(0xFFFF); longword(Layout::snapshot); /* MOVEM.L D0-A7,abs.l */
        word(0x40F9); longword(Layout::snapshot + 64);         /* MOVE.W SR,abs.l */
    }
};

using Build = std::function<void(Machine &)>;

static bool library_factory_ok;

/* A second CPU over the same arena: first retranslate its code after the
   parent's flush, then invalidate already cached code in the parent. */
static IPTR library_factory_probe(KaffeineContext *ctx, void *cpu)
{
    const auto *factory = ctx->cpu_factory;
    library_factory_ok = false;
    if (!factory) return 0;
    auto *child = factory->create(ctx);
    if (!child) return 0;
    Kaffeine_SetD(child, 2, 0x87654321);
    Kaffeine_SetA(child, 7, Layout::stack - 0x100);
    Kaffeine_SetPC(child, Layout::hook_code);
    child->ops->execute(child->backend_state);
    ULONG first = Kaffeine_GetD(child, 0);
    m68k_write16(ctx, Layout::hook_code, 0x7002); /* MOVEQ #2,D0 */
    Kaffeine_FlushCode(cpu, Layout::hook_code, 2);
    Kaffeine_SetPC(child, Layout::hook_code);
    child->ops->execute(child->backend_state);
    const auto *stats = HJ_GetStats(static_cast<HJBackend *>(child->backend_state));
    library_factory_ok = first == 1 && Kaffeine_GetD(child, 0) == 2 &&
        Kaffeine_GetD(child, 2) == 0x87654321 && Kaffeine_GetD(cpu, 2) == 0x12345678 &&
        stats->blocks == 2 && stats->enters == 2 && stats->flushes == 1;
    m68k_write16(ctx, Layout::hook_code + 0x100, 0x7004); /* MOVEQ #4,D0 */
    Kaffeine_FlushCode(child, Layout::hook_code + 0x100, 2);
    factory->destroy(ctx, child);
    return 0;
}

static IPTR library_bad_sr(KaffeineContext *, void *cpu)
{
    Kaffeine_SetSR(cpu, 0); /* would require a different stack bank */
    return 99;
}

static void library_backend()
{
    const M68KThunkEntry entries[] = { {30, library_factory_probe}, {36, library_bad_sr} };
    Machine m;
    m.library(0, Layout::probe, "probe.library", entries, 2, nullptr, 0);
    m.code(Layout::hook_code, {0x7001, 0x60FC}); /* MOVEQ #1,D0; BRA.S self */
    m.code(Layout::hook_code + 0x100, {0x7003, 0x4E75});
    m.d(2, 0x12345678);
    m.word(0x4EB9); m.longword(Layout::hook_code + 0x100);
    m.word(0x23C0); m.longword(Layout::data);
    m.jsr(Layout::probe, 30);
    m.word(0x4EB9); m.longword(Layout::hook_code + 0x100);
    m.rts();
    CHECK(m.run(JIT_LIBRARY) == 4);
    CHECK(m.read(Layout::data) == 3 && library_factory_ok);
    CHECK(!m.ctx.cpu && !m.ctx.cpu_factory && !m.ctx.sched);
    CHECK(m.ctx.jit_generation == 2 && HJ_GetStats(m.jit)->flushes == 2);
    CHECK(HJ_GetStats(m.jit)->exits[HJ_EXIT_BUDGET] > 0);

    Machine callback;
    callback.code(Layout::hook_code, {0x702A, 0x4E75});
    callback.hook_at(Layout::hook, Layout::hook_code);
    callback.hook_call(); callback.rts();
    CHECK(callback.run(JIT_LIBRARY) == 42);
    CHECK(HJ_GetStats(callback.jit)->max_depth == 2);
    CHECK(!callback.ctx.cpu_factory && !callback.ctx.callback_depth);

    for (UWORD opcode : {0x46FCu, 0x007Cu}) /* MOVE to SR / ORI to SR */
    {
        Machine refused;
        refused.word(0x33FC); refused.word(0x1234); refused.longword(Layout::data);
        refused.word(opcode); refused.word(0x2000);
        refused.word(0x33FC); refused.word(0xFFFF); refused.longword(Layout::data + 2);
        refused.rts();
        CHECK(refused.run(JIT_LIBRARY) == 20);
        CHECK(refused.read(Layout::data) == 0x1234A5A5);
        CHECK(std::strstr(HJ_LastFault(refused.jit), "full SR write"));
        CHECK(!refused.ctx.cpu_factory);
    }
    Machine native_sr;
    native_sr.library(0, Layout::probe, "probe.library", entries, 2, nullptr, 0);
    native_sr.jsr(Layout::probe, 36);
    native_sr.word(0x33FC); native_sr.word(0xFFFF); native_sr.longword(Layout::data);
    native_sr.rts();
    CHECK(native_sr.run(JIT_LIBRARY) == 20);
    CHECK(native_sr.read(Layout::data) == 0xA5A5A5A5);
    CHECK(std::strstr(HJ_LastFault(native_sr.jit), "SR mode/trace change"));
}

/* DIV.W has architected results independent of either CPU implementation.
   The manual defines X unchanged, C clear and N/Z from a successful
   quotient; N/Z are undefined on overflow. Arithmetic uses widened host
   integers, including INT32_MIN / -1. */
static void word_division()
{
    struct Case { bool sign; ULONG dividend; UWORD divisor; unsigned form, dst, flags; };
    std::vector<Case> cases;
    const ULONG dividends[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF,
        0x8000, 0xFFFF, 0x10000, 0x1FFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF};
    const UWORD divisors[] = {1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF};
    unsigned serial = 0, executed = 0, reference_flags_differ = 0;
    for (bool sign : {false, true})
        for (ULONG dividend : dividends)
            for (UWORD divisor : divisors)
            {
                unsigned i = serial++;
                cases.push_back({sign, dividend, divisor, i % 8, (i / 8) % 8, (i * 7) & 31});
            }
    ULONG seed = 0xD1706802;
    auto random = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    for (unsigned i = 0; i < 512; ++i)
    {
        ULONG dividend = random(), divisor = random();
        cases.push_back({bool(i & 1), dividend, (UWORD)((divisor & 0xFFFF) ? divisor : 1),
                         i % 8, (i / 8) % 8, random() & 31});
    }
    for (auto c : cases)
    {
        UWORD divisor = c.form == 5 ? (UWORD)c.dividend : c.divisor;
        if (!divisor) continue;
        ++executed;
        std::int64_t numerator = c.sign ? (std::int32_t)c.dividend : std::int64_t(c.dividend);
        std::int64_t denominator = c.sign ? (std::int16_t)divisor : std::int64_t(divisor);
        std::int64_t quotient = numerator / denominator, remainder = numerator % denominator;
        bool overflow = quotient < (c.sign ? -32768 : 0) || quotient > (c.sign ? 32767 : 65535);
        ULONG expected = overflow ? c.dividend :
            (ULONG)(UWORD)remainder << 16 | (UWORD)quotient;
        UWORD mask = overflow ? 0x13 : 0x1F;
        UWORD expected_flags = (UWORD)((c.flags & 0x10) | (overflow ? 2 :
            ((quotient & 0x8000) ? 8 : 0) | (quotient == 0 ? 4 : 0)));
        auto build = [&](Machine &m) {
            const ULONG data = Layout::scratch;
            unsigned source = (c.dst + 1) & 7, index = (c.dst + 2) & 7;
            unsigned ea = c.form == 0 ? source : c.form == 1 ? 0x3C :
                c.form == 2 ? 0x39 : c.form == 3 ? 0x18 : c.form == 4 ? 0x20 :
                c.form == 5 ? c.dst : c.form == 6 ? 0x3A : 0x30;
            m.d(source, 0xC0DE0000 | divisor);
            m.d(index, 0xC0DE0004);
            m.d(c.dst, c.dividend);
            m.a(0, c.form == 4 ? data + 2 : c.form == 7 ? data - 4 : data);
            m68k_write16(&m.ctx, data, divisor);
            m.word(0x44FC); m.word((UWORD)c.flags); /* MOVE #flags,CCR */
            m.word((UWORD)((c.sign ? 0x81C0 : 0x80C0) | (c.dst << 9) | ea));
            if (c.form == 1) m.word(divisor);
            if (c.form == 2) m.longword(data);
            if (c.form == 6) m.word((UWORD)(data - m.pc));
            if (c.form == 7) m.word((UWORD)(index << 12));
            m.save(); m.rts();
        };
        Machine reference, translated;
        build(reference); build(translated);
        reference.run(MOIRA);
        CHECK(reference.read(Layout::snapshot + 4 * c.dst) == expected);
        UWORD reference_sr = m68k_read16(&reference.ctx, Layout::snapshot + 64);
        if ((reference_sr & mask) != expected_flags)
        {
            if (++reference_flags_differ == 1)
                std::printf("  first pinned-reference flag difference: SR=%04x, ISA flags=%02x, mask=%02x\n",
                            reference_sr, expected_flags, mask);
        }
        translated.run(JIT_LIBRARY);
        CHECK(HJ_LastFault(translated.jit)[0] == 0);
        CHECK(translated.read(Layout::snapshot + 4 * c.dst) == expected);
        CHECK((m68k_read16(&translated.ctx, Layout::snapshot + 64) & mask) == expected_flags);
        CHECK(std::memcmp(reference.mem.data() + Layout::snapshot,
                          translated.mem.data() + Layout::snapshot, 64) == 0);
        if (c.form == 3 || c.form == 4)
            CHECK(translated.read(Layout::snapshot + 32) == Layout::scratch + (c.form == 3 ? 2 : 0));
        CHECK(translated.mem[Layout::scratch] == (UBYTE)(divisor >> 8) &&
              translated.mem[Layout::scratch + 1] == (UBYTE)divisor);
    }
    std::printf("  %u word-division cases; %u pinned-reference flag differences from the independent ISA oracle\n",
                executed, reference_flags_differ);
    /* A following MOVEQ kills NZVC. Scc observes just one flag before
       that overwrite, so these also exercise partial and dead needs. */
    for (auto c : {Case{false, 0xFFFFFFFF, 0xFFFF, 0, 0, 31},
                   Case{false, 0xFFFF, 1, 0, 0, 0},
                   Case{true, 0, 0xFFFF, 0, 0, 31},
                   Case{true, 0xFFFFFFF9, 2, 0, 0, 0}})
    {
        std::int64_t n = c.sign ? (std::int32_t)c.dividend : std::int64_t(c.dividend);
        std::int64_t d = c.sign ? (std::int16_t)c.divisor : std::int64_t(c.divisor);
        std::int64_t q = n / d, r = n % d;
        bool overflow = q < (c.sign ? -32768 : 0) || q > (c.sign ? 32767 : 65535);
        ULONG expected = overflow ? c.dividend : (ULONG)(UWORD)r << 16 | (UWORD)q;
        for (unsigned cond : {0u, 5u, 9u, 11u, 7u}) /* dead, C, V, N, Z */
        {
            if (overflow && (cond == 11 || cond == 7)) continue;
            Machine m;
            m.d(0, c.dividend); m.d(1, 0xC0DE0000 | c.divisor); m.d(2, 0x12345600);
            m.word(0x44FC); m.word((UWORD)c.flags);
            m.word(c.sign ? 0x81C1 : 0x80C1);
            if (cond) m.word((UWORD)(0x50C2 | (cond << 8)));
            m.word(0x7600); /* MOVEQ #0,D3 */
            m.save(); m.rts();
            m.run(JIT_LIBRARY);
            CHECK(!HJ_LastFault(m.jit)[0]);
            CHECK(m.read(Layout::snapshot) == expected);
            bool set = cond == 9 ? overflow : cond == 11 ? (q & 0x8000) : cond == 7 ? q == 0 : false;
            CHECK(m.read(Layout::snapshot + 8) == (0x12345600U | (cond && set ? 255 : 0)));
        }
    }
    for (bool sign : {false, true})
    {
        Machine zero;
        zero.d(0, 0x12345678); zero.d(1, 0xC0DE0000);
        zero.word(sign ? 0x81C1 : 0x80C1);
        zero.word(0x23FC); zero.longword(0xBAD); zero.longword(Layout::data);
        zero.rts();
        CHECK(zero.run(JIT_LIBRARY) == 20);
        CHECK(zero.read(Layout::data) == 0xA5A5A5A5);
        CHECK(HJ_LastFault(zero.jit)[0] != 0);
    }
}

/* Moira plants its RTE and trampoline in the vector page and leaves the
   frame of a Line-A exception in the 8 bytes below the stack pointer of
   the call; the probe needs neither. */
static bool same_memory(const Machine &x, const Machine &y, ULONG sp)
{
    for (ULONG i = KAFFEINE_VECTORS_SIZE; i < x.size; i++)
        if (x.mem[i] != y.mem[i] && !(i >= sp - 8 && i < sp))
            return false;
    return true;
}

/* Original programs for the 68020 full extension's non-indirect forms.
   Compare every guest register, CCR and the entire arena (apart from the
   reference adapter's documented vector/exception-frame scratch). */
static void full_index()
{
    auto seed = [](Machine &m) {
        for (unsigned i = 0; i < 8; i++) m.d(i, 0xD0000000u + i);
        for (unsigned i = 0; i < 7; i++) m.a(i, 0xA0000000u + i);
    };
    auto equal = [](Machine &reference, Machine &translated) {
        return !HJ_LastFault(translated.jit)[0] &&
            std::memcmp(reference.mem.data() + Layout::snapshot,
                        translated.mem.data() + Layout::snapshot, 66) == 0 &&
            same_memory(reference, translated, reference.read(Layout::snapshot + 60));
    };
    for (ULONG target : {Layout::scratch + 3696 + 7*4, (1u << 20) - 1, 1u << 20})
    {
        Machine reference, translated;
        auto build = [&](Machine &m) {
            seed(m); m.a(0, 7); m.a(2, target - 3696 - 7*4);
            m.word(0x44FC); m.word(0x001F); /* MOVE #$1F,CCR */
            m.code(m.pc, {0x4232, 0x8D20, 0x0E70}); m.pc += 6;
            /* CLR.B (3696,A2,A0.L*4), with a full extension word. */
            m.save(); m.rts();
        };
        build(reference); build(translated);
        CHECK(reference.run(MOIRA) == translated.run(JIT_LIBRARY));
        CHECK(equal(reference, translated));
        CHECK(target >= translated.size || translated.mem[target] == 0);
        CHECK(std::all_of(translated.mem.begin() + translated.size, translated.mem.end(),
                          [](UBYTE byte) { return byte == 0; }));
        CHECK((m68k_read16(&translated.ctx, Layout::snapshot + 64) & 0x1F) == 0x14);
    }
    /* A separate integer address oracle checks suppression, signed word
       indexes, scaling and 32-bit wrapping, including out-of-arena reads. */
    for (bool pc_relative : {false, true})
        for (unsigned bd_size : {1u, 2u, 3u})
            for (unsigned suppress = 0; suppress < 4; suppress++)
                for (bool long_index : {false, true})
                    for (unsigned scale = 0; scale < 4; scale++)
                        for (bool address_index : {false, true})
                        {
                            const bool bs = suppress & 1, is = suppress & 2;
                            const ULONG index = long_index ? (scale & 1 ? 0xFFFFFFFDu : 3u) :
                                (scale & 1 ? 0xCAFEFFFDu : 0xCAFE0003u);
                            const LONG disp = bd_size == 1 ? 0 : bd_size == 2 ?
                                (scale & 1 ? -3696 : 3696) :
                                (scale & 1 ? INT32_MIN : INT32_MAX);
                            const ULONG scaled = (long_index ? index : ULONG(LONG(int16_t(index)))) << scale;
                            const ULONG base = bs ? 0xBAD00002u : Layout::scratch - ULONG(disp) - (is ? 0 : scaled);
                            const UWORD ext = UWORD((address_index ? 0x8000 : 0x1000) |
                                (long_index ? 0x0800 : 0) | (scale << 9) | 0x0100 |
                                (bs ? 0x0080 : 0) | (is ? 0x0040 : 0) | (bd_size << 4));
                            Machine reference, translated;
                            ULONG expected_address = 0, read_address = 0;
                            auto address = [&](ULONG pcbase) {
                                return (bs ? 0 : pc_relative ? pcbase : base) + ULONG(disp) + (is ? 0 : scaled);
                            };
                            auto build = [&](Machine &m) {
                                seed(m); m.a(2, base);
                                if (address_index) m.a(0, index); else m.d(1, index);
                                m.word(0x44FC); m.word(0x001F);
                                auto operand = [&] {
                                    m.word(ext);
                                    if (bd_size == 2) m.word(UWORD(disp));
                                    if (bd_size == 3) m.longword(ULONG(disp));
                                };
                                m.word(pc_relative ? 0x49FB : 0x49F2); /* LEA <ea>,A4 */
                                expected_address = address(m.pc); operand();
                                m.word(pc_relative ? 0x263B : 0x2632); /* MOVE.L <ea>,D3 */
                                read_address = address(m.pc); operand();
                                m.save(); m.rts();
                            };
                            build(reference); build(translated);
                            auto word = [&](ULONG at) -> ULONG {
                                return uint64_t(at) + 2 <= reference.size ?
                                    ULONG(reference.mem[at]) << 8 | reference.mem[at+1] : 0;
                            };
                            LONG reference_rc = reference.run(MOIRA), translated_rc = translated.run(JIT_LIBRARY);
                            /* Both adapters initialize the reset vectors. A
                               fully suppressed null BD reads vector zero. */
                            const ULONG expected_value = word(read_address) << 16 | word(read_address + 2);
                            CHECK(reference_rc == translated_rc);
                            CHECK(equal(reference, translated));
                            CHECK(translated.read(Layout::snapshot + 48) == expected_address);
                            CHECK(translated.read(Layout::snapshot + 12) == expected_value);
                            CHECK((m68k_read16(&translated.ctx, Layout::snapshot + 64) & 0x1F) ==
                                  (0x10 | (expected_value == 0 ? 4 : expected_value & 0x80000000u ? 8 : 0)));
                        }
    for (UWORD ext : {0x0100, 0x0118, 0x0111, 0x0112, 0x0113, 0x0114, 0x0115, 0x0116, 0x0117})
    {
        Machine m;
        m.a(2, Layout::scratch); m.d(0, 0x12345678);
        m.word(0x4232); m.word(ext); m.word(0x4E75);
        const auto before = m.mem;
        CHECK(m.run(JIT_LIBRARY) == 20 && std::strstr(HJ_LastFault(m.jit), "unary mode"));
        CHECK(std::equal(before.begin() + Layout::scratch, before.begin() + Layout::scratch_end,
                         m.mem.begin() + Layout::scratch));
    }
    /* A valid long base displacement must not read beyond the code arena. */
    for (ULONG bytes : {4u, 6u})
    {
        Machine m;
        m.ctx.entry_point = m.size - bytes;
        m.code(m.ctx.entry_point, {0x4AB2, 0x0130});
        if (bytes == 6) m68k_write16(&m.ctx, m.size - 2, 0x1234);
        CHECK(m.run(JIT_LIBRARY) == 20 && std::strstr(HJ_LastFault(m.jit), "fetch outside guest memory"));
    }
}

/* ------------------------------------------------------------------
   Portable checks
   ------------------------------------------------------------------ */

static void encoder()
{
    char message[160];
    CHECK(HJ_EncoderSelfTest(message, sizeof(message)) == 0);
    std::printf("  %s\n", message);
}

static const Build checksum_program = [](Machine &m) {
    /* LEA data,A0; MOVE.L #1023,D1; MOVEQ #0,D0
       loop: MOVE.L (A0)+,D2; ROL.L #5,D0; EOR.L D2,D0; ADD.L D1,D0; DBRA D1,loop; RTS */
    m.word(0x41F9); m.longword(Layout::data);
    m.d(1, 1023);
    m.word(0x7000);
    m.code(m.pc, {0x2418, 0xEB98, 0xB580, 0xD081, 0x51C9, 0xFFF6, 0x4E75});
    for (ULONG i = 0; i < 1024; i++)
        m68k_write32(&m.ctx, Layout::data + 4 * i, i * 0x9E3779B9u ^ 0x5A5AA5A5u);
};

static ULONG checksum_reference(const Machine &m)
{
    ULONG d0 = 0;
    for (ULONG i = 0, d1 = 1023; i < 1024; i++, d1--)
    {
        ULONG v = m68k_read32(const_cast<KaffeineContext *>(&m.ctx), Layout::data + 4 * i);
        d0 = (d0 << 5) | (d0 >> 27);
        d0 ^= v;
        d0 += d1;
    }
    return d0;
}

static void translate_only()
{
    Machine m;
    checksum_program(m);
    /* A block ends at a branch: the first one runs into the loop. */
    unsigned words = 0, insns = HJ_TranslateOnly(&m.ctx, Layout::entry, &words);
    CHECK(insns == 8 && words > 0);
    insns = HJ_TranslateOnly(&m.ctx, Layout::entry + 14, &words);
    CHECK(insns == 5 && words > 0);
    std::printf("  loop block: 5 instructions, %u words\n", words);
}

/* ------------------------------------------------------------------
   1. Executable memory
   ------------------------------------------------------------------ */

static void exec_memory()
{
    Machine m;
    m.code(Layout::entry, {0x702A, 0x4E75}); /* MOVEQ #42,D0; RTS */
    CHECK(m.run(JIT) == 42);
    const HJStats *s = HJ_GetStats(m.jit);
    CHECK(s->blocks == 1 && s->write_windows == 1 && s->icache_syncs == 1);
    /* RTS to the sentinel leaves through the lookup; C stops there. */
    CHECK(s->enters == 1 && s->exits[HJ_EXIT_LOOKUP] == 1);
    std::printf("  %s: %lu block, %lu write window, %lu instruction cache sync\n",
                HJ_ExecMemoryKind(), s->blocks, s->write_windows, s->icache_syncs);
}

/* ------------------------------------------------------------------
   2. Big-endian guest memory
   ------------------------------------------------------------------ */

static const Build endian_program = [](Machine &m) {
    const ULONG end = m.size;
    m.a(0, Layout::scratch);
    m.a(1, Layout::scratch + 0x100);
    m.a(2, end - 4);
    m.a(3, end);
    m.code(m.pc, {
        0x20BC, 0x1122, 0x3344,         /* MOVE.L #$11223344,(A0) */
        0x317C, 0x5566, 0x0004,         /* MOVE.W #$5566,4(A0) */
        0x117C, 0x0077, 0x0006,         /* MOVE.B #$77,6(A0) */
        0x2228, 0x0001,                 /* MOVE.L 1(A0),D1: misaligned */
        0x3428, 0x0003,                 /* MOVE.W 3(A0),D2: misaligned */
        0x1628, 0x0005,                 /* MOVE.B 5(A0),D3 */
        0x2811,                         /* MOVE.L (A1),D4 */
        0x2A28, 0x0003,                 /* MOVE.L 3(A0),D5 */
        0x24BC, 0xDEAD, 0xBEEF,         /* MOVE.L #$DEADBEEF,(A2): last long */
        0x2C12,                         /* MOVE.L (A2),D6 */
        0x2E2B, 0xFFFE,                 /* MOVE.L -2(A3),D7: one word inside */
        0x23C4, 0xFFFF, 0xFFF0,         /* MOVE.L D4,$FFFFFFF0: dropped */
        0x2A79, 0xFFFF, 0xFFF0,         /* MOVEA.L $FFFFFFF0,A5: zero */
        0x377C, 0x1234, 0xFFFF,         /* MOVE.W #$1234,-1(A3): dropped */
        0x4EAE, (UWORD)-36,             /* JSR -36(A6) */
        0x4E75
    });
    m68k_write32(&m.ctx, Layout::scratch + 0x100, 0xCAFEBABE);
};

static void endian()
{
    Machine moira, jit;
    for (Machine *m : { &moira, &jit })
    {
        endian_program(*m);
        /* A6 for the snapshot */
        m->ctx.m68k_sysbase = Layout::probe;
    }
    CHECK(moira.run(MOIRA) == 0);
    Seen ref = snap;
    CHECK(jit.run(JIT) == 0);
    const UBYTE want[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    CHECK(std::memcmp(jit.mem.data() + Layout::scratch, want, sizeof(want)) == 0);
    CHECK(std::memcmp(moira.mem.data() + Layout::scratch, want, sizeof(want)) == 0);
    CHECK(snap.d[1] == 0x22334455 && (snap.d[2] & 0xFFFF) == 0x4455 && (snap.d[3] & 0xFF) == 0x66);
    CHECK(snap.d[4] == 0xCAFEBABE && snap.d[5] == 0x44556677);
    CHECK(snap.d[6] == 0xDEADBEEF && snap.d[7] == 0xBEEF0000 && snap.a[5] == 0);
    CHECK(jit.read(jit.size - 4) == 0xDEADBEEF && jit.mem[jit.size - 1] == 0xEF);
    CHECK(std::memcmp(snap.d, ref.d, sizeof(ref.d)) == 0 && std::memcmp(snap.a, ref.a, sizeof(ref.a)) == 0);
    CHECK(snap.sr == ref.sr && same_memory(jit, moira, snap.a[7]));
    /* the long at the end that is one word inside, two stores and a load
       outside: the rest takes the inline path */
    CHECK(HJ_GetStats(jit.jit)->slow_accesses == 4);
    std::printf("  host arena at %p, %s 4 GB\n", (void *)jit.ctx.mem,
                (uintptr_t)jit.ctx.mem > 0xFFFFFFFFu ? "above" : "below");
}

/* An arena that holds the custom chip page: a long is two word accesses,
   as in the Moira adapter. Read at $DFEFFE it takes its second word from
   the first register, at $DFFFFE its first word from the last one, and
   both reads move the beam; long stores there leave the memory behind the
   page alone. */
static const Build custom_program = [](Machine &m) {
    m.code(m.pc, {
        0x2639, 0x00DF, 0xEFFE,         /* MOVE.L $DFEFFE,D3 */
        0x2839, 0x00DF, 0xFFFE,         /* MOVE.L $DFFFFE,D4 */
        0x3A39, 0x00DF, 0xEFFF,         /* MOVE.W $DFEFFF,D5: one access */
        0x243C, 0x1122, 0x3344,         /* MOVE.L #$11223344,D2 */
        0x23C2, 0x00DF, 0xEFFE,         /* MOVE.L D2,$DFEFFE */
        0x23C2, 0x00DF, 0xFFFE,         /* MOVE.L D2,$DFFFFE */
        0x4EAE, (UWORD)-36,             /* JSR -36(A6) */
        0x4E75
    });
    m68k_write32(&m.ctx, 0xDFEFFC, 0xA1A2A3A4);
    m68k_write32(&m.ctx, 0xDFF000, 0xB1B2B3B4);
    m68k_write32(&m.ctx, 0xDFFFFC, 0xC1C2C3C4);
    m68k_write32(&m.ctx, 0xE00000, 0xD1D2D3D4);
    m.ctx.m68k_sysbase = Layout::probe;
};

static void custom_page()
{
    Machine moira(1u << 24), jit(1u << 24);
    for (Machine *m : { &moira, &jit })
        custom_program(*m);
    CHECK(moira.run(MOIRA) == 0);
    Seen ref = snap;
    CHECK(jit.run(JIT) == 0);
    CHECK(ref.d[3] == 0xA3A40000 && ref.d[4] == 0x0000D1D2);
    CHECK(snap.d[3] == ref.d[3] && snap.d[4] == ref.d[4]);
    CHECK((ref.d[5] & 0xFFFF) == 0xA4B1 && snap.d[5] == ref.d[5]);
    CHECK(jit.read(0xDFEFFC) == 0xA1A21122 && jit.read(0xDFF000) == 0xB1B2B3B4);
    CHECK(jit.read(0xDFFFFC) == 0xC1C2C3C4 && jit.read(0xE00000) == 0x3344D3D4);
    CHECK(jit.ctx.beam_v == moira.ctx.beam_v && jit.ctx.beam_h == moira.ctx.beam_h);
    CHECK(same_memory(jit, moira, snap.a[7]));
    /* the two reads and the two stores, none inline */
    CHECK(HJ_GetStats(jit.jit)->slow_accesses == 4);
}

/* ------------------------------------------------------------------
   3. A translated program
   ------------------------------------------------------------------ */

static const Build sort_program = [](Machine &m) {
    /* Bubble sort of 64 words at data, with a subroutine that saves and
       restores registers with MOVEM:
         MOVEQ #62,D7
       outer: LEA data,A0; MOVE.W D7,D6
       inner: BSR.W cmpswap; DBRA D6,inner
         DBRA D7,outer; JSR -36(A6); RTS
       cmpswap: MOVEM.L D0-D1,-(SP); MOVE.W (A0)+,D0; MOVE.W (A0),D1
         CMP.W D1,D0; BLS.S keep; MOVE.W D0,(A0); MOVE.W D1,-2(A0)
       keep: MOVEM.L (SP)+,D0-D1; RTS */
    const ULONG start = m.pc;
    m.word(0x7E3E);
    ULONG outer = m.pc;
    m.word(0x41F9); m.longword(Layout::data);
    m.word(0x3C07);
    ULONG inner = m.pc;
    m.word(0x6100); ULONG bsr_at = m.pc; m.word(0);
    m.word(0x51CE); m.word((UWORD)(inner - m.pc));
    m.word(0x51CF); m.word((UWORD)(outer - m.pc));
    m.word(0x4EAE); m.word((UWORD)-36);
    m.word(0x4E75);
    ULONG cmpswap = m.pc;
    m68k_write16(&m.ctx, bsr_at, (UWORD)(cmpswap - bsr_at));
    m.code(m.pc, {0x48E7, 0xC000, 0x3018, 0x3210, 0xB041, 0x6306, 0x3080, 0x3141, 0xFFFE,
                  0x4CDF, 0x0003, 0x4E75});
    (void)start;
    for (ULONG i = 0; i < 64; i++)
        m68k_write16(&m.ctx, Layout::data + 2 * i, (UWORD)((i * 0x9E37u) ^ 0x5A5Au));
    m.ctx.m68k_sysbase = Layout::probe;
};

static void program()
{
    Machine moira, jit;
    checksum_program(moira);
    checksum_program(jit);
    ULONG want = checksum_reference(jit);
    CHECK((ULONG)moira.run(MOIRA) == want);
    CHECK((ULONG)jit.run(JIT) == want);
    const HJStats *s = HJ_GetStats(jit.jit);
    CHECK(s->blocks == 3 && s->exits[HJ_EXIT_BUDGET] == 0);
    std::printf("  checksum: %lu blocks, %lu 68k instructions, %.1f AArch64 words each\n",
                s->blocks, s->guest_insns, (double)s->host_words / (double)s->guest_insns);

    Machine msort, jsort;
    sort_program(msort);
    sort_program(jsort);
    std::vector<UWORD> values;
    for (ULONG i = 0; i < 64; i++)
        values.push_back(m68k_read16(&jsort.ctx, Layout::data + 2 * i));
    std::sort(values.begin(), values.end());
    msort.run(MOIRA);
    Seen ref = snap;
    jsort.run(JIT);
    for (ULONG i = 0; i < 64; i++)
        CHECK(m68k_read16(&jsort.ctx, Layout::data + 2 * i) == values[i]);
    CHECK(snap == ref);
    CHECK(same_memory(jsort, msort, snap.a[7]));
    s = HJ_GetStats(jsort.jit);
    std::printf("  sort: %lu blocks, %lu enters from C\n", s->blocks, s->enters);
}

/* DBcc displacement is relative to the extension word, in both directions.
   Keep the branch destinations observable without a decoder-only oracle. */
static void dbcc_targets()
{
    for (Backend which : { MOIRA, JIT, JIT_LIBRARY })
    {
        Machine m;
        /* MOVEQ #0,D0; MOVEQ #1,D1; loop: ADDQ.W #1,D0;
           DBF D1,loop; RTS. A target two bytes high skips the second ADDQ. */
        m.code(Layout::entry, {0x7000, 0x7201, 0x5240, 0x51C9, 0xFFFC, 0x4E75});
        CHECK(m.run(which) == 2);
        if (which != MOIRA)
        {
            CHECK(!HJ_LastFault(m.jit)[0]);
            CHECK(HJ_GetStats(m.jit)->enters && HJ_GetStats(m.jit)->icache_syncs);
        }
    }
    for (unsigned form = 0; form < 3; ++form)
    {
        std::array<UBYTE, 66> reference = {};
        for (Backend which : { MOIRA, JIT, JIT_LIBRARY })
        {
            Machine m;
            m.d(0, form == 2 ? 3 : 0);
            m.d(1, 0xC0DE0001);
            m.d(2, 0x87650000);
            if (form == 0)
            {
                ULONG loop = m.pc;
                m.word(0x5240);                  /* ADDQ.W #1,D0 */
                m.word(0x44FC); m.word(0x001F); /* MOVE #$1F,CCR */
                m.word(0x51C9);                  /* DBF D1,loop */
                m.word((UWORD)(loop - m.pc));
            }
            else
            {
                m.word(0x44FC); m.word(0x001F);
                m.word(form == 1 ? 0x51C9 : 0x57C9); /* DBF / DBEQ D1,target */
                ULONG extension = m.pc;
                m.word(0);
                ULONG target;
                if (form == 1)
                {
                    m.word(0x70FF);              /* Fallthrough: MOVEQ #-1,D0 */
                    m.word(0x6006);              /* BRA.S finish */
                    m.word(0x4E71);
                    target = m.pc;
                    m.word(0x42C2);              /* Target: MOVE CCR,D2 */
                    m.word(0x7007);              /* MOVEQ #7,D0 */
                }
                else
                {
                    /* Z is set: DBEQ must leave D1 and every CCR bit alone. */
                    m.word(0x6004);              /* Fallthrough: BRA.S finish */
                    target = m.pc;
                    m.word(0x70FF);              /* Wrongly taken: MOVEQ #-1,D0 */
                    m.word(0x4E71);
                }
                m68k_write16(&m.ctx, extension, (UWORD)(target - extension));
            }
            m.save(); m.rts();
            CHECK(m.run(which) == (form == 0 ? 2 : form == 1 ? 7 : 3));
            CHECK(m.read(Layout::snapshot + 4) ==
                  (form == 0 ? 0xC0DEFFFFu : form == 1 ? 0xC0DE0000u : 0xC0DE0001u));
            CHECK(m.read(Layout::snapshot + 8) == (form == 1 ? 0x8765001Fu : 0x87650000u));
            CHECK((m68k_read16(&m.ctx, Layout::snapshot + 64) & 0x1F) == (form == 1 ? 0x10 : 0x1F));
            if (which == MOIRA)
                std::copy_n(m.mem.data() + Layout::snapshot, reference.size(), reference.begin());
            else
            {
                CHECK(!HJ_LastFault(m.jit)[0]);
                CHECK(HJ_GetStats(m.jit)->enters && HJ_GetStats(m.jit)->icache_syncs);
                CHECK(std::equal(reference.begin(), reference.end(), m.mem.begin() + Layout::snapshot));
            }
        }
    }
}

/* A block cut at 64 instructions, with the X of its last one read in the
   next block; and a loop of 200000 back edges, more than one budget. */
static void block_limits()
{
    Seen ref = {};
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        m.ctx.m68k_sysbase = Layout::probe;
        m.d(1, 0xFFFFFFFFu);
        m.word(0x7401);                          /* MOVEQ #1,D2 */
        for (int i = 0; i < 61; i++) m.word(0x4E71);
        m.word(0xD481);                          /* ADD.L D1,D2: Z, C and X */
        m.word(0x40C3);                          /* MOVE SR,D3 */
        m.word(0x7800);                          /* MOVEQ #0,D4: X stays */
        m.word(0x40C5);                          /* MOVE SR,D5 */
        m.word(0x4EAE); m.word((UWORD)-36);
        m.rts();
        CHECK(m.run(which) == 0);
        CHECK((snap.d[3] & 0x1F) == 0x15 && (snap.d[5] & 0x1F) == 0x14);
        if (which == MOIRA)
            ref = snap;
        else
        {
            CHECK(snap == ref);
            CHECK(HJ_GetStats(m.jit)->guest_insns == 64 + 4 + 1 + 1);
        }
    }
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        m.d(1, 199999);
        m.code(m.pc, {0x5280, 0x5381, 0x6AFA, 0x4E75}); /* ADDQ.L #1,D0; SUBQ.L #1,D1; BPL.S; RTS */
        CHECK(m.run(which) == 200000);
        if (which == JIT)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            CHECK(s->exits[HJ_EXIT_BUDGET] == 3 && s->blocks == 3);
            std::printf("  200000 iterations: %lu budget exits, %lu enters\n",
                        s->exits[HJ_EXIT_BUDGET], s->enters);
        }
    }
}

/* More blocks than the code memory keeps, inside a callback: the probe
   empties it while the caller is suspended in the dispatcher, and a hook
   flushes the guest caches in the middle of a nested call. */
static void code_reset()
{
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        const unsigned units = 40000;
        const ULONG chain = 0x50000;          /* 240 KB, above the stack */
        m.pc = chain;
        for (unsigned i = 0; i < units; i++)
        {
            m.word(0x5280);                      /* ADDQ.L #1,D0 */
            m.word(0x6002); m.word(0x4E71);      /* BRA.S +2; NOP */
        }
        m.word(0x2E00);                          /* MOVE.L D0,D7: D0 is scratch */
        m.jsr(Layout::exec, 636);                /* CacheClearU() */
        m.word(0x2007);                          /* MOVE.L D7,D0 */
        m.rts();
        m.pc = Layout::entry;
        m.hook_at(Layout::hook, chain);
        m.word(0x7000);
        m.hook_call();
        m.word(0x5A80);                          /* ADDQ.L #5,D0 */
        m.rts();
        CHECK(m.run(which) == (LONG)units + 5);
        if (which == JIT)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            CHECK(s->resets >= 1 && s->flushes == 1 && s->max_depth == 2);
            std::printf("  %u chained blocks in a callback: %lu blocks, %lu resets\n",
                        units, s->blocks, s->resets);
        }
    }
}

/* ------------------------------------------------------------------
   4. Library calls through the dispatcher
   ------------------------------------------------------------------ */

static const Build call_program = [](Machine &m) {
    for (unsigned i = 0; i < 8; i++) m.d(i, 0xD0000000u + i);
    for (unsigned i = 0; i < 6; i++) m.a(i, 0xA0000000u + i);
    m.a(6, Layout::probe);
    m.code(m.pc, {
        0x44FC, 0x0000,          /* MOVE #0,CCR */
        0x4EAE, (UWORD)-30,      /* JSR -30(A6): echo */
        0x6702, 0x7EFF,          /* BEQ.S +2; MOVEQ #-1,D7 */
        0x6502, 0x7CFE,          /* BCS.S +2; MOVEQ #-2,D6 */
        0x4EAE, (UWORD)-60,      /* JSR -60(A6): no thunk, D0 = 0 */
        0x2A00,                  /* MOVE.L D0,D5 */
        0x7003,                  /* MOVEQ #3,D0 */
        0x4EAE, (UWORD)-36,      /* JSR -36(A6): snapshot */
        0x4E75
    });
};

static void library_calls()
{
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        call_program(m);
        echo_calls = snap_calls = 0;
        CHECK(m.run(which) == 3);
        CHECK(echo_calls == 1 && snap_calls == 1);
        for (unsigned i = 0; i < 8; i++) CHECK(echo_seen.d[i] == 0xD0000000u + i);
        for (unsigned i = 0; i < 6; i++) CHECK(echo_seen.a[i] == 0xA0000000u + i);
        CHECK(echo_seen.a[6] == Layout::probe && echo_seen.a[7] == Layout::stack - 4);
        CHECK((echo_seen.sr & 0x1F) == 0 && (echo_seen.sr & 0x2700) == 0x2700);
        CHECK(snap.d[2] == 0x76543210 && snap.a[3] == 0xABCDEF00);
        CHECK(snap.d[7] == 0xD0000007 && snap.d[6] == 0xD0000006 && snap.d[5] == 0);
        CHECK(snap.d[1] == 0xD0000001 && snap.d[3] == 0xD0000003 && snap.a[7] == Layout::stack - 4);
    }
    /* A thousand calls: each leaves translated code at the A-line and comes
       back to the block after the JSR, translated once. */
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        m.a(6, Layout::probe);
        m.code(m.pc, {0x7000, 0x323C, 999, 0x4EAE, (UWORD)-42, 0x51C9, 0xFFFA, 0x4E75});
        count_calls = 0;
        CHECK(m.run(which) == 1000 && count_calls == 1000);
        if (which == JIT)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            /* entry, vector, DBRA after the return, JSR of the loop, RTS */
            CHECK(s->exits[HJ_EXIT_LINEA] == 1000 && s->blocks == 5);
            std::printf("  1000 calls: %lu blocks, %lu enters, %lu A-line exits\n",
                        s->blocks, s->enters, s->exits[HJ_EXIT_LINEA]);
        }
    }
}

/* ------------------------------------------------------------------
   5. Callbacks: dispatcher, 68k hook, dispatcher
   ------------------------------------------------------------------ */

static void callback_depth()
{
    for (Backend which : { MOIRA, JIT })
        for (ULONG depth : { KAFFEINE_CALLBACK_DEPTH, KAFFEINE_CALLBACK_DEPTH + 1 })
        {
            Machine m;
            /* SUBQ.L #1,D2; BEQ leaf; JSR -102(A6); ADDQ.L #1,D0; RTS;
               leaf: MOVEQ #1,D0; RTS. The real hook thunk is reentered. */
            m.code(Layout::hook_code, {0x5382, 0x6708, 0x4EAE, (UWORD)-102,
                                       0x5280, 0x4E75, 0x7001, 0x4E75});
            m.hook_at(Layout::hook, Layout::hook_code);
            m.d(2, depth); m.hook_call();
            m.word(0x13FC); m.word(0x003C); m.longword(Layout::buffer); /* MOVE.B #'<',buffer */
            m.rts();
            bool deep = depth > KAFFEINE_CALLBACK_DEPTH;
            CHECK(m.run(which) == (deep ? 20 : (LONG)depth));
            CHECK(m.ctx.callback_depth == 0 && m.ctx.callback_failed == (BOOL)deep);
            CHECK(m.mem[Layout::buffer] == (deep ? 0xA5 : '<'));
            if (which == JIT && !deep)
                CHECK(HJ_GetStats(m.jit)->max_depth == depth + 1);
        }
}

static void callback_registers()
{
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        /* The hook clobbers every register but A7, and the flags. */
        m.pc = Layout::hook_code;
        m.word(0x2008); m.word(0xD089); m.word(0xD08A); /* D0 = A0 + A1 + A2 */
        for (unsigned i = 1; i < 8; i++) m.d(i, 0xBAD00000u + i);
        for (unsigned i = 0; i < 7; i++) m.a(i, 0xBAD10000u + i);
        m.word(0x44FC); m.word(0x001F); m.rts();
        m.pc = Layout::entry;
        m.hook_at(Layout::hook, Layout::hook_code);
        for (unsigned i = 0; i < 8; i++) m.d(i, 0xD0000000u + i);
        for (unsigned i = 0; i < 7; i++) m.a(i, 0xA0000000u + i);
        m.a(0, Layout::hook); m.a(1, Layout::args); m.a(2, Layout::buffer);
        m.a(6, Layout::utility);
        m.word(0x46FC); m.word(0x2715);               /* MOVE #$2715,SR */
        m.word(0x4EAE); m.word((UWORD)-102);
        m.save();
        m.rts();
        ULONG want = Layout::hook + Layout::args + Layout::buffer;
        CHECK((ULONG)m.run(which) == want);
        CHECK(m.read(Layout::snapshot) == want);
        for (unsigned i = 1; i < 8; i++) CHECK(m.read(Layout::snapshot + 4 * i) == 0xD0000000u + i);
        const ULONG address[] = { Layout::hook, Layout::args, Layout::buffer, 0xA0000003u,
                                  0xA0000004u, 0xA0000005u, Layout::utility, Layout::stack - 4 };
        for (unsigned i = 0; i < 8; i++) CHECK(m.read(Layout::snapshot + 32 + 4 * i) == address[i]);
        CHECK(m68k_read16(&m.ctx, Layout::snapshot + 64) == 0x2715);
        CHECK(m.ctx.callback_depth == 0 && !generated_calls);
    }
}

static void callback_format()
{
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        /* RawDoFmt() with a 68k PutChProc that is not the copy the bridge
           recognizes, so it runs once per character. Each character also
           calls a second hook through CallHookPkt(), which counts. */
        const ULONG counter = Layout::buffer + 0x400, hook2 = Layout::hook + 32;
        m.pc = Layout::hook_code;                      /* PutChProc */
        m.word(0x4E71);                                /* NOP */
        m.word(0x48E7); m.word(0xC0E2);                /* MOVEM.L D0-D1/A0-A2/A6,-(SP) */
        m.a(0, hook2); m.a(6, Layout::utility);
        m.word(0x4EAE); m.word((UWORD)-102);
        m.word(0x4CDF); m.word(0x4703);                /* MOVEM.L (SP)+,D0-D1/A0-A2/A6 */
        m.word(0x16C0);                                /* MOVE.B D0,(A3)+ */
        m.rts();
        ULONG count_code = m.pc;
        m.word(0x52B9); m.longword(counter);           /* ADDQ.L #1,counter */
        m.rts();
        m.hook_at(hook2, count_code);
        m.pc = Layout::entry;
        m.text(Layout::fmt, "%d|%s|%lx");
        m68k_write16(&m.ctx, Layout::args, (UWORD)-1234);
        m68k_write32(&m.ctx, Layout::args + 2, Layout::fmt + 0x100);
        m68k_write32(&m.ctx, Layout::args + 6, 0xBEEF);
        m.text(Layout::fmt + 0x100, "guest");
        m68k_write32(&m.ctx, counter, 0);
        m.a(0, Layout::fmt); m.a(1, Layout::args); m.a(2, Layout::hook_code); m.a(3, Layout::buffer);
        m.jsr(Layout::exec, 522);
        m.rts();
        CHECK((ULONG)m.run(which) == Layout::args + 10);
        CHECK(m.string_at(Layout::buffer) == "-1234|guest|BEEF");
        CHECK(m.read(counter) == 17 && !m.ctx.callback_failed && !generated_calls);
        if (which == JIT)
            CHECK(HJ_GetStats(m.jit)->max_depth == 3);
    }
}

static void callback_faults()
{
    const std::vector<std::vector<UWORD>> cases = {
        {0x4AFC},                           /* ILLEGAL */
        {0xA000},                           /* Line-A outside a library */
        {0xFFFF},                           /* Line-F */
        {0x2039, 0x0010, 0x0000},           /* MOVE.L outside arena,D0 */
        {0x13FC, 0x0041, 0xFFFF, 0xFFFF},   /* MOVE.B #65,$FFFFFFFF */
        {0x2E7C, 0x0010, 0x0000, 0x4E71},   /* SP outside arena */
        {0x4EF9, 0x0000, (UWORD)KAFFEINE_CALLBACK_RETURN_BASE},       /* JMP, unpopped SP */
        {0x4EF9, 0x0000, (UWORD)(KAFFEINE_CALLBACK_RETURN_BASE + 4)}  /* wrong level */
    };
    for (Backend which : { MOIRA, JIT })
        for (const auto &code : cases)
        {
            Machine m;
            m.words(Layout::hook_code, code);
            m.hook_at(Layout::hook, Layout::hook_code);
            m.hook_call();
            m.word(0x13FC); m.word(0x003C); m.longword(Layout::buffer);
            m.rts();
            CHECK(m.run(which) == 20);
            CHECK(m.mem[Layout::buffer] == 0xA5 && m.ctx.exit_code == 20);
            CHECK(m.ctx.callback_failed && !m.ctx.callback_depth);
            if (which == JIT)
                CHECK(HJ_LastFault(m.jit)[0] || code[0] == 0x2E7C || code[0] == 0x4EF9);
        }
    /* A thunk that stops the program inside a callback keeps its code. */
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        m.pc = Layout::hook_code; m.jsr(Layout::probe, 48); m.rts();
        m.pc = Layout::entry;
        m.hook_at(Layout::hook, Layout::hook_code);
        m.hook_call();
        m.word(0x13FC); m.word(0x003C); m.longword(Layout::buffer);
        m.rts();
        CHECK(m.run(which) == 7 && m.ctx.callback_failed && m.mem[Layout::buffer] == 0xA5);
    }
}

/* A void thunk may return 0 after recording a fatal stop. The callback
   and both dispatch adapters must retain that code through every level. */
static void callback_stop_codes()
{
    for (Backend which : { MOIRA, JIT, JIT_LIBRARY })
        for (UWORD lvo : { 48, 54 })
        {
            Machine m;
            const ULONG inner = Layout::hook + 32, inner_code = Layout::hook_code + 0x100;
            m.pc = inner_code; m.jsr(Layout::probe, lvo); m.word(0x702A); m.rts();
            m.hook_at(inner, inner_code);
            m.pc = Layout::hook_code; m.hook_call(inner); m.rts();
            m.hook_at(Layout::hook, Layout::hook_code);
            m.pc = Layout::entry; m.hook_call();
            m.word(0x13FC); m.word(0x003C); m.longword(Layout::buffer); m.rts();
            LONG expected = lvo == 54 ? 20 : 7;
            CHECK(m.run(which) == expected);
            CHECK(m.ctx.exit_code == expected && m.ctx.callback_failed);
            CHECK(!m.ctx.running && !m.ctx.callback_depth && m.mem[Layout::buffer] == 0xA5);
            CHECK(!generated_calls);
            if (which != MOIRA)
                CHECK(HJ_GetStats(m.jit)->max_depth == 3 && !HJ_LastFault(m.jit)[0]);
        }
}

static void bitfield_boundaries()
{
    /* The reference backend accesses a longword even for a one-bit field.
       Compare the observable boundary behavior, including partial word
       stores outside a callback and the untouched host guard. */
    for (unsigned op = 0; op < 8; ++op)
        for (ULONG bytes : {1u, 2u, 3u, 4u})
        {
            Seen reference = {};
            std::vector<UBYTE> memory;
            for (Backend which : {MOIRA, JIT})
            {
                Machine m;
                const ULONG at = m.size - bytes;
                m.mem[at] = 0x80;
                m.a(0, at); m.d(1, 0x89ABCDEF);
                m.word(0x44FC); m.word(0x001B);
                m.word((UWORD)(0xE8D0 + (op << 8)));
                m.word((UWORD)(0x0001 + ((op & 1) ? 0x1000 : 0)));
                m.jsr(Layout::probe, 36); m.rts();
                snap = {}; snap_calls = 0;
                CHECK(m.run(which) == 0 && snap_calls == 1);
                CHECK(!m.ctx.callback_failed);
                CHECK(std::all_of(m.mem.begin() + m.size, m.mem.end(),
                                  [](UBYTE b) { return b == 0; }));
                std::vector<UBYTE> tail(m.mem.begin() + m.size - 8, m.mem.end());
                if (which == MOIRA) { reference = snap; memory = tail; }
                else CHECK(snap == reference && tail == memory);
            }
        }

    /* A five-byte field can finish its first longword write before the
       fifth byte faults inside a callback. Failure does not roll back
       those completed stores or resume the caller. */
    const ULONG expected[] = {0x12345678, 0x12345678, 0x13CBA987, 0x12345678,
                              0x12000000, 0x12345678, 0x13FFFFFF, 0x1313579B};
    for (Backend which : {MOIRA, JIT})
        for (unsigned op = 0; op < 8; ++op)
        {
            Machine m;
            const ULONG at = m.size - 4;
            m68k_write32(&m.ctx, at, 0x12345678);
            m.code(Layout::hook_code, {
                0x207C, (UWORD)(at >> 16), (UWORD)at,
                0x223C, 0x89AB, 0xCDEF, (UWORD)(0xE8D0 + (op << 8)),
                (UWORD)(0x01C0 + ((op & 1) ? 0x1000 : 0)), 0x4E75});
            m.hook_at(Layout::hook, Layout::hook_code); m.hook_call();
            m.word(0x13FC); m.word(0x003C); m.longword(Layout::buffer); m.rts();
            CHECK(m.run(which) == 20);
            CHECK(m.ctx.callback_failed && !m.ctx.callback_depth);
            CHECK(m.mem[Layout::buffer] == 0xA5);
            if (m.read(at) != expected[op])
                std::printf("  bitfield %u %s: completed prefix %08lx, expected %08lx\n",
                            op, which == MOIRA ? "Moira" : "JIT",
                            (unsigned long)m.read(at), (unsigned long)expected[op]);
            CHECK(m.read(at) == expected[op]);
            CHECK(std::all_of(m.mem.begin() + m.size, m.mem.end(),
                              [](UBYTE b) { return b == 0; }));
        }
}

/* ------------------------------------------------------------------
   Guest code that changes, and the cache flushes that make it count
   ------------------------------------------------------------------ */

static void code_changes()
{
    for (Backend which : { MOIRA, JIT })
    {
        Machine m;
        const ULONG sub = Layout::hook_code, same = sub + 0x10, results = Layout::buffer;
        m.code(sub, {0x7001, 0x4E75});                      /* MOVEQ #1,D0; RTS */
        m.code(same, {0x7205, 0x4E75});                     /* MOVEQ #5,D1; RTS */
        auto call = [&](unsigned slot) {
            m.word(0x4EB9); m.longword(sub);                /* JSR sub */
            m.word(0x23C0); m.longword(results + 4 * slot); /* MOVE.L D0,results+n */
            m.word(0x4EB9); m.longword(same);               /* JSR same: unchanged */
        };
        call(0);
        m.word(0x33FC); m.word(0x7002); m.longword(sub);    /* MOVE.W #$7002,sub */
        call(1);
        m.jsr(Layout::exec, 636);                           /* CacheClearU() */
        call(2);
        m.word(0x33FC); m.word(0x7003); m.longword(sub);
        call(3);
        m.a(0, sub); m.d(0, 2); m.d(1, 0x0808);             /* CacheClearE(sub, 2, I|D) */
        m.jsr(Layout::exec, 642);
        call(4);
        m.rts();
        m.run(which);
        const ULONG jit_want[] = { 1, 1, 2, 2, 3 }, moira_want[] = { 1, 2, 2, 3, 3 };
        for (unsigned i = 0; i < 5; i++)
            CHECK(m.read(results + 4 * i) == (which == JIT ? jit_want[i] : moira_want[i]));
        if (which == JIT)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            CHECK(s->flushes == 2 && s->revalidated >= 2);
            std::printf("  2 flushes: %lu blocks kept unchanged, %lu blocks in all\n",
                        s->revalidated, s->blocks);
        }
    }
}

/* ------------------------------------------------------------------
   Faults outside callbacks
   ------------------------------------------------------------------ */

static void faults()
{
    struct { std::vector<UWORD> code; const char *why; } cases[] = {
        { {0x7005, 0x4AFC}, "ILLEGAL" },
        { {0x7005, 0x80C8}, "DIV.W mode" },                       /* DIVU.W A0,D0 is illegal */
        { {0x7005, 0x4C00, 0x8000}, "MUL.L extension" },          /* MULU.L with bit 15 set */
        { {0x7005, 0x4C40, 0x8000}, "DIV.L extension" },          /* DIVU.L with bit 15 set */
        { {0x7005, 0x4C7C, 0x0000, 0x0000, 0x0000}, "divide by zero" },
        { {0x7005, 0xE9C0, 0x8000}, "bitfield extension" },       /* BFEXTU with bit 15 set */
        { {0x7005, 0x0AD0, 0x8000}, "CAS extension" },            /* CAS.B with bit 15 set */
        { {0x7005, 0xF200, 0xB000}, "Line-F" },
        { {0x7005, 0x4E73}, "opcode outside the subset" }         /* RTE */
    };
    for (auto &c : cases)
    {
        Machine m;
        m.words(Layout::entry, c.code);
        CHECK(m.run(JIT) == 20 && m.ctx.exit_code == 20);
        CHECK(std::strncmp(HJ_LastFault(m.jit), c.why, std::strlen(c.why)) == 0);
        CHECK(std::strstr(HJ_LastFault(m.jit), "PC=0x00001002") != nullptr);
    }
    /* The extension words of a faulting instruction count when a flush
       checks its block: with a brief extension word instead of a full one,
       the same MOVE.L runs on the next run of the backend. */
    Machine x;
    m68k_write32(&x.ctx, Layout::scratch, 0x12345678);
    x.jsr(Layout::exec, 636);                   /* CacheClearU() */
    x.a(0, Layout::scratch);
    x.word(0x7000);                             /* MOVEQ #0,D0 */
    const ULONG ext = x.pc + 2;
    x.word(0x2030); x.word(0x0100);             /* MOVE.L (0,A0,D0.W),D0, full format */
    x.rts();
    CHECK(x.run(JIT) == 20 && std::strncmp(HJ_LastFault(x.jit), "MOVE source", 11) == 0);
    x.code(ext, {0x0000});                      /* the brief format */
    CHECK(x.run(JIT) == 0x12345678 && HJ_GetStats(x.jit)->flushes == 2);
    /* Supervisor() is not part of the probe. */
    Machine s;
    s.a(5, Layout::entry + 64);
    s.jsr(Layout::exec, 30);
    s.rts();
    static const M68KThunkEntry supervisor[] = { { 30, [](KaffeineContext *ctx, void *cpu) -> IPTR {
        ctx->sv_redirect = Kaffeine_GetA(cpu, 5); return 0; } } };
    s.ctx.libs[2].thunks = supervisor; s.ctx.libs[2].num_thunks = 1;
    CHECK(s.run(JIT) == 20 && std::strstr(HJ_LastFault(s.jit), "Supervisor") != nullptr);
}

/* ------------------------------------------------------------------
   Random programs against Moira
   ------------------------------------------------------------------ */

struct Rng
{
    uint64_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
    unsigned below(unsigned n) { return next() % n; }
    bool chance(unsigned pct) { return below(100) < pct; }
};

struct GenEa
{
    int mode, reg;
    std::vector<UWORD> ext;
};

/* Programs of the probe's subset whose effects Moira defines the same way:
   data accesses only in the scratch area, out of the arena or at its end,
   or reads of the beam position; A0-A3 point into the scratch area and
   move by small steps, A4-A5 hold data, A6 is the probe library, A7 moves
   in pairs. Forward branches, DBcc loops, BSR, MOVEM and SR moves. */
struct Gen
{
    Rng &rng;
    ULONG arena;
    std::vector<UWORD> out;
    unsigned no_d = 0;     /* D registers that must keep their value */

    Gen(Rng &r, ULONG a) : rng(r), arena(a) {}

    void w(UWORD x) { out.push_back(x); }
    void l(ULONG x) { w((UWORD)(x >> 16)); w((UWORD)x); }
    static int sz(int size) { return size == 1 ? 0 : size == 2 ? 1 : 2; }
    int dreg() { int r; do r = (int)rng.below(8); while (no_d & (1u << r)); return r; }
    int ptr() { return (int)rng.below(4); }
    int size() { static const int s[3] = { 1, 2, 4 }; return s[rng.below(3)]; }
    ULONG scratch_addr(int size)
    { return (Layout::scratch + 0x400 + rng.below(Layout::scratch_end - Layout::scratch - 0x800)) & (size > 1 ? ~1u : ~0u); }
    ULONG odd_addr()
    {
        switch (rng.below(5))
        {
        case 0: return arena - 1 - rng.below(4);
        case 1: return arena + rng.below(64);
        case 2: return 0xFFFFFF00u + rng.below(0xF0);
        case 3: return 0x00F00000u + rng.below(0x100) * 2;
        default: return 0xDFF004u + 2 * rng.below(2);
        }
    }
    GenEa imm(int size)
    {
        ULONG v = rng.next();
        if (rng.chance(30)) v = rng.below(8) - 4;
        if (rng.chance(10)) v = 0x80000000u >> (32 - 8 * size);
        if (size == 4) return { 7, 4, { (UWORD)(v >> 16), (UWORD)v } };
        return { 7, 4, { (UWORD)(size == 1 ? (v & 0xFF) : (v & 0xFFFF)) } };
    }
    /* The MOVEQ before an indexed operand keeps its index small. */
    GenEa indexed(int base, bool pc_relative)
    {
        int x = dreg();
        int scale = (int)rng.below(4);
        int disp = (int)rng.below(64) - 32;
        w((UWORD)(0x7000 | x << 9 | ((rng.below(33) - 16) & 0xFF)));
        UWORD ext = (UWORD)(x << 12 | (rng.below(2) << 11) | (scale << 9) | (disp & 0xFF));
        return { pc_relative ? 7 : 6, pc_relative ? 3 : base, { ext } };
    }
    GenEa memory(int size, bool write)
    {
        int p = ptr();
        switch (rng.below(write ? 7 : 9))
        {
        case 0: return { 2, p, {} };
        case 1: return { 3, p, {} };
        case 2: return { 4, p, {} };
        case 3: return { 5, p, { (UWORD)(int16_t)((int)rng.below(512) - 256) } };
        case 4: return indexed(p, false);
        case 5: { ULONG a = scratch_addr(size); return { 7, 1, { (UWORD)(a >> 16), (UWORD)a } }; }
        case 6:
        {
            ULONG a = odd_addr();
            if (write && (a & ~0xFFFu) == 0xDFF000u) a = arena + 16;
            if (rng.chance(30) && a >= 0xFFFF8000u) return { 7, 0, { (UWORD)a } };
            return { 7, 1, { (UWORD)(a >> 16), (UWORD)a } };
        }
        case 7: return { 7, 2, { (UWORD)(int16_t)(-(int)rng.below(64) * 2) } };
        default: return indexed(0, true);
        }
    }
    GenEa source(int size, bool data_only)
    {
        unsigned k = rng.below(10);
        if (k < 3) return { 0, (int)rng.below(8), {} };
        if (k == 3 && !data_only && size > 1) return { 1, (int)rng.below(8), {} };
        if (k < 6) return imm(size);
        return memory(size, false);
    }
    GenEa dest(int size, bool memory_only = false)
    {
        if (!memory_only && rng.chance(40)) return { 0, dreg(), {} };
        return memory(size, true);
    }
    void ea_ext(const GenEa &e) { for (UWORD x : e.ext) w(x); }

    void simple()
    {
        int s = size();
        switch (rng.below(37))
        {
        case 0: w((UWORD)(0x7000 | dreg() << 9 | (rng.next() & 0xFF))); break;
        case 1: case 2:
        {
            GenEa src = source(s, s == 1), dst = dest(s);
            w((UWORD)((s == 1 ? 1 : s == 2 ? 3 : 2) << 12 | dst.reg << 9 | dst.mode << 6 | src.mode << 3 | src.reg));
            ea_ext(src); ea_ext(dst);
            break;
        }
        case 3:
        {
            int sw = rng.chance(50) ? 2 : 4;
            GenEa src = source(sw, false);
            w((UWORD)((sw == 2 ? 3 : 2) << 12 | (4 + (int)rng.below(2)) << 9 | 1 << 6 | src.mode << 3 | src.reg));
            ea_ext(src);
            break;
        }
        case 4:
        {
            if (rng.chance(50))
            {
                int p = ptr(), q = ptr();
                w((UWORD)(0x41E8 | p << 9 | q)); w((UWORD)(int16_t)((int)rng.below(128) - 64));
            }
            else
            {
                GenEa src = memory(4, false);
                if (src.mode == 3 || src.mode == 4) src = { 2, src.reg, {} };
                w((UWORD)(0x41C0 | (4 + (int)rng.below(2)) << 9 | src.mode << 3 | src.reg));
                ea_ext(src);
            }
            break;
        }
        case 5: case 6:
        {
            static const UWORD base[5] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
            int op = (int)rng.below(5);
            GenEa src = source(s, op == 2 || op == 3 || s == 1);
            w((UWORD)(base[op] | dreg() << 9 | sz(s) << 6 | src.mode << 3 | src.reg));
            ea_ext(src);
            break;
        }
        case 7:
        {
            static const UWORD base[5] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
            int op = (int)rng.below(5);
            GenEa dst = dest(s, op != 4);
            w((UWORD)(base[op] | (int)rng.below(8) << 9 | (4 + sz(s)) << 6 | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 8: case 9:
        {
            static const UWORD base[6] = { 0x0000, 0x0200, 0x0400, 0x0600, 0x0A00, 0x0C00 };
            unsigned which = rng.below(6);
            GenEa im = imm(s);
            GenEa dst = (which == 5 && rng.chance(30)) ? memory(s, false) : dest(s);
            w((UWORD)(base[which] | sz(s) << 6 | dst.mode << 3 | dst.reg));
            ea_ext(im); ea_ext(dst);
            break;
        }
        case 10:
        {
            static const UWORD base[3] = { 0xD000, 0x9000, 0xB000 };
            int op = (int)rng.below(3), sw = rng.chance(50) ? 2 : 4;
            GenEa src = source(sw, false);
            int an = op == 2 ? (int)rng.below(8) : 4 + (int)rng.below(2);
            w((UWORD)(base[op] | an << 9 | (sw == 2 ? 3 : 7) << 6 | src.mode << 3 | src.reg));
            ea_ext(src);
            break;
        }
        case 11: case 12:
        {
            unsigned q = rng.below(8);
            GenEa dst = dest(s);
            if (s > 1 && rng.chance(25)) dst = { 1, rng.chance(70) ? ptr() : 4 + (int)rng.below(2), {} };
            w((UWORD)(0x5000 | q << 9 | (rng.below(2) << 8) | sz(s) << 6 | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 13:
        {
            static const UWORD base[5] = { 0x4000, 0x4200, 0x4400, 0x4600, 0x4A00 };
            unsigned which = rng.below(5);
            GenEa dst = (which == 4 && rng.chance(40)) ? source(s, s == 1) : dest(s);
            w((UWORD)(base[which] | sz(s) << 6 | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 14:
        {
            static const UWORD base[4] = { 0x4840, 0x4880, 0x48C0, 0x49C0 };
            w((UWORD)(base[rng.below(4)] | dreg()));
            break;
        }
        case 15:
        {
            /* ASL ASR LSL LSR ROL ROR ROXL ROXR: register/immediate (.B/.W/.L) or 1-bit <ea>.W */
            if (rng.chance(20))
            {
                GenEa dst = dest(2, true);
                w((UWORD)(0xE0C0 | (rng.below(4) << 9) | (rng.below(2) << 8) | dst.mode << 3 | dst.reg));
                ea_ext(dst);
                break;
            }
            unsigned cnt = rng.below(8), reg_cnt = rng.chance(45) ? 1 : 0;
            w((UWORD)(0xE000 | (cnt << 9) | (rng.below(2) << 8) | (sz(s) << 6) |
                      (reg_cnt << 5) | (rng.below(4) << 3) | dreg()));
            break;
        }
        case 16:
        {
            GenEa dst = dest(1);
            w((UWORD)(0x50C0 | rng.below(16) << 8 | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 17:
        {
            GenEa dst = dest(2);
            w((UWORD)((rng.chance(50) ? 0x40C0 : 0x42C0) | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 18:
        {
            GenEa src = source(2, true);
            w((UWORD)(0x44C0 | src.mode << 3 | src.reg));
            ea_ext(src);
            break;
        }
        case 19:
        {
            unsigned form = rng.below(4);
            if (form == 0)
            {
                w(0x46FC); w((UWORD)(0x2000 | rng.below(8) << 8 | rng.below(32)));
            }
            else if (form == 1 || form == 2)
            {
                static const UWORD ops[3] = { 0x003C, 0x023C, 0x0A3C };
                w(ops[rng.below(3)]); w((UWORD)rng.below(32));
            }
            else
            {
                static const UWORD ops[3] = { 0x007C, 0x027C, 0x0A7C };
                unsigned k = rng.below(3);
                UWORD imm = (UWORD)((rng.below(8) << 8) | rng.below(32));
                if (k == 1) imm |= 0x2000;
                else if (k == 2) imm &= (UWORD)~0x2000u;
                w(ops[k]); w(imm);
            }
            break;
        }
        case 20:
        {
            /* MOVEM.W and MOVEM.L to and from the scratch area */
            bool wide = rng.chance(60);
            UWORD szbit = wide ? 0x0040 : 0;
            UWORD mask = (UWORD)(rng.next() & 0x30FF & ~no_d);
            if (!mask) mask = 1u << dreg();
            if (rng.chance(50))
            {
                int p = ptr();
                UWORD stored = (UWORD)(rng.next() & ~(1u << (8 + p)));
                UWORD rev = 0;
                for (int i = 0; i < 16; i++) if (stored & (1u << i)) rev |= (UWORD)(1u << (15 - i));
                w((UWORD)(0x48A0 | szbit | p)); w(rev ? rev : 1);
            }
            else
            {
                GenEa src = memory(wide ? 4 : 2, false);
                if (src.mode == 4) src.mode = 2;
                if (src.mode == 3) mask &= (UWORD)~(1u << (8 + src.reg));
                if (src.mode == 7 && src.reg == 1) src = { 5, ptr(), { 0 } };
                if (!mask) mask = 1;
                w((UWORD)(0x4C80 | szbit | src.mode << 3 | src.reg)); w(mask);
                ea_ext(src);
            }
            break;
        }
        case 21: case 22:
        {
            /* BTST BCHG BCLR BSET (#imm or Dn) */
            unsigned bop = rng.below(4);
            bool dyn = rng.chance(50);
            GenEa dst = (bop == 0 && rng.chance(30)) ? source(1, true) : dest(1);
            if (bop == 0 && !dyn && dst.mode == 7 && dst.reg == 4)
                dst = { 0, dreg(), {} };
            if (dyn)
            {
                w((UWORD)(0x0100 | (rng.below(8) << 9) | (bop << 6) | dst.mode << 3 | dst.reg));
                ea_ext(dst);
            }
            else
            {
                w((UWORD)(0x0800 | (bop << 6) | dst.mode << 3 | dst.reg));
                w((UWORD)(rng.next() & 0xFF));
                ea_ext(dst);
            }
            break;
        }
        case 23:
        {
            /* ADDX / SUBX (Dy,Dx or -(Ay),-(Ax)) */
            UWORD base = rng.chance(50) ? 0xD100 : 0x9100;
            if (rng.chance(65))
                w((UWORD)(base | dreg() << 9 | sz(s) << 6 | rng.below(8)));
            else
                w((UWORD)(base | ptr() << 9 | sz(s) << 6 | 0x0008 | ptr()));
            break;
        }
        case 24:
        {
            /* CMPM (Ay)+,(Ax)+ */
            w((UWORD)(0xB108 | ptr() << 9 | sz(s) << 6 | ptr()));
            break;
        }
        case 25:
        {
            /* EXG Dx,Dy / Ax,Ay / Dx,Ay */
            unsigned mode = rng.below(3);
            if (mode == 0)
                w((UWORD)(0xC140 | dreg() << 9 | dreg()));
            else if (mode == 1)
            {
                int a1 = rng.chance(50) ? ptr() : 4 + (int)rng.below(2);
                int a2 = (a1 < 4) ? ptr() : 4 + (int)rng.below(2);
                w((UWORD)(0xC148 | a1 << 9 | a2));
            }
            else
                w((UWORD)(0xC188 | dreg() << 9 | (4 + (int)rng.below(2))));
            break;
        }
        case 26: case 27:
        {
            /* MULU.W / MULS.W <ea>,Dn */
            GenEa src = source(2, true);
            w((UWORD)((rng.chance(50) ? 0xC0C0 : 0xC1C0) | dreg() << 9 | src.mode << 3 | src.reg));
            ea_ext(src);
            break;
        }
        case 28: case 29:
        {
            /* MULU.L / MULS.L <ea>,Dl or Dh:Dl */
            GenEa src = source(4, true);
            int dl = dreg(), dh = rng.chance(20) ? dl : dreg();
            UWORD ext = (UWORD)((dl << 12) | (rng.below(2) << 11) | (rng.below(2) << 10) | dh);
            w((UWORD)(0x4C00 | src.mode << 3 | src.reg));
            w(ext);
            ea_ext(src);
            break;
        }
        case 30:
        {
            /* DIVU.L / DIVS.L / DIVUL.L / DIVSL.L with non-zero divisor */
            int dq = dreg(), dr = rng.chance(20) ? dq : dreg();
            UWORD ext = (UWORD)((dq << 12) | (rng.below(2) << 11) | (rng.below(2) << 10) | dr);
            if (rng.chance(50))
            {
                ULONG div = rng.chance(40) ? (rng.below(64) + 1) : (rng.next() | 1u);
                if (rng.chance(20)) div = (ULONG)-(int32_t)(rng.below(16) + 1);
                w(0x4C7C); w(ext); l(div);
            }
            else
            {
                int dv = dreg();
                w((UWORD)(0x0000 | dv)); w(0x0001); /* ORI.B #1,Dv */
                w((UWORD)(0x4C40 | dv)); w(ext);
            }
            break;
        }
        case 31: case 32:
        {
            /* BFTST BFEXTU BFCHG BFEXTS BFCLR BFFFO BFSET BFINS */
            unsigned bf_op = rng.below(8);
            int dn = (bf_op & 1) ? dreg() : 0;
            bool do_reg = rng.chance(45), dw_reg = rng.chance(45);
            int of = do_reg ? (int)rng.below(8) : (int)rng.below(32);
            int wi = dw_reg ? (int)rng.below(8) : (int)rng.below(32);
            UWORD ext = (UWORD)((dn << 12) | (do_reg << 11) | (of << 6) | (dw_reg << 5) | wi);
            if (rng.chance(55))
            {
                w((UWORD)(0xE8C0 | (bf_op << 8) | dreg()));
                w(ext);
            }
            else
            {
                if (do_reg)
                {
                    int r_of = dreg();
                    of = r_of;
                    ext = (UWORD)((dn << 12) | (1 << 11) | (of << 6) | (dw_reg << 5) | wi);
                    w((UWORD)(0x49C0 | r_of)); /* EXTB.L D_of: keep memory bit offset in -128..127 */
                }
                int p = ptr();
                if (rng.chance(50))
                {
                    w((UWORD)(0xE8D0 | (bf_op << 8) | p));
                    w(ext);
                }
                else
                {
                    w((UWORD)(0xE8E8 | (bf_op << 8) | p));
                    w(ext);
                    w((UWORD)(int16_t)((int)rng.below(128) - 64));
                }
            }
            break;
        }
        case 33:
        {
            /* TAS <ea> */
            GenEa dst = dest(1);
            w((UWORD)(0x4AC0 | dst.mode << 3 | dst.reg));
            ea_ext(dst);
            break;
        }
        case 34:
        {
            /* MOVEP.W / MOVEP.L Dx,(d16,Ay) or (d16,Ay),Dx */
            bool to_mem = rng.chance(50), wide = rng.chance(50);
            int dx = dreg(), ay = ptr();
            UWORD op = (UWORD)(0x0108 | (dx << 9) | (to_mem ? 0x0080 : 0) | (wide ? 0x0040 : 0) | ay);
            w(op);
            w((UWORD)(int16_t)((int)rng.below(128) - 64));
            break;
        }
        case 35:
        {
            /* CAS.B / CAS.W / CAS.L Dc,Du,<ea> */
            GenEa dst = dest(s, true);
            UWORD base = s == 1 ? 0x0AC0 : s == 2 ? 0x0CC0 : 0x0EC0;
            int dc = dreg(), du = dreg();
            w((UWORD)(base | dst.mode << 3 | dst.reg));
            w((UWORD)((du << 6) | dc));
            ea_ext(dst);
            break;
        }
        default:
            w(0x4E71);
            break;
        }
    }

    /* A branch over a few instructions, on any condition. */
    void skip()
    {
        std::vector<UWORD> saved;
        saved.swap(out);
        unsigned n = 1 + rng.below(4);
        for (unsigned i = 0; i < n; i++) simple();
        std::vector<UWORD> body;
        body.swap(out);
        out.swap(saved);
        /* Bcc.S, Bcc.W, Bcc.L, BRA among them, or JMP (d16,PC) or through A4 */
        unsigned cond = rng.chance(10) ? 0 : 2 + rng.below(14), len = 2 * (unsigned)body.size();
        unsigned form = rng.below(10);
        if (form < 5 && len < 128)
            w((UWORD)(0x6000 | cond << 8 | len));
        else if (form < 7)
        {
            w((UWORD)(0x6000 | cond << 8));
            w((UWORD)(len + 2));
        }
        else if (form < 8)
        {
            w((UWORD)(0x60FF | cond << 8));
            l(len + 4);
        }
        else if (form < 9)
        {
            w(0x4EFA); w((UWORD)(len + 2));                  /* JMP (d16,PC) */
        }
        else
        {
            w(0x49FA); w((UWORD)(len + 4));                  /* LEA (d16,PC),A4 */
            w(0x4ED4);                                       /* JMP (A4) */
        }
        out.insert(out.end(), body.begin(), body.end());
    }

    void anchor(int p)
    {
        w((UWORD)(0x207C | p << 9));
        l((Layout::scratch + 0x3000 + rng.below(0x2000)) & ~1u);
    }

    void loop()
    {
        for (int p = 0; p < 4; p++) anchor(p);
        w((UWORD)(0x7E00 | rng.below(6)));        /* MOVEQ #n,D7 */
        size_t start = out.size();
        no_d |= 0x80;
        unsigned n = 1 + rng.below(5);
        for (unsigned i = 0; i < n; i++) simple();
        no_d &= ~0x80u;
        unsigned cond = rng.chance(60) ? 1 : rng.below(16);
        w((UWORD)(0x50C8 | cond << 8 | 7));
        w((UWORD)(int16_t)(-(int)(2 * (out.size() - start))));
    }

    void push_pop()
    {
        if (rng.chance(25))
        {
            /* LINK.W or LINK.L on A4/A5, frame accesses, then UNLK */
            int an = 4 + (int)rng.below(2);
            int disp = -((int)(1 + rng.below(8)) * 4);
            if (rng.chance(50))
            {
                w((UWORD)(0x4E50 | an));
                w((UWORD)(int16_t)disp);
            }
            else
            {
                w((UWORD)(0x4808 | an));
                l((ULONG)(int32_t)disp);
            }
            w((UWORD)(0x2940 | dreg() | (an << 9))); w((UWORD)(int16_t)-4); /* MOVE.L Dn,-4(An) */
            w((UWORD)(0x2028 | (dreg() << 9) | an)); w((UWORD)(int16_t)-4); /* MOVE.L -4(An),Dm */
            w((UWORD)(0x4E58 | an));                                        /* UNLK An */
            return;
        }
        if (rng.chance(30))
        {
            /* PEA, then the address into A4 or A5, or dropped */
            switch (rng.below(3))
            {
            case 0: w((UWORD)(0x4868 | ptr())); w((UWORD)(int16_t)((int)rng.below(128) - 64)); break;
            case 1: w(0x4879); l(rng.next()); break;
            default: w(0x487A); w((UWORD)(int16_t)(-(int)rng.below(32) * 2)); break;
            }
            if (rng.chance(50))
                w(rng.chance(50) ? 0x285F : 0x2A5F);         /* MOVEA.L (A7)+,A4/A5 */
            else
                w(0x588F);                                   /* ADDQ.L #4,A7 */
            return;
        }
        int r = (int)rng.below(8), s = rng.chance(30) ? 1 : rng.chance(50) ? 2 : 4;
        int code = s == 1 ? 1 : s == 2 ? 3 : 2;
        w((UWORD)(code << 12 | 7 << 9 | 4 << 6 | r));      /* MOVE.s Dr,-(A7) */
        unsigned n = rng.below(3);
        for (unsigned i = 0; i < n; i++) simple();
        w((UWORD)(code << 12 | dreg() << 9 | 0 << 6 | 3 << 3 | 7)); /* MOVE.s (A7)+,Dn */
    }

    /* A call of subroutine 'sub', patched when its place is known:
       BSR.W, BSR.L, JSR (d16,PC), JSR abs.l, JSR (A4), JSR 8(A5). */
    struct Call { size_t at; unsigned kind, sub; };
    std::vector<Call> calls;

    void call(unsigned subs)
    {
        unsigned kind = rng.below(6), sub = rng.below(subs);
        static const UWORD op[6] = { 0x6100, 0x61FF, 0x4EBA, 0x4EB9, 0x49FA, 0x4BFA };
        w(op[kind]);
        calls.push_back({ out.size(), kind, sub });
        w(0);
        if (kind == 1 || kind == 3) w(0);
        if (kind == 4) w(0x4E94);
        if (kind == 5) { w(0x4EAD); w(8); }
    }

    void patch_calls(const std::vector<size_t> &sub_at, ULONG base)
    {
        for (auto &c : calls)
        {
            int32_t rel = (int32_t)(2 * ((int64_t)sub_at[c.sub] - (int64_t)c.at));
            ULONG abs = base + 2 * (ULONG)sub_at[c.sub];
            if (c.kind == 5) rel -= 8;
            if (c.kind == 1) { out[c.at] = (UWORD)((ULONG)rel >> 16); out[c.at + 1] = (UWORD)rel; }
            else if (c.kind == 3) { out[c.at] = (UWORD)(abs >> 16); out[c.at + 1] = (UWORD)abs; }
            else out[c.at] = (UWORD)rel;
        }
    }

    /* main and its subroutines */
    std::vector<UWORD> program(ULONG base)
    {
        calls.clear();
        out.clear();
        for (int i = 0; i < 8; i++) { w((UWORD)(0x203C | i << 9)); l(rng.next()); }
        for (int p = 0; p < 4; p++) anchor(p);
        for (int i = 4; i < 6; i++) { w((UWORD)(0x207C | i << 9)); l(rng.next()); }
        w(0x2C7C); l(Layout::probe);
        w(0x44FC); w((UWORD)rng.below(32));
        unsigned subs = 1 + rng.below(3), items = 20 + rng.below(30);
        for (unsigned i = 0; i < items; i++)
        {
            unsigned k = rng.below(100);
            if (k < 66) simple();
            else if (k < 78) skip();
            else if (k < 86) loop();
            else if (k < 92) call(subs);
            else if (k < 97) push_pop();
            else anchor(ptr());
            if (i % 12 == 11) anchor(ptr());
        }
        w(0x4EAE); w((UWORD)-36);
        w(0x4E75);
        std::vector<size_t> sub_at;
        for (unsigned s = 0; s < subs; s++)
        {
            sub_at.push_back(out.size());
            unsigned ret_kind = rng.below(3);
            int rtd_bytes = 0;
            if (ret_kind == 1)
            {
                if (rng.chance(50))
                    w(0x40E7);                               /* MOVE SR,-(A7) */
                else
                {
                    w(0x3F3C); w((UWORD)rng.below(256));     /* MOVE.W #imm,-(A7) */
                }
            }
            else if (ret_kind == 2)
            {
                unsigned k = 1 + rng.below(3);
                for (unsigned j = 0; j < k; j++)
                    w(0x2F17);                               /* MOVE.L (A7),-(A7) */
                rtd_bytes = (int)(4 * k);
            }
            unsigned n = 2 + rng.below(7);
            for (unsigned i = 0; i < n; i++) rng.chance(15) ? skip() : simple();
            if (ret_kind == 1)
                w(0x4E77);
            else if (ret_kind == 2)
            {
                w(0x4E74); w((UWORD)(int16_t)rtd_bytes);
            }
            else
                w(0x4E75);
        }
        patch_calls(sub_at, base);
        return out;
    }
};

struct Outcome
{
    LONG rc;
    Seen regs;
    unsigned snaps;
    std::vector<UBYTE> scratch, stack;
    UWORD beam_v;
    UBYTE beam_h;
    std::string fault;
};

static Outcome run_program(const std::vector<UWORD> &code, const std::vector<UBYTE> &data,
                           Backend which, ULONG arena, HJStats *stats)
{
    Machine m(arena, 0);
    m.words(Layout::entry, code);
    std::copy(data.begin(), data.end(), m.mem.begin() + Layout::scratch);
    snap_calls = 0;
    std::memset(&snap, 0, sizeof(snap));
    Outcome o;
    o.rc = m.run(which);
    o.regs = snap;
    o.snaps = snap_calls;
    o.scratch.assign(m.mem.begin() + Layout::scratch, m.mem.begin() + Layout::scratch_end);
    o.stack.assign(m.mem.begin() + Layout::stack - 0x1000, m.mem.begin() + Layout::stack);
    /* Moira's frame of the snapshot call; see same_memory() */
    for (ULONG at = snap.a[7] - 8; at < snap.a[7]; at++)
        if (at >= Layout::stack - 0x1000 && at < Layout::stack)
            o.stack[at - (Layout::stack - 0x1000)] = 0;
    o.beam_v = m.ctx.beam_v;
    o.beam_h = m.ctx.beam_h;
    if (which == JIT && m.jit)
    {
        o.fault = HJ_LastFault(m.jit);
        if (stats)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            stats->blocks += s->blocks;
            stats->guest_insns += s->guest_insns;
            stats->host_words += s->host_words;
            stats->slow_accesses += s->slow_accesses;
            for (int i = 0; i < HJ_EXIT_COUNT; i++) stats->exits[i] += s->exits[i];
        }
    }
    return o;
}

static void check_same_program(const std::vector<UWORD> &code, const std::vector<UBYTE> &data)
{
    const ULONG arena = 1u << 20;
    Outcome ref = run_program(code, data, MOIRA, arena, nullptr);
    Outcome got = run_program(code, data, JIT, arena, nullptr);
    bool same = ref.rc == got.rc && ref.snaps == 1 && got.snaps == 1 &&
                ref.regs == got.regs && ref.scratch == got.scratch && ref.stack == got.stack &&
                ref.beam_v == got.beam_v && ref.beam_h == got.beam_h;
    if (!same)
    {
        std::printf("  mismatch (check %u): rc %ld/%ld snaps %u/%u SR %04x/%04x scratch %s stack %s %s\n",
                    checks + 1, (long)ref.rc, (long)got.rc, ref.snaps, got.snaps,
                    ref.regs.sr, got.regs.sr,
                    ref.scratch == got.scratch ? "same" : "differs",
                    ref.stack == got.stack ? "same" : "differs",
                    got.fault.c_str());
        for (int i = 0; i < 8; i++)
            if (ref.regs.d[i] != got.regs.d[i] || ref.regs.a[i] != got.regs.a[i])
                std::printf("  D%d %08lx/%08lx A%d %08lx/%08lx\n", i,
                            (unsigned long)ref.regs.d[i], (unsigned long)got.regs.d[i], i,
                            (unsigned long)ref.regs.a[i], (unsigned long)got.regs.a[i]);
    }
    CHECK(same);
}

static void extended_insns()
{
    std::vector<UBYTE> data(Layout::scratch_end - Layout::scratch);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (UBYTE)((i * 73u + 19u) & 0xFFu);

    /* 1. LINK.W, LINK.L, UNLK (including A7), RTR, EXG, CMPM, EXTB.L, MOVEM.W */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, (UWORD)((Layout::scratch + 0x100) >> 16), (UWORD)(Layout::scratch + 0x100),
        0x227C, (UWORD)((Layout::scratch + 0x200) >> 16), (UWORD)(Layout::scratch + 0x200),
        0x4E55, 0xFFF0,                         /* LINK.W A5,#-16 */
        0x2B7C, 0x1122, 0x3344, 0xFFF8,         /* MOVE.L #$11223344,-8(A5) */
        0x480C, 0xFFFF, 0xFFE0,                 /* LINK.L A4,#-32 */
        0x202D, 0xFFF8,                         /* MOVE.L -8(A5),D0 */
        0x4E5C,                                 /* UNLK A4 */
        0x4E5D,                                 /* UNLK A5 */
        0x4E57, 0xFFF8,                         /* LINK.W A7,#-8 */
        0x222F, 0x0008,                         /* MOVE.L 8(A7),D1 */
        0x508F,                                 /* ADDQ.L #8,A7 */
        0x4E5F,                                 /* UNLK A7 */
        0x588F,                                 /* ADDQ.L #4,A7 */
        0x6104,                                 /* BSR.S sub */
        0x40C2,                                 /* MOVE SR,D2 */
        0x6008,                                 /* BRA.S after_sub */
        0x3F3C, 0x001B,                         /* sub: MOVE.W #$001B,-(A7) */
        0x7663,                                 /* MOVEQ #99,D3 */
        0x4E77,                                 /* RTR */
        0xC343,                                 /* after_sub: EXG D1,D3 */
        0xC149,                                 /* EXG A0,A1 */
        0xC588,                                 /* EXG D2,A0 */
        0xC588,                                 /* EXG D2,A0 */
        0x20BC, 0x8001, 0xFFFE,                 /* MOVE.L #$8001FFFE,(A0) */
        0x217C, 0x7FFF, 0x8000, 0x0004,         /* MOVE.L #$7FFF8000,4(A0) */
        0x4CD8, 0x0C30,                         /* MOVEM.W (A0)+,D4-D5/A2-A3 */
        0x48A1, 0x0C30,                         /* MOVEM.W D4-D5/A2-A3,-(A1) */
        0xB149,                                 /* CMPM.W (A1)+,(A0)+ */
        0xB349,                                 /* CMPM.W (A1)+,(A1)+ */
        0x2C3C, 0x1234, 0x5680,                 /* MOVE.L #$12345680,D6 */
        0x49C6,                                 /* EXTB.L D6 */
        0x2E3C, 0xABCD, 0x007F,                 /* MOVE.L #$ABCD007F,D7 */
        0x49C7,                                 /* EXTB.L D7 */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 2. 68020 TST, CMPI PC-relative, MOVE from CCR, ORI/ANDI/EORI to CCR and SR */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, 0x8000, 0x0000,                 /* MOVEA.L #$80000000,A0 */
        0x4A88,                                 /* TST.L A0 */
        0x72FF,                                 /* MOVEQ #-1,D1 */
        0x42C1,                                 /* MOVE CCR,D1 */
        0x327C, 0xFFFF,                         /* MOVEA.W #-1,A1 */
        0x4A49,                                 /* TST.W A1 */
        0x42C2,                                 /* MOVE CCR,D2 */
        0x4A3C, 0x0080,                         /* TST.B #$80 */
        0x4A7C, 0x0000,                         /* TST.W #$0000 */
        0x4ABC, 0x7FFF, 0xFFFF,                 /* TST.L #$7FFFFFFF */
        0x4ABA, 0xFFE2,                         /* TST.L (-30,PC) */
        0x0CBA, 0x8000, 0x0000, 0xFFD6,         /* CMPI.L #$80000000,(-42,PC) */
        0x003C, 0x001F,                         /* ORI #$1F,CCR */
        0x42C3,                                 /* MOVE CCR,D3 */
        0x023C, 0x0015,                         /* ANDI #$15,CCR */
        0x42C4,                                 /* MOVE CCR,D4 */
        0x0A3C, 0x000F,                         /* EORI #$0F,CCR */
        0x42C5,                                 /* MOVE CCR,D5 */
        0x007C, 0x0503,                         /* ORI #$0503,SR */
        0x027C, 0x231C,                         /* ANDI #$231C,SR */
        0x0A7C, 0x0106,                         /* EORI #$0106,SR */
        0x40C6,                                 /* MOVE SR,D6 */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 3. BTST, BCHG, BCLR, BSET (static and dynamic, Dn and memory) */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, (UWORD)((Layout::scratch + 0x100) >> 16), (UWORD)(Layout::scratch + 0x100),
        0x203C, 0x8000, 0x0005,                 /* MOVE.L #$80000005,D0 */
        0x7223,                                 /* MOVEQ #35,D1 (bit 3) */
        0x0800, 0x001F,                         /* BTST #31,D0 */
        0x42C2,                                 /* MOVE CCR,D2 */
        0x0840, 0x001F,                         /* BCHG #31,D0 */
        0x0880, 0x0000,                         /* BCLR #0,D0 */
        0x08C0, 0x000F,                         /* BSET #15,D0 */
        0x03C0,                                 /* BSET D1,D0 */
        0x0140,                                 /* BCHG D0,D0 (same register) */
        0x033C, 0x00A5,                         /* BTST D1,#$A5 */
        0x42C3,                                 /* MOVE CCR,D3 */
        0x10BC, 0x0081,                         /* MOVE.B #$81,(A0) */
        0x0810, 0x0007,                         /* BTST #7,(A0) */
        0x42C4,                                 /* MOVE CCR,D4 */
        0x0858, 0x0007,                         /* BCHG #7,(A0)+ */
        0x08A0, 0x0000,                         /* BCLR #0,-(A0) */
        0x03D0,                                 /* BSET D1,(A0) */
        0x1A10,                                 /* MOVE.B (A0),D5 */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 4. ADDX, SUBX, NEGX (.B, .W, .L, sticky Z, signed overflow with X, -(Ay),-(Ax)) */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, (UWORD)((Layout::scratch + 0x110) >> 16), (UWORD)(Layout::scratch + 0x110),
        0x227C, (UWORD)((Layout::scratch + 0x120) >> 16), (UWORD)(Layout::scratch + 0x120),
        /* -128 + (-1) + X(1) = -128 in .B: no signed overflow (V = 0) */
        0x7080,                                 /* MOVEQ #-128,D0 */
        0x72FF,                                 /* MOVEQ #-1,D1 */
        0x44FC, 0x0014,                         /* MOVE #$14,CCR (X=1, Z=1) */
        0xD101,                                 /* ADDX.B D1,D0 */
        0x42C2,                                 /* MOVE CCR,D2 */
        /* 127 + 0 + X(1) = -128 in .B: signed overflow (V = 1) */
        0x707F,                                 /* MOVEQ #127,D0 */
        0x7200,                                 /* MOVEQ #0,D1 */
        0x44FC, 0x0014,                         /* MOVE #$14,CCR (X=1, Z=1) */
        0xD101,                                 /* ADDX.B D1,D0 */
        0x42C3,                                 /* MOVE CCR,D3 */
        /* 64-bit SUBX with sticky Z: (0:1) - (0:1) -> Z=1 vs (0:2) - (0:1) -> Z=0 */
        0x7001, 0x7200, 0x7801, 0x7A00,
        0x9084,                                 /* SUB.L D4,D0 (Z=1, X=0) */
        0x9385,                                 /* SUBX.L D5,D1 (Z stays 1) */
        0x42C6,                                 /* MOVE CCR,D6 */
        0x7002, 0x7200, 0x7801, 0x7A00,
        0x9084,                                 /* SUB.L D4,D0 (Z=0, X=0) */
        0x9385,                                 /* SUBX.L D5,D1 (0 result, Z stays 0) */
        0x42C7,                                 /* MOVE CCR,D7 */
        /* NEGX.W and memory ADDX/SUBX -(A0),-(A1) and -(A0),-(A0) */
        0x303C, 0x8000,
        0x44FC, 0x0014,
        0x4040,                                 /* NEGX.W D0 */
        0xD348,                                 /* ADDX.W -(A0),-(A1) */
        0x9188,                                 /* SUBX.L -(A0),-(A0) */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 5. MULU.W, MULS.W, MULU.L, MULS.L (32-bit, 64-bit Dh:Dl, Dh == Dl, overflow V) */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x303C, 0xFFFF, 0xC0FC, 0xFFFF,         /* MULU.W #$FFFF,D0 */
        0x323C, 0x8000, 0xC3FC, 0x7FFF,         /* MULS.W #$7FFF,D1 */
        /* MULU.L #$10000,D2 (overflows 32-bit -> V=1) */
        0x243C, 0x0002, 0x0000,
        0x4C3C, 0x2000, 0x0001, 0x0000,
        0x42C3,                                 /* MOVE CCR,D3 */
        /* MULS.L #-1,D4 with D4 = $80000000 (overflows 32-bit signed -> V=1) */
        0x283C, 0x8000, 0x0000,
        0x4C3C, 0x4800, 0xFFFF, 0xFFFF,
        0x42C4,                                 /* MOVE CCR,D4 */
        /* MULU.L 64-bit $FFFFFFFF * $FFFFFFFF -> D6:D5 */
        0x2A3C, 0xFFFF, 0xFFFF,
        0x4C3C, 0x5406, 0xFFFF, 0xFFFF,
        /* MULS.L 64-bit with Dh == Dl (D7:D7, Dl wins) */
        0x2E3C, 0x8000, 0x0003,
        0x4C3C, 0x7C07, 0xFFFF, 0xFFFE,
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 6. All 8 shifts/rotates across .B/.W/.L, counts 0..63, ASL overflow V, and <ea>.W */
    static const unsigned counts[] = { 0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63 };
    static const ULONG inputs[] = { 0x00000000u, 0x80000001u, 0x40008040u, 0x7FFF7F7Fu, 0xFFFFFFFFu, 0xA55AA55Au };
    for (unsigned cnt : counts)
        for (ULONG init : inputs)
            for (UWORD ccr : { (UWORD)0x00, (UWORD)0x1F })
            {
                std::vector<UWORD> code = {
                    0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
                    0x207C, (UWORD)((Layout::scratch + 0x100) >> 16), (UWORD)(Layout::scratch + 0x100),
                    0x7E00,                                                 /* MOVEQ #0,D7 */
                    0x1E3C, (UWORD)cnt                                      /* MOVE.B #cnt,D7 */
                };
                for (int op = 0; op < 8; op++)
                    for (int s = 0; s < 3; s++)
                    {
                        int type = op >> 1, left = op & 1;
                        code.push_back(0x203C);
                        code.push_back((UWORD)(init >> 16));
                        code.push_back((UWORD)init);
                        code.push_back(0x44FC);
                        code.push_back(ccr);
                        code.push_back((UWORD)(0xE020 | (7 << 9) | (left << 8) | (s << 6) | (type << 3) | 0));
                        code.push_back(0x42C1);                             /* MOVE CCR,D1 */
                        code.push_back(0x20C0);                             /* MOVE.L D0,(A0)+ */
                        code.push_back(0x30C1);                             /* MOVE.W D1,(A0)+ */
                    }
                for (int op = 0; op < 8; op++)
                {
                    int type = op >> 1, left = op & 1;
                    code.push_back(0x30BC);
                    code.push_back((UWORD)init);
                    code.push_back(0x44FC);
                    code.push_back(ccr);
                    code.push_back((UWORD)(0xE0C0 | (type << 9) | (left << 8) | 0x0018)); /* OP.W (A0)+ */
                    code.push_back(0x42D8);                                 /* MOVE CCR,(A0)+ */
                }
                code.push_back(0x4EAE);
                code.push_back((UWORD)-36);
                code.push_back(0x4E75);
                check_same_program(code, data);
            }

    /* 7. DIVU.L, DIVS.L, DIVUL.L, DIVSL.L (32-bit, 64-bit Dr:Dq, Dr == Dq, 64-bit overflow V) */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        /* 32-bit DIVUL.L #10,D1:D0 */
        0x203C, 0x0001, 0x86A7,                 /* MOVE.L #100007,D0 */
        0x4C7C, 0x0001, 0x0000, 0x000A,         /* DIVUL.L #10,D1:D0 */
        /* 32-bit DIVSL.L #7,D3:D2 with negative dividend */
        0x243C, 0xFFFF, 0xFF9C,                 /* MOVE.L #-100,D2 */
        0x4C7C, 0x2803, 0x0000, 0x0007,         /* DIVSL.L #7,D3:D2 */
        /* 32-bit DIVS.L #-1,D4 with D4 = $80000000 (Dr == Dq) */
        0x283C, 0x8000, 0x0000,
        0x4C7C, 0x4804, 0xFFFF, 0xFFFF,
        0x42C5,                                 /* MOVE CCR,D5 */
        /* 64-bit DIVU.L #10,D1:D0 */
        0x7003, 0x7202,
        0x4C7C, 0x0401, 0x0000, 0x000A,
        /* 64-bit DIVU.L overflow: D1:D0 = $00000010:00000005 / #2 -> V=1, D1:D0 kept */
        0x7210, 0x7005,
        0x44FC, 0x0014,
        0x4C7C, 0x0401, 0x0000, 0x0002,
        0x42C6,                                 /* MOVE CCR,D6 */
        /* 64-bit DIVS.L overflow: D3:D2 = $FFFFFFFF:80000000 / #-1 -> V=1, D3:D2 kept */
        0x76FF, 0x243C, 0x8000, 0x0000,
        0x44FC, 0x0000,
        0x4C7C, 0x2C03, 0xFFFF, 0xFFFF,
        0x42C7,                                 /* MOVE CCR,D7 */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 8. 68020 bitfield instructions (BFTST, BFEXTU, BFCHG, BFEXTS, BFCLR, BFFFO, BFSET, BFINS) */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, (UWORD)((Layout::scratch + 0x100) >> 16), (UWORD)(Layout::scratch + 0x100),
        0x203C, 0xF0A5, 0x5A0F,                 /* MOVE.L #$F0A55A0F,D0 */
        0xE8C0, 0x0108,                         /* BFTST D0{4:8} */
        0xE9C0, 0x1108,                         /* BFEXTU D0{4:8},D1 */
        0xEBC0, 0x2208,                         /* BFEXTS D0{8:8},D2 */
        0xEDC0, 0x3108,                         /* BFFFO D0{4:8},D3 */
        0xEDC0, 0x4604,                         /* BFFFO D0{24:4},D4 (all zero -> 28) */
        0xEAC0, 0x0708,                         /* BFCHG D0{28:8} (wraps around bit 0) */
        0xECC0, 0x0004,                         /* BFCLR D0{0:4} */
        0xEEC0, 0x0604,                         /* BFSET D0{24:4} */
        0x7A35,                                 /* MOVEQ #$35,D5 */
        0xEFC0, 0x5306,                         /* BFINS D5,D0{12:6} */
        0x7C1B, 0x7E0A,                         /* D6 = 27, D7 = 10 */
        0xEDC0, 0x39A7,                         /* BFFFO D0{D6:D7},D3 */
        /* Memory bitfields across 5 bytes and with negative dynamic bit offset */
        0x20BC, 0x1234, 0x5678,                 /* MOVE.L #$12345678,(A0) */
        0x217C, 0x9ABC, 0xDEF0, 0x0004,         /* MOVE.L #$9ABCDEF0,4(A0) */
        0xE9D0, 0x11E0,                         /* BFEXTU (A0){7:32},D1 (5-byte span) */
        0xEBD0, 0x2154,                         /* BFEXTS (A0){5:20},D2 */
        0xEDD0, 0x3154,                         /* BFFFO (A0){5:20},D3 */
        0xEFD0, 0x5767,                         /* BFINS D5,(A0){29:7} (into 5th byte) */
        0x7CF3, 0x7E10,                         /* D6 = -13, D7 = 16 */
        0xEAE8, 0x09A7, 0x0004,                 /* BFCHG 4(A0){D6:D7} */
        0x42C4,                                 /* MOVE CCR,D4 */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);

    /* 9. RTD #d16, TAS <ea>, MOVEP.W/.L, and CAS.B/.W/.L */
    check_same_program({
        0x2C7C, (UWORD)(Layout::probe >> 16), (UWORD)Layout::probe,
        0x207C, (UWORD)((Layout::scratch + 0x100) >> 16), (UWORD)(Layout::scratch + 0x100),
        0x2F3C, 0x1111, 0x2222,                 /* MOVE.L #$11112222,-(A7) */
        0x2F3C, 0x3333, 0x4444,                 /* MOVE.L #$33334444,-(A7) */
        0x6102,                                 /* BSR.S rtd_sub */
        0x6008,                                 /* BRA.S after_rtd */
        0x202F, 0x0004,                         /* rtd_sub: MOVE.L 4(A7),D0 */
        0x4E74, 0x0008,                         /* RTD #8 */
        /* after_rtd: TAS on Dn and memory */
        0x7200,                                 /* MOVEQ #0,D1 */
        0x4AC1,                                 /* TAS D1 (Z=1, N=0 -> D1=$80) */
        0x42C2,                                 /* MOVE CCR,D2 */
        0x4AC1,                                 /* TAS D1 (Z=0, N=1 -> D1=$80) */
        0x42C3,                                 /* MOVE CCR,D3 */
        0x10BC, 0x007F,                         /* MOVE.B #$7F,(A0) */
        0x4AD8,                                 /* TAS (A0)+ */
        /* MOVEP.L and MOVEP.W */
        0x223C, 0xCAFE, 0xBABE,                 /* MOVE.L #$CAFEBABE,D1 */
        0x03C8, 0x0000,                         /* MOVEP.L D1,0(A0) */
        0x243C, 0xDEAD, 0xBEEF,                 /* MOVE.L #$DEADBEEF,D2 */
        0x0588, 0x0001,                         /* MOVEP.W D2,1(A0) */
        0x0948, 0x0000,                         /* MOVEP.L 0(A0),D4 */
        0x2A3C, 0x7777, 0x0000,                 /* MOVE.L #$77770000,D5 */
        0x0B08, 0x0001,                         /* MOVEP.W 1(A0),D5 (keeps upper word) */
        /* CAS.B, CAS.W, CAS.L (both equal and not-equal) */
        0x20BC, 0x1234, 0x5678,                 /* MOVE.L #$12345678,(A0) */
        0x7C12, 0x7E9A,                         /* D6 = $12, D7 = $9A */
        0x0AD0, 0x01C6,                         /* CAS.B D6,D7,(A0) (equal -> (A0)=$9A) */
        0x0AD0, 0x01C6,                         /* CAS.B D6,D7,(A0) (not equal -> D6=$9A) */
        0x283C, 0x9A34, 0x5678,                 /* D4 = $9A345678 */
        0x2A3C, 0xABCD, 0xEF01,                 /* D5 = $ABCDEF01 */
        0x0ED8, 0x0144,                         /* CAS.L D4,D5,(A0)+ (equal -> writes D5) */
        0x4EAE, (UWORD)-36,
        0x4E75
    }, data);
}

static unsigned fuzz_programs = 3000;

static void random_programs()
{
    const ULONG arena = 1u << 20;
    HJStats total = {};
    unsigned mismatches = 0;
    for (unsigned n = 0; n < fuzz_programs; n++)
    {
        Rng rng { 0x9E3779B97F4A7C15ull ^ (uint64_t)(n + 1) * 0xD1B54A32D192ED03ull };
        Gen gen(rng, arena);
        std::vector<UWORD> code = gen.program(Layout::entry);
        std::vector<UBYTE> data(Layout::scratch_end - Layout::scratch);
        for (auto &b : data) b = (UBYTE)rng.next();
        Outcome ref = run_program(code, data, MOIRA, arena, nullptr);
        Outcome got = run_program(code, data, JIT, arena, &total);
        bool same = ref.rc == got.rc && ref.snaps == 1 && got.snaps == 1 &&
                    ref.regs == got.regs &&
                    ref.scratch == got.scratch && ref.stack == got.stack &&
                    ref.beam_v == got.beam_v && ref.beam_h == got.beam_h;
        checks++;
        if (same)
            continue;
        failures++;
        if (++mismatches > 3)
            continue;
        std::printf("FAIL random program %u: rc %ld/%ld snaps %u/%u %s\n", n, (long)ref.rc,
                    (long)got.rc, ref.snaps, got.snaps, got.fault.c_str());
        for (int i = 0; i < 8; i++)
            if (ref.regs.d[i] != got.regs.d[i] || ref.regs.a[i] != got.regs.a[i])
                std::printf("  D%d %08lx/%08lx A%d %08lx/%08lx\n", i, (unsigned long)ref.regs.d[i],
                            (unsigned long)got.regs.d[i], i, (unsigned long)ref.regs.a[i],
                            (unsigned long)got.regs.a[i]);
        std::printf("  SR %04x/%04x scratch %s stack %s\n  code:", ref.regs.sr, got.regs.sr,
                    ref.scratch == got.scratch ? "same" : "differs", ref.stack == got.stack ? "same" : "differs");
        for (size_t i = 0; i < code.size(); i++)
            std::printf("%s%04x", i % 16 ? " " : "\n   ", code[i]);
        std::printf("\n");
    }
    std::printf("  %u programs, %lu blocks, %lu 68k instructions, %.1f words each, %lu slow accesses\n",
                fuzz_programs, total.blocks, total.guest_insns,
                total.guest_insns ? (double)total.host_words / (double)total.guest_insns : 0.0,
                total.slow_accesses);
}

/* ------------------------------------------------------------------
   Timing (--bench)
   ------------------------------------------------------------------ */

static double seconds(const std::function<void()> &f)
{
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void bench()
{
    const unsigned repeat = 400;
    for (Backend which : { MOIRA, JIT })
    {
        /* The checksum loop over 4 KB, 'repeat' times: 5 instructions a
           word, plus 4 a pass. */
        Machine m(KAFFEINE_MEM_SIZE);
        checksum_program(m);
        ULONG at = m.pc - 14;
        m.pc = at;
        m.d(3, repeat - 1);
        ULONG pass = m.pc;
        m.word(0x41F9); m.longword(Layout::data);
        m.d(1, 1023);
        m.code(m.pc, {0x2418, 0xEB98, 0xB580, 0xD081, 0x51C9, 0xFFF6,
                      0x51CB, (UWORD)(pass - (m.pc + 14)), 0x4E75});
        double insns = (double)repeat * (1024.0 * 5 + 3) + 2;
        LONG rc = 0;
        double t = seconds([&] { rc = m.run(which); });
        std::printf("  %-5s compute: %8.2f ms, %7.2f ns per 68k instruction, %8.1f MIPS (rc %08lx)\n",
                    which == JIT ? "JIT" : "Moira", t * 1e3, t * 1e9 / insns, insns / t / 1e6,
                    (unsigned long)rc);
    }
    for (Backend which : { MOIRA, JIT })
    {
        const unsigned calls = 200000;
        Machine m;
        m.a(6, Layout::probe);
        m.d(1, calls - 1);
        m.code(m.pc, {0x7000, 0x4EAE, (UWORD)-42, 0x5381, 0x6AF8, 0x4E75});
        count_calls = 0;
        double t = seconds([&] { m.run(which); });
        std::printf("  %-5s library call: %7.1f ns per JSR to a thunk and back\n",
                    which == JIT ? "JIT" : "Moira", t * 1e9 / calls);
    }
    for (Backend which : { MOIRA, JIT })
    {
        const unsigned calls = 100000;
        Machine m;
        m.code(Layout::hook_code, {0x2009, 0x4E75});   /* MOVE.L A1,D0; RTS */
        m.hook_at(Layout::hook, Layout::hook_code);
        m.d(3, calls - 1);
        ULONG top = m.pc;
        m.hook_call();
        m.word(0x5383); m.word(0x6A00); m.word((UWORD)(top - m.pc));  /* SUBQ.L #1,D3; BPL.W top */
        m.rts();
        double t = seconds([&] { m.run(which); });
        std::printf("  %-5s callback: %7.1f ns per CallHookPkt() of a 68k hook\n",
                    which == JIT ? "JIT" : "Moira", t * 1e9 / calls);
    }
    for (Backend which : { MOIRA, JIT })
    {
        const unsigned chains = 5000;
        Machine m;
        m.code(Layout::hook_code, {0x5382, 0x6708, 0x4EAE, (UWORD)-102, 0x5280, 0x4E75, 0x7001, 0x4E75});
        m.hook_at(Layout::hook, Layout::hook_code);
        m.d(4, chains - 1);
        ULONG top = m.pc;
        m.d(2, KAFFEINE_CALLBACK_DEPTH);
        m.hook_call();
        m.word(0x5384); m.word(0x6A00); m.word((UWORD)(top - m.pc));
        m.rts();
        double t = seconds([&] { m.run(which); });
        std::printf("  %-5s 16 nested callbacks: %7.2f us per chain\n",
                    which == JIT ? "JIT" : "Moira", t * 1e6 / chains);
        if (which == JIT)
        {
            const HJStats *s = HJ_GetStats(m.jit);
            std::printf("  JIT   translation: %lu blocks, %.0f ns each, %.1f words per 68k instruction\n",
                        s->blocks, (double)s->translate_ns / (double)s->blocks,
                        (double)s->host_words / (double)s->guest_insns);
        }
    }
}

int main(int argc, char **argv)
{
    bool run_bench = false;
    std::string only;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--bench") run_bench = true;
        else if (arg.rfind("--programs=", 0) == 0) fuzz_programs = (unsigned)std::strtoul(arg.c_str() + 11, nullptr, 10);
        else only = arg;
    }
    const struct { const char *name; void (*run)(); bool hosted; } tests[] = {
        {"encoder", encoder, false}, {"translate-only", translate_only, false},
        {"exec-memory", exec_memory, true}, {"endian", endian, true},
        {"custom-page", custom_page, true}, {"program", program, true},
        {"dbcc-targets", dbcc_targets, true},
        {"block-limits", block_limits, true}, {"code-reset", code_reset, true},
        {"library-calls", library_calls, true}, {"callback-depth", callback_depth, true},
        {"callback-registers", callback_registers, true}, {"callback-format", callback_format, true},
        {"callback-faults", callback_faults, true}, {"callback-stop-codes", callback_stop_codes, true},
        {"bitfield-boundaries", bitfield_boundaries, true},
        {"code-changes", code_changes, true},
        {"faults", faults, true}, {"extended-insns", extended_insns, true},
        {"full-index", full_index, true},
        {"library-backend", library_backend, true},
        {"word-division", word_division, true},
        {"random-programs", random_programs, true}
    };
    unsigned skipped = 0;
    for (auto test : tests)
        if (only.empty() || only == test.name)
        {
            if (test.hosted && !HJ_Available())
            {
                skipped++;
                continue;
            }
            unsigned before = failures;
            std::printf("RUN: %s\n", test.name);
            test.run();
            std::printf("%s: %s\n", test.name, before == failures ? "PASS" : "FAIL");
        }
    if (skipped)
        std::printf("SKIP: %u groups need translated code, which runs only on AArch64 Darwin or Linux\n", skipped);
    if (run_bench && HJ_Available())
        bench();
    std::printf("Hosted AArch64 translation probe (%s): %u checks, %u failed\n",
                HJ_ExecMemoryKind(), checks, failures);
    return failures ? 1 : 0;
}

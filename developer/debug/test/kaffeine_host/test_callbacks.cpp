/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Production C thunks and dispatcher, with literal ABI vectors and real
    instructions through the production Moira adapter. No native guest code.
*/
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "kaffeine_intern.h"
#include "kaffeine_thunks_utility.h"
#include "kaffeine_exec_compat.h"
#include <utility/date.h>

extern "C" APTR AllocMem(ULONG size, ULONG) { return std::calloc(1, size); }
extern "C" void FreeMem(APTR ptr, ULONG) { std::free(ptr); }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }
extern "C" void Amiga2Date(ULONG, ClockData *) { std::abort(); }
extern "C" ULONG Date2Amiga(ClockData *) { std::abort(); }
extern "C" ULONG CheckDate(ClockData *) { std::abort(); }

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

static unsigned failures, checks, generated_calls;
#define CHECK(c) do { checks++; if (!(c)) { \
    std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); failures++; return; \
} } while (0)

static IPTR poison(KaffeineContext *, void *) { generated_calls++; return 0xBADCA11; }
static IPTR stop_seven(KaffeineContext *ctx, void *)
{
    ctx->running = FALSE;
    ctx->exit_code = 7;
    return 7;
}
const M68KThunkEntry kaffeine_thunks_exec[] = {
    { 522, thunk_exec_RawDoFmt }, { 564, Kaffeine_ObtainSemaphore }, { 60, stop_seven }
};
const ULONG kaffeine_thunks_exec_count = 3;
const M68KThunkEntry kaffeine_thunks_dos[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_dos_count = 0;
static const M68KThunkEntry utility_generated[] = { { 102, poison }, { 312, poison } };
static const M68KThunkEntry exec_generated[] = { { 522, poison } };

struct Guest
{
    static constexpr ULONG entry = 0x800, hook_code = 0x1000, hook = 0x2000;
    static constexpr ULONG args = 0x2500, object = 0x2600, fmt = 0x3000;
    static constexpr ULONG utility = 0x4000, exec = 0x6000, buffer = 0x7000;
    static constexpr ULONG snapshot = 0x7800, stack = 0xF000, arena = 0x10000;
    std::vector<UBYTE> mem = std::vector<UBYTE>(arena, 0xA5);
    KaffeineContext ctx = {};
    ULONG pc = entry;

    Guest()
    {
        ctx.mem = mem.data(); ctx.mem_size = arena;
        ctx.entry_point = entry; ctx.stack_top = stack; ctx.m68k_sysbase = exec;
        ctx.num_libs = 2;
        library(0, utility, "utility.library", kaffeine_thunks_utility_manual,
                kaffeine_thunks_utility_manual_count, utility_generated, 2);
        library(1, exec, "exec.library", kaffeine_thunks_exec,
                kaffeine_thunks_exec_count, exec_generated, 1);
        m68k_write32(&ctx, hook + 8, hook_code);
        code(hook_code, {0x2008, 0xD089, 0xD08A, 0x4E75}); /* D0=A0+A1+A2 */
        generated_calls = 0;
    }
    void library(unsigned index, ULONG base, const char *name,
                 const M68KThunkEntry *manual, ULONG count,
                 const M68KThunkEntry *gen, ULONG gen_count)
    {
        auto &lib = ctx.libs[index];
        lib.m68k_addr = base; lib.jt_start = base - 768;
        std::strcpy(lib.name, name);
        lib.thunks = manual; lib.num_thunks = count;
        lib.gen_thunks = gen; lib.num_gen_thunks = gen_count;
        for (ULONG addr = lib.jt_start; addr < base; addr += 2)
            m68k_write16(&ctx, addr, 0xA000);
    }
    void code(ULONG at, std::initializer_list<UWORD> words)
    { for (UWORD word : words) { m68k_write16(&ctx, at, word); at += 2; } }
    void word(UWORD value) { m68k_write16(&ctx, pc, value); pc += 2; }
    void longword(ULONG value) { m68k_write32(&ctx, pc, value); pc += 4; }
    void a(unsigned reg, ULONG value) { word((UWORD)(0x207C + (reg << 9))); longword(value); }
    void d(unsigned reg, ULONG value) { word((UWORD)(0x203C + (reg << 9))); longword(value); }
    void jsr(ULONG base, UWORD lvo) { a(6, base); word(0x4EAE); word((UWORD)-lvo); }
    void hook_call() { a(0, hook); a(1, args); a(2, object); jsr(utility, 102); }
    void raw(ULONG proc, ULONG data = buffer, ULONG format = fmt, ULONG stream = args)
    { a(0, format); a(1, stream); a(2, proc); a(3, data); jsr(exec, 522); }
    void vsn(ULONG size, ULONG data = buffer, ULONG format = fmt, ULONG stream = args)
    { a(0, data); d(0, size); a(1, format); a(2, stream); jsr(utility, 312); }
    void text(ULONG at, const std::string &str)
    { std::memcpy(mem.data() + at, str.c_str(), str.size() + 1); }
    std::string output() const { return std::string((const char *)mem.data() + buffer); }
    LONG run() { word(0x4E75); return Kaffeine_Execute(&ctx); }
    ULONG read(ULONG at) { return m68k_read32(&ctx, at); }
    void save()
    {
        word(0x48F9); word(0xFFFF); longword(snapshot); /* MOVEM.L D0-A7,abs.l */
        word(0x40F9); longword(snapshot + 64);         /* MOVE.W SR,abs.l */
    }
};

static void hook_registers()
{
    Guest g;
    /* Callee deliberately clobbers every input register except its SP. */
    ULONG saved_pc = g.pc;
    g.pc = Guest::hook_code;
    g.word(0x2008); g.word(0xD089); g.word(0xD08A);
    for (unsigned i = 1; i < 8; i++) g.d(i, 0xBAD00000 + i);
    for (unsigned i = 0; i < 7; i++) g.a(i, 0xBAD10000 + i);
    g.word(0x44FC); g.word(0x001F); g.word(0x4E75);
    g.pc = saved_pc;
    for (unsigned i = 0; i < 8; i++) g.d(i, 0xD0000000 + i);
    for (unsigned i = 0; i < 7; i++) g.a(i, 0xA0000000 + i);
    g.a(0, Guest::hook); g.a(1, Guest::args); g.a(2, Guest::object);
    g.a(6, Guest::utility);
    g.word(0x46FC); g.word(0x2715);
    g.word(0x4EAE); g.word((UWORD)-102);
    g.save();
    ULONG want = Guest::hook + Guest::args + Guest::object;
    CHECK((ULONG)g.run() == want);
    CHECK(g.read(Guest::snapshot) == want);
    for (unsigned i = 1; i < 8; i++) CHECK(g.read(Guest::snapshot + 4 * i) == 0xD0000000 + i);
    const ULONG address[] = { Guest::hook, Guest::args, Guest::object,
        0xA0000003, 0xA0000004, 0xA0000005, Guest::utility, Guest::stack - 4 };
    for (unsigned i = 0; i < 8; i++) CHECK(g.read(Guest::snapshot + 32 + 4 * i) == address[i]);
    CHECK(m68k_read16(&g.ctx, Guest::snapshot + 64) == 0x2715);
    CHECK(g.ctx.callback_depth == 0 && !generated_calls);
}

static void nested_hooks()
{
    Guest g;
    m68k_write32(&g.ctx, Guest::hook + 32 + 8, Guest::hook_code + 0x100);
    g.code(Guest::hook_code + 0x100, {0x2008, 0xD089, 0xD08A, 0x4E75});
    g.pc = Guest::hook_code;
    g.a(0, Guest::hook + 32); g.jsr(Guest::utility, 102);
    g.word(0x5280); g.word(0x4E75); /* ADDQ.L #1,D0; RTS */
    g.pc = Guest::entry; g.hook_call();
    CHECK((ULONG)g.run() == Guest::hook + 32 + Guest::args + Guest::object + 1);
    CHECK(g.ctx.callback_depth == 0 && !g.ctx.callback_failed && !generated_calls);
}

static void depth_limit()
{
    for (ULONG depth : { KAFFEINE_CALLBACK_DEPTH, KAFFEINE_CALLBACK_DEPTH + 1 })
    {
        Guest g;
        /* SUBQ.L #1,D2; BEQ leaf; JSR -102(A6); ADDQ.L #1,D0; RTS;
           leaf: MOVEQ #1,D0; RTS. The real hook thunk is reentered. */
        g.code(Guest::hook_code, {0x5382, 0x6708, 0x4EAE, (UWORD)-102,
                                 0x5280, 0x4E75, 0x7001, 0x4E75});
        g.d(2, depth); g.hook_call();
        CHECK(g.run() == (depth == KAFFEINE_CALLBACK_DEPTH ? (LONG)depth : 20));
        CHECK(g.ctx.callback_depth == 0);
        CHECK(g.ctx.callback_failed == (depth > KAFFEINE_CALLBACK_DEPTH));
        CHECK(!generated_calls);
    }
}

static void callback_vectors()
{
    Guest g;
    /* A nested Supervisor redirect can leave LINEA pointing at its special
       trampoline. Simulate a leftover vector with a visible early stop. */
    g.code(Guest::hook_code, {0x23FC, (UWORD)(SENTINEL_ADDR >> 16),
        (UWORD)SENTINEL_ADDR, 0, M68K_VEC_LINEA, 0x7009, 0x4E75});
    g.hook_call();
    g.word(0x13FC); g.word(0x003C); g.longword(Guest::buffer);
    CHECK(g.run() == 9);
    CHECK(g.mem[Guest::buffer] == '<');
    CHECK(g.read(M68K_VEC_LINEA) == RTE_ADDR && !g.ctx.callback_failed);
}

static void invalid_hooks()
{
    for (ULONG entry : { 0u, Guest::hook_code + 1, Guest::arena, 0xFFFFFFFEu, 0xFFFFFFFFu })
    {
        Guest g;
        m68k_write32(&g.ctx, Guest::hook + 8, entry);
        g.hook_call(); CHECK(g.run() == 0);
        CHECK(!g.ctx.callback_failed && !generated_calls);
    }
    for (ULONG hook : { 0u, Guest::hook + 1, Guest::arena - 19, Guest::arena - 1, 0xFFFFFFFCu })
    {
        Guest g;
        g.a(0, hook); g.a(1, 0xFFFFFFFFu); g.a(2, 0xFFFFFFFFu); g.jsr(Guest::utility, 102);
        CHECK(g.run() == 0);
        CHECK(!g.ctx.callback_failed && !generated_calls);
    }
}

static void hook_faults()
{
    const std::vector<std::vector<UWORD>> cases = {
        {0x4AFC},                         /* ILLEGAL */
        {0xA000},                         /* Line-A outside a library */
        {0xFFFF},                         /* Line-F */
        {0xF200, 0xB000},                 /* unsupported FPU */
        {0x2039, 0x0001, 0x0000},         /* MOVE.L outside arena,D0 */
        {0x13FC, 0x0041, 0xFFFF, 0xFFFF}, /* MOVE.B #65,$FFFFFFFF */
        {0x2E7C, 0x0001, 0x0000, 0x4E71}, /* SP outside arena */
        {0x2E7C, 0xFFFF, 0xFFFE, 0x4E71}, /* wrapping SP */
        {0x4EF9, 0x0000, (UWORD)KAFFEINE_CALLBACK_RETURN_BASE}, /* JMP with unpopped SP */
        {0x4E74, 0x0004},                 /* RTD #4 overpops */
        {0x4EF9, 0x0000, (UWORD)(KAFFEINE_CALLBACK_RETURN_BASE + 4)} /* wrong level */
    };
    for (const auto &code : cases)
    {
        Guest g;
        ULONG at = Guest::hook_code;
        for (UWORD word : code) { m68k_write16(&g.ctx, at, word); at += 2; }
        g.hook_call();
        /* Must not resume after the failed CallHookPkt. */
        g.word(0x13FC); g.word(0x003C); g.longword(Guest::buffer);
        CHECK(g.run() == 20);
        CHECK(g.mem[Guest::buffer] == 0xA5 && g.ctx.exit_code == 20);
        CHECK(g.ctx.callback_failed && !g.ctx.callback_depth && !generated_calls);
    }
    /* A second return to a consumed token cannot resume a vanished call. */
    Guest twice;
    twice.hook_call(); twice.word(0x4EF9); twice.longword(KAFFEINE_CALLBACK_RETURN_BASE);
    CHECK(twice.run() == 20);
}

static void callback_stops()
{
    for (UWORD lvo : { 564, 60 })
    {
        Guest g;
        g.pc = Guest::hook_code;
        g.a(0, 0); g.jsr(Guest::exec, lvo); g.word(0x702A); g.word(0x4E75);
        g.pc = Guest::entry; g.hook_call();
        g.word(0x13FC); g.word(0x003C); g.longword(Guest::buffer);
        CHECK(g.run() == (lvo == 564 ? 20 : 7));
        CHECK(g.mem[Guest::buffer] == 0xA5 && g.ctx.callback_failed);
        CHECK(g.ctx.callback_depth == 0 && !generated_calls);
    }
}

static void raw_copy()
{
    Guest g;
    g.text(Guest::fmt, "%d|%u|%x|%X|%c|%ld|%lu|%lx|%lX|%lc|%s|%b|%%");
    ULONG p = Guest::args;
    auto w = [&](UWORD v) { m68k_write16(&g.ctx, p, v); p += 2; };
    auto l = [&](ULONG v) { m68k_write32(&g.ctx, p, v); p += 4; };
    w(0x8000); w(65535); w(0xBEEF); w(0xABCD); w('Q');
    l(0x80000000u); l(0xFFFFFFFFu); l(0xDEADBEEFu); l(0xABCDEF01u); l('R');
    g.text(0x2800, "guest"); l(0x2800);
    g.mem[0x2900] = 3; std::memcpy(g.mem.data() + 0x2901, "B!?", 3); l(0x2900 / 4);
    g.code(Guest::hook_code, {0x16C0, 0x4E75});
    g.raw(Guest::hook_code); g.save();
    CHECK((ULONG)g.run() == p);
    CHECK(g.output() == "-32768|65535|BEEF|ABCD|Q|-2147483648|4294967295|DEADBEEF|ABCDEF01|R|guest|B!?|%");
    CHECK(g.read(Guest::snapshot + 32 + 4 * 3) == Guest::buffer);
    CHECK(!generated_calls && !g.ctx.callback_failed);
}

static void raw_custom()
{
    Guest count;
    count.text(Guest::fmt, "abc");
    count.code(Guest::hook_code, {0x5293, 0x4E75}); /* ADDQ.L #1,(A3); RTS */
    m68k_write32(&count.ctx, Guest::buffer, 10);
    count.raw(Guest::hook_code);
    CHECK((ULONG)count.run() == Guest::args);
    CHECK(count.read(Guest::buffer) == 14); /* includes the terminating NUL */

    Guest copy;
    copy.text(Guest::fmt, "carried A3");
    copy.code(Guest::hook_code, {0x4E71, 0x16C0, 0x4E75}); /* bypass fast signature */
    copy.raw(Guest::hook_code); copy.save();
    CHECK((ULONG)copy.run() == Guest::args);
    CHECK(copy.output() == "carried A3");
    CHECK(copy.read(Guest::snapshot + 32 + 4 * 3) == Guest::buffer);
    CHECK(copy.ctx.callback_depth == 0 && !copy.ctx.callback_failed && !generated_calls);
}

static void raw_flags()
{
    Guest g;
    g.text(Guest::fmt, "[%05d][%-05d][%6.3s][%-6.3b][%.0u][%5.2ld][%p][%P][%D][%U][%id][%q][end%]");
    ULONG p = Guest::args;
    auto w = [&](UWORD v) { m68k_write16(&g.ctx, p, v); p += 2; };
    auto l = [&](ULONG v) { m68k_write32(&g.ctx, p, v); p += 4; };
    w((UWORD)-12); w((UWORD)-12);
    g.text(0x2800, "abcdef"); l(0x2800);
    g.mem[0x2900] = 4; std::memcpy(g.mem.data() + 0x2901, "ABCD", 4); l(0x2900 / 4);
    w(9); l(12345); l(0x123); l(0xABCDEF); w((UWORD)-4); w(7); l((ULONG)-99);
    g.raw(0);
    CHECK((ULONG)g.run() == p);
    CHECK(g.output() == "[-0012][-1200][   abc][ABC   ][][   12][00000123][00ABCDEF][-4][7][-99][q][end]");
    CHECK(!generated_calls);
}

static void raw_special_and_long()
{
    for (ULONG proc : { 0u, 1u, 2u })
    {
        Guest g;
        g.text(Guest::fmt, "special");
        m68k_write32(&g.ctx, Guest::buffer, 100);
        g.raw(proc); CHECK((ULONG)g.run() == Guest::args);
        if (proc == 0) CHECK(g.output() == "special");
        if (proc == 1) CHECK(g.read(Guest::buffer) == 100);
        if (proc == 2) CHECK(g.read(Guest::buffer) == 108);
        CHECK(!generated_calls);
    }
    Guest g;
    std::string long_format(1800, 'Z');
    g.text(Guest::fmt, long_format); g.raw(0);
    CHECK((ULONG)g.run() == Guest::args && g.output() == long_format);
}

static void raw_invalid()
{
    enum { FORMAT, STRING, BSTRING, BPTR_WRAP, ARGUMENT, OUTPUT, PROC, COUNT, WIDTH };
    for (int kind = FORMAT; kind <= WIDTH; kind++)
    {
        Guest g;
        ULONG fmt = Guest::fmt, args = Guest::args, output = Guest::buffer, proc = 0;
        g.text(fmt, "abc");
        if (kind == FORMAT) fmt = Guest::arena - 1;
        if (kind == STRING || kind == BSTRING || kind == BPTR_WRAP)
        {
            g.text(fmt, kind == STRING ? "%s" : "%b");
            m68k_write32(&g.ctx, args, kind == STRING ? Guest::arena - 1 :
                         kind == BSTRING ? (Guest::arena - 4) / 4 : 0x40000000u);
        }
        if (kind == ARGUMENT) { g.text(fmt, "%ld"); args = Guest::arena - 2; }
        if (kind == OUTPUT) output = Guest::arena - 2;
        if (kind == PROC) proc = Guest::hook_code + 1;
        if (kind == COUNT) { proc = 2; output = Guest::arena - 3; }
        if (kind == WIDTH) g.text(fmt, "%9999999999999999999d");
        auto before = g.mem;
        g.raw(proc, output, fmt, args);
        CHECK(g.run() == 20);
        CHECK(g.ctx.callback_failed && !generated_calls);
        CHECK(std::equal(before.begin() + Guest::buffer, before.begin() + Guest::buffer + 32,
                         g.mem.begin() + Guest::buffer));
    }
    /* A custom writer can run out after earlier successful callbacks. */
    Guest custom;
    custom.text(Guest::fmt, "abc");
    custom.code(Guest::hook_code, {0x4E71, 0x16C0, 0x4E75});
    custom.raw(Guest::hook_code, Guest::arena - 2);
    CHECK(custom.run() == 20 && custom.ctx.callback_failed);
    CHECK(custom.mem[Guest::arena - 2] == 'a' && custom.mem[Guest::arena - 1] == 'b');
    /* The custom writer itself can stop with a non-fault process result. */
    Guest stop;
    stop.text(Guest::fmt, "abc");
    stop.pc = Guest::hook_code; stop.jsr(Guest::exec, 60); stop.word(0x4E75);
    stop.pc = Guest::entry; stop.raw(Guest::hook_code);
    CHECK(stop.run() == 7 && stop.ctx.callback_failed);
    CHECK(stop.mem[Guest::buffer] == 0xA5);
}

static void vsnprintf_checks()
{
    for (ULONG size : { 1u, 4u, 6u, 32u })
    {
        Guest g;
        g.text(Guest::fmt, "[%03d]"); m68k_write16(&g.ctx, Guest::args, 7);
        g.vsn(size); CHECK(g.run() == 6);
        std::string want = "[007]"; want.resize(std::min((size_t)(size - 1), want.size()));
        CHECK(g.output() == want);
        CHECK(g.mem[Guest::buffer + size] == 0xA5 && !generated_calls);
    }
    Guest count;
    count.text(Guest::fmt, "hello"); count.vsn(0, 0);
    CHECK(count.run() == 6 && !generated_calls);
    Guest null_strings;
    null_strings.text(Guest::fmt, "%s%b");
    m68k_write32(&null_strings.ctx, Guest::args, 0);
    m68k_write32(&null_strings.ctx, Guest::args + 4, 0);
    null_strings.vsn(1);
    CHECK(null_strings.run() == 1 && null_strings.output().empty());
    Guest embedded;
    embedded.text(Guest::fmt, "%cXYZ"); m68k_write16(&embedded.ctx, Guest::args, 0);
    embedded.vsn(16);
    CHECK(embedded.run() == 5 && embedded.mem[Guest::buffer] == 0 &&
          embedded.mem[Guest::buffer + 1] == 0xA5);
    for (ULONG buffer : { Guest::arena - 2, Guest::arena, 0xFFFFFFFEu, Guest::buffer })
    {
        Guest g;
        g.text(Guest::fmt, "hello");
        auto before = g.mem;
        g.vsn(buffer == Guest::buffer ? 0 : 4, buffer);
        CHECK(g.run() == -1 && !g.ctx.callback_failed && !generated_calls);
        CHECK(std::equal(before.begin() + Guest::buffer, before.begin() + Guest::buffer + 32,
                         g.mem.begin() + Guest::buffer));
    }
    for (bool bad_format : { false, true })
    {
        Guest g;
        g.text(Guest::fmt, "%s"); m68k_write32(&g.ctx, Guest::args, Guest::arena - 1);
        g.vsn(16, Guest::buffer, bad_format ? Guest::arena - 1 : Guest::fmt);
        CHECK(g.run() == -1 && !g.ctx.callback_failed && g.mem[Guest::buffer] == 0xA5);
    }
    /* The destination may overlap a string argument or the template: as in
       AROS, the whole result is formatted before it is copied. */
    Guest overlap_string;
    overlap_string.text(Guest::buffer, "ABCDEF");
    overlap_string.text(Guest::fmt, "%s");
    m68k_write32(&overlap_string.ctx, Guest::args, Guest::buffer);
    overlap_string.vsn(16, Guest::buffer + 1);
    CHECK(overlap_string.run() == 7 && overlap_string.output() == "AABCDEF" && !generated_calls);
    Guest overlap_format;
    overlap_format.text(Guest::buffer, "%ldZ");
    m68k_write32(&overlap_format.ctx, Guest::args, 123456);
    overlap_format.vsn(16, Guest::buffer, Guest::buffer);
    CHECK(overlap_format.run() == 8 && overlap_format.output() == "123456Z" && !generated_calls);
}

static void fallback_controls()
{
    Guest g;
    g.ctx.libs[0].num_thunks = 0;
    g.hook_call(); CHECK(g.run() == 0xBADCA11 && generated_calls == 1);
    Guest v;
    v.ctx.libs[0].num_thunks = 0;
    v.vsn(16); CHECK(v.run() == 0xBADCA11 && generated_calls == 1);
    Guest r;
    r.ctx.libs[1].num_thunks = 0;
    r.raw(0); CHECK(r.run() == 0xBADCA11 && generated_calls == 1);
}

/* Repeat CPU faults through execute() directly, bypassing all adapter
   catches. This is the path needed on AROS, where unwinding is unavailable. */
static void execute_no_catch(void *state)
{
    auto *cpu = static_cast<AROSMoira *>(state);
    cpu->execute();
    if (cpu->ctx->callback_failed || cpu->faulted)
        cpu->setD(0, (ULONG)cpu->ctx->exit_code);
}

static void no_unwinding()
{
    for (UWORD opcode : { 0x4AFC, 0xA000, 0xFFFF, 0xF200, 0x2010 })
    {
        Guest g;
        g.code(Guest::hook_code, { opcode, 0xB000, 0x4E75 });
        g.a(0, Guest::hook); g.a(1, 0); g.a(2, 0);
        /* For MOVE.L (A0),D0 make the entry load an invalid pointer first. */
        if (opcode == 0x2010)
            g.code(Guest::hook_code, {0x207C, 0xFFFF, 0xFFFC, 0x2010, 0x4E75});
        g.jsr(Guest::utility, 102); g.word(0x4E75);
        AROSMoira cpu(&g.ctx);
        auto direct_ops = moira_cpu_ops; direct_ops.execute = execute_no_catch;
        cpu.cpu_handle.ops = &direct_ops;
        m68k_write32(&g.ctx, 0, Guest::stack);
        m68k_write32(&g.ctx, 4, Guest::entry);
        cpu.reset();
        m68k_write16(&g.ctx, RTE_ADDR, M68K_OP_RTE);
        m68k_write32(&g.ctx, M68K_VEC_LINEA, RTE_ADDR);
        m68k_write32(&g.ctx, Guest::stack - 4, SENTINEL_ADDR);
        cpu.setA(7, Guest::stack - 4);
        CHECK(Kaffeine_RunCPU(&g.ctx, &cpu.cpu_handle) == 20);
        CHECK(g.ctx.callback_failed && !g.ctx.running && !g.ctx.callback_depth);
    }
}

int main(int argc, char **argv)
{
    const struct { const char *name; void (*run)(); } tests[] = {
        {"hook", hook_registers}, {"nested", nested_hooks}, {"depth", depth_limit},
        {"vectors", callback_vectors},
        {"invalid-hooks", invalid_hooks}, {"faults", hook_faults}, {"stops", callback_stops},
        {"raw", raw_copy}, {"custom", raw_custom}, {"flags", raw_flags},
        {"special", raw_special_and_long}, {"invalid-format", raw_invalid},
        {"vsn", vsnprintf_checks}, {"fallback", fallback_controls}, {"no-unwind", no_unwinding}
    };
    for (auto test : tests)
        if (argc == 1 || std::string(argv[1]) == test.name)
        {
            unsigned before = failures;
            test.run();
            std::printf("%s: %s\n", test.name, before == failures ? "PASS" : "FAIL");
        }
    std::printf("Guest callbacks and formatting: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}

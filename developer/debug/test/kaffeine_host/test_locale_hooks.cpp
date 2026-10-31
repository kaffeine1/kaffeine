/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Literal locale/utility LVOs through the dispatcher and real Moira guest
    hooks. Native stubs invoke the native Hook with AROS's argument order.
*/
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>
#include <aros/asmcall.h>
#include <proto/locale.h>
#include <utility/date.h>
#include "kaffeine_intern.h"
#include "kaffeine_thunks_locale.h"
#include "kaffeine_thunks_utility.h"

static unsigned checks, failures, generated_calls;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)

static std::map<APTR, ULONG> allocations;
static bool fail_alloc;
static int allocations_left = -1;
extern "C" APTR AllocMem(ULONG size, ULONG)
{
    if (fail_alloc || allocations_left == 0) return nullptr;
    if (allocations_left > 0) allocations_left--;
    APTR p = std::malloc(size);
    if (p) allocations[p] = size;
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    auto it = allocations.find(p);
    CHECK(it != allocations.end() && it->second == size);
    if (it != allocations.end()) { allocations.erase(it); std::free(p); }
}
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }
extern "C" void Amiga2Date(ULONG, ClockData *) { std::abort(); }
extern "C" ULONG Date2Amiga(ClockData *) { std::abort(); }
extern "C" ULONG CheckDate(ClockData *) { std::abort(); }

/* The other locale entry points are linked but must never be called here. */
extern "C" {
void CloseCatalog(Catalog *) { std::abort(); }
Catalog *OpenCatalogA(Locale *, CONST_STRPTR, const void *) { std::abort(); }
CONST_STRPTR GetCatalogStr(Catalog *, LONG, CONST_STRPTR) { std::abort(); }
CONST_STRPTR GetLocaleStr(Locale *, ULONG) { std::abort(); }
ULONG ConvToLower(Locale *, ULONG) { std::abort(); }
ULONG ConvToUpper(Locale *, ULONG) { std::abort(); }
#define UNUSED_CLASSIFY(name) BOOL name(Locale *, ULONG) { std::abort(); }
UNUSED_CLASSIFY(IsAlNum) UNUSED_CLASSIFY(IsAlpha) UNUSED_CLASSIFY(IsCntrl)
UNUSED_CLASSIFY(IsDigit) UNUSED_CLASSIFY(IsGraph) UNUSED_CLASSIFY(IsLower)
UNUSED_CLASSIFY(IsPrint) UNUSED_CLASSIFY(IsPunct) UNUSED_CLASSIFY(IsSpace)
UNUSED_CLASSIFY(IsUpper) UNUSED_CLASSIFY(IsXDigit)
#undef UNUSED_CLASSIFY
ULONG StrConvert(Locale *, CONST_STRPTR, APTR, ULONG, ULONG) { std::abort(); }
LONG StrnCmp(Locale *, CONST_STRPTR, CONST_STRPTR, LONG, ULONG) { std::abort(); }
}

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

static IPTR poison(KaffeineContext *, void *) { generated_calls++; return 0xBADCA11; }
static IPTR stop_seven(KaffeineContext *ctx, void *cpu)
{ Kaffeine_StopGuest(ctx, cpu, 7); return 7; }
const M68KThunkEntry kaffeine_thunks_exec[] = { {60, stop_seven} };
const ULONG kaffeine_thunks_exec_count = 1;
const M68KThunkEntry kaffeine_thunks_dos[] = { {0, nullptr} };
const ULONG kaffeine_thunks_dos_count = 0;
static const M68KThunkEntry locale_generated[] = {
    {42, poison}, {60, poison}, {66, poison}, {162, poison}
};
static const M68KThunkEntry utility_generated[] = { {102, poison} };

static Locale *native_locale;
static unsigned format_calls, parse_calls, close_calls;
static bool native_closed, wrong_object, wrong_message;
static BOOL parse_result;
static std::string expected_template;
static std::vector<ULONG> native_results;
static const DateStamp input_date = { -7, 123, 200 };
static const DateStamp output_date = { -123456, 1439, 2999 };
static UBYTE grouping[] = {3, 0};
/* NULL selects the default locale for one call; its grouping differs. */
static Locale default_loc;
static UBYTE default_grouping[] = {2, 0};
static const Locale *expected_locale;
static unsigned default_opens, default_closes;
static bool fail_default_locale;

extern "C" Locale *OpenLocale(CONST_STRPTR name)
{
    CHECK(name == nullptr);
    if (fail_default_locale) return nullptr;
    default_opens++;
    return &default_loc;
}

extern "C" void CloseLocale(Locale *locale)
{
    if (locale == &default_loc)
    {
        default_closes++;
        CHECK(default_closes <= default_opens);
        return;
    }
    CHECK(locale == native_locale && !native_closed);
    close_calls++;
    native_closed = true;
}

static IPTR invoke(const Hook *hook, const Locale *locale, IPTR message)
{
    CHECK(!native_closed);
    CHECK(hook != nullptr && hook->h_Entry != nullptr && hook->h_Data != nullptr);
    IPTR result = AROS_UFC3(IPTR, hook->h_Entry,
        AROS_UFCA(const Hook *, hook, A0),
        AROS_UFCA(const Locale *, wrong_object ? nullptr : locale, A2),
        AROS_UFCA(IPTR, wrong_message ? 1 : message, A1));
    native_results.push_back((ULONG)result);
    return result;
}

extern "C" void FormatDate(const Locale *locale, CONST_STRPTR format,
                            const DateStamp *date, const Hook *hook)
{
    format_calls++;
    CHECK(locale == expected_locale);
    CHECK(date->ds_Days == input_date.ds_Days && date->ds_Minute == input_date.ds_Minute &&
          date->ds_Tick == input_date.ds_Tick);
    /* Continue calling even after a hook has stopped the guest. Also reread
       the template, as native AROS does between calls to the hook. */
    for (size_t i = 0; i <= expected_template.size(); i++)
    {
        CHECK(std::strcmp(format, expected_template.c_str()) == 0);
        invoke(hook, locale, (IPTR)(SIPTR)(char)format[i]);
    }
}

extern "C" BOOL ParseDate(const Locale *locale, DateStamp *date,
                           CONST_STRPTR format, const Hook *hook)
{
    parse_calls++;
    CHECK(locale == expected_locale);
    /* Deliberately write even on failure, and return success after a callback
       stop. Neither may cause a guest DateStamp writeback. */
    *date = output_date;
    for (unsigned i = 0; i < 4; i++)
    {
        CHECK(std::strcmp(format, expected_template.c_str()) == 0);
        invoke(hook, locale, 0);
    }
    *date = output_date;
    return parse_result;
}

struct Guest
{
    static constexpr ULONG arena = 0x10000, entry = 0x800, hook_code = 0x1000;
    static constexpr ULONG inner_code = 0x1800, hook = 0x2000, inner_hook = 0x2040;
    static constexpr ULONG state = 0x2100, locale = 0x2800, date = 0x2A00;
    static constexpr ULONG fmt = 0x3000, input = 0x3100, utility = 0x5000;
    static constexpr ULONG localebase = 0x7000, exec = 0x9000, trace = 0xA000;
    static constexpr ULONG snapshot = 0xB000, marker = 0xB100, stack = 0xF000;
    std::vector<UBYTE> mem = std::vector<UBYTE>(arena, 0xA5);
    KaffeineContext ctx = {};
    Locale loc = {};
    ULONG pc = entry;

    Guest()
    {
        CHECK(allocations.empty());
        generated_calls = format_calls = parse_calls = close_calls = 0;
        native_closed = wrong_object = wrong_message = fail_alloc = false;
        allocations_left = -1;
        parse_result = 0x1357;
        native_results.clear();
        native_locale = &loc;
        expected_locale = &loc;
        default_opens = default_closes = 0;
        fail_default_locale = false;
        default_loc.loc_Grouping = default_grouping;
        default_loc.loc_GroupSeparator = const_cast<char *>(",");
        loc.loc_Grouping = grouping;
        loc.loc_GroupSeparator = const_cast<char *>(".");
        expected_template = std::string("A\xE9Z");
        ctx.mem = mem.data(); ctx.mem_size = arena;
        ctx.entry_point = entry; ctx.stack_top = stack; ctx.m68k_sysbase = exec;
        ctx.num_libs = 3;
        library(0, localebase, "locale.library", kaffeine_thunks_locale_manual,
                kaffeine_thunks_locale_manual_count, locale_generated, 4);
        library(1, utility, "utility.library", kaffeine_thunks_utility_manual,
                kaffeine_thunks_utility_manual_count, utility_generated, 1);
        library(2, exec, "exec.library", kaffeine_thunks_exec, 1, nullptr, 0);
        /* OpenLocale's mapping and guest copy are tested in test_locale_thunks.
           Install a controlled open record here to isolate hook execution. */
        ctx.locale_map[0].guest_addr = locale;
        ctx.locale_map[0].native_locale = &loc;
        m68k_write32(&ctx, hook + 8, hook_code);
        m68k_write32(&ctx, hook + 16, state);
        m68k_write32(&ctx, inner_hook + 8, inner_code);
        m68k_write32(&ctx, inner_hook + 16, state);
        m68k_write32(&ctx, state, 0);
        m68k_write32(&ctx, state + 4, trace);
        m68k_write32(&ctx, state + 8, input);
        set_date(date, input_date);
        text(fmt, expected_template);
        text(input, "abc");
        recorder(hook_code, false);
        recorder(inner_code, false);
    }
    ~Guest()
    {
        Kaffeine_LocaleCleanup(&ctx);
        CHECK(allocations.empty());
    }
    void library(unsigned i, ULONG base, const char *name, const M68KThunkEntry *manual,
                 ULONG count, const M68KThunkEntry *gen, ULONG gen_count)
    {
        auto &lib = ctx.libs[i];
        lib.m68k_addr = base; lib.jt_start = base - 768;
        std::strcpy(lib.name, name);
        lib.thunks = manual; lib.num_thunks = count;
        lib.gen_thunks = gen; lib.num_gen_thunks = gen_count;
        for (ULONG at = lib.jt_start; at < base; at += 2) m68k_write16(&ctx, at, 0xA000);
    }
    void word(UWORD value) { m68k_write16(&ctx, pc, value); pc += 2; }
    void longword(ULONG value) { m68k_write32(&ctx, pc, value); pc += 4; }
    void a(unsigned reg, ULONG value) { word((UWORD)(0x207C + (reg << 9))); longword(value); }
    void d(unsigned reg, ULONG value) { word((UWORD)(0x203C + (reg << 9))); longword(value); }
    void words(std::initializer_list<UWORD> values) { for (UWORD v : values) word(v); }
    void jsr(ULONG base, UWORD lvo) { a(6, base); words({0x4EAE, (UWORD)-lvo}); }
    void text(ULONG at, const std::string &s) { std::memcpy(mem.data() + at, s.c_str(), s.size() + 1); }
    void set_date(ULONG at, const DateStamp &ds)
    {
        m68k_write32(&ctx, at, (ULONG)ds.ds_Days);
        m68k_write32(&ctx, at + 4, (ULONG)ds.ds_Minute);
        m68k_write32(&ctx, at + 8, (ULONG)ds.ds_Tick);
    }
    void record()
    {
        words({0x2868, 16,          /* MOVEA.L h_Data(A0),A4 */
               0x266C, 4,           /* MOVEA.L 4(A4),A3 */
               0x26C8, 0x26CA, 0x26C9, /* append A0, A2, A1 */
               0x294B, 4, 0x5294}); /* save cursor; ADDQ.L #1,(A4) */
    }
    void recorder(ULONG at, bool reader)
    {
        ULONG saved = pc; pc = at;
        record();
        if (reader)
            words({0x266C, 8, 0x7000, 0x101B, 0x294B, 8}); /* read *(cursor)++ */
        else d(0, 0x87654321);
        for (unsigned i = 1; i < 8; i++) d(i, 0xBAD00000 + i);
        for (unsigned i = 0; i < 7; i++) a(i, 0xBAD10000 + i);
        words({0x44FC, 0x001F, 0x4E75});
        pc = saved;
    }
    void call(bool parse, ULONG loc_addr = locale, ULONG date_addr = date,
              ULONG format_addr = fmt, ULONG hook_addr = hook)
    {
        a(0, loc_addr); a(1, parse ? date_addr : format_addr);
        a(2, parse ? format_addr : date_addr); a(3, hook_addr);
        jsr(localebase, parse ? 162 : 60);
    }
    void format_string(ULONG stream = input, ULONG format_addr = fmt,
                       ULONG loc_addr = locale, ULONG hook_addr = hook)
    {
        a(0, loc_addr); a(1, format_addr); a(2, stream); a(3, hook_addr);
        jsr(localebase, 66);
    }
    void save()
    { words({0x48F9, 0xFFFF}); longword(snapshot); word(0x40F9); longword(snapshot + 64); }
    void mark() { words({0x13FC, 0x003C}); longword(marker); }
    LONG run() { word(0x4E75); return Kaffeine_Execute(&ctx); }
    LONG run_without_catches()
    {
        word(0x4E75);
        AROSMoira cpu(&ctx);
        auto ops = moira_cpu_ops;
        ops.execute = [](void *s) {
            auto *cpu = static_cast<AROSMoira *>(s);
            cpu->execute();
            /* Keep the adapter's result reconciliation, without its catches. */
            if (cpu->ctx->callback_failed || cpu->faulted)
                cpu->setD(0, (ULONG)cpu->ctx->exit_code);
        };
        cpu.cpu_handle.ops = &ops;
        m68k_write32(&ctx, 0, stack); m68k_write32(&ctx, 4, entry);
        cpu.reset();
        m68k_write16(&ctx, RTE_ADDR, M68K_OP_RTE);
        m68k_write32(&ctx, M68K_VEC_LINEA, RTE_ADDR);
        m68k_write32(&ctx, stack - 4, SENTINEL_ADDR);
        cpu.setA(7, stack - 4);
        return Kaffeine_RunCPU(&ctx, &cpu.cpu_handle);
    }
    ULONG read(ULONG at) { return m68k_read32(&ctx, at); }
    void check_date(const DateStamp &want)
    {
        CHECK(read(date) == (ULONG)want.ds_Days);
        CHECK(read(date + 4) == (ULONG)want.ds_Minute);
        CHECK(read(date + 8) == (ULONG)want.ds_Tick);
    }
    void check_trace(const std::vector<ULONG> &messages, ULONG guest_hook = hook,
                     ULONG object = locale)
    {
        CHECK(read(state) == messages.size());
        for (size_t i = 0; i < messages.size(); i++)
        {
            CHECK(read(trace + (ULONG)i * 12) == guest_hook);
            CHECK(read(trace + (ULONG)i * 12 + 4) == object);
            CHECK(read(trace + (ULONG)i * 12 + 8) == messages[i]);
        }
        CHECK(!generated_calls && !ctx.callback_depth && !ctx.callback_failed);
    }
};

static void registers_and_characters()
{
    for (bool parse : {false, true})
    {
        Guest g;
        if (parse) g.recorder(Guest::hook_code, true);
        for (unsigned i = 0; i < 8; i++) g.d(i, 0xD0000000 + i);
        for (unsigned i = 0; i < 7; i++) g.a(i, 0xA0000000 + i);
        g.a(0, Guest::locale); g.a(1, parse ? Guest::date : Guest::fmt);
        g.a(2, parse ? Guest::fmt : Guest::date); g.a(3, Guest::hook);
        g.a(6, Guest::localebase);
        g.words({0x46FC, 0x2715, 0x4EAE, (UWORD)(parse ? -162 : -60)});
        g.save();
        ULONG want = parse ? (ULONG)parse_result : 0xD0000000;
        CHECK((ULONG)g.run() == want);
        CHECK(g.read(Guest::snapshot) == want);
        for (unsigned i = 1; i < 8; i++) CHECK(g.read(Guest::snapshot + 4 * i) == 0xD0000000 + i);
        const ULONG addresses[] = {Guest::locale, parse ? Guest::date : Guest::fmt,
            parse ? Guest::fmt : Guest::date, Guest::hook, 0xA0000004, 0xA0000005,
            Guest::localebase, Guest::stack - 4};
        for (unsigned i = 0; i < 8; i++) CHECK(g.read(Guest::snapshot + 32 + 4 * i) == addresses[i]);
        CHECK(m68k_read16(&g.ctx, Guest::snapshot + 64) == 0x2715);
        if (parse)
        {
            CHECK(parse_calls == 1 && format_calls == 0);
            CHECK(native_results == std::vector<ULONG>({'a', 'b', 'c', 0}));
            g.check_trace({0, 0, 0, 0});
            g.check_date(output_date);
        }
        else
        {
            CHECK(format_calls == 1 && parse_calls == 0);
            CHECK(native_results == std::vector<ULONG>(4, 0x87654321));
            g.check_trace({'A', 0xE9, 'Z', 0});
            g.check_date(input_date);
        }
    }
}

static void parse_failure_and_full_result()
{
    for (BOOL result : {0, -1})
    {
        Guest g;
        parse_result = result;
        g.recorder(Guest::hook_code, true);
        g.call(true);
        CHECK(g.run() == result);
        g.check_date(result ? output_date : input_date);
        g.check_trace({0, 0, 0, 0});
    }
    Guest g;
    ULONG saved = g.pc; g.pc = Guest::hook_code;
    g.record(); g.d(0, 0x123401); g.word(0x4E75); g.pc = saved;
    g.call(true);
    CHECK(g.run() == parse_result);
    CHECK(native_results == std::vector<ULONG>(4, 0x123401));
}

static void invalid_inputs()
{
    for (bool parse : {false, true})
    {
        /* Locale, date, template, Hook and entry addresses respectively.
           A NULL locale is valid: null_locale_and_template() covers it. */
        const std::vector<std::vector<ULONG>> bad = {
            {Guest::locale + 2, Guest::arena, 0xFFFFFFFF},
            {0, Guest::date + 1, Guest::arena - 10, Guest::arena - 2,
             Guest::arena, 0xFFFFFFFC, 0xFFFFFFFF},
            {0, Guest::arena - 1, Guest::arena, 0xFFFFFFFF},
            {0, Guest::hook + 1, Guest::arena - 18, Guest::arena, 0xFFFFFFFC},
            {0, Guest::hook_code + 1, Guest::arena, 0xFFFFFFFE,
             0xFFFFFFFF, KAFFEINE_CALLBACK_RETURN_BASE}
        };
        for (unsigned field = 0; field < bad.size(); field++)
        {
            for (ULONG value : bad[field])
            {
                Guest g;
                ULONG loc = Guest::locale, date = Guest::date, fmt = Guest::fmt, hook = Guest::hook;
                if (field == 0) loc = value;
                if (field == 1) date = value;
                if (field == 2) fmt = value;
                if (field == 3) hook = value;
                if (field == 4) m68k_write32(&g.ctx, Guest::hook + 8, value);
                g.d(0, 0x10203040); g.call(parse, loc, date, fmt, hook);
                CHECK((ULONG)g.run() == (parse ? 0 : 0x10203040));
                CHECK(!format_calls && !parse_calls && !generated_calls);
                CHECK(g.read(Guest::state) == 0 && g.mem[Guest::trace] == 0xA5);
                CHECK(!g.ctx.callback_failed);
                g.check_date(input_date);
            }
        }
        for (bool out_of_memory : {false, true})
        {
            Guest g;
            if (out_of_memory) fail_alloc = true;
            else g.text(Guest::fmt, "%Y%");
            g.d(0, 0x10203040); g.call(parse);
            CHECK((ULONG)g.run() == (parse ? 0 : 0x10203040));
            CHECK(!format_calls && !parse_calls && !generated_calls);
            g.check_date(input_date);
        }
        Guest closed;
        closed.a(0, Guest::locale); closed.jsr(Guest::localebase, 42);
        closed.d(0, 0x10203040); closed.call(parse);
        CHECK((ULONG)closed.run() == (parse ? 0 : 0x10203040));
        CHECK(close_calls == 1 && !format_calls && !parse_calls && !generated_calls);
    }
}

static void boundary_inputs()
{
    for (bool parse : {false, true})
    {
        for (bool date_at_end : {false, true})
        {
            Guest g;
            ULONG date = Guest::date, fmt = Guest::fmt;
            if (date_at_end)
            {
                date = Guest::arena - 12;
                g.set_date(date, input_date);
            }
            else
            {
                expected_template = "";
                g.mem.back() = 0; fmt = Guest::arena - 1;
            }
            if (parse) g.recorder(Guest::hook_code, true);
            g.d(0, 17); g.call(parse, Guest::locale, date, fmt);
            CHECK(g.run() == (parse ? parse_result : 17));
            CHECK((parse ? parse_calls : format_calls) == 1 && !generated_calls);
            if (date_at_end && parse)
            {
                CHECK(g.read(date) == (ULONG)output_date.ds_Days);
                CHECK(g.read(date + 4) == (ULONG)output_date.ds_Minute);
                CHECK(g.read(date + 8) == (ULONG)output_date.ds_Tick);
            }
            if (!parse) g.check_trace(date_at_end ? std::vector<ULONG>({'A', 0xE9, 'Z', 0}) : std::vector<ULONG>({0}));
        }
    }
}

static void callback_faults_and_stops()
{
    for (bool parse : {false, true})
    {
        for (unsigned kind = 0; kind < 5; kind++)
        {
            Guest g;
            ULONG saved = g.pc; g.pc = Guest::hook_code;
            g.record();
            if (kind == 0) g.word(0x4AFC); /* ILLEGAL */
            if (kind == 1) g.words({0x2039, 0xFFFF, 0xFFFE}); /* invalid guest read */
            if (kind == 2) g.jsr(Guest::exec, 60); /* deliberate rc 7 */
            if (kind == 3) wrong_object = true;
            if (kind == 4) wrong_message = true;
            g.word(0x4E75); g.pc = saved;
            if (kind == 4 && !parse) continue; /* output accepts a character */
            g.call(parse); g.mark();
            CHECK(g.run() == (kind == 2 ? 7 : 20));
            CHECK(g.ctx.exit_code == (kind == 2 ? 7 : 20));
            CHECK(g.ctx.callback_failed && !g.ctx.callback_depth);
            CHECK(g.read(Guest::state) == (kind < 3 ? 1u : 0u));
            CHECK(native_results == std::vector<ULONG>(4, 0));
            CHECK(g.mem[Guest::marker] == 0xA5 && !generated_calls);
            g.check_date(input_date);
        }
    }
}

static void nested_callhookpkt()
{
    for (bool locale_outer : {false, true})
    {
        Guest g;
        ULONG saved = g.pc; g.pc = Guest::hook_code;
        if (locale_outer)
        {
            g.a(0, Guest::inner_hook); g.jsr(Guest::utility, 102);
        }
        else g.call(false, Guest::locale, Guest::date, Guest::fmt, Guest::inner_hook);
        g.d(0, 17); g.word(0x4E75); g.pc = saved;
        if (locale_outer) { g.d(0, 17); g.call(false); }
        else
        {
            g.a(0, Guest::hook); g.a(1, 23); g.a(2, 31); g.jsr(Guest::utility, 102);
        }
        CHECK(g.run() == 17);
        CHECK(format_calls == 1);
        g.check_trace({'A', 0xE9, 'Z', 0}, Guest::inner_hook);
    }
    Guest g;
    ULONG saved = g.pc; g.pc = Guest::hook_code;
    g.a(0, Guest::inner_hook); g.jsr(Guest::utility, 102); g.word(0x4E75);
    g.pc = Guest::inner_code; g.record(); g.word(0x4AFC); g.pc = saved;
    g.call(false); g.mark();
    CHECK(g.run() == 20);
    CHECK(g.read(Guest::state) == 1 && native_results == std::vector<ULONG>(4, 0));
    CHECK(g.ctx.callback_failed && !g.ctx.callback_depth && !generated_calls);
    CHECK(g.mem[Guest::marker] == 0xA5);
}

static void callback_mutation_and_close()
{
    for (bool parse : {false, true})
    {
        Guest g;
        ULONG saved = g.pc; g.pc = Guest::hook_code;
        g.record();
        /* Destroy the guest template's NUL while native code is reading its
           copy, and close the same locale on every call to the hook. */
        g.words({0x13FC, 'X'}); g.longword(Guest::fmt + (ULONG)expected_template.size());
        g.a(0, Guest::locale); g.jsr(Guest::localebase, 42);
        g.d(0, 0); g.word(0x4E75); g.pc = saved;
        g.call(parse);
        CHECK(g.run() == (parse ? parse_result : 0));
        CHECK(close_calls == 1 && native_closed);
        CHECK(g.ctx.locale_map[0].guest_addr == 0 && !g.ctx.locale_map[0].active_calls);
        g.check_trace(parse ? std::vector<ULONG>(4, 0) : std::vector<ULONG>({'A', 0xE9, 'Z', 0}));
    }
    Guest stopped;
    ULONG saved = stopped.pc; stopped.pc = Guest::hook_code;
    stopped.a(0, Guest::locale); stopped.jsr(Guest::localebase, 42);
    stopped.jsr(Guest::exec, 60); stopped.word(0x4E75); stopped.pc = saved;
    stopped.call(true);
    CHECK(stopped.run() == 7);
    CHECK(!native_closed && close_calls == 0 && stopped.ctx.locale_map[0].close_pending);
    CHECK(!stopped.ctx.locale_map[0].active_calls);
    stopped.check_date(input_date);
    Kaffeine_LocaleCleanup(&stopped.ctx);
    CHECK(native_closed && close_calls == 1);
}

static void nested_native_hooks()
{
    Guest g;
    ULONG saved = g.pc; g.pc = Guest::hook_code;
    g.call(false, Guest::locale, Guest::date, Guest::fmt, Guest::inner_hook);
    g.d(0, 0x9876); g.word(0x4E75); g.pc = saved;
    g.d(0, 17); g.call(false);
    CHECK(g.run() == 17);
    CHECK(format_calls == 5 && native_results.size() == 20);
    std::vector<ULONG> messages;
    for (unsigned i = 0; i < 4; i++) messages.insert(messages.end(), {'A', 0xE9, 'Z', 0});
    g.check_trace(messages, Guest::inner_hook);
    CHECK(!g.ctx.locale_map[0].active_calls && !g.ctx.locale_map[0].close_pending);
}

static void no_unwinding()
{
    for (bool parse : {false, true})
    {
        for (UWORD opcode : {0x4AFC, 0xFFFF, 0x2010})
        {
            Guest g;
            ULONG saved = g.pc; g.pc = Guest::hook_code;
            g.record();
            if (opcode == 0x2010) g.a(0, 0xFFFFFFFC);
            g.word(opcode); g.word(0x4E75); g.pc = saved;
            g.call(parse); g.mark();
            CHECK(g.run_without_catches() == 20);
            CHECK(g.ctx.callback_failed && !g.ctx.running && !g.ctx.callback_depth);
            CHECK(g.read(Guest::state) == 1 && native_results == std::vector<ULONG>(4, 0));
            CHECK(g.mem[Guest::marker] == 0xA5 && !generated_calls);
            g.check_date(input_date);
        }
    }
}

extern "C" APTR locale_native_InternalFormatString(const Locale *, CONST_STRPTR,
    const void *, ULONG *, const Hook *);

static const Locale *oracle_locale;
static IPTR oracle_put(Hook *hook, APTR object, IPTR message)
{
    CHECK(object == oracle_locale);
    auto *bytes = static_cast<std::vector<ULONG> *>(hook->h_Data);
    bytes->push_back((UBYTE)message);
    return 0;
}

/* This executes AROS's actual renderer with aligned native slots. It calls
   its native Hook in the same A0/A2/A1 order used by the production bridge. */
static std::vector<ULONG> oracle(const std::string &format,
                                std::vector<uint64_t> values,
                                const Locale *locale = nullptr)
{
    oracle_locale = locale ? locale : native_locale;
    std::vector<ULONG> indices, bytes;
    for (size_t i = 0; i < values.size(); i++) indices.push_back((ULONG)i * 8);
    Hook hook = {};
    hook.h_Entry = (APTR)oracle_put;
    hook.h_Data = &bytes;
    locale_native_InternalFormatString(oracle_locale, format.c_str(), values.data(),
                                       indices.data(), &hook);
    return bytes;
}

static std::vector<ULONG> messages(const std::string &s)
{
    std::vector<ULONG> result(s.begin(), s.end());
    for (ULONG &ch : result) ch = (UBYTE)ch;
    result.push_back(0);
    return result;
}

static void formatted(Guest &g, const std::vector<ULONG> &want, ULONG next,
                       bool without_catches = false)
{
    CHECK((ULONG)(without_catches ? g.run_without_catches() : g.run()) == next);
    g.check_trace(want);
    CHECK(!format_calls && !parse_calls && !generated_calls);
    CHECK(!g.ctx.locale_map[0].active_calls);
}

static void formatstring_conversions()
{
    struct Length { const char *prefix; unsigned bytes; };
    for (const Length &length : {Length{"", 2}, {"l", 4}, {"i", 4}, {"ll", 8}, {"L", 8}})
    {
        const uint64_t high = UINT64_C(1) << (length.bytes * 8 - 1);
        const uint64_t mask = length.bytes == 8 ? UINT64_MAX : (high * 2 - 1);
        for (char code : std::string("dDuUxXc"))
        {
            for (uint64_t value : {UINT64_C(0), UINT64_C(1), high - 1, high, mask})
            {
                Guest g;
                std::string format = std::string("[%") + length.prefix + code + "]";
                g.text(Guest::fmt, format);
                if (length.bytes == 2) m68k_write16(&g.ctx, Guest::input, (UWORD)value);
                else if (length.bytes == 4) m68k_write32(&g.ctx, Guest::input, (ULONG)value);
                else
                {
                    m68k_write32(&g.ctx, Guest::input, (ULONG)(value >> 32));
                    m68k_write32(&g.ctx, Guest::input + 4, (ULONG)value);
                }
                uint64_t native_value = value;
                /* Native %i has host pointer width; guest %i has 68k width. */
                if (std::strcmp(length.prefix, "i") == 0 &&
                    (code == 'd' || code == 'D') && (value & high))
                    native_value |= ~mask;
                auto want = oracle(format, {native_value});
                g.format_string();
                formatted(g, want, Guest::input + length.bytes);
            }
        }
    }
    for (const char *format : {"%05d", "%-05d", "%8.2d", "%08.3ld", "%.d",
                               "%-.0d", "%5c", "%0.0u", "%q/%%/%-04q"})
    {
        Guest g;
        g.text(Guest::fmt, format);
        m68k_write32(&g.ctx, Guest::input, 0xFFF4FFF4);
        auto want = oracle(format, {UINT64_C(0xFFF4FFF4)});
        g.format_string();
        formatted(g, want, Guest::input + (std::strchr(format, 'q') ? 0 :
                                          std::strchr(format, 'l') ? 4 : 2));
    }
    for (char code : {'p', 'P'})
    {
        for (const Length &length : {Length{"", 4}, {"l", 4}, {"i", 4}, {"ll", 8}, {"L", 8}})
        {
            Guest g;
            g.text(Guest::fmt, std::string("[%") + length.prefix + code + "]");
            m68k_write32(&g.ctx, Guest::input, length.bytes == 4 ? 0x89ABCDEF : 0);
            if (length.bytes == 8) m68k_write32(&g.ctx, Guest::input + 4, 0x89ABCDEF);
            g.format_string();
            formatted(g, messages(code == 'p' ? "[89abcdef]" : "[89ABCDEF]"),
                      Guest::input + length.bytes);
        }
    }
}

static void formatstring_strings_and_positions()
{
    constexpr ULONG string = 0x3400, bstr = 0x3500;
    for (const char *format : {"[%s]", "[%10.3s]", "[%-08.2s]", "[%l.2s]", "[%b]",
                               "[%10.2b]", "[%-08.0b]", "[%ls]", "[%lb]"})
    {
        /* '%l.2s' is an unknown '.' conversion followed by literal '2s'. */
        for (bool null : {false, true})
        {
            Guest g;
            const char text[] = "a\xE9" "cd";
            const UBYTE bcpl[] = {4, 'A', 0, 0xE9, 'Z'};
            bool b = std::strchr(format, 'b') != nullptr;
            bool unknown = std::strcmp(format, "[%l.2s]") == 0;
            g.text(string, text);
            std::memcpy(g.mem.data() + bstr, bcpl, sizeof(bcpl));
            m68k_write32(&g.ctx, Guest::input, null ? 0 : b ? bstr / 4 : string);
            g.text(Guest::fmt, format);
            auto native = null ? 0 : b ? (uintptr_t)bcpl : (uintptr_t)text;
            auto want = oracle(format, {native});
            g.format_string();
            formatted(g, want, Guest::input + (unknown ? 0 : 4));
        }
    }
    for (unsigned length : {0u, 255u})
    {
        Guest g;
        g.mem[bstr] = (UBYTE)length;
        std::memset(g.mem.data() + bstr + 1, 'X', length);
        m68k_write32(&g.ctx, Guest::input, bstr / 4);
        g.text(Guest::fmt, "%b");
        g.format_string();
        formatted(g, messages(std::string(length, 'X')), Guest::input + 4);
    }
    {
        Guest g;
        const char format[] = "%3$s/%2$ld/%1$d/%2$lx/%1$u";
        const char text[] = "positional";
        g.text(Guest::fmt, format); g.text(string, text);
        m68k_write16(&g.ctx, Guest::input, 0xFFF4);
        m68k_write32(&g.ctx, Guest::input + 2, 1234567);
        m68k_write32(&g.ctx, Guest::input + 6, string);
        auto want = oracle(format, {0xFFF4, 1234567, (uintptr_t)text});
        g.format_string(); formatted(g, want, Guest::input + 10);
    }
    for (const char *format : {"%1$d/%1$x/%1$u", "%2$ld/%1$d", "%2$d/%1$d/%d"})
    {
        Guest g;
        g.text(Guest::fmt, format);
        m68k_write16(&g.ctx, Guest::input, 7);
        bool wide = std::strchr(format, 'l') != nullptr;
        if (wide) m68k_write32(&g.ctx, Guest::input + 2, 1234567);
        else
        {
            m68k_write16(&g.ctx, Guest::input + 2, 23);
            m68k_write16(&g.ctx, Guest::input + 4, 31);
        }
        auto want = oracle(format, {7, wide ? 1234567u : 23u, 31});
        g.format_string();
        formatted(g, want, Guest::input + (std::strstr(format, "%1$x") ? 2 : 6));
    }
    {
        Guest g;
        static UBYTE groups[] = {3, 2, 0};
        g.loc.loc_Grouping = groups; g.loc.loc_GroupSeparator = const_cast<char *>(".,");
        g.text(Guest::fmt, "%lD/%lU");
        m68k_write32(&g.ctx, Guest::input, (ULONG)-123456789);
        m68k_write32(&g.ctx, Guest::input + 4, 123456789);
        g.format_string();
        formatted(g, messages("-12,34,56.789/12,34,56.789"), Guest::input + 8);
    }
}

static void formatstring_invalid_inputs()
{
    for (const char *format : {"%", "%l", "%ll", "%1$", "%0$d", "%2$d",
                               "%3$d/%1$d", "%1$d/%1$ld", "%1$s/%1$ld", "%1$s/%1$b",
                               "%2147483648$d", "%2147483648d", "%.2147483648s"})
    {
        Guest g;
        g.text(Guest::fmt, format);
        const auto before = g.mem;
        g.format_string();
        CHECK(g.run() == 0);
        CHECK(!g.read(Guest::state) && !generated_calls && !g.ctx.callback_failed);
        CHECK(std::memcmp(g.mem.data() + Guest::fmt, before.data() + Guest::fmt, 0x800) == 0);
    }
    /* NULL locales and templates are valid: see null_locale_and_template(). */
    const std::vector<std::vector<ULONG>> bad = {
        {Guest::locale + 2, Guest::arena, 0xFFFFFFFF},
        {Guest::arena - 1, Guest::arena, 0xFFFFFFFF},
        {0, Guest::input + 1, Guest::arena - 2, Guest::arena, 0xFFFFFFFE, 0xFFFFFFFF},
        {0, Guest::hook + 1, Guest::arena - 18, Guest::arena, 0xFFFFFFFC},
        {0, Guest::hook_code + 1, Guest::arena - 1, Guest::arena, 0xFFFFFFFE,
         KAFFEINE_CALLBACK_RETURN_BASE}
    };
    for (unsigned field = 0; field < bad.size(); field++)
    {
        for (ULONG value : bad[field])
        {
            Guest g;
            g.text(Guest::fmt, "%ld");
            ULONG loc = Guest::locale, fmt = Guest::fmt, stream = Guest::input, hook = Guest::hook;
            if (field == 0) loc = value;
            if (field == 1) fmt = value;
            if (field == 2) stream = value;
            if (field == 3) hook = value;
            if (field == 4) m68k_write32(&g.ctx, Guest::hook + 8, value);
            g.format_string(stream, fmt, loc, hook);
            CHECK(g.run() == 0);
            CHECK(!g.read(Guest::state) && !generated_calls && !format_calls && !parse_calls);
            CHECK(!g.ctx.callback_failed && !g.ctx.locale_map[0].active_calls);
        }
    }
    for (bool bstr : {false, true})
    {
        for (ULONG value : {Guest::arena - 1, Guest::arena, 0x40000000u, 0xFFFFFFFFu})
        {
            Guest g;
            g.text(Guest::fmt, bstr ? "prefix%.0b" : "prefix%.0s");
            m68k_write32(&g.ctx, Guest::input, value);
            g.format_string();
            CHECK(g.run() == 0 && !g.read(Guest::state) && !generated_calls);
        }
    }
    for (unsigned kind = 0; kind < 7; kind++)
    {
        Guest g;
        g.text(Guest::fmt, "%s");
        m68k_write32(&g.ctx, Guest::input, Guest::arena - 1);
        if (kind == 0) { g.text(Guest::fmt, "%b"); m68k_write32(&g.ctx, Guest::input, (Guest::arena - 4) / 4); }
        if (kind == 1) { g.text(Guest::fmt, "%d/%ld"); g.format_string(Guest::arena - 4); }
        if (kind == 2) { g.a(0, Guest::locale); g.jsr(Guest::localebase, 42); }
        if (kind == 3) g.ctx.locale_map[0].close_pending = TRUE;
        if (kind == 4) { g.text(Guest::fmt, "%D"); g.loc.loc_Grouping = nullptr; }
        if (kind == 5) { g.text(Guest::fmt, "%lU"); g.loc.loc_GroupSeparator = nullptr; }
        if (kind == 6) g.text(Guest::fmt, "%2147483647d");
        if (kind != 1) g.format_string();
        CHECK(g.run() == 0 && !g.read(Guest::state) && !generated_calls);
    }
    for (int allocation = 0; allocation < 3; allocation++)
    {
        Guest g;
        g.text(Guest::fmt, "%ld");
        allocations_left = allocation;
        g.format_string();
        CHECK(g.run() == 0 && !g.read(Guest::state) && !generated_calls);
        CHECK(allocations.empty());
    }
    {
        Guest g;
        g.text(Guest::fmt, "prefix%lld");
        g.format_string(Guest::arena - 4);
        CHECK(g.run() == 0 && !g.read(Guest::state) && !generated_calls);
    }
}

static void formatstring_boundaries_and_registers()
{
    for (bool quad : {false, true})
    {
        Guest g;
        const ULONG size = quad ? 8 : 2;
        g.text(Guest::fmt, quad ? "%lld" : "%d");
        if (quad)
        {
            m68k_write32(&g.ctx, Guest::arena - 8, 0);
            m68k_write32(&g.ctx, Guest::arena - 4, 42);
        }
        else m68k_write16(&g.ctx, Guest::arena - 2, 42);
        g.format_string(Guest::arena - size);
        formatted(g, messages("42"), Guest::arena);
    }
    for (bool string : {false, true})
    {
        Guest g;
        g.text(Guest::input, "");
        g.text(Guest::fmt, string ? "%s" : "%ld");
        m68k_write32(&g.ctx, Guest::arena - 4, string ? Guest::input : 42);
        g.format_string(Guest::arena - 4);
        formatted(g, messages(string ? "" : "42"), Guest::arena);
    }
    {
        Guest g;
        g.mem.back() = 0;
        g.text(Guest::fmt, "%s");
        m68k_write32(&g.ctx, Guest::input, Guest::arena - 1);
        g.format_string(); formatted(g, {0}, Guest::input + 4);
    }
    for (ULONG stream : {0u, Guest::arena, 0xFFFFFFFFu})
    {
        Guest g;
        g.mem.back() = 0;
        g.format_string(stream, Guest::arena - 1);
        formatted(g, {0}, stream);
    }
    {
        Guest g;
        /* The BSTR length byte plus its exact payload reaches mem_size. */
        g.mem[Guest::arena - 4] = 3;
        std::memcpy(g.mem.data() + Guest::arena - 3, "ABC", 3);
        g.text(Guest::fmt, "%b");
        m68k_write32(&g.ctx, Guest::input, (Guest::arena - 4) / 4);
        g.format_string(); formatted(g, messages("ABC"), Guest::input + 4);
    }
    Guest g;
    g.text(Guest::fmt, "%ld"); m68k_write32(&g.ctx, Guest::input, 42);
    for (unsigned i = 0; i < 8; i++) g.d(i, 0xD0000000 + i);
    for (unsigned i = 0; i < 7; i++) g.a(i, 0xA0000000 + i);
    g.a(0, Guest::locale); g.a(1, Guest::fmt); g.a(2, Guest::input); g.a(3, Guest::hook);
    g.a(6, Guest::localebase); g.words({0x46FC, 0x2715, 0x4EAE, (UWORD)-66});
    g.save();
    formatted(g, messages("42"), Guest::input + 4);
    CHECK(g.read(Guest::snapshot) == Guest::input + 4);
    for (unsigned i = 1; i < 8; i++) CHECK(g.read(Guest::snapshot + 4 * i) == 0xD0000000 + i);
    const ULONG addresses[] = {Guest::locale, Guest::fmt, Guest::input, Guest::hook,
        0xA0000004, 0xA0000005, Guest::localebase, Guest::stack - 4};
    for (unsigned i = 0; i < 8; i++) CHECK(g.read(Guest::snapshot + 32 + 4 * i) == addresses[i]);
    CHECK(m68k_read16(&g.ctx, Guest::snapshot + 64) == 0x2715);
}

static void formatstring_callbacks()
{
    for (unsigned kind = 0; kind < 5; kind++)
    {
        Guest g;
        g.text(Guest::fmt, "%ld"); m68k_write32(&g.ctx, Guest::input, 1234567);
        ULONG saved = g.pc; g.pc = Guest::hook_code;
        g.record();
        if (kind == 0) g.word(0x4AFC);
        if (kind == 1) g.word(0xFFFF);
        if (kind == 2) g.words({0x2039, 0xFFFF, 0xFFFE});
        if (kind == 3) g.jsr(Guest::exec, 60);
        if (kind == 4)
        {
            g.a(0, Guest::locale); g.jsr(Guest::localebase, 42);
            g.jsr(Guest::exec, 60);
        }
        g.word(0x4E75); g.pc = saved;
        g.format_string(); g.mark();
        CHECK(g.run_without_catches() == (kind >= 3 ? 7 : 20));
        CHECK(g.ctx.callback_failed && !g.ctx.callback_depth && !g.ctx.running);
        CHECK(g.read(Guest::state) == 1 && !generated_calls);
        CHECK(g.mem[Guest::marker] == 0xA5 && allocations.empty());
        CHECK(!g.ctx.locale_map[0].active_calls && !close_calls);
        if (kind == 4) CHECK(g.ctx.locale_map[0].close_pending);
    }
    {
        Guest g;
        constexpr ULONG string = 0x3400;
        g.text(Guest::fmt, "[%s] %2$ld"); g.text(string, "stable");
        m68k_write32(&g.ctx, Guest::input, string);
        m68k_write32(&g.ctx, Guest::input + 4, 1234567);
        ULONG saved = g.pc; g.pc = Guest::hook_code;
        g.record();
        for (ULONG at : {Guest::fmt + 10, string + 6, Guest::input + 4})
        { g.words({0x13FC, 'X'}); g.longword(at); }
        g.a(0, Guest::locale); g.jsr(Guest::localebase, 42);
        g.word(0x4E75); g.pc = saved;
        g.format_string();
        formatted(g, messages("[stable] 1234567"), Guest::input + 8);
        CHECK(close_calls == 1 && native_closed && !g.ctx.locale_map[0].guest_addr);
    }
    {
        Guest g;
        constexpr ULONG inner_format = 0x3400;
        g.text(Guest::fmt, "A"); g.text(inner_format, "B");
        ULONG saved = g.pc; g.pc = Guest::hook_code;
        g.format_string(0, inner_format, Guest::locale, Guest::inner_hook);
        g.word(0x4E75); g.pc = saved;
        g.format_string();
        CHECK((ULONG)g.run() == Guest::input);
        g.check_trace({'B', 0, 'B', 0}, Guest::inner_hook);
        CHECK(!g.ctx.locale_map[0].active_calls && !generated_calls);
    }
}

/* As in AROS, a NULL locale selects the default locale for one call, and a
   NULL FormatString() template returns the data stream untouched. */
static void null_locale_and_template()
{
    for (bool parse : {false, true})
    {
        Guest g;
        expected_locale = &default_loc;
        if (parse) g.recorder(Guest::hook_code, true);
        g.call(parse, 0);
        LONG result = g.run();
        if (parse)
        {
            CHECK(result == parse_result && parse_calls == 1 && !format_calls);
            g.check_trace({0, 0, 0, 0}, Guest::hook, 0);
            g.check_date(output_date);
        }
        else
        {
            CHECK(format_calls == 1 && !parse_calls);
            g.check_trace({'A', 0xE9, 'Z', 0}, Guest::hook, 0);
            g.check_date(input_date);
        }
        CHECK(default_opens == 1 && default_closes == 1);
        CHECK(!g.ctx.locale_map[0].active_calls && !native_closed);
    }
    for (bool parse : {false, true})
    {
        Guest g;
        fail_default_locale = true;
        if (parse) g.recorder(Guest::hook_code, true);
        g.call(parse, 0);
        LONG result = g.run();
        CHECK(!format_calls && !parse_calls && !default_opens && !default_closes);
        CHECK(g.read(Guest::state) == 0 && !generated_calls);
        if (parse) CHECK(result == 0);
        g.check_date(input_date);
    }
    {
        Guest g;
        g.text(Guest::fmt, "%lD|%lU");
        m68k_write32(&g.ctx, Guest::input, 1234567);
        m68k_write32(&g.ctx, Guest::input + 4, 7654321);
        auto want = oracle("%lD|%lU", {1234567, 7654321}, &default_loc);
        CHECK(want != oracle("%lD|%lU", {1234567, 7654321}));
        g.format_string(Guest::input, Guest::fmt, 0);
        CHECK((ULONG)g.run() == Guest::input + 8);
        g.check_trace(want, Guest::hook, 0);
        CHECK(default_opens == 1 && default_closes == 1 && !format_calls && !parse_calls);
        CHECK(!g.ctx.locale_map[0].active_calls && !native_closed);
    }
    {
        Guest g;
        fail_default_locale = true;
        g.format_string(Guest::input, Guest::fmt, 0);
        CHECK(g.run() == 0 && g.read(Guest::state) == 0 && !default_opens);
    }
    for (ULONG loc_addr : {(ULONG)Guest::locale, 0u, (ULONG)Guest::locale + 4})
        for (ULONG hook_addr : {(ULONG)Guest::hook, 0u})
        {
            Guest g;
            g.format_string(Guest::input, 0, loc_addr, hook_addr);
            CHECK((ULONG)g.run() == Guest::input);
            CHECK(g.read(Guest::state) == 0 && !default_opens && !default_closes);
            CHECK(!generated_calls && !g.ctx.locale_map[0].active_calls);
        }
}

int main()
{
    registers_and_characters();
    parse_failure_and_full_result();
    invalid_inputs();
    boundary_inputs();
    callback_faults_and_stops();
    nested_callhookpkt();
    callback_mutation_and_close();
    nested_native_hooks();
    no_unwinding();
    formatstring_conversions();
    formatstring_strings_and_positions();
    formatstring_invalid_inputs();
    formatstring_boundaries_and_registers();
    formatstring_callbacks();
    null_locale_and_template();
    std::printf("test_locale_hooks: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

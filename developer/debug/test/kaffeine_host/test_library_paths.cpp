/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Production lookup, wrappers, cache lookup, generated registrations,
    CPU accessors and dispatcher. Only setup and disk loading are controlled.
*/
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kaffeine_intern.h"
#include "kaffeine_thunks.h"
#include "kaffeine_dispatch.h"

static unsigned checks, failures, setup_calls, disk_calls;
static bool fail_setup, stop_disk;
static ULONG disk_result, disk_version;
static std::string disk_name;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    if (failures < 30) std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

extern "C" IPTR library_paths_unexpected_thunk(KaffeineContext *, void *)
{ std::abort(); }

/* Distinct tables make incorrect manual/generated selection observable. */
#define MANUAL_TABLE(name) \
    extern "C" const M68KThunkEntry kaffeine_thunks_##name[] = { \
        {30, library_paths_unexpected_thunk}, {36, library_paths_unexpected_thunk} }; \
    extern "C" const ULONG kaffeine_thunks_##name##_count = 2
MANUAL_TABLE(intuition_manual);
MANUAL_TABLE(graphics_manual);
MANUAL_TABLE(diskfont_manual);
MANUAL_TABLE(utility_manual);
MANUAL_TABLE(locale_manual);
MANUAL_TABLE(gadtools_manual);
MANUAL_TABLE(mathffp);
MANUAL_TABLE(workbench);
MANUAL_TABLE(mathieeedoubbas);
MANUAL_TABLE(mathieeedoubtrans);
MANUAL_TABLE(mathieeesingbas);
#undef MANUAL_TABLE

extern "C" ULONG Kaffeine_SetupLibBase(KaffeineContext *ctx, UWORD id,
    const char *name, UWORD vectors, const M68KThunkEntry *manual,
    ULONG manual_count, const M68KThunkEntry *generated, ULONG generated_count)
{
    ++setup_calls;
    CHECK(id == ctx->num_libs && vectors == KAFFEINE_MAX_LVO);
    CHECK(std::strlen(name) < sizeof(ctx->libs[0].name));
    if (fail_setup) { fail_setup = false; return 0; }
    if (ctx->num_libs == KAFFEINE_MAX_LIBS) return 0;
    auto &lib = ctx->libs[ctx->num_libs++];
    lib.m68k_addr = 0x2000u * (id + 1);
    lib.jt_start = lib.m68k_addr - 6u * vectors;
    lib.lib_id = id;
    lib.num_vectors = vectors;
    lib.thunks = manual;
    lib.num_thunks = manual_count;
    lib.gen_thunks = generated;
    lib.num_gen_thunks = generated_count;
    std::snprintf(lib.name, sizeof(lib.name), "%s", name);
    if (ctx->num_libmap < KAFFEINE_MAX_LIBMAP) {
        auto &entry = ctx->libmap[ctx->num_libmap++];
        std::snprintf(entry.name, sizeof(entry.name), "%s", name);
        entry.m68k_base = lib.m68k_addr;
    }
    return lib.m68k_addr;
}

extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *ctx, void *,
                                         const char *name, ULONG version)
{
    ++disk_calls;
    disk_name = name;
    disk_version = version;
    if (stop_disk) { ctx->running = FALSE; ctx->exit_code = 20; }
    return disk_result;
}

struct TestCPU { ULONG d[8] = {}, a[8] = {}, pc = 0x13572468; UWORD sr = 0xa51f; };
static ULONG get_d(void *p, int n) { return static_cast<TestCPU *>(p)->d[n]; }
static ULONG get_a(void *p, int n) { return static_cast<TestCPU *>(p)->a[n]; }
static void set_d(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->d[n] = v; }
static void set_a(void *p, int n, ULONG v) { static_cast<TestCPU *>(p)->a[n] = v; }
static UWORD get_sr(void *p) { return static_cast<TestCPU *>(p)->sr; }
static void set_sr(void *p, UWORD v) { static_cast<TestCPU *>(p)->sr = v; }
static ULONG get_pc(void *p) { return static_cast<TestCPU *>(p)->pc; }
static void set_pc(void *p, ULONG v) { static_cast<TestCPU *>(p)->pc = v; }
static void execute(void *) { std::abort(); }
static const KaffeineCPUOps ops = {
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

struct Fixture
{
    KaffeineContext ctx = {};
    std::array<UBYTE, 32768> memory;
    TestCPU state;
    KaffeineCPU cpu = {&ops, &state};

    Fixture()
    {
        memory.fill(0xa5);
        ctx.mem = memory.data(); ctx.mem_size = memory.size(); ctx.running = TRUE;
        fail_setup = stop_disk = false; disk_result = 0; disk_name.clear();
        ctx.m68k_sysbase = Kaffeine_SetupLibBase(&ctx, 0, "exec.library",
            KAFFEINE_MAX_LVO, kaffeine_thunks_exec, kaffeine_thunks_exec_count, nullptr, 0);
        Kaffeine_SetupLibBase(&ctx, 1, "dos.library", KAFFEINE_MAX_LVO, nullptr, 0, nullptr, 0);
        setup_calls = disk_calls = 0;
        for (unsigned i = 0; i < 8; ++i) {
            state.d[i] = 0xd0d00100 + i; state.a[i] = 0xa0a00100 + i;
        }
    }

    ULONG call_at(ULONG lvo, ULONG at, ULONG version = 0xbeef0022)
    {
        state.d[0] = version; state.a[1] = at; state.a[7] = 0x100;
        m68k_write32(&ctx, 0x100, 0x76543210);
        const auto before = state;
        const auto bytes = memory;
        KaffeineDispatchResult result = {};
        CHECK(Kaffeine_DispatchLibraryCall(&ctx, &cpu, ctx.m68k_sysbase - lvo, &result));
        CHECK(result.return_pc == 0x76543210 && !result.supervisor);
        CHECK(state.a[7] == before.a[7] + 4 && state.pc == before.pc && state.sr == before.sr);
        for (unsigned i = 1; i < 8; ++i) CHECK(state.d[i] == before.d[i]);
        for (unsigned i = 0; i < 7; ++i) CHECK(state.a[i] == before.a[i]);
        CHECK(memory == bytes);
        return state.d[0];
    }

    ULONG call(const std::string &name, ULONG lvo = 552, ULONG version = 0xbeef0022)
    {
        CHECK(name.size() + 1 < memory.size() - 0x1000);
        std::memcpy(memory.data() + 0x1000, name.c_str(), name.size() + 1);
        return call_at(lvo, 0x1000, version);
    }

    ULONG native(const std::string &name)
    {
        auto caller = name;
        const auto bytes = memory;
        ULONG base = Kaffeine_OpenLibrary(&ctx, caller.c_str());
        CHECK(caller == name && memory == bytes);
        return base;
    }
};

struct Manual { const char *name; const M68KThunkEntry *table; ULONG count; };
static const Manual manuals[] = {
    {"intuition.library", kaffeine_thunks_intuition_manual, kaffeine_thunks_intuition_manual_count},
    {"graphics.library", kaffeine_thunks_graphics_manual, kaffeine_thunks_graphics_manual_count},
    {"diskfont.library", kaffeine_thunks_diskfont_manual, kaffeine_thunks_diskfont_manual_count},
    {"utility.library", kaffeine_thunks_utility_manual, kaffeine_thunks_utility_manual_count},
    {"locale.library", kaffeine_thunks_locale_manual, kaffeine_thunks_locale_manual_count},
    {"gadtools.library", kaffeine_thunks_gadtools_manual, kaffeine_thunks_gadtools_manual_count},
    {"mathffp.library", kaffeine_thunks_mathffp, kaffeine_thunks_mathffp_count},
    {"workbench.library", kaffeine_thunks_workbench, kaffeine_thunks_workbench_count},
    {"mathieeedoubbas.library", kaffeine_thunks_mathieeedoubbas, kaffeine_thunks_mathieeedoubbas_count},
    {"mathieeedoubtrans.library", kaffeine_thunks_mathieeedoubtrans, kaffeine_thunks_mathieeedoubtrans_count},
    {"mathieeesingbas.library", kaffeine_thunks_mathieeesingbas, kaffeine_thunks_mathieeesingbas_count}
};

static const M68KLibThunkSet *generated(const char *name)
{
    for (ULONG i = 0; i < kaffeine_all_gen_libs_count; ++i)
        if (!std::strcmp(name, kaffeine_all_gen_libs[i].name)) return &kaffeine_all_gen_libs[i];
    return nullptr;
}

static void reuse(const char *name, const Manual *manual, const M68KLibThunkSet *gen)
{
    for (bool path_first : {true, false}) {
        Fixture f;
        std::string path = std::string("LIBS:") + name;
        ULONG base = f.call(path_first ? path : name);
        CHECK(base != 0);
        CHECK(f.call(path_first ? name : path, 408) == base);
        CHECK(f.call(std::string("SYS:Classes//LIBS:m68k/") + name, 552, 0xffffffff) == base);
        const auto &lib = f.ctx.libs[2];
        CHECK(!std::strcmp(lib.name, name) && !std::strcmp(f.ctx.libmap[2].name, name));
        CHECK(lib.thunks == (manual ? manual->table : nullptr));
        CHECK(lib.num_thunks == (manual ? manual->count : 0));
        CHECK(lib.gen_thunks == (gen ? gen->thunks : nullptr));
        CHECK(lib.num_gen_thunks == (gen ? gen->count : 0));
        CHECK(Kaffeine_FindLibBase(&f.ctx, name) == base);
        std::string other_case = name;
        other_case[0] -= 'a' - 'A';
        CHECK(f.native("LIBS:" + other_case) == 0);
        CHECK(setup_calls == 1 && disk_calls == 0 && f.ctx.num_libs == 3 && f.ctx.num_libmap == 3);
    }
}

static void registrations_and_reuse()
{
    const auto *diskfont = generated("diskfont.library");
    CHECK(diskfont && diskfont->count == 3);
    if (diskfont) {
        CHECK(diskfont->thunks[0].lvo == 36 && diskfont->thunks[1].lvo == 42 && diskfont->thunks[2].lvo == 48);
    }
    for (ULONG i = 0; i < kaffeine_all_gen_libs_count; ++i) {
        const auto &gen = kaffeine_all_gen_libs[i];
        if (!std::strcmp(gen.name, "exec.library") || !std::strcmp(gen.name, "dos.library")) continue;
        const Manual *manual = nullptr;
        for (const auto &candidate : manuals)
            if (!std::strcmp(candidate.name, gen.name)) manual = &candidate;
        reuse(gen.name, manual, &gen);
    }
    for (const auto &manual : manuals)
        if (!generated(manual.name)) reuse(manual.name, &manual, nullptr);

    Fixture f;
    for (unsigned i = 0; i < 48; ++i) {
        const unsigned slot = i % 2;
        CHECK(f.call("Volume" + std::to_string(i) + ":dir//" + f.ctx.libs[slot].name,
                     slot ? 408 : 552, 0xffff0000) == f.ctx.libs[slot].m68k_addr);
    }
    CHECK(setup_calls == 0 && disk_calls == 0 && f.ctx.num_libs == 2 && f.ctx.num_libmap == 2);
}

static void rejection_and_fallback()
{
    Fixture f;
    CHECK(Kaffeine_OpenLibrary(&f.ctx, nullptr) == 0);
    for (const char *name : {"", ":", "/", "LIBS:/", "LIBS:diskfont.library/",
         "LIBS:mathffp.library:", "LIBS:xdiskfont.library", "LIBS:diskfont.libraryx",
         "LIBS:mathffp.library.bak", "LIBS:xworkbench.library", "LIBS:DISKFONT.library",
         "LIBS:Mathffp.library", "LIBS:Workbench.library", "LIBS:Exec.library",
         "LIBS:DOS.library", "LIBS:diskfont.library/unknown.library", "LIBS\\diskfont.library",
         "LIBS:diskfont.library ", "LIBS:\xff" "diskfont.library"})
        CHECK(f.native(name) == 0);
    CHECK(setup_calls == 0 && disk_calls == 0 && f.ctx.num_libs == 2 && f.ctx.num_libmap == 2);

    for (ULONG lvo : {552u, 408u}) {
        for (const char *name : {"MiXeD:dir//Other:unknown.library", "libs:Diskfont.library",
                               "LIBS:workbench.libraryx", "", "LIBS:diskfont.library/"}) {
            for (ULONG version : {0xbeef0022u, 0xffffffffu, 0x81230000u}) {
                const unsigned before = disk_calls;
                disk_result = version == 0xbeef0022u ? 0x12345678 : 0;
                CHECK(f.call(name, lvo, version) == disk_result);
                CHECK(disk_calls == before + 1 && disk_name == name);
                CHECK(disk_version == (lvo == 552 ? (version & 0xffff) : 0));
            }
        }
    }
    stop_disk = true; disk_result = 0x12345678;
    CHECK(f.call("LIBS:unknown.library") == 20 && !f.ctx.running);
    CHECK(setup_calls == 0 && f.ctx.num_libs == 2 && f.ctx.num_libmap == 2);

    for (const char *name : {"diskfont.library", "mathffp.library", "workbench.library"}) {
        Fixture retry;
        fail_setup = true;
        CHECK(retry.call(std::string("LIBS:") + name) == 0);
        CHECK(setup_calls == 1 && disk_calls == 1 && disk_name == std::string("LIBS:") + name);
        CHECK(disk_version == 34 && retry.ctx.num_libs == 2 && retry.ctx.num_libmap == 2);
        ULONG base = retry.call(std::string("SYS:Libs/") + name);
        CHECK(base != 0 && retry.call(name) == base);
        CHECK(setup_calls == 2 && disk_calls == 1 && retry.ctx.num_libs == 3 && retry.ctx.num_libmap == 3);
    }
}

static void name_boundaries()
{
    for (ULONG lvo : {552u, 408u}) {
        Fixture f;
        for (ULONG at : {0u, f.ctx.mem_size, f.ctx.mem_size + 1, 0xffffffffu})
            CHECK(f.call_at(lvo, at) == 0);
        CHECK(f.call_at(lvo, f.ctx.mem_size - 1) == 0); // Unterminated final byte.
        CHECK(setup_calls == 0 && disk_calls == 0);

        const std::string leaf = "diskfont.library";
        const std::string longest = std::string(254 - leaf.size(), 'p') + ":" + leaf;
        CHECK(longest.size() == 255);
        CHECK(f.call("p" + longest, lvo) == 0); // 256 bytes before NUL.
        CHECK(setup_calls == 0 && disk_calls == 0);
        ULONG base = f.call(longest, lvo);
        CHECK(base != 0 && f.call(leaf, lvo) == base);
        const ULONG at = f.ctx.mem_size - leaf.size() - 1;
        std::memcpy(f.memory.data() + at, leaf.c_str(), leaf.size() + 1);
        CHECK(f.call_at(lvo, at) == base); // NUL at the last guest byte.
        CHECK(setup_calls == 1 && disk_calls == 0 && f.ctx.num_libs == 3 && f.ctx.num_libmap == 3);

        const std::string unknown = std::string(239, 'P') + ":missing.library";
        CHECK(unknown.size() == 255);
        CHECK(f.call(unknown, lvo, 0xfaceffff) == 0);
        CHECK(disk_calls == 1 && disk_name == unknown && disk_version == (lvo == 552 ? 65535u : 0u));
    }
    Fixture f;
    const std::string long_name = std::string(8192, 'P') + "/LIBS:mathffp.library";
    ULONG base = f.native(long_name);
    CHECK(base != 0 && f.native("mathffp.library") == base);
    CHECK(!std::strcmp(f.ctx.libs[2].name, "mathffp.library"));
    CHECK(!std::strcmp(f.ctx.libmap[2].name, "mathffp.library"));
    CHECK(setup_calls == 1 && disk_calls == 0 && f.ctx.num_libs == 3 && f.ctx.num_libmap == 3);
}

int main()
{
    registrations_and_reuse();
    rejection_and_fallback();
    name_boundaries();
    std::printf("library paths: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

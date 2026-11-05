/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The struct Library header of fake library bases with the production
    base layout (kaffeine_memory.c): positive-area sizes, the bytes that
    Kaffeine_PublishLibrary() writes, the bases it refuses, and the jump
    tables, gaps and heap around every base, which stay as they were.
*/
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <proto/exec.h>
#include "kaffeine_intern.h"

static unsigned checks, failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    if (failures < 40) std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    APTR block = std::malloc(size);
    if (block) std::memset(block, (flags & MEMF_CLEAR) ? 0 : 0xCD, size);
    return block;
}

extern "C" void FreeMem(APTR block, ULONG) { std::free(block); }

/* Collaborators of the library/HUNK functions, not used by these tests. */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }

static const ULONG region_end = KAFFEINE_LIBBASE_REGION + KAFFEINE_LIBBASE_SIZE;
static const UBYTE fill = 0xA5;   /* never a byte of a header or a slot */
static const UBYTE nt_library = 9;

/* A production context whose base region holds only the fill byte, with
   ExecBase first as on the target, and a view of the heap's first bytes,
   which start where the region ends. ExecBase gets its header from its own
   owner (kaffeine_shadow.c), never from Kaffeine_PublishLibrary(), so its
   positive area must keep every byte here. */
struct Context
{
    KaffeineContext *ctx;
    ULONG exec;
    std::vector<UBYTE> heap_head, exec_area;

    Context()
    {
        ctx = Kaffeine_CreateContext(nullptr);
        if (!ctx) { std::puts("FAIL: no context"); std::exit(1); }
        std::memset(ctx->mem + KAFFEINE_LIBBASE_REGION, fill, KAFFEINE_LIBBASE_SIZE);
        heap_head.assign(ctx->mem + region_end, ctx->mem + region_end + 64);
        exec = Kaffeine_SetupLibBase(ctx, 0, "exec.library", KAFFEINE_MAX_LVO,
                                     nullptr, 0, nullptr, 0);
        CHECK(exec == (ULONG)(KAFFEINE_LIBBASE_REGION + KAFFEINE_MAX_LVO * 6));
        exec_area.assign(ctx->mem + exec, ctx->mem + exec + 632);
    }
    ~Context()
    {
        CHECK(std::equal(heap_head.begin(), heap_head.end(), ctx->mem + region_end));
        CHECK(std::equal(exec_area.begin(), exec_area.end(), ctx->mem + exec));
        Kaffeine_DestroyContext(ctx);
    }

    std::vector<UBYTE> region() const
    {
        return std::vector<UBYTE>(ctx->mem + KAFFEINE_LIBBASE_REGION, ctx->mem + region_end + 64);
    }
    UWORD be16(ULONG at) const { return (UWORD)(ctx->mem[at] << 8 | ctx->mem[at + 1]); }
    const M68KFakeLibBase *record(ULONG base) const
    {
        for (UWORD i = 0; i < ctx->num_libs; ++i)
            if (ctx->libs[i].m68k_addr == base) return &ctx->libs[i];
        return nullptr;
    }

    ULONG library(const char *name, UWORD vectors = KAFFEINE_MAX_LVO)
    {
        return Kaffeine_SetupLibBase(ctx, ctx->num_libs, name, vectors, nullptr, 0, nullptr, 0);
    }
    ULONG sized(const char *name, UWORD positive, UWORD vectors = KAFFEINE_MAX_LVO)
    {
        return Kaffeine_SetupLibBaseSized(ctx, ctx->num_libs, name, vectors, positive,
                                          nullptr, 0, nullptr, 0);
    }
    ULONG device(const char *name, UWORD vectors = 8)
    {
        return Kaffeine_SetupDeviceBase(ctx, name, vectors, nullptr, 0);
    }
};

/* Every byte of the published header, read as the guest reads it. */
static void check_header(const Context &c, ULONG base, UWORD version, UWORD revision)
{
    const M68KFakeLibBase *lib = c.record(base);
    CHECK(lib != nullptr);
    if (!lib) return;
    for (ULONG i = 0; i < 8; ++i) CHECK(c.ctx->mem[base + i] == 0);   /* ln_Succ, ln_Pred */
    CHECK(c.ctx->mem[base + 8] == nt_library);                        /* ln_Type */
    /* ln_Pri, ln_Name, lib_Flags, lib_pad */
    for (ULONG i = 9; i < 16; ++i) CHECK(c.ctx->mem[base + i] == 0);
    CHECK(c.be16(base + 16) == lib->num_vectors * 6);                 /* lib_NegSize */
    CHECK(c.be16(base + 18) == lib->pos_size);                        /* lib_PosSize */
    CHECK(c.be16(base + 20) == version);                              /* lib_Version */
    CHECK(c.be16(base + 22) == revision);                             /* lib_Revision */
    /* lib_IdString, lib_Sum, lib_OpenCnt */
    for (ULONG i = 24; i < 34; ++i) CHECK(c.ctx->mem[base + i] == 0);
}

/* Every slot of the jump table of each record is still A-line, NOP, NOP. */
static void check_slots(const Context &c)
{
    for (UWORD i = 0; i < c.ctx->num_libs; ++i) {
        const M68KFakeLibBase &lib = c.ctx->libs[i];
        CHECK(lib.jt_start + lib.num_vectors * 6u == lib.m68k_addr);
        for (ULONG at = lib.jt_start; at < lib.m68k_addr; at += 6)
            CHECK(c.be16(at) == 0xA000 && c.be16(at + 2) == 0x4E71 && c.be16(at + 4) == 0x4E71);
    }
}

/* The records lie in the region one after the other, without overlap,
   each jump table at the 4-aligned end of the previous positive area, and
   every byte outside them is still the fill byte. */
static void check_extents(const Context &c)
{
    std::vector<const M68KFakeLibBase *> sorted;
    for (UWORD i = 0; i < c.ctx->num_libs; ++i) sorted.push_back(&c.ctx->libs[i]);
    std::sort(sorted.begin(), sorted.end(),
              [](const M68KFakeLibBase *a, const M68KFakeLibBase *b)
              { return a->jt_start < b->jt_start; });
    ULONG next = KAFFEINE_LIBBASE_REGION, covered = KAFFEINE_LIBBASE_REGION;
    for (const M68KFakeLibBase *lib : sorted) {
        CHECK(lib->jt_start == next);
        CHECK(lib->m68k_addr + lib->pos_size <= region_end);
        for (ULONG at = covered; at < lib->jt_start; ++at) CHECK(c.ctx->mem[at] == fill);
        covered = lib->m68k_addr + lib->pos_size;
        next = (covered + 3) & ~3u;
    }
    for (ULONG at = covered; at < region_end; ++at) CHECK(c.ctx->mem[at] == fill);
}

static void sizes()
{
    Context c;
    const ULONG utility = c.library("utility.library");
    CHECK(utility != 0);
    CHECK(c.record(c.exec)->pos_size == 632 && c.record(c.exec)->library);
    CHECK(c.record(utility)->jt_start == c.exec + 632);
    CHECK(c.record(utility)->pos_size == 34 && c.record(utility)->library);

    /* A positive area smaller than a struct Library is refused before
       anything is written or recorded. */
    for (UWORD positive : {0, 1, 4, 33}) {
        const auto before = c.region();
        const UWORD libs = c.ctx->num_libs, map = c.ctx->num_libmap;
        CHECK(c.sized("small.library", positive) == 0);
        CHECK(c.ctx->num_libs == libs && c.ctx->num_libmap == map);
        CHECK(c.region() == before && Kaffeine_FindLibBase(c.ctx, "small.library") == 0);
    }
    const ULONG exact = c.sized("exact.library", 34), intuition = c.sized("intuition.library", 80);
    CHECK(exact && c.record(exact)->pos_size == 34 && c.record(exact)->library);
    CHECK(intuition && c.record(intuition)->pos_size == 80 && c.record(intuition)->library);

    /* A device gets a cleared struct Device and no library record kind;
       OpenLibrary() does not find it. */
    const ULONG timer = c.device("timer.device");
    CHECK(timer && c.record(timer)->pos_size == 34 && !c.record(timer)->library);
    for (ULONG i = 0; i < 34; ++i) CHECK(c.ctx->mem[timer + i] == 0);
    CHECK(Kaffeine_FindLibBase(c.ctx, "timer.device") == 0);
    check_slots(c);
}

static void published_bytes()
{
    Context c;
    const ULONG base = c.library("utility.library"), next = c.library("icon.library");
    CHECK(base && next && c.record(next)->jt_start == base + 36);
    /* Before publication the positive area is what the region held: the
       heap never wrote a header there, and a guest read lib_Version from
       neighbouring bytes when the area was 4 bytes long. */
    CHECK(c.ctx->mem[base + 20] == fill && c.ctx->mem[base + 21] == fill);

    auto before = c.region();
    CHECK(Kaffeine_PublishLibrary(c.ctx, base, 40, 17));
    check_header(c, base, 40, 17);
    auto after = c.region();
    for (ULONG i = 0; i < before.size(); ++i) {
        const ULONG at = KAFFEINE_LIBBASE_REGION + i;
        if (at < base || at >= base + 34) CHECK(after[i] == before[i]);
    }
    CHECK(c.ctx->mem[base + 34] == fill && c.ctx->mem[base + 35] == fill);

    /* A second publication clears the whole header again, whatever was
       stored in it meanwhile, and keeps the full 16-bit values. */
    c.ctx->mem[base + 10] = 0x12; c.ctx->mem[base + 32] = 0x34; c.ctx->mem[base + 9] = 0x56;
    CHECK(Kaffeine_PublishLibrary(c.ctx, base, 0xFFFF, 0x8001));
    check_header(c, base, 0xFFFF, 0x8001);
    CHECK(Kaffeine_PublishLibrary(c.ctx, next, 0, 0));
    check_header(c, next, 0, 0);
    check_header(c, base, 0xFFFF, 0x8001);
    check_slots(c);
    check_extents(c);
}

static void owner_fields()
{
    /* Fields past the struct Library belong to the owner of the base, as
       those of IntuitionBase and GfxBase do (ChipRevBits0 at 236 and
       DefaultFont at 154 of a 552-byte positive area). */
    Context c;
    const ULONG intuition = c.sized("intuition.library", 80);
    const ULONG graphics = c.sized("graphics.library", 552);
    CHECK(intuition && graphics);
    for (ULONG i = 34; i < 80; ++i) c.ctx->mem[intuition + i] = (UBYTE)(0x40 + i);
    for (ULONG i = 34; i < 552; ++i) c.ctx->mem[graphics + i] = (UBYTE)(i * 7);
    const auto before = c.region();
    CHECK(Kaffeine_PublishLibrary(c.ctx, intuition, 41, 3));
    CHECK(Kaffeine_PublishLibrary(c.ctx, graphics, 41, 5));
    check_header(c, intuition, 41, 3);
    check_header(c, graphics, 41, 5);
    const auto after = c.region();
    for (ULONG i = 0; i < before.size(); ++i) {
        const ULONG at = KAFFEINE_LIBBASE_REGION + i;
        const bool header = (at >= intuition && at < intuition + 34) ||
                            (at >= graphics && at < graphics + 34);
        if (!header) CHECK(after[i] == before[i]);
    }
    CHECK(c.ctx->mem[graphics + 236] == (UBYTE)(236 * 7));
    CHECK(c.ctx->mem[graphics + 154] == (UBYTE)(154 * 7));
    CHECK(c.ctx->mem[intuition + 79] == 0x40 + 79);
}

static void refusals()
{
    Context c;
    const ULONG base = c.library("utility.library");
    const ULONG timer = c.device("timer.device");
    CHECK(base && timer);
    /* The owner of the device fills its struct Device. */
    c.ctx->mem[timer + 8] = 3;                 /* NT_DEVICE */
    c.ctx->mem[timer + 20] = 0; c.ctx->mem[timer + 21] = 50;
    c.ctx->mem[timer + 32] = 0; c.ctx->mem[timer + 33] = 1;

    /* An address that is no base, a jump table, a device, an empty
       context: FALSE and not a byte written. */
    const auto before = c.region();
    CHECK(!Kaffeine_PublishLibrary(nullptr, base, 40, 1));
    CHECK(!Kaffeine_PublishLibrary(c.ctx, 0, 40, 1));
    const ULONG no_base[] = {base - 2, base + 2, base + 34, c.record(base)->jt_start,
                             timer - 6, KAFFEINE_LIBBASE_REGION, region_end,
                             c.ctx->mem_size - 2, c.ctx->mem_size, 0xFFFFFFFFu};
    for (ULONG at : no_base)
        CHECK(!Kaffeine_PublishLibrary(c.ctx, at, 40, 1));
    CHECK(!Kaffeine_PublishLibrary(c.ctx, timer, 40, 1));
    CHECK(c.region() == before);
    CHECK(c.ctx->mem[timer + 8] == 3 && c.be16(timer + 20) == 50 && c.be16(timer + 32) == 1);

    /* A record that claims a library with a positive area smaller than a
       struct Library (no setup function makes one any more) is refused. */
    const UWORD libs = c.ctx->num_libs;
    M68KFakeLibBase &small = c.ctx->libs[c.ctx->num_libs++];
    small = M68KFakeLibBase();
    small.jt_start = (c.record(timer)->m68k_addr + 34 + 3) & ~3u;
    small.num_vectors = 1;
    small.m68k_addr = small.jt_start + 6;
    small.pos_size = 4;
    small.library = TRUE;
    std::snprintf(small.name, sizeof(small.name), "%s", "small.library");
    const auto with_small = c.region();
    CHECK(!Kaffeine_PublishLibrary(c.ctx, small.m68k_addr, 40, 1));
    CHECK(c.region() == with_small);
    c.ctx->num_libs = libs;
}

static void kind_not_libmap()
{
    /* The library kind is in the record. A library that the full libmap no
       longer holds still gets its header, and a device still gets none. */
    Context c;
    std::vector<ULONG> bases;
    for (unsigned i = 0; i < KAFFEINE_MAX_LIBMAP + 2; ++i) {
        const std::string name = "lib" + std::to_string(i) + ".library";
        bases.push_back(c.library(name.c_str(), 32));
        CHECK(bases.back() != 0);
    }
    CHECK(c.ctx->num_libmap == KAFFEINE_MAX_LIBMAP);
    CHECK(Kaffeine_FindLibBase(c.ctx, "lib17.library") == 0);
    const ULONG timer = c.device("timer.device", 4);
    CHECK(timer != 0);
    for (unsigned i = 0; i < bases.size(); ++i)
        CHECK(Kaffeine_PublishLibrary(c.ctx, bases[i], (UWORD)(30 + i), (UWORD)i));
    const auto before = c.region();
    CHECK(!Kaffeine_PublishLibrary(c.ctx, timer, 40, 1));
    CHECK(c.region() == before);
    for (unsigned i = 0; i < bases.size(); ++i)
        check_header(c, bases[i], (UWORD)(30 + i), (UWORD)i);
    check_slots(c);
    check_extents(c);
}

/* One layout of bases after ExecBase: a kind per letter, made in this
   order, with every library published and every device refused. */
static void layout(const char *kinds)
{
    Context c;
    std::vector<ULONG> libraries, devices;
    unsigned n = 0;
    for (const char *k = kinds; *k; ++k, ++n) {
        const std::string name = "l" + std::to_string(n) + (*k == 'd' ? ".device" : ".library");
        ULONG base = 0;
        switch (*k) {
        case 'g': base = c.library(name.c_str()); break;
        case 'o': base = c.library(name.c_str(), 7); break;   /* base 2 bytes off 4 */
        case 'i': base = c.sized(name.c_str(), 80); break;
        case 'G': base = c.sized(name.c_str(), 552); break;
        case 'p': base = c.sized(name.c_str(), 35, 1); break; /* odd positive size */
        case 'd': base = c.device(name.c_str()); break;
        }
        CHECK(base != 0);
        if (!base) return;
        if (*k == 'd') devices.push_back(base);
        else libraries.push_back(base);
    }
    for (unsigned i = 0; i < libraries.size(); ++i)
        CHECK(Kaffeine_PublishLibrary(c.ctx, libraries[i], (UWORD)(33 + i), (UWORD)(100 + i)));
    for (ULONG device : devices) {
        CHECK(!Kaffeine_PublishLibrary(c.ctx, device, 40, 1));
        for (ULONG i = 0; i < 34; ++i) CHECK(c.ctx->mem[device + i] == 0);
    }
    for (unsigned i = 0; i < libraries.size(); ++i)
        check_header(c, libraries[i], (UWORD)(33 + i), (UWORD)(100 + i));
    check_slots(c);
    check_extents(c);
}

static void region_full()
{
    /* Bases until the region is full: the last one that fits ends exactly
       at the region's end, a byte more is refused, and the heap after the
       region keeps every byte. */
    Context c;
    ULONG last = 0;
    for (;;) {
        const auto before = c.region();
        const UWORD libs = c.ctx->num_libs, map = c.ctx->num_libmap;
        const ULONG base = c.library("full.library");
        if (!base) {
            CHECK(c.ctx->num_libs == libs && c.ctx->num_libmap == map && c.region() == before);
            break;
        }
        CHECK(Kaffeine_PublishLibrary(c.ctx, base, 40, 1));
        last = base;
    }
    CHECK(last != 0 && c.ctx->num_libs < KAFFEINE_MAX_LIBS);
    const ULONG start = (last + 34 + 3) & ~3u, room = region_end - start;
    CHECK(room >= 40);
    if (room < 40) return;
    const UWORD vectors = (UWORD)((room - 34) / 6);
    const UWORD positive = (UWORD)(room - vectors * 6u);
    CHECK(positive >= 34 && positive < 40);
    const auto before = c.region();
    CHECK(c.sized("over.library", positive + 1, vectors) == 0 && c.region() == before);
    const ULONG exact = c.sized("exact.library", positive, vectors);
    CHECK(exact != 0 && exact + positive == region_end);
    CHECK(Kaffeine_PublishLibrary(c.ctx, exact, 40, 1));
    check_header(c, exact, 40, 1);
    CHECK(c.sized("after.library", 34, 1) == 0);
    check_slots(c);
    check_extents(c);
}

int main()
{
    sizes();
    published_bytes();
    owner_fields();
    refusals();
    kind_not_libmap();
    for (const char *kinds : {"gigGd", "dGgig", "gdogpdiG", "pogdigGd", "ddgg", "Gpo"})
        layout(kinds);
    region_full();
    std::printf("library metadata: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

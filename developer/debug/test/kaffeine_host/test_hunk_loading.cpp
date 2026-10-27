/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    HUNK loader regression tests using the production memory manager.
    The seglist tests load an image with the DOS HUNK loader
    (rom/dos/internalloadseg_aos.c, built by dos_loadseg_aos.c), copy the
    native seglist with Kaffeine_LoadHunks() and run it. They do not run
    the rest of DOS, GetSegListInfo() or RunHunk().
*/
#include "kaffeine_intern.h"
#include <dos/doshunks.h>
#include "kaffeine_thunks.h"
#include "kaffeine_offsets.h"
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
BPTR host_load_hunks(const UBYTE *image, ULONG size, struct DosLibrary *DOSBase, LONG *error);
void host_unload_hunks(BPTR seglist);

/* Called by the DOS loader; keeps a copy as the DOS seglist node does. */
static std::vector<struct HunkRelocation> registered_relocs;
static ULONG registered_hunks;
static bool registered;

void register_hunk(BPTR, BPTR, APTR, const struct HunkRelocTable *relocs, struct DosLibrary *)
{
    registered = relocs != nullptr;
    registered_relocs.clear();
    registered_hunks = 0;
    if (relocs) {
        registered_hunks = relocs->hrt_NumHunks;
        registered_relocs.assign(relocs->hrt_Relocs,
                                 relocs->hrt_Relocs + relocs->hrt_NumRelocs);
    }
}
}

const struct M68KThunkEntry kaffeine_thunks_exec[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_exec_count = 0;
const struct M68KThunkEntry kaffeine_thunks_dos[] = { { 0, nullptr } };
const ULONG kaffeine_thunks_dos_count = 0;

extern "C" {
ULONG Kaffeine_OpenLibrary(struct KaffeineContext *, const char *) { return 0; }
ULONG Kaffeine_OpenM68KLibrary(struct KaffeineContext *, void *, const char *, ULONG) { return 0; }
BOOL Kaffeine_CloseM68KLibrary(struct KaffeineContext *, void *, ULONG) { return 0; }
}

#include "kaffeine_memory.c"
#include "kaffeine_cpu.c"
#include "kaffeine_dispatch.c"
#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#include "kaffeine_moira.cpp"

static void append32(std::vector<UBYTE> &out, ULONG value)
{
    out.push_back((UBYTE)(value >> 24));
    out.push_back((UBYTE)(value >> 16));
    out.push_back((UBYTE)(value >> 8));
    out.push_back((UBYTE)value);
}

static std::vector<UBYTE> relocation_image()
{
    std::vector<UBYTE> image;
    const ULONG header[] = { HUNK_HEADER, 0, 3, 0, 2, 5, 1, 2,
                             HUNK_CODE, 5 };
    for (ULONG word : header) append32(image, word);

    /* MOVEA.L #data,A0; MOVE.L (A0),D0; MOVEA.L #bss,A1;
       MOVE.L D0,(A1); RTS; NOP. Two absolute operands need relocation. */
    const UBYTE code[] = {
        0x20, 0x7c, 0, 0, 0, 0, 0x20, 0x10,
        0x22, 0x7c, 0, 0, 0, 0, 0x22, 0x80,
        0x4e, 0x75, 0x4e, 0x71
    };
    image.insert(image.end(), code, code + sizeof(code));
    const ULONG tail[] = {
        HUNK_RELOC32, 1, 1, 2, 1, 2, 10, 0, HUNK_END,
        HUNK_DATA, 1, 0x12345678, HUNK_END,
        HUNK_BSS, 2, HUNK_END
    };
    for (ULONG word : tail) append32(image, word);
    return image;
}

static bool raw_relocations()
{
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) return false;
    auto image = relocation_image();
    ULONG entry = Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size());
    ULONG data = entry ? m68k_read32(ctx, entry + 2) : 0;
    ULONG bss = entry ? m68k_read32(ctx, entry + 10) : 0;
    bool ok = entry && data && bss && data != bss && data != entry && bss != entry &&
              data < ctx->mem_size - 4 && bss < ctx->mem_size - 8 &&
              m68k_read32(ctx, data) == 0x12345678 &&
              m68k_read32(ctx, bss) == 0 && m68k_read32(ctx, bss + 4) == 0 &&
              m68k_read16(ctx, entry + 16) == 0x4e75;
    std::printf("HUNK CODE/DATA/BSS with two cross-segment RELOC32: %s\n",
                ok ? "PASS" : "FAIL");
    if (ok) {
        ctx->entry_point = entry;
        ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;
        LONG result = Kaffeine_Execute(ctx);
        ok = result == (LONG)0x12345678 &&
             m68k_read32(ctx, bss) == 0x12345678 &&
             m68k_read32(ctx, bss + 4) == 0 &&
             m68k_read32(ctx, data) == 0x12345678;
        std::printf("Execute relocated code, return DATA value and write BSS: %s\n",
                    ok ? "PASS" : "FAIL");
    }
    Kaffeine_DestroyContext(ctx);
    return ok;
}

/* A backend that keeps translations: what the loaders tell it. */
static std::vector<std::pair<ULONG, ULONG>> flushed;
static void record_flush(void *, ULONG addr, ULONG size) { flushed.push_back({ addr, size }); }
static const struct KaffeineCPUOps flushing_ops =
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, record_flush };

/* Each loader of 68k code tells a running backend which guest bytes it
   wrote; outside Kaffeine_RunCPU() there is no backend to tell. */
static bool loader_flushes()
{
    struct KaffeineLibBase base = {};
    struct KaffeineCPU running = { &flushing_ops, nullptr };
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) return false;
    auto image = relocation_image();

    flushed.clear();
    ULONG quiet = Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size());
    bool ok = quiet && flushed.empty() && !ctx->cpu;

    ctx->cpu = &running;
    ULONG entry = Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size());
    ULONG data = entry ? m68k_read32(ctx, entry + 2) : 0;
    ULONG bss = entry ? m68k_read32(ctx, entry + 10) : 0;
    const std::vector<std::pair<ULONG, ULONG>> hunks = { { entry, 20 }, { data, 4 }, { bss, 8 } };
    ok &= entry && entry != quiet && flushed == hunks;

    flushed.clear();
    ULONG lib = Kaffeine_SetupLibBase(ctx, ctx->num_libs, "flush.library", 10, nullptr, 0, nullptr, 0);
    const std::vector<std::pair<ULONG, ULONG>> vectors = { { lib - 60, 60 } };
    ok &= lib && flushed == vectors && m68k_read16(ctx, lib - 6) == M68K_OP_ALINE;

    flushed.clear();
    ctx->cpu = nullptr;
    ok &= Kaffeine_SetupLibBase(ctx, ctx->num_libs, "quiet.library", 10, nullptr, 0, nullptr, 0) &&
          flushed.empty();
    std::printf("Loaded hunks and vectors flushed for a running backend: %s\n",
                ok ? "PASS" : "FAIL");
    Kaffeine_DestroyContext(ctx);
    return ok;
}

static bool invalid_header()
{
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) return false;
    UBYTE bad[] = { 0, 0, 0, 0 };
    bool ok = Kaffeine_LoadHunksFromMemory(ctx, bad, sizeof(bad)) == 0;
    std::printf("Reject non-HUNK header: %s\n", ok ? "PASS" : "FAIL");
    Kaffeine_DestroyContext(ctx);
    return ok;
}

static bool raw_bounds()
{
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) return false;
    bool ok = true;

    for (ULONG count : { 0x40000000u, 0x40000001u, 0xFFFFFFFFu }) {
        std::vector<UBYTE> image;
        for (ULONG word : { (ULONG)HUNK_HEADER, count, 0u, 1u, 0u, 0u, 1u,
                            (ULONG)HUNK_CODE, 1u, 0u, (ULONG)HUNK_END })
            append32(image, word);
        ok &= Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size()) == 0;
    }
    std::printf("Raw HUNK: reject wrapping/truncated resident-name lengths: %s\n", ok ? "PASS" : "FAIL");

    for (ULONG count : { 2u, 0x40000001u, 0xFFFFFFFFu }) {
        std::vector<UBYTE> image;
        for (ULONG word : { (ULONG)HUNK_HEADER, 0u, 1u, 0u, 0u, 1u,
                            (ULONG)HUNK_CODE, count, 0u, 0u, (ULONG)HUNK_END })
            append32(image, word);
        ok &= Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size()) == 0;
    }
    std::printf("Raw HUNK: reject CODE length overflow and payload past allocation: %s\n", ok ? "PASS" : "FAIL");

    for (ULONG offset : { 1u, 4u, 0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu }) {
        std::vector<UBYTE> image;
        for (ULONG word : { (ULONG)HUNK_HEADER, 0u, 1u, 0u, 0u, 1u,
                            (ULONG)HUNK_CODE, 1u, 0u, (ULONG)HUNK_RELOC32,
                            1u, 0u, offset, 0u, (ULONG)HUNK_END })
            append32(image, word);
        ULONG entry = Kaffeine_LoadHunksFromMemory(ctx, image.data(), (LONG)image.size());
        /* Out-of-hunk relocations are skipped, without touching the header. */
        ok &= entry && m68k_read32(ctx, entry) == 0 &&
              m68k_read32(ctx, entry - 8) == 16 && m68k_read32(ctx, entry - 4) == 0;
    }
    std::printf("Raw HUNK: skip out-of-hunk and wrapping relocation offsets: %s\n", ok ? "PASS" : "FAIL");
    auto image = relocation_image();
    for (LONG size : { -1, 0, 1, 2, 3 })
        ok &= Kaffeine_LoadHunksFromMemory(ctx, image.data(), size) == 0;
    ok &= Kaffeine_LoadHunksFromMemory(ctx, nullptr, 4) == 0;
    std::printf("Raw HUNK: reject negative/short sizes and NULL input: %s\n", ok ? "PASS" : "FAIL");
    Kaffeine_DestroyContext(ctx);
    return ok;
}

static void append16(std::vector<UBYTE> &out, UWORD value)
{
    out.push_back((UBYTE)(value >> 8));
    out.push_back((UBYTE)value);
}

/* Same program as relocation_image(), but the BSS operand uses
   HUNK_RELOC32SHORT and a trailing longword holds a HUNK_RELRELOC32
   (DATA - CODE + 4). AROS reads HUNK_RELRELOC32 in the short format. */
static const ULONG seglist_relative_addend = 4;

static std::vector<UBYTE> seglist_image()
{
    std::vector<UBYTE> image;
    const ULONG header[] = { HUNK_HEADER, 0, 3, 0, 2, 6, 1, 2,
                             HUNK_CODE, 6 };
    for (ULONG word : header) append32(image, word);
    const UBYTE code[] = {
        0x20, 0x7c, 0, 0, 0, 0, 0x20, 0x10,
        0x22, 0x7c, 0, 0, 0, 0, 0x22, 0x80,
        0x4e, 0x75, 0x4e, 0x71,
        0, 0, 0, (UBYTE)seglist_relative_addend
    };
    image.insert(image.end(), code, code + sizeof(code));
    append32(image, HUNK_RELOC32);
    for (ULONG word : { 1, 1, 2, 0 }) append32(image, word);
    append32(image, HUNK_RELOC32SHORT);
    for (UWORD word : { 1, 2, 10, 0 }) append16(image, word);
    append32(image, HUNK_RELRELOC32);
    for (UWORD word : { 1, 1, 20, 0 }) append16(image, word);
    const ULONG tail[] = {
        HUNK_END,
        HUNK_DATA, 1, 0x12345678, HUNK_END,
        HUNK_BSS, 2, HUNK_END
    };
    for (ULONG word : tail) append32(image, word);
    return image;
}

static ULONG native_read32(BPTR seg, ULONG offset)
{
    const UBYTE *p = (const UBYTE *)BADDR(seg) + sizeof(BPTR) + offset;
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static bool check(const char *name, bool ok)
{
    std::printf("%-58s %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

static bool dos_seglist()
{
    struct DosLibrary *dos = (struct DosLibrary *)&registered_hunks; /* only non-NULL */
    auto image = seglist_image();
    LONG error = 0;
    bool all = true;

    registered = false;
    BPTR segs = host_load_hunks(image.data(), (ULONG)image.size(), dos, &error);
    BPTR seg_data = segs ? *(BPTR *)BADDR(segs) : nullptr;
    ULONG native_code = segs ? (ULONG)(IPTR)((UBYTE *)BADDR(segs) + sizeof(BPTR)) : 0;
    ULONG native_data = seg_data ? (ULONG)(IPTR)((UBYTE *)BADDR(seg_data) + sizeof(BPTR)) : 0;
    all &= check("DOS loader: RELOC32, RELOC32SHORT and RELRELOC32 applied",
                 segs && error == 0 &&
                 native_read32(segs, 2) == native_data &&
                 native_read32(segs, 20) == native_data - native_code + seglist_relative_addend);
    if (!segs)
        return false;

    bool table_ok = registered && registered_hunks == 3 && registered_relocs.size() == 3;
    if (table_ok) {
        const struct HunkRelocation expect[] = {
            { 2, 0, 1, HRT_ABSOLUTE, 0 },
            { 10, 0, 2, HRT_ABSOLUTE, 0 },
            { 20, 0, 1, HRT_RELATIVE, 0 },
        };
        for (int i = 0; i < 3; i++) {
            const auto &r = registered_relocs[i];
            table_ok &= r.hr_Offset == expect[i].hr_Offset &&
                        r.hr_Source == expect[i].hr_Source &&
                        r.hr_Target == expect[i].hr_Target &&
                        r.hr_Type == expect[i].hr_Type;
        }
    }
    all &= check("DOS loader: relocation record registered with the seglist", table_ok);

    struct HunkRelocTable table = { registered_hunks, (ULONG)registered_relocs.size(),
                                    registered_relocs.data() };
    struct KaffeineLibBase base = {};
    auto *ctx = Kaffeine_CreateContext(&base);
    if (!ctx) {
        host_unload_hunks(segs);
        return false;
    }

    all &= check("Copy without a relocation record is refused",
                 Kaffeine_LoadHunks(ctx, segs, nullptr) == 0);
    struct HunkRelocTable short_table = table;
    short_table.hrt_NumHunks = 2;
    all &= check("Copy with a different hunk count is refused",
                 Kaffeine_LoadHunks(ctx, segs, &short_table) == 0);
    struct HunkRelocation bad = registered_relocs.empty() ? HunkRelocation{} : registered_relocs[0];
    bad.hr_Offset = 6 * 4 - 2;
    struct HunkRelocTable bad_table = { registered_hunks, 1, &bad };
    all &= check("Copy with a relocation past the hunk end is refused",
                 Kaffeine_LoadHunks(ctx, segs, &bad_table) == 0);

    struct KaffeineCPU running = { &flushing_ops, nullptr };
    flushed.clear();
    ctx->cpu = &running;
    ULONG entry = Kaffeine_LoadHunks(ctx, segs, &table);
    ctx->cpu = nullptr;
    ULONG data = entry ? m68k_read32(ctx, entry + 2) : 0;
    ULONG bss = entry ? m68k_read32(ctx, entry + 10) : 0;
    ULONG rel = entry ? m68k_read32(ctx, entry + 20) : 0;
    std::vector<std::pair<ULONG, ULONG>> copies;
    ULONG guest_addr[3] = { entry, data, bss }, n = 0;
    /* [ULONG size][BPTR next]: on a 64-bit host next is not 8-byte aligned. */
    for (BPTR seg = segs; seg && n < 3; n++)
    {
        ULONG alloc_size;
        std::memcpy(&alloc_size, (ULONG *)BADDR(seg) - 1, sizeof(alloc_size));
        copies.push_back({ guest_addr[n], alloc_size - (ULONG)(sizeof(ULONG) + sizeof(BPTR)) });
        std::memcpy(&seg, BADDR(seg), sizeof(seg));
    }
    all &= check("Copied hunks flushed for a running backend", entry && flushed == copies);
    bool rebased = entry && data && bss && data != bss &&
                   data < ctx->mem_size - 4 && bss < ctx->mem_size - 8 &&
                   m68k_read32(ctx, data) == 0x12345678 &&
                   m68k_read32(ctx, bss) == 0 && m68k_read32(ctx, bss + 4) == 0 &&
                   rel == data - entry + seglist_relative_addend &&
                   native_read32(segs, 2) == native_data;
    all &= check("Copied seglist rebased into the guest arena", rebased);

    if (rebased) {
        ctx->entry_point = entry;
        ctx->m68k_argptr = ctx->m68k_argsize = ctx->m68k_sysbase = 0;
        LONG result = Kaffeine_Execute(ctx);
        all &= check("Execute copied seglist, return DATA value and write BSS",
                     result == (LONG)0x12345678 &&
                     m68k_read32(ctx, bss) == 0x12345678 &&
                     m68k_read32(ctx, bss + 4) == 0);
    } else {
        all = false;
    }

    Kaffeine_DestroyContext(ctx);
    host_unload_hunks(segs);
    return all;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--raw-bounds") == 0)
        return raw_bounds() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--diagnose-seglist") == 0)
        return dos_seglist() ? 0 : 1;
    if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--diagnose-seglist|--raw-bounds]\n", argv[0]);
        return 2;
    }
    bool relocations_ok = raw_relocations();
    bool header_ok = invalid_header();
    bool bounds_ok = raw_bounds();
    bool flush_ok = loader_flushes();
    bool seglist_ok = dos_seglist();
    return relocations_ok && header_ok && bounds_ok && flush_ok && seglist_ok ? 0 : 1;
}

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Disk library loader integration tests. Native seglists use the host
    BPTR ABI, guest structures use literal big-endian 68k layouts, and all
    init/Open/Close routines execute as 68k instructions through Moira.
    No fake CallGuest or replacement loader/allocator is used.
*/
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <dos/doshunks.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_callback.h"
#include "kaffeine_thunks.h"

extern "C" KaffeineCPU *m68klib_test_create_cpu(KaffeineContext *);
extern "C" void m68klib_test_delete_cpu(KaffeineCPU *);

static unsigned checks, failures, cases;
static const char *case_name;
struct Failed {};
#define CHECK(c) do { ++checks; if (!(c)) { \
    std::printf("FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, case_name, #c); \
    ++failures; throw Failed{}; } } while (0)
static std::map<APTR, ULONG> native_allocations;
extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    APTR p = std::malloc(size);
    if (p) {
        std::memset(p, flags & MEMF_CLEAR ? 0 : 0xA5, size);
        native_allocations.emplace(p, size);
    }
    return p;
}
extern "C" void FreeMem(APTR p, ULONG size)
{
    auto it = native_allocations.find(p);
    if (it == native_allocations.end() || it->second != size) std::abort();
    native_allocations.erase(it);
    std::free(p);
}

/* Stable low memory is outside all library hunks and guest heap blocks. */
static constexpr ULONG ENTRY = 0x3000, NAME = 0x3800, SNAP = 0x4000;
static constexpr ULONG AFTER = 0x4300, ALTERNATE = 0x6000;
static constexpr ULONG INIT = 0x40, OPEN = 0x100, CLOSE = 0x200;
static constexpr ULONG EXTRA = 0x300, TABLE = 0x400;
static constexpr ULONG RESIDENT = 0x40, AUTOINIT = 0x80, DATAINIT = 0x100;
static constexpr ULONG POS = 128, VERSION = 7;

static ULONG be32(const UBYTE *p)
{ return (ULONG(p[0]) << 24) | (ULONG(p[1]) << 16) | (ULONG(p[2]) << 8) | p[3]; }
static void put32(UBYTE *p, ULONG v)
{ p[0] = UBYTE(v >> 24); p[1] = UBYTE(v >> 16); p[2] = UBYTE(v >> 8); p[3] = UBYTE(v); }
static void put16(UBYTE *p, UWORD v)
{ p[0] = UBYTE(v >> 8); p[1] = UBYTE(v); }

/* Encodes bytes, rather than sharing the production InitStruct decoder.
   Padding is poison so missing source/command alignment changes the result. */
struct InitTable
{
    std::vector<UBYTE> bytes;
    void byte(UBYTE b) { bytes.push_back(b); }
    void align() { if (bytes.size() & 1) byte(0xED); }
    void value(unsigned width, ULONG v)
    { for (unsigned n = width; n; --n) byte(UBYTE(v >> (8 * (n - 1)))); }
    void command(unsigned kind, unsigned size, unsigned count,
                 ULONG offset, const std::vector<ULONG> &values)
    {
        align();
        byte(UBYTE((kind << 6) | (size << 4) | (count - 1)));
        if (kind == 2) byte(UBYTE(offset));
        if (kind == 3) value(3, offset);
        const unsigned width = size == 0 ? 4 : size == 1 ? 2 : 1;
        if (width > 1) align();
        for (ULONG v : values) value(width, v);
        align();
    }
    void end() { align(); byte(0); }
};

struct Image
{
    std::vector<std::vector<UBYTE>> h = {
        std::vector<UBYTE>(0x1800, 0), std::vector<UBYTE>(0x7000, 0)
    };
    std::vector<HunkRelocation> relocs;
    bool word_table = false;
    unsigned vectors = 3;
    ULONG positive_size = POS;
    ULONG snapshot;

    void w(unsigned i, ULONG at, UWORD v) { put16(h[i].data() + at, v); }
    void l(unsigned i, ULONG at, ULONG v) { put32(h[i].data() + at, v); }
    ULONG read(unsigned i, ULONG at) const { return be32(h[i].data() + at); }
    void remove_reloc(unsigned i, ULONG at)
    {
        std::erase_if(relocs, [=](const auto &r) { return r.hr_Source == i && r.hr_Offset == at; });
    }
    void raw(unsigned i, ULONG at, ULONG v) { remove_reloc(i, at); l(i, at, v); }
    void ptr(unsigned i, ULONG at, unsigned target, ULONG offset, UWORD type = HRT_ABSOLUTE)
    {
        raw(i, at, offset);
        relocs.push_back({at, UWORD(i), UWORD(target), type, 0});
    }
    void code(ULONG at, std::initializer_list<UWORD> words)
    { for (UWORD v : words) { w(0, at, v); at += 2; } }
    void emit_w(ULONG &pc, UWORD v) { w(0, pc, v); pc += 2; }
    void emit_l(ULONG &pc, ULONG v) { l(0, pc, v); pc += 4; }
    void save(ULONG &pc, UWORD opcode, ULONG dest)
    { emit_w(pc, opcode); emit_l(pc, dest); }
    void routine(ULONG at, unsigned counter, unsigned d0, unsigned a0, unsigned a6)
    {
        ULONG pc = at;
        save(pc, 0x52B9, snapshot + counter); /* ADDQ.L #1,abs.l */
        if (d0) save(pc, 0x23C0, snapshot + d0); /* MOVE.L D0,abs.l */
        if (a0) save(pc, 0x23C8, snapshot + a0); /* MOVE.L A0,abs.l */
        if (a6) save(pc, 0x23CE, snapshot + a6); /* MOVE.L A6,abs.l */
        emit_w(pc, 0x4E75);
    }
    void functions(bool words, unsigned count = 3)
    {
        word_table = words; vectors = count;
        std::erase_if(relocs, [](const auto &r) {
            return r.hr_Source == 0 && r.hr_Offset >= TABLE;
        });
        const ULONG fns[] = {OPEN, CLOSE, EXTRA};
        if (words) w(0, TABLE, 0xFFFF);
        for (unsigned i = 0; i < count; ++i) {
            ULONG fn = fns[i < 3 ? i : 2];
            if (words) w(0, TABLE + 2 + 2 * i, UWORD(fn - TABLE));
            else ptr(0, TABLE + 4 * i, 0, fn);
        }
        if (words) w(0, TABLE + 2 + 2 * count, 0xFFFF);
        else raw(0, TABLE + 4 * count, 0xFFFFFFFF);
    }
    void data(const std::vector<UBYTE> &bytes)
    {
        std::fill(h[1].begin() + DATAINIT, h[1].end(), 0);
        std::copy(bytes.begin(), bytes.end(), h[1].begin() + DATAINIT);
    }
    void no_init() { raw(1, AUTOINIT + 12, 0); }
    void no_data() { raw(1, AUTOINIT + 8, 0); }
    void return_init(ULONG result)
    {
        /* Preserve the snapshots, replace its final RTS. */
        ULONG pc = INIT + 24;
        emit_w(pc, 0x203C); emit_l(pc, result); emit_w(pc, 0x4E75);
    }
    void return_open(ULONG result)
    {
        ULONG pc = OPEN + 18;
        emit_w(pc, 0x203C); emit_l(pc, result); emit_w(pc, 0x4E75);
    }
    void stop(ULONG at, ULONG sysbase, UBYTE status)
    {
        ULONG pc = at;
        emit_w(pc, UWORD(0x7000 | status)); /* MOVEQ #status,D0 */
        emit_w(pc, 0x2C7C); emit_l(pc, sysbase);
        emit_w(pc, 0x4EAE); emit_w(pc, UWORD(-60));
        emit_w(pc, 0x7063); emit_w(pc, 0x4E75); /* must never return 99 */
    }
    void expunge(ULONG sysbase, ULONG pc = CLOSE + 12)
    {
        /* Leave the counter and A6 snapshot when replacing Close(). */
        emit_w(pc, 0x224E); /* MOVEA.L A6,A1 */
        emit_w(pc, 0x93FC); emit_l(pc, (vectors * 6 + 3) & ~3u); /* SUBA.L #neg,A1 */
        emit_w(pc, 0x203C); emit_l(pc, ((vectors * 6 + 3) & ~3u) + positive_size);
        emit_w(pc, 0x2C7C); emit_l(pc, sysbase);
        emit_w(pc, 0x4EAE); emit_w(pc, UWORD(-210)); /* real Exec FreeMem thunk */
        emit_w(pc, 0x2039); emit_l(pc, snapshot + 8); /* return seglist saved by init */
        emit_w(pc, 0x4E75);
    }
    explicit Image(ULONG snapshot = SNAP) : snapshot(snapshot)
    {
        /* An absolute self-reference and relative relocations both ways. */
        ptr(0, 0x10, 1, 0x50, HRT_RELATIVE);
        ptr(0, 0x18, 0, 0x32);
        ptr(1, 0x20, 0, 0x30, HRT_RELATIVE);
        w(1, RESIDENT, 0x4AFC);
        ptr(1, RESIDENT + 2, 1, RESIDENT);
        ptr(1, RESIDENT + 6, 1, AUTOINIT);
        h[1][RESIDENT + 10] = 0x80; /* RTF_AUTOINIT */
        h[1][RESIDENT + 11] = VERSION;
        h[1][RESIDENT + 12] = 9; /* NT_LIBRARY */
        ptr(1, RESIDENT + 14, 1, 0x70);
        ptr(1, RESIDENT + 18, 1, 0x70);
        ptr(1, RESIDENT + 22, 1, AUTOINIT);
        std::memcpy(h[1].data() + 0x70, "tiny.library", 13);
        l(1, AUTOINIT, POS);
        ptr(1, AUTOINIT + 4, 0, TABLE);
        ptr(1, AUTOINIT + 8, 1, DATAINIT);
        ptr(1, AUTOINIT + 12, 0, INIT);
        data({0x90, 20, 0, VERSION, 0}); /* INITWORD(lib_Version,7) */
        routine(INIT, 0, 4, 8, 12);
        routine(OPEN, 16, 20, 0, 24);
        code(OPEN + 18, {0x200E, 0x4E75}); /* MOVE.L A6,D0; RTS */
        routine(CLOSE, 28, 0, 0, 32);
        code(CLOSE + 12, {0x7000, 0x4E75});
        routine(EXTRA, 36, 40, 0, 0);
        code(EXTRA + 12, {0x705B, 0x4E75});
        functions(false);
    }
};

/* Native DOS memory has a native-endian ULONG allocation size, a native
   BPTR next link, then already relocated big-endian 68k bytes. Host BPTRs
   are pointer-sized, as in the other seglist tests. */
struct NativeSegments
{
    std::vector<UBYTE *> blocks, payload;
    std::vector<BPTR> segs;
    std::vector<HunkRelocation> records;
    HunkRelocTable table = {};
    explicit NativeSegments(const Image &image) : blocks(image.h.size()),
        payload(image.h.size()), segs(image.h.size()), records(image.relocs)
    {
        for (unsigned i = 0; i < image.h.size(); ++i) {
            blocks[i] = static_cast<UBYTE *>(std::calloc(1, image.h[i].size() + 16));
            segs[i] = MKBADDR(blocks[i] + 8);
            payload[i] = blocks[i] + 8 + sizeof(BPTR);
            ULONG size = ULONG(image.h[i].size() + sizeof(ULONG) + sizeof(BPTR));
            std::memcpy(blocks[i] + 4, &size, sizeof(size));
            std::memcpy(payload[i], image.h[i].data(), image.h[i].size());
        }
        for (unsigned i = 0; i + 1 < segs.size(); ++i)
            std::memcpy(BADDR(segs[i]), &segs[i + 1], sizeof(BPTR));
        for (const auto &r : records) {
            if (r.hr_Source >= image.h.size() || r.hr_Target >= image.h.size() ||
                r.hr_Offset + 4 > image.h[r.hr_Source].size())
                continue; /* malformed records are passed unchanged to the copier */
            ULONG v = image.read(r.hr_Source, r.hr_Offset) + ULONG(IPTR(payload[r.hr_Target]));
            if (r.hr_Type == HRT_RELATIVE) v -= ULONG(IPTR(payload[r.hr_Source]));
            put32(payload[r.hr_Source] + r.hr_Offset, v);
        }
        table = {ULONG(segs.size()), ULONG(records.size()), records.data()};
    }
    ~NativeSegments() { for (UBYTE *p : blocks) std::free(p); }
};

static Image *disk_image;
static std::map<std::string, const Image *> disk_images;
static std::unique_ptr<NativeSegments> native_segments;
static Process process;
static std::vector<std::string> attempts;
static std::string accepted_path;
static unsigned loads, unloads, infos;
static bool info_success, have_record, requester_ok, stub_ok;
static unsigned native_lookups;
static std::vector<BPTR> unload_calls;
static bool allow_foreign_unloads;
static SIPTR io_error;
struct RunCommandCall { BPTR seglist; ULONG stacksize; STRPTR args; ULONG size; };
static std::vector<RunCommandCall> run_commands;

extern "C" Task *FindTask(CONST_STRPTR name)
{ if (name) stub_ok = false; return &process.pr_Task; }
extern "C" BPTR LoadSeg(CONST_STRPTR path)
{
    attempts.emplace_back(path);
    if (process.pr_Task.tc_Node.ln_Type == NT_PROCESS && process.pr_WindowPtr != APTR(-1))
        requester_ok = false;
    if (!accepted_path.empty() && accepted_path != path) return BNULL;
    const Image *image = disk_image;
    if (!disk_images.empty()) {
        std::string file(path);
        file.erase(0, file.find_last_of(":/") + 1);
        auto it = disk_images.find(file);
        if (it == disk_images.end()) return BNULL;
        image = it->second;
    }
    if (native_segments) { stub_ok = false; return BNULL; }
    native_segments = std::make_unique<NativeSegments>(*image);
    ++loads;
    return native_segments->segs[0];
}
extern "C" void UnLoadSeg(BPTR seg)
{
    unload_calls.push_back(seg);
    if (!native_segments || seg != native_segments->segs[0]) {
        if (!allow_foreign_unloads) stub_ok = false;
        return;
    }
    ++unloads;
    native_segments.reset(); /* no native payload can be used after this */
}
extern "C" LONG GetSegListInfo(BPTR seg, const TagItem *tags)
{
    ++infos;
    if (!native_segments || seg != native_segments->segs[0] ||
        tags[0].ti_Tag != GSLI_68KRelocs || tags[1].ti_Tag != TAG_DONE) {
        stub_ok = false; return 0;
    }
    if (!info_success) return 0;
    *reinterpret_cast<IPTR *>(tags[0].ti_Data) = have_record ? IPTR(&native_segments->table) : 0;
    return 1;
}

extern "C" SIPTR IoErr(void) { return io_error; }
extern "C" SIPTR SetIoErr(SIPTR error)
{ SIPTR previous = io_error; io_error = error; return previous; }
extern "C" LONG RunCommand(BPTR seglist, ULONG stacksize, STRPTR args, ULONG size)
{ run_commands.push_back({seglist, stacksize, args, size}); return 73; }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *ctx, const char *name)
{ ++native_lookups; return std::strcmp(name, "exec.library") == 0 ? ctx->m68k_sysbase : 0; }
extern "C" IPTR m68klib_test_stop(KaffeineContext *ctx, void *cpu)
{
    LONG status = LONG(Kaffeine_GetD(cpu, 0));
    Kaffeine_StopGuest(ctx, cpu, status);
    return IPTR(status);
}

struct Guest
{
    Image image;
    KaffeineLibBase emu = {};
    KaffeineContext *ctx;
    KaffeineCPU *cpu;
    ULONG initial_avail, pc = ENTRY, dosbase = 0;
    size_t initial_records;
    Guest()
    {
        disk_image = &image;
        disk_images.clear();
        attempts.clear(); accepted_path.clear();
        unload_calls.clear(); run_commands.clear();
        allow_foreign_unloads = false; native_lookups = 0; io_error = 0;
        loads = unloads = infos = 0;
        info_success = have_record = requester_ok = stub_ok = true;
        process = {}; process.pr_Task.tc_Node.ln_Type = NT_PROCESS;
        process.pr_WindowPtr = APTR(IPTR(0x12345678));
        ctx = Kaffeine_CreateContext(&emu);
        if (!ctx) std::abort();
        ctx->entry_point = ENTRY; ctx->seg_start = 0x2468;
        ctx->m68k_sysbase = Kaffeine_SetupLibBase(ctx, 0, "exec.library", 100,
            kaffeine_thunks_exec, kaffeine_thunks_exec_count, nullptr, 0);
        /* Dirty future library storage, so calloc of the arena cannot hide
           removal of MEMF_CLEAR from MakeLibrary. */
        ULONG dirt = Kaffeine_HeapAlloc(ctx, 0x10000, 0);
        std::memset(ctx->mem + dirt, 0xA5, 0x10000);
        Kaffeine_HeapFree(ctx, dirt, 0x10000);
        std::memset(ctx->mem + SNAP, 0, 512);
        ctx->mem[AFTER] = 0xA5;
        text(NAME, "tiny.library");
        cpu = m68klib_test_create_cpu(ctx);
        initial_avail = avail(); initial_records = native_allocations.size();
    }
    ~Guest()
    {
        m68klib_test_delete_cpu(cpu);
        Kaffeine_DestroyContext(ctx);
        native_segments.reset();
        disk_image = nullptr;
    }
    ULONG read(ULONG addr) const { return m68k_read32(ctx, addr); }
    UWORD word(ULONG addr) const { return m68k_read16(ctx, addr); }
    ULONG snap(unsigned offset) const { return read(SNAP + offset); }
    ULONG avail() const { return Kaffeine_HeapAvail(ctx, 0); }
    ULONG open(const char *name = "tiny.library", ULONG version = VERSION)
    { return Kaffeine_OpenM68KLibrary(ctx, cpu, name, version); }
    bool close(ULONG base) { return Kaffeine_CloseM68KLibrary(ctx, cpu, base); }
    void text(ULONG at, const char *str) { std::strcpy(reinterpret_cast<char *>(ctx->mem + at), str); }
    void w(UWORD v) { m68k_write16(ctx, pc, v); pc += 2; }
    void l(ULONG v) { m68k_write32(ctx, pc, v); pc += 4; }
    void a(unsigned reg, ULONG v) { w(UWORD(0x207C + (reg << 9))); l(v); }
    void d(ULONG v) { w(0x203C); l(v); }
    void jsr(UWORD lvo) { a(6, ctx->m68k_sysbase); w(0x4EAE); w(UWORD(-lvo)); }
    void setup_dos()
    {
        dosbase = Kaffeine_SetupLibBase(ctx, ctx->num_libs, "dos.library", 100,
            kaffeine_thunks_dos, kaffeine_thunks_dos_count, nullptr, 0);
        CHECK(dosbase);
    }
    void dos_jsr(UWORD lvo) { a(6, dosbase); w(0x4EAE); w(UWORD(-lvo)); }
    ULONG call()
    {
        w(0x4E75);
        KaffeineGuestCall result = {};
        CHECK(Kaffeine_CallGuest(ctx, cpu, ENTRY, &result));
        pc = ENTRY;
        return result.d[0];
    }
    ULONG thunk_open(const char *name = "tiny.library", ULONG version = VERSION)
    { text(NAME, name); a(1, NAME); d(version); jsr(552); return call(); }
    void thunk_close(ULONG base) { a(1, base); jsr(414); CHECK(call() == 0); }
    LONG run() { w(0x4E75); return Kaffeine_Execute(ctx); }
    void after() { w(0x13FC); w(0x003C); l(AFTER); }
    std::array<ULONG, 2> hunks() const
    {
        ULONG seg = ctx->m68klibs[0].seglist;
        return {(seg << 2) + 4, (read(seg << 2) << 2) + 4};
    }
    void unchanged()
    {
        CHECK(ctx->entry_point == ENTRY && ctx->seg_start == 0x2468);
        CHECK(stub_ok && requester_ok && !native_segments);
        CHECK(process.pr_WindowPtr == APTR(IPTR(0x12345678)));
        CHECK(loads == unloads);
    }
    void refused()
    {
        CHECK(open() == 0);
        CHECK(ctx->num_m68klibs == 0 && snap(0) == 0 && snap(16) == 0);
        CHECK(avail() == initial_avail);
        CHECK(native_allocations.size() == initial_records);
        CHECK(ctx->running && !ctx->callback_failed);
        unchanged();
    }
};

static ULONG allocation_cost(ULONG bytes) { return ((bytes + 7) & ~7u) + 8; }

static void seglist_copy()
{
    Guest g;
    NativeSegments native(g.image);
    ULONG seg = Kaffeine_LoadLibraryHunks(g.ctx, native.segs[0], &native.table);
    CHECK(seg && seg < g.ctx->mem_size / 4);
    std::array<ULONG, 2> guest;
    ULONG cursor = seg;
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(cursor && cursor < g.ctx->mem_size / 4);
        guest[i] = (cursor << 2) + 4;
        CHECK((guest[i] & 3) == 0);
        CHECK(g.read(guest[i] - 8) == g.image.h[i].size() + 8);
        cursor = g.read(guest[i] - 4);
    }
    CHECK(cursor == 0);
    for (const auto &r : g.image.relocs) {
        ULONG want = g.image.read(r.hr_Source, r.hr_Offset) + guest[r.hr_Target];
        if (r.hr_Type == HRT_RELATIVE) want -= guest[r.hr_Source];
        CHECK(g.read(guest[r.hr_Source] + r.hr_Offset) == want);
    }
    auto expected = g.image.h;
    for (const auto &r : g.image.relocs) {
        ULONG v = g.image.read(r.hr_Source, r.hr_Offset) + guest[r.hr_Target];
        if (r.hr_Type == HRT_RELATIVE) v -= guest[r.hr_Source];
        put32(expected[r.hr_Source].data() + r.hr_Offset, v);
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(std::equal(expected[i].begin(), expected[i].end(), g.ctx->mem + guest[i]));
    CHECK(g.ctx->entry_point == ENTRY && g.ctx->seg_start == 0x2468);
    for (unsigned i = 0; i < 2; ++i)
        Kaffeine_HeapFree(g.ctx, guest[i] - 8, ULONG(g.image.h[i].size()) + 8);
    CHECK(g.avail() == g.initial_avail);
    CHECK(native_allocations.size() == g.initial_records);
}

static void copy_refusal(unsigned mode)
{
    Guest g;
    if (mode >= 3) g.image.relocs.push_back({mode == 5 ? 0x17FDu : 0u,
        UWORD(mode == 3 ? 2 : 0), UWORD(mode == 4 ? 2 : 0), UWORD(mode == 6 ? 2 : 0), 0});
    NativeSegments native(g.image);
    if (mode == 2) native.table.hrt_NumHunks = 1;
    CHECK(Kaffeine_LoadLibraryHunks(g.ctx, mode == 1 ? BNULL : native.segs[0],
        mode == 0 ? nullptr : &native.table) == 0);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

/* What InitResident() copies from the resident tag of the image after
   MakeLibrary() and before the init: ln_Type, ln_Name, lib_Flags
   (LIBF_SUMUSED | LIBF_CHANGED), lib_Version and lib_IdString. */
static void resident_fields(std::vector<UBYTE> &area, ULONG name)
{
    area[8] = 9;
    put32(area.data() + 10, name);
    area[14] = 0x06;
    put16(area.data() + 20, VERSION);
    put32(area.data() + 24, name);
}

static void library_layout(bool words, unsigned count)
{
    Guest g; g.image.functions(words, count);
    ULONG base = g.open();
    CHECK(base && base == g.snap(4));
    CHECK(g.ctx->num_m68klibs == 1 && g.ctx->m68klibs[0].base == base);
    CHECK(std::string(g.ctx->m68klibs[0].name) == "tiny.library");
    CHECK(g.snap(0) == 1 && g.snap(8) == g.ctx->m68klibs[0].seglist);
    CHECK(g.snap(12) == g.ctx->m68k_sysbase);
    CHECK(g.snap(16) == 1 && g.snap(20) == VERSION && g.snap(24) == base);
    const ULONG neg = (count * 6 + 3) & ~3u;
    CHECK(g.word(base + 16) == neg && g.word(base + 18) == POS);
    CHECK((base & 3) == 0 && (base - neg) % 8 == 0);
    auto hunks = g.hunks();
    const ULONG fns[] = {OPEN, CLOSE, EXTRA};
    for (unsigned i = 0; i < count; ++i) {
        CHECK(g.word(base - 6 * (i + 1)) == 0x4EF9);
        CHECK(g.read(base - 6 * (i + 1) + 2) == hunks[0] + fns[i < 3 ? i : 2]);
    }
    /* Check the entire positive area, untouched but for the sizes and the
       fields of the resident tag, and the negative padding. */
    std::vector<UBYTE> positive(POS, 0);
    resident_fields(positive, hunks[1] + 0x70);
    put16(positive.data() + 16, UWORD(neg)); put16(positive.data() + 18, UWORD(POS));
    for (ULONG i = 0; i < POS; ++i) CHECK(g.ctx->mem[base + i] == positive[i]);
    for (ULONG i = count * 6; i < neg; ++i) CHECK(g.ctx->mem[base - i - 1] == 0);
    CHECK(g.initial_avail - g.avail() == allocation_cost(ULONG(g.image.h[0].size()) + 8) +
        allocation_cost(ULONG(g.image.h[1].size()) + 8) + allocation_cost(neg + POS));
    CHECK(native_allocations.size() == g.initial_records + 3);
    if (count >= 3) {
        KaffeineGuestCall call = {};
        call.d[0] = 0x13579BDF; call.d_mask = 1;
        CHECK(Kaffeine_CallGuest(g.ctx, g.cpu, base - 18, &call));
        CHECK(call.d[0] == 91 && g.snap(36) == 1 && g.snap(40) == 0x13579BDF);
    }
    CHECK(g.close(base));
    CHECK(g.snap(28) == 1 && g.snap(32) == base && g.ctx->num_m68klibs == 1);
    g.unchanged();
}

static void init_commands(unsigned kind, unsigned size)
{
    Guest g;
    InitTable it;
    const ULONG positive = kind == 3 ? 512 : POS;
    g.image.l(1, AUTOINIT, positive);
    std::vector<UBYTE> expected(positive, 0);
    /* A byte makes the implicit destination odd; the next WORD/LONG
       must advance to an even address, never to a four-byte boundary. */
    it.command(2, 2, 1, 40, {0xA7}); expected[40] = 0xA7;
    unsigned width = size == 0 ? 4 : size == 1 ? 2 : 1;
    ULONG dest = kind < 2 ? (width > 1 ? 42 : 41) : kind == 2 ? 66 : 322;
    std::vector<ULONG> values = kind == 1 ? std::vector<ULONG>{0x12345678} :
        std::vector<ULONG>{0x12345678, 0x89ABCDEF, 0x31415926};
    it.command(kind, size, 3, dest, values);
    for (unsigned i = 0; i < 3; ++i) {
        ULONG v = values[kind == 1 ? 0 : i];
        for (unsigned n = 0; n < width; ++n)
            expected[dest + width * i + n] = UBYTE(v >> (8 * (width - n - 1)));
    }
    /* Following command must be even after an odd number of byte values. */
    it.command(2, 2, 1, 100, {0xBC}); expected[100] = 0xBC;
    it.command(2, 1, 1, 20, {VERSION});
    it.end(); g.image.data(it.bytes);
    ULONG base = g.open(); CHECK(base);
    resident_fields(expected, g.hunks()[1] + 0x70);
    put16(expected.data() + 16, 20); put16(expected.data() + 18, UWORD(positive));
    for (unsigned i = 0; i < positive; ++i) CHECK(g.ctx->mem[base + i] == expected[i]);
    CHECK(g.snap(0) == 1 && g.snap(16) == 1);
    g.unchanged();
}

static void paths(unsigned which)
{
    Guest g;
    const std::array<const char *, 5> names = {"tiny.library", "tiny.library", "tiny.library",
        "PROGDIR:libs/tiny.library", "libs/tiny.library"};
    const std::array<const char *, 5> paths = {"LIBS68K:tiny.library", "LIBS:m68k/tiny.library",
        "LIBS:tiny.library", "PROGDIR:libs/tiny.library", "libs/tiny.library"};
    accepted_path = paths[which];
    CHECK(g.open(names[which]));
    if (which < 3) {
        CHECK(attempts.size() == which + 1);
        for (unsigned i = 0; i <= which; ++i) CHECK(attempts[i] == paths[i]);
    } else CHECK(attempts == std::vector<std::string>{paths[which]});
    CHECK(loads == 1 && infos == 1 && unloads == 1);
    g.unchanged();
}

static void decoy(bool real_resident)
{
    Guest g;
    /* Reserve space after the hunks, where a containment-wide scan would
       find a valid-looking resident even if none exists in the seglist. */
    ULONG outside = Kaffeine_HeapAlloc(g.ctx, 256, MEMF_CLEAR);
    m68k_write16(g.ctx, outside, 0x4AFC);
    m68k_write32(g.ctx, outside + 2, outside);
    g.ctx->mem[outside + 10] = 0x80;
    g.ctx->mem[outside + 12] = 9;
    m68k_write32(g.ctx, outside + 22, outside + 32);
    m68k_write32(g.ctx, outside + 32, POS);
    m68k_write32(g.ctx, outside + 36, outside + 48);
    m68k_write32(g.ctx, outside + 40, outside + 96);
    m68k_write32(g.ctx, outside + 48, outside + 80);
    m68k_write32(g.ctx, outside + 52, outside + 84);
    m68k_write32(g.ctx, outside + 56, 0xFFFFFFFF);
    m68k_write16(g.ctx, outside + 80, 0x200E); /* MOVE.L A6,D0; RTS */
    m68k_write16(g.ctx, outside + 82, 0x4E75);
    m68k_write16(g.ctx, outside + 84, 0x7000);
    m68k_write16(g.ctx, outside + 86, 0x4E75);
    const UBYTE version_init[] = {0x90, 20, 0, VERSION, 0};
    std::memcpy(g.ctx->mem + outside + 96, version_init, sizeof(version_init));
    if (!real_resident) g.image.w(1, RESIDENT, 0);
    ULONG before = g.avail();
    ULONG base = g.open();
    CHECK(real_resident ? base != 0 : base == 0);
    CHECK(g.snap(0) == (real_resident ? 1u : 0u));
    if (!real_resident) CHECK(g.avail() == before && g.ctx->num_m68klibs == 0);
    CHECK(g.word(outside) == 0x4AFC && g.read(outside + 2) == outside);
    g.unchanged();
}

static void init_result(unsigned mode)
{
    Guest g;
    if (mode == 0) {
        g.image.no_init();
        CHECK(g.open() && g.snap(0) == 0 && g.snap(16) == 1);
    } else if (mode == 1) {
        /* The init may return another base. Its vectors deliberately differ
           from those in the just allocated MakeLibrary block. */
        m68k_write16(g.ctx, ALTERNATE + 20, VERSION);
        m68k_write16(g.ctx, ALTERNATE - 6, 0x4EF9);
        m68k_write32(g.ctx, ALTERNATE - 4, ALTERNATE + POS);
        ULONG at = ALTERNATE + POS;
        m68k_write16(g.ctx, at, 0x52B9); m68k_write32(g.ctx, at + 2, SNAP + 16);
        m68k_write16(g.ctx, at + 6, 0x23CE); m68k_write32(g.ctx, at + 8, SNAP + 24);
        m68k_write16(g.ctx, at + 12, 0x200E); m68k_write16(g.ctx, at + 14, 0x4E75);
        g.image.return_init(ALTERNATE);
        CHECK(g.open() == ALTERNATE);
        CHECK(g.snap(4) != ALTERNATE && g.snap(0) == 1);
        CHECK(g.ctx->m68klibs[0].base == ALTERNATE && g.snap(24) == ALTERNATE && g.snap(16) == 1);
    } else {
        g.image.return_init(0);
        CHECK(g.open() == 0 && g.snap(0) == 1 && g.snap(16) == 0);
        CHECK(g.ctx->num_m68klibs == 0);
        /* Like MakeLibrary(), the bridge leaves this allocation to init.
           Only the hunks are freed. Account for the surviving block. */
        CHECK(g.avail() < g.initial_avail);
        Kaffeine_HeapFree(g.ctx, g.snap(4) - 20, 20 + POS);
        CHECK(g.avail() == g.initial_avail);
        CHECK(native_allocations.size() == g.initial_records);
    }
    g.unchanged();
}

static void reuse(unsigned mode)
{
    Guest g;
    const char *first = mode == 0 ? "tiny.library" : "PROGDIR:libs/tiny.library";
    const char *second = mode == 1 ? "tiny.library" : "OTHER:sub/tiny.library";
    ULONG base = g.open(first, 3); CHECK(base);
    ULONG before = g.avail();
    CHECK(g.open(second, 6) == base);
    CHECK(g.ctx->num_m68klibs == 1 && g.avail() == before);
    CHECK(g.snap(0) == 1 && g.snap(16) == 2 && g.snap(20) == 6 && g.snap(24) == base);
    CHECK(loads == 1 && attempts.size() == 1);
    g.unchanged();
}

static void versions_and_open_zero(unsigned mode)
{
    Guest g;
    if (mode == 0) {
        CHECK(g.open("tiny.library", VERSION + 1) == 0);
        CHECK(g.snap(0) == 1 && g.snap(16) == 0 && g.ctx->num_m68klibs == 1);
        CHECK(g.open("tiny.library", VERSION));
        CHECK(g.snap(0) == 1 && g.snap(16) == 1);
    } else if (mode == 1) {
        ULONG base = g.open(); CHECK(base);
        CHECK(g.open("tiny.library", 0x10000) == 0);
        CHECK(g.snap(16) == 1 && loads == 1);
    } else {
        g.image.return_open(0);
        CHECK(g.open() == 0);
        CHECK(g.ctx->num_m68klibs == 1 && g.snap(0) == 1 && g.snap(16) == 1);
        CHECK(g.open() == 0 && g.snap(16) == 2 && g.snap(0) == 1 && loads == 1);
    }
    g.unchanged();
}

static void full_list()
{
    Guest g;
    ULONG first = 0;
    for (unsigned i = 0; i < 32; ++i) {
        std::string name = "lib" + std::to_string(i) + ".library";
        ULONG base = g.open(name.c_str()); CHECK(base);
        if (!i) first = base;
        CHECK(g.ctx->num_m68klibs == i + 1);
    }
    ULONG before = g.avail();
    CHECK(g.open("overflow.library") == 0 && loads == 32);
    CHECK(g.avail() == before && g.snap(0) == 32 && g.snap(16) == 32);
    CHECK(g.open("RAM:lib0.library") == first);
    CHECK(loads == 32 && g.snap(0) == 32 && g.snap(16) == 33);
    g.unchanged();
}

static void expunge_reload()
{
    Guest g; g.image.expunge(g.ctx->m68k_sysbase);
    ULONG base = g.open(); CHECK(base);
    CHECK(g.close(base));
    CHECK(g.snap(28) == 1 && g.snap(32) == base && g.ctx->num_m68klibs == 0);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    CHECK(g.open());
    CHECK(loads == 2 && g.snap(0) == 2 && g.snap(16) == 2);
    CHECK(g.close(g.ctx->m68klibs[0].base));
    CHECK(g.avail() == g.initial_avail && g.ctx->num_m68klibs == 0);
    g.unchanged();
}

static void thunk_calls(unsigned mode)
{
    Guest g;
    if (mode < 2) {
        g.a(1, NAME); g.d(5); g.jsr(mode ? 408 : 552);
        g.w(0x23C0); g.l(SNAP + 48); /* save result */
        g.w(0x2240); /* MOVEA.L D0,A1 */
        g.jsr(414); g.after();
        CHECK(g.run() == 0);
        CHECK(g.snap(0) == 1 && g.snap(16) == 1 && g.snap(20) == (mode ? 0u : 5u));
        CHECK(g.snap(48) == g.ctx->m68klibs[0].base && g.snap(28) == 1);
        CHECK(g.ctx->mem[AFTER] == '<');
    } else {
        CHECK(g.open()); /* a foreign base must not close the loaded library */
        ULONG before = g.avail();
        CHECK(!g.close(g.ctx->m68k_sysbase));
        CHECK(!g.close(0) && !g.close(0x12345678));
        g.a(1, g.ctx->m68k_sysbase); g.jsr(414); g.after();
        CHECK(g.run() == 0 && g.ctx->mem[AFTER] == '<');
        CHECK(g.snap(28) == 0 && loads == 1 && g.avail() == before && g.ctx->num_m68klibs == 1);
    }
    g.unchanged();
}

static void thunk_stop(unsigned phase, UWORD lvo)
{
    Guest g;
    g.image.stop(phase == 0 ? INIT : phase == 1 ? OPEN : CLOSE, g.ctx->m68k_sysbase, 37);
    if (phase == 2) {
        ULONG base = g.open(); CHECK(base);
        g.a(1, base); g.jsr(414);
    } else {
        g.a(1, NAME); g.d(VERSION); g.jsr(lvo);
    }
    g.after(); g.d(99);
    CHECK(g.run() == 37);
    CHECK(g.ctx->exit_code == 37 && !g.ctx->running && g.ctx->callback_failed);
    CHECK(g.ctx->callback_depth == 0 && g.ctx->mem[AFTER] == 0xA5);
    if (phase == 0) CHECK(g.snap(16) == 0 && g.ctx->num_m68klibs == 0);
    g.unchanged();
}

static void init_odd_offset(unsigned kind, unsigned size, ULONG offset, bool refuse = false)
{
    Guest g; InitTable it;
    it.command(2, 1, 1, 20, {VERSION});
    it.command(kind, size, 1, offset, {0x12345678});
    it.end(); g.image.data(it.bytes);
    if (refuse) {
        g.refused();
        return;
    }
    ULONG base = g.open(); CHECK(base);
    /* Exec applies an explicit offset after destination alignment. Check
       every positive-area byte, including both neighbours of the write;
       the fields of the resident tag come after InitStruct(). */
    std::vector<UBYTE> expected(POS, 0);
    put16(expected.data() + 16, 20); put16(expected.data() + 18, POS);
    if (size == 0) put32(expected.data() + offset, 0x12345678);
    else put16(expected.data() + offset, 0x5678);
    resident_fields(expected, g.hunks()[1] + 0x70);
    for (unsigned i = 0; i < POS; ++i) CHECK(g.ctx->mem[base + i] == expected[i]);
    CHECK(g.snap(0) == 1 && g.snap(16) == 1);
    g.unchanged();
}

/* These helpers emit guest calls, not host replacements for the loader.
   Separate snapshot areas let nested libraries report their own registers. */
static void image_a(Image &image, ULONG &pc, unsigned reg, ULONG value)
{ image.emit_w(pc, UWORD(0x207C + (reg << 9))); image.emit_l(pc, value); }

static void image_jsr(Image &image, ULONG &pc, ULONG base, UWORD lvo)
{
    image_a(image, pc, 6, base);
    image.emit_w(pc, 0x4EAE); image.emit_w(pc, UWORD(-lvo));
}

static void init_opens(Image &image, ULONG sysbase, ULONG name, ULONG result,
                       bool first_only = false)
{
    ULONG pc = INIT + 24, branch = 0;
    if (first_only) {
        /* The old loader must recurse just once, so this test stays bounded. */
        image.emit_w(pc, 0x0CB9); image.emit_l(pc, 1); image.emit_l(pc, image.snapshot);
        branch = pc; image.emit_w(pc, 0x6600); image.emit_w(pc, 0);
    }
    image.emit_w(pc, 0x2F00); /* save the base supplied to init */
    image_a(image, pc, 1, name);
    image.emit_w(pc, 0x203C); image.emit_l(pc, VERSION);
    image_jsr(image, pc, sysbase, 552);
    image.save(pc, 0x23C0, result);
    image.emit_w(pc, 0x201F); /* MOVE.L (SP)+,D0 */
    if (branch) image.w(0, branch + 2, UWORD(pc - branch - 2));
    image.emit_w(pc, 0x4E75);
    CHECK(pc <= OPEN);
}

static void reserved_slot_before_init()
{
    Guest g;
    for (unsigned i = 0; i < 31; ++i) {
        std::string name = "held" + std::to_string(i) + ".library";
        CHECK(g.thunk_open(name.c_str()));
    }
    std::array<UBYTE, sizeof(g.ctx->m68klibs)> before;
    std::memcpy(before.data(), g.ctx->m68klibs, before.size());
    auto *emu_base = g.ctx->emuBase;
    CHECK(g.ctx->num_m68klibs == 31);
    Image b(SNAP + 128);
    disk_images = {{"A.library", &g.image}, {"B.library", &b}};
    g.text(0x3A00, "B.library");
    init_opens(g.image, g.ctx->m68k_sysbase, 0x3A00, SNAP + 64);
    ULONG base = g.thunk_open("A.library");
    CHECK(base && g.snap(64) == 0);
    CHECK(g.ctx->num_m68klibs == 32 && g.ctx->emuBase == emu_base);
    CHECK(std::memcmp(before.data(), g.ctx->m68klibs, 31 * sizeof(g.ctx->m68klibs[0])) == 0);
    CHECK(g.ctx->m68klibs[31].base == base && g.ctx->m68klibs[31].seglist);
    CHECK(std::string(g.ctx->m68klibs[31].name) == "A.library");
    CHECK(loads == 32 && attempts.size() == 32 && g.read(SNAP + 128) == 0);
    g.unchanged();
}

static void init_keeps_name_copy()
{
    Guest g;
    const std::string original = "PROGDIR:directory/with/a/long/path/original.library";
    const std::string replacement = "RAM:" + std::string(90, 'z') + "/changed.library";
    CHECK(original.size() > 32 && replacement.size() > 32);
    g.text(0x3C00, replacement.c_str());
    ULONG pc = INIT + 24;
    image_a(g.image, pc, 1, 0x3C00); image_a(g.image, pc, 2, NAME);
    g.image.code(pc, {0x14D9, 0x66FC, 0x4E75}); /* copy bytes through the NUL */
    ULONG base = g.thunk_open(original.c_str());
    CHECK(base && std::string(reinterpret_cast<char *>(g.ctx->mem + NAME)) == replacement);
    CHECK(std::string(g.ctx->m68klibs[0].name) == "original.library");
    CHECK(g.thunk_open(original.c_str()) == base);
    CHECK(loads == 1 && g.ctx->num_m68klibs == 1 && g.snap(0) == 1 && g.snap(16) == 2);
    CHECK(attempts == std::vector<std::string>{original});
    g.unchanged();
}

static void init_self_open()
{
    Guest g;
    g.text(0x3A00, "tiny.library");
    init_opens(g.image, g.ctx->m68k_sysbase, 0x3A00, SNAP + 64, true);
    ULONG base = g.thunk_open();
    CHECK(base && g.snap(64) == 0);
    CHECK(g.ctx->num_m68klibs == 1 && loads == 1 && g.snap(0) == 1 && g.snap(16) == 1);
    CHECK(g.ctx->m68klibs[0].base == base);
    for (unsigned i = 1; i < 32; ++i) CHECK(!g.ctx->m68klibs[i].name[0]);
    g.unchanged();
}

static void close_other_library()
{
    Guest g;
    Image b(SNAP + 128), c(SNAP + 256);
    b.expunge(g.ctx->m68k_sysbase);
    /* C stays on its first Close(), then expunges for the final leak check. */
    ULONG c_pc = CLOSE + 12;
    c.emit_w(c_pc, 0x0CB9); c.emit_l(c_pc, 2); c.emit_l(c_pc, c.snapshot + 28);
    c.code(c_pc, {0x6704, 0x7000, 0x4E75}); c_pc += 6;
    c.expunge(g.ctx->m68k_sysbase, c_pc);
    disk_images = {{"B.library", &b}, {"C.library", &c}, {"A.library", &g.image}};
    ULONG base_b = g.thunk_open("B.library"); CHECK(base_b);
    ULONG after_b = g.avail();
    ULONG base_c = g.thunk_open("C.library"); CHECK(base_c);
    ULONG c_cost = after_b - g.avail();
    auto c_slot = g.ctx->m68klibs[1];
    ULONG pc = CLOSE + 12;
    g.image.emit_w(pc, 0x2F0E); /* preserve this Close()'s A6 */
    image_a(g.image, pc, 1, base_b);
    image_jsr(g.image, pc, g.ctx->m68k_sysbase, 414);
    g.image.emit_w(pc, 0x2C5F); /* MOVEA.L (SP)+,A6 */
    g.image.expunge(g.ctx->m68k_sysbase, pc);
    ULONG base_a = g.thunk_open("A.library"); CHECK(base_a);
    CHECK(g.ctx->m68klibs[0].base == base_b && g.ctx->m68klibs[2].base == base_a);
    g.thunk_close(base_a);
    CHECK(g.snap(28) == 1 && g.read(b.snapshot + 28) == 1);
    CHECK(g.ctx->num_m68klibs == 1);
    CHECK(!g.ctx->m68klibs[0].name[0] && !g.ctx->m68klibs[2].name[0]);
    CHECK(std::memcmp(&c_slot, &g.ctx->m68klibs[1], sizeof(c_slot)) == 0);
    /* Freeing B before the still-live C creates one extra free interval,
       whose eight-byte header and native record are allocator metadata. */
    CHECK(g.avail() + 8 == g.initial_avail - c_cost);
    CHECK(native_allocations.size() == g.initial_records + 4);
    CHECK(g.thunk_open("C.library") == base_c && loads == 3);
    g.thunk_close(base_c);
    CHECK(g.read(c.snapshot + 16) == 2 && g.read(c.snapshot + 28) == 1);
    CHECK(g.ctx->m68klibs[1].base == base_c && g.ctx->num_m68klibs == 1);
    CHECK(g.avail() + 8 == g.initial_avail - c_cost);
    g.thunk_close(base_c);
    CHECK(g.ctx->num_m68klibs == 0);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

static void close_self_reentrant()
{
    Guest g;
    ULONG pc = CLOSE + 12;
    g.image.emit_w(pc, 0x0CB9); g.image.emit_l(pc, 1); g.image.emit_l(pc, SNAP + 28);
    ULONG branch = pc; g.image.emit_w(pc, 0x6600); g.image.emit_w(pc, 0);
    g.image.emit_w(pc, 0x224E); /* MOVEA.L A6,A1 */
    image_jsr(g.image, pc, g.ctx->m68k_sysbase, 414);
    g.image.save(pc, 0x2039, SNAP + 8); /* outer Close() also returns the seglist */
    g.image.emit_w(pc, 0x4E75);
    g.image.w(0, branch + 2, UWORD(pc - branch - 2));
    g.image.expunge(g.ctx->m68k_sysbase, pc); /* only the inner Close() frees its base */
    ULONG base = g.thunk_open(); CHECK(base && g.ctx->num_m68klibs == 1);
    g.thunk_close(base);
    CHECK(g.snap(28) == 2 && g.snap(32) == base);
    CHECK(g.ctx->num_m68klibs == 0);
    CHECK(!g.ctx->m68klibs[0].base && !g.ctx->m68klibs[0].seglist && !g.ctx->m68klibs[0].name[0]);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

static ULONG dos_call(Guest &g, UWORD lvo, ULONG seglist, ULONG args = 0, ULONG size = 0)
{
    g.w(0x223C); g.l(seglist); /* D1 */
    g.w(0x243C); g.l(8192); /* D2 */
    g.w(0x263C); g.l(args); /* D3 */
    g.w(0x283C); g.l(size); /* D4 */
    g.dos_jsr(lvo);
    return g.call();
}

static void unload_library_seglist()
{
    Guest g; g.setup_dos(); allow_foreign_unloads = true;
    g.image.expunge(g.ctx->m68k_sysbase);
    ULONG base = g.thunk_open(); CHECK(base);
    ULONG seg = g.ctx->m68klibs[0].seglist, before = g.avail();
    size_t calls = unload_calls.size();
    CHECK(dos_call(g, 156, seg) == 0);
    CHECK(unload_calls.size() == calls && g.avail() == before);
    CHECK(dos_call(g, 156, 0x12345) == 0);
    CHECK(unload_calls.size() == calls + 1 && unload_calls.back() == BPTR(0x12345));
    CHECK(g.ctx->m68klibs[0].seglist == seg && g.ctx->num_m68klibs == 1);
    g.thunk_close(base);
    CHECK(g.ctx->num_m68klibs == 0 && g.avail() == g.initial_avail);
    g.unchanged();
}

static void run_library_seglist()
{
    Guest g; g.setup_dos();
    g.image.expunge(g.ctx->m68k_sysbase);
    ULONG base = g.thunk_open(); CHECK(base);
    ULONG seg = g.ctx->m68klibs[0].seglist, before = g.avail();
    CHECK(dos_call(g, 504, seg) == ULONG(-1));
    CHECK(IoErr() == ERROR_OBJECT_WRONG_TYPE && run_commands.empty());
    g.text(0x3A00, "hello\n"); SetIoErr(73);
    CHECK(dos_call(g, 504, 0x23456, 0x3A00, 6) == 73);
    CHECK(run_commands.size() == 1 && run_commands[0].seglist == BPTR(0x23456));
    CHECK(run_commands[0].stacksize == 8192 && run_commands[0].args ==
        reinterpret_cast<char *>(g.ctx->mem + 0x3A00) && run_commands[0].size == 6);
    CHECK(IoErr() == 73 && g.avail() == before);
    g.thunk_close(base);
    CHECK(g.ctx->num_m68klibs == 0 && g.avail() == g.initial_avail);
    g.unchanged();
}

static ULONG observed_init_base, observed_init_seg, observed_init_count;
static void observe_init_slot(KaffeineContext *ctx)
{
    observed_init_base = ctx->m68klibs[0].base;
    observed_init_seg = ctx->m68klibs[0].seglist;
    observed_init_count = ctx->num_m68klibs;
}

static void init_seglist_guards()
{
    Guest g; g.setup_dos(); allow_foreign_unloads = true;
    g.ctx->after_thunk = observe_init_slot;
    observed_init_base = observed_init_seg = observed_init_count = 0xFFFFFFFF;
    ULONG pc = INIT + 24;
    g.image.emit_w(pc, 0x2F00); /* retain init's result across DOS calls */
    g.image.emit_w(pc, 0x2208); /* MOVE.L A0,D1: guest seglist */
    image_jsr(g.image, pc, g.dosbase, 156);
    g.image.save(pc, 0x2239, SNAP + 8); /* reload D1 from init's snapshot */
    g.image.code(pc, {0x7400, 0x7600, 0x7800}); pc += 6;
    image_jsr(g.image, pc, g.dosbase, 504);
    g.image.save(pc, 0x23C0, SNAP + 64);
    g.image.code(pc, {0x201F, 0x4E75});
    g.image.expunge(g.ctx->m68k_sysbase);
    ULONG base = g.thunk_open(); CHECK(base);
    CHECK(observed_init_base == 0 && observed_init_seg == g.snap(8) && observed_init_count == 1);
    CHECK(unload_calls.size() == 1 && run_commands.empty()); /* only the loader's native unload */
    CHECK(g.snap(64) == ULONG(-1) && IoErr() == ERROR_OBJECT_WRONG_TYPE);
    CHECK(g.ctx->m68klibs[0].base == base && g.ctx->m68klibs[0].seglist == g.snap(8));
    g.thunk_close(base);
    CHECK(g.ctx->num_m68klibs == 0 && g.avail() == g.initial_avail);
    g.unchanged();
}

static void close_per_opener_base()
{
    Guest g;
    ULONG pc = OPEN + 18;
    g.image.emit_w(pc, 0x2F0E); /* save the canonical base */
    g.image.emit_w(pc, 0x203C); g.image.emit_l(pc, 20 + POS);
    g.image.emit_w(pc, 0x223C); g.image.emit_l(pc, MEMF_PUBLIC | MEMF_CLEAR);
    image_jsr(g.image, pc, g.ctx->m68k_sysbase, 198);
    g.image.code(pc, {0x2240, 0x43E9, 20, 0x2C5F}); pc += 8;
    for (unsigned lvo : {6u, 12u, 18u}) {
        g.image.code(pc, {0x236E, UWORD(-lvo), UWORD(-lvo),
                          0x336E, UWORD(4 - lvo), UWORD(4 - lvo)});
        pc += 12; /* copy each complete JMP abs.l into the fresh block */
    }
    g.image.code(pc, {0x2009, 0x4E75});
    pc = CLOSE + 12;
    g.image.emit_w(pc, 0x2F0E); /* retain the per-opener A6 */
    g.image.save(pc, 0x2279, SNAP + 4); /* canonical MakeLibrary base */
    g.image.emit_w(pc, 0x93FC); g.image.emit_l(pc, 20);
    g.image.emit_w(pc, 0x203C); g.image.emit_l(pc, 20 + POS);
    image_jsr(g.image, pc, g.ctx->m68k_sysbase, 210);
    g.image.emit_w(pc, 0x2C5F);
    g.image.expunge(g.ctx->m68k_sysbase, pc);
    ULONG base = g.thunk_open(); CHECK(base);
    ULONG canonical = g.ctx->m68klibs[0].base;
    CHECK(base != canonical && canonical == g.snap(4) && g.snap(24) == canonical);
    CHECK(base >= g.ctx->heap_start && base < g.ctx->heap_end);
    for (unsigned lvo : {6u, 12u, 18u}) {
        CHECK(g.word(base - lvo) == 0x4EF9);
        CHECK(g.read(base - lvo + 2) == g.read(canonical - lvo + 2));
    }
    g.thunk_close(base);
    CHECK(g.snap(28) == 1 && g.snap(32) == base);
    CHECK(g.ctx->num_m68klibs == 0 && !g.ctx->m68klibs[0].name[0]);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

static void close_foreign_base(bool jump)
{
    Guest g;
    ULONG library = g.thunk_open(); CHECK(library);
    ULONG block = Kaffeine_HeapAlloc(g.ctx, 20 + POS, MEMF_CLEAR), base = block + 20;
    CHECK(block);
    m68k_write16(g.ctx, base - 12, jump ? 0x4EF9 : 0x4E75);
    m68k_write32(g.ctx, base - 10, jump ? ALTERNATE : g.hunks()[0] + CLOSE);
    m68k_write16(g.ctx, ALTERNATE, 0x4E75);
    ULONG before = g.avail();
    CHECK(!g.close(base)); /* the API's FALSE selects the native path */
    g.thunk_close(base);
    CHECK(g.snap(28) == 0 && g.ctx->m68klibs[0].base == library);
    CHECK(g.ctx->num_m68klibs == 1 && g.avail() == before);
    Kaffeine_HeapFree(g.ctx, block, 20 + POS);
    g.unchanged();
}

static void library_hunk_limit(unsigned count)
{
    Guest g;
    g.image.h.resize(count, std::vector<UBYTE>(4));
    g.image.expunge(g.ctx->m68k_sysbase);
    ULONG base = g.thunk_open();
    if (count > 1000) {
        CHECK(base == 0);
        CHECK(g.ctx->num_m68klibs == 0 && !g.ctx->m68klibs[0].name[0]);
        CHECK(g.snap(0) == 0 && g.snap(16) == 0);
    } else {
        CHECK(base && g.ctx->num_m68klibs == 1);
        ULONG seg = g.ctx->m68klibs[0].seglist;
        for (unsigned i = 0; i < count; ++i) {
            CHECK(seg && g.read((seg << 2) - 4) == g.image.h[i].size() + 8);
            seg = g.read(seg << 2);
        }
        CHECK(seg == 0);
        g.thunk_close(base);
        CHECK(g.ctx->num_m68klibs == 0 && g.snap(28) == 1);
    }
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

static void program_hunks_unlimited()
{
    Guest g;
    g.image.h.resize(1001, std::vector<UBYTE>(4));
    NativeSegments native(g.image);
    ULONG entry = Kaffeine_LoadHunks(g.ctx, native.segs[0], &native.table);
    CHECK(entry && g.ctx->entry_point == entry && g.ctx->seg_start == entry);
    CHECK(native_allocations.size() == g.initial_records + 1001);
    ULONG cost = 0;
    for (const auto &h : g.image.h) cost += allocation_cost(ULONG(h.size()));
    CHECK(g.initial_avail - g.avail() == cost);
    /* Program teardown owns its arena; the fixture destroys it as usual. */
    CHECK(stub_ok && loads == 0 && !native_segments);
}

static void bounded_library_name(unsigned mode)
{
    Guest g;
    ULONG at = NAME;
    std::string accepted;
    if (mode == 0) {
        at = g.ctx->mem_size / 2;
        std::memset(g.ctx->mem + at, 'x', 65536);
        g.ctx->mem[at + 65536] = 0; /* the old UWORD scan wraps before this NUL */
    } else if (mode == 1) {
        std::memset(g.ctx->mem + at, 'x', 256); g.ctx->mem[at + 256] = 0;
    } else if (mode == 2) {
        accepted = "RAM:" + std::string(235, 'p') + "/missing.library";
        CHECK(accepted.size() == 255);
        g.text(at, accepted.c_str());
        accepted_path = "not-present";
    } else if (mode == 3) {
        at = g.ctx->mem_size - 16;
        std::memset(g.ctx->mem + at, 'x', 16);
    } else at = 0;
    /* A host-side scan can hang between Moira instructions. SIGALRM makes
       that a failing test process even without an external timeout runner. */
    struct Deadline { Deadline() { alarm(3); } ~Deadline() { alarm(0); } } deadline;
    g.a(1, at); g.d(VERSION); g.jsr(552);
    CHECK(g.call() == 0);
    CHECK(native_lookups == (mode == 2 ? 1u : 0u));
    CHECK(attempts == (mode == 2 ? std::vector<std::string>{accepted} : std::vector<std::string>{}));
    CHECK(loads == 0 && g.ctx->num_m68klibs == 0);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

/* Literal guest Library fields: lib_Flags is byte 14 and lib_OpenCnt is
   word 32. The fixture owns both fields, as a disk library does. */
static constexpr ULONG VERSION_EXPUNGES = 64, VERSION_EXPUNGE_A6 = 68;
static constexpr ULONG VERSION_EXPUNGE_COUNT = 72;
enum class VersionExpunge { Defer, ReturnSeglist, Stop };

static void version_library(Guest &g, VersionExpunge mode = VersionExpunge::Defer)
{
    g.image.code(OPEN + 18, {0x526E, 32, 0x200E, 0x4E75}); /* ++OpenCnt; return A6 */
    ULONG pc = CLOSE + 12;
    g.image.code(pc, {0x536E, 32}); pc += 4; /* --OpenCnt */
    ULONG branch = pc; g.image.code(pc, {0x6600, 0}); pc += 4;
    g.image.code(pc, {0x082E, 3, 14, 0x6604}); pc += 8; /* BTST LIBB_DELEXP; BNE */
    g.image.w(0, branch + 2, UWORD(pc - branch - 2));
    g.image.code(pc, {0x7000, 0x4E75}); pc += 4;
    g.image.expunge(g.ctx->m68k_sysbase, pc); /* deferred removal at the last close */

    g.image.routine(EXTRA, VERSION_EXPUNGES, 0, 0, VERSION_EXPUNGE_A6);
    pc = EXTRA + 12;
    g.image.code(pc, {0x33EE, 32}); pc += 4; /* MOVE.W 32(A6),abs.l */
    g.image.emit_l(pc, SNAP + VERSION_EXPUNGE_COUNT);
    if (mode == VersionExpunge::ReturnSeglist)
        g.image.expunge(g.ctx->m68k_sysbase, pc);
    else if (mode == VersionExpunge::Stop)
        g.image.stop(pc, g.ctx->m68k_sysbase, 37);
    else
        g.image.code(pc, {0x002E, 8, 14, 0x7000, 0x4E75}); /* set LIBF_DELEXP; return 0 */
}

static void version_expunge_open()
{
    Guest g; version_library(g);
    ULONG base = g.thunk_open(); CHECK(base && g.word(base + 32) == 1);
    auto slot = g.ctx->m68klibs[0];
    ULONG available = g.avail();
    size_t records = native_allocations.size();
    CHECK(g.snap(VERSION_EXPUNGES) == 0 && g.ctx->mem[base + 14] == 0x06);
    for (unsigned request = 1; request <= 2; ++request) {
        CHECK(g.thunk_open("tiny.library", VERSION + 1) == 0);
        CHECK(g.snap(VERSION_EXPUNGES) == request && g.snap(VERSION_EXPUNGE_A6) == base);
        CHECK(g.word(SNAP + VERSION_EXPUNGE_COUNT) == 1 && g.word(base + 32) == 1);
        CHECK(g.ctx->mem[base + 14] == 0x0E && g.snap(16) == 1);
        CHECK(g.ctx->num_m68klibs == 1 && std::memcmp(&slot, &g.ctx->m68klibs[0], sizeof(slot)) == 0);
        CHECK(g.avail() == available && native_allocations.size() == records && loads == 1);
    }
    CHECK(g.thunk_open() == base && loads == 1 && g.word(base + 32) == 2);
    CHECK(g.thunk_open("tiny.library", VERSION + 2) == 0);
    CHECK(g.snap(VERSION_EXPUNGES) == 3 && g.snap(VERSION_EXPUNGE_A6) == base);
    CHECK(g.word(SNAP + VERSION_EXPUNGE_COUNT) == 2 && g.word(base + 32) == 2 && g.snap(16) == 2);
    CHECK(g.ctx->num_m68klibs == 1 && std::memcmp(&slot, &g.ctx->m68klibs[0], sizeof(slot)) == 0);
    g.thunk_close(base);
    CHECK(g.word(base + 32) == 1 && g.ctx->num_m68klibs == 1 && g.avail() == available);
    g.thunk_close(base);
    CHECK(g.snap(28) == 2 && g.snap(VERSION_EXPUNGES) == 3 && g.ctx->num_m68klibs == 0);
    CHECK(!g.ctx->m68klibs[0].base && !g.ctx->m68klibs[0].seglist && !g.ctx->m68klibs[0].name[0]);
    CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    g.unchanged();
}

static void version_expunge_closed()
{
    Guest g; version_library(g);
    ULONG base = g.thunk_open(); CHECK(base && g.word(base + 32) == 1);
    auto slot = g.ctx->m68klibs[0];
    g.thunk_close(base);
    CHECK(g.word(base + 32) == 0 && g.snap(28) == 1 && g.ctx->num_m68klibs == 1);
    ULONG available = g.avail();
    size_t records = native_allocations.size();
    CHECK(g.thunk_open("tiny.library", VERSION + 1) == 0);
    CHECK(g.snap(VERSION_EXPUNGES) == 0 && g.snap(16) == 1 && g.word(base + 32) == 0);
    CHECK(g.ctx->mem[base + 14] == 0x06);
    CHECK(g.ctx->num_m68klibs == 1 && std::memcmp(&slot, &g.ctx->m68klibs[0], sizeof(slot)) == 0);
    CHECK(g.avail() == available && native_allocations.size() == records && loads == 1);
    CHECK(g.thunk_open() == base && loads == 1 && g.word(base + 32) == 1);
    g.thunk_close(base);
    CHECK(g.word(base + 32) == 0 && g.snap(VERSION_EXPUNGES) == 0 && g.ctx->num_m68klibs == 1);
    g.unchanged();
}

static void version_expunge_seglist()
{
    Guest g; version_library(g, VersionExpunge::ReturnSeglist);
    for (unsigned load = 1; load <= 2; ++load) {
        ULONG base = g.thunk_open(); CHECK(base && g.word(base + 32) == 1);
        CHECK(loads == load && g.snap(0) == load && g.snap(16) == load);
        CHECK(g.ctx->num_m68klibs == 1 && g.ctx->m68klibs[0].base == base);
        CHECK(g.thunk_open("tiny.library", VERSION + 1) == 0);
        CHECK(g.snap(VERSION_EXPUNGES) == load && g.snap(VERSION_EXPUNGE_A6) == base);
        CHECK(g.word(SNAP + VERSION_EXPUNGE_COUNT) == 1 && g.snap(28) == 0);
        CHECK(g.ctx->num_m68klibs == 0);
        CHECK(!g.ctx->m68klibs[0].base && !g.ctx->m68klibs[0].seglist && !g.ctx->m68klibs[0].name[0]);
        CHECK(g.avail() == g.initial_avail && native_allocations.size() == g.initial_records);
    }
    g.unchanged();
}

static void version_expunge_stop()
{
    Guest g; version_library(g, VersionExpunge::Stop);
    ULONG base = g.thunk_open(); CHECK(base && g.word(base + 32) == 1);
    auto slot = g.ctx->m68klibs[0];
    ULONG available = g.avail();
    size_t records = native_allocations.size();
    g.a(1, NAME); g.d(VERSION + 1); g.jsr(552);
    g.after(); g.d(99); g.w(0x4E75);
    KaffeineGuestCall result = {};
    BOOL returned = Kaffeine_CallGuest(g.ctx, g.cpu, ENTRY, &result);
    if (returned)
        std::printf("Expunge stop was missed: guest returned D0=%lu, after=0x%02x\n",
            (unsigned long)result.d[0], unsigned(g.ctx->mem[AFTER]));
    CHECK(!returned);
    CHECK(Kaffeine_GetD(g.cpu, 0) == 37 && g.ctx->exit_code == 37);
    CHECK(!g.ctx->running && g.ctx->callback_failed && g.ctx->callback_depth == 0);
    CHECK(g.ctx->mem[AFTER] == 0xA5);
    CHECK(g.snap(VERSION_EXPUNGES) == 1 && g.snap(VERSION_EXPUNGE_A6) == base);
    CHECK(g.word(SNAP + VERSION_EXPUNGE_COUNT) == 1 && g.word(base + 32) == 1 && g.snap(16) == 1);
    CHECK(g.ctx->num_m68klibs == 1 && std::memcmp(&slot, &g.ctx->m68klibs[0], sizeof(slot)) == 0);
    CHECK(g.avail() == available && native_allocations.size() == records && loads == 1);
    g.unchanged();
}

static void version_expunge_first_load()
{
    Guest g; version_library(g);
    CHECK(g.thunk_open("tiny.library", VERSION + 1) == 0);
    ULONG base = g.ctx->m68klibs[0].base;
    CHECK(base && g.ctx->num_m68klibs == 1 && g.ctx->m68klibs[0].seglist);
    CHECK(g.snap(0) == 1 && g.snap(16) == 0 && g.snap(28) == 0);
    CHECK(g.word(base + 32) == 0 && g.snap(VERSION_EXPUNGES) == 0 && g.ctx->mem[base + 14] == 0x06);
    auto slot = g.ctx->m68klibs[0];
    ULONG available = g.avail();
    CHECK(g.thunk_open() == base && loads == 1 && g.snap(0) == 1 && g.snap(16) == 1);
    CHECK(g.word(base + 32) == 1 && g.snap(VERSION_EXPUNGES) == 0 && g.avail() == available);
    CHECK(std::memcmp(&slot, &g.ctx->m68klibs[0], sizeof(slot)) == 0);
    g.thunk_close(base);
    CHECK(g.word(base + 32) == 0 && g.ctx->num_m68klibs == 1 && g.snap(VERSION_EXPUNGES) == 0);
    g.unchanged();
}

/* Classic callers may set D0.W alone: OpenLibrary() takes the low word as
   the version, as AmigaOS 3.2.3 does, for the version check, for the
   Expunge() of an open library and for the D0 of Open(). */
static void version_low_word()
{
    Guest g; version_library(g);
    ULONG base = g.thunk_open("tiny.library", 0x12340000 | VERSION);
    CHECK(base && g.snap(20) == VERSION && g.word(base + 32) == 1);
    CHECK(g.thunk_open("tiny.library", 0xFFFF0000 | VERSION) == base);
    CHECK(g.snap(20) == VERSION && g.word(base + 32) == 2 && g.snap(VERSION_EXPUNGES) == 0);
    CHECK(g.thunk_open("tiny.library", 0x00010000 | (VERSION + 1)) == 0);
    CHECK(g.snap(VERSION_EXPUNGES) == 1 && g.word(base + 32) == 2 && loads == 1);
    g.unchanged();
}

/* InitResident() sets the node type, name, version and IdString of the
   resident tag after MakeLibrary() and before the init: a library without
   a data table opens for the version of its tag, and its init already
   sees the name and the version in the base. */
static void resident_fields_before_init()
{
    Guest g; g.image.no_data();
    ULONG pc = INIT + 24; /* after the snapshots of the init */
    g.image.emit_w(pc, 0x2240); /* MOVEA.L D0,A1 */
    g.image.emit_w(pc, 0x23E9); g.image.emit_w(pc, 10); g.image.emit_l(pc, SNAP + 100); /* MOVE.L 10(A1),abs.l */
    g.image.emit_w(pc, 0x33E9); g.image.emit_w(pc, 20); g.image.emit_l(pc, SNAP + 104); /* MOVE.W 20(A1),abs.l */
    g.image.emit_w(pc, 0x4E75);
    CHECK(pc <= OPEN);
    ULONG base = g.open();
    CHECK(base && g.snap(0) == 1 && g.snap(16) == 1);
    ULONG name = g.hunks()[1] + 0x70;
    CHECK(std::string(reinterpret_cast<char *>(g.ctx->mem + name)) == "tiny.library");
    CHECK(g.read(SNAP + 100) == name && g.word(SNAP + 104) == VERSION);
    CHECK(g.ctx->mem[base + 8] == 9 && g.read(base + 10) == name && g.ctx->mem[base + 14] == 0x06);
    CHECK(g.word(base + 20) == VERSION && g.read(base + 24) == name);
    g.unchanged();
}

/* A backend that keeps translations, as in test_hunk_loading. OpenLibrary()
   runs inside Kaffeine_RunCPU(), which keeps its CPU in ctx->cpu, and tells
   it each hunk that it copied and the jump table that MakeLibrary() wrote;
   a library from the cache writes no code. */
static std::vector<std::pair<ULONG, ULONG>> flushed;
static void record_flush(void *, ULONG addr, ULONG size) { flushed.push_back({addr, size}); }

static void loader_flushes()
{
    Guest g;
    const KaffeineCPUOps *moira = g.cpu->ops;
    KaffeineCPUOps recording = *moira;
    recording.flush_code = record_flush;
    g.cpu->ops = &recording;
    g.ctx->cpu = g.cpu;
    flushed.clear();
    ULONG base = g.thunk_open();
    CHECK(base);
    ULONG seg = g.ctx->m68klibs[0].seglist, next = g.read(seg << 2);
    ULONG neg = g.word(base + 16);
    const std::vector<std::pair<ULONG, ULONG>> written = {
        {(seg << 2) + 4, g.read((seg << 2) - 4) - 8},
        {(next << 2) + 4, g.read((next << 2) - 4) - 8},
        {base - neg, neg}};
    CHECK(next && !g.read(next << 2) && neg == ((g.image.vectors * 6 + 3) & ~3u));
    CHECK(flushed == written);
    flushed.clear();
    CHECK(g.thunk_open() == base && flushed.empty());
    g.ctx->cpu = nullptr;
    g.cpu->ops = moira;
    g.unchanged();
}

static void run(const std::string &name, const std::function<void()> &fn, const char *filter)
{
    if (filter && name.find(filter) == std::string::npos) return;
    ++cases; case_name = name.c_str();
    unsigned before = failures, checks_before = checks;
    try { fn(); CHECK(native_allocations.empty()); }
    catch (const Failed &) {}
    std::printf("%s: %s (%u checks)\n", case_name,
        failures != before ? "FAIL" : "PASS",
        checks - checks_before);
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    run("seglist-layout-relocations", seglist_copy, filter);
    for (unsigned i = 0; i < 7; ++i)
        run("copy-refusal-" + std::to_string(i), [=] { copy_refusal(i); }, filter);
    for (bool words : {false, true})
        for (unsigned count : {3u, 4u, 1024u})
            run(std::string(words ? "word" : "long") + "-vectors-" + std::to_string(count),
                [=] { library_layout(words, count); }, filter);
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned size = 0; size < 3; ++size)
            run("init-kind-" + std::to_string(kind) + "-size-" + std::to_string(size),
                [=] { init_commands(kind, size); }, filter);
    for (unsigned i = 0; i < 5; ++i)
        run("search-path-" + std::to_string(i), [=] { paths(i); }, filter);
    run("missing-file", [] { Guest g; accepted_path = "missing"; g.refused(); CHECK(attempts.size() == 3); }, filter);
    run("explicit-path-missing", [] { Guest g; accepted_path = "missing";
        CHECK(g.open("RAM:tiny.library") == 0); CHECK(attempts.size() == 1 && g.avail() == g.initial_avail); g.unchanged(); }, filter);
    run("decoy-with-resident", [] { decoy(true); }, filter);
    run("decoy-without-resident", [] { decoy(false); }, filter);
    run("no-resident", [] { Guest g; g.image.w(1, RESIDENT, 0); g.refused(); }, filter);
    run("bad-matchtag", [] { Guest g; g.image.raw(1, RESIDENT + 2, 0); g.refused(); }, filter);
    run("not-autoinit", [] { Guest g; g.image.h[1][RESIDENT + 10] = 0; g.refused(); }, filter);
    run("not-library", [] { Guest g; g.image.h[1][RESIDENT + 12] = 3; g.refused(); }, filter);
    run("no-relocation-record", [] { Guest g; have_record = false; g.refused(); CHECK(infos == 1); }, filter);
    run("seglist-info-fails", [] { Guest g; info_success = false; g.refused(); CHECK(infos == 1); }, filter);
    for (unsigned kind = 0; kind < 4; ++kind)
        run("refuse-fourth-size-" + std::to_string(kind), [=] { Guest g; InitTable it;
            it.command(kind, 3, 1, 40, {0x33}); it.end(); g.image.data(it.bytes); g.refused(); }, filter);
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned size = 0; size < 3; ++size)
            run("refuse-write-bounds-" + std::to_string(kind) + "-" + std::to_string(size), [=] {
                Guest g; InitTable it;
                it.command(2, 2, 1, POS - 1, {0x45});
                it.command(kind, size, 2, POS - 1, kind == 1 ? std::vector<ULONG>{0x55} : std::vector<ULONG>{0x55, 0x66});
                it.end(); g.image.data(it.bytes); g.refused(); }, filter);
    run("refuse-vector-overflow", [] { Guest g; g.image.functions(false, 1026); g.refused(); }, filter);
    run("refuse-word-vector-overflow", [] { Guest g; g.image.functions(true, 1026); g.refused(); }, filter);
    run("refuse-vector-1025", [] { Guest g; g.image.functions(false, 1025); g.refused(); }, filter);
    run("refuse-word-vector-1025", [] { Guest g; g.image.functions(true, 1025); g.refused(); }, filter);
    for (ULONG fn : {1u, 0x02000000u, 0xFFFFFFFEu})
        run("refuse-function-" + std::to_string(fn), [=] { Guest g; g.image.raw(0, TABLE, fn); g.refused(); }, filter);
    run("refuse-odd-word-function", [] { Guest g; g.image.functions(true); g.image.w(0, TABLE + 2, UWORD(OPEN - TABLE + 1)); g.refused(); }, filter);
    run("refuse-word-function-outside", [] { Guest g;
        g.image.raw(1, AUTOINIT + 4, 0x2000);
        m68k_write16(g.ctx, 0x2000, 0xFFFF); m68k_write16(g.ctx, 0x2002, 0xC000);
        m68k_write16(g.ctx, 0x2004, 0xFFFF); g.refused(); }, filter);
    for (unsigned field : {0u, 4u, 8u})
        run("refuse-table-" + std::to_string(field), [=] { Guest g;
            g.image.raw(1, field == 0 ? RESIDENT + 22 : AUTOINIT + field, 0x02000000);
            g.refused(); }, filter);
    run("refuse-truncated-autoinit", [] { Guest g; g.image.raw(1, RESIDENT + 22, g.ctx->mem_size - 14); g.refused(); }, filter);
    run("refuse-truncated-function-table", [] { Guest g; g.image.raw(1, AUTOINIT + 4, g.ctx->mem_size - 2); g.refused(); }, filter);
    run("refuse-truncated-init-values", [] { Guest g; ULONG at = g.ctx->mem_size - 2;
        g.ctx->mem[at] = 0x01; g.image.raw(1, AUTOINIT + 8, at); g.refused(); }, filter);
    run("refuse-datasize-overflow", [] { Guest g; g.image.l(1, AUTOINIT, 65536); g.refused(); }, filter);
    for (ULONG offset : {0x010042u, 0xFFFFFFu})
        run("refuse-24bit-offset-" + std::to_string(offset), [=] { Guest g; InitTable it;
            it.command(3, 0, 1, offset, {0x12345678}); it.end(); g.image.data(it.bytes); g.refused(); }, filter);
    run("minimum-positive-size", [] { Guest g; g.image.l(1, AUTOINIT, 1); ULONG base = g.open(); CHECK(base && g.word(base + 18) == 34); g.unchanged(); }, filter);
    run("no-data-table", [] { Guest g; g.image.no_data(); ULONG base = g.open(); CHECK(base && g.word(base + 20) == VERSION); g.unchanged(); }, filter);
    run("resident-fields-before-init", resident_fields_before_init, filter);
    run("version-low-word", version_low_word, filter);
    run("open-returns-another-base", [] { Guest g; g.image.return_open(ALTERNATE);
        CHECK(g.open() == ALTERNATE); CHECK(g.ctx->m68klibs[0].base != ALTERNATE);
        CHECK(g.snap(16) == 1 && g.snap(24) == g.snap(4)); g.unchanged(); }, filter);
    for (unsigned count : {4095u, 4096u, 4097u})
        run("init-command-limit-" + std::to_string(count), [=] { Guest g; InitTable it;
            for (unsigned i = 0; i < count; ++i)
                it.command(2, 2, 1, 40, {i == 4095 ? 0xBCu : 0x55u});
            it.end(); g.image.data(it.bytes);
            if (count > 4096) g.refused(); else {
                ULONG base = g.open("tiny.library", 0); CHECK(base);
                CHECK(g.ctx->mem[base + 40] == (count == 4096 ? 0xBC : 0x55));
                CHECK(g.snap(0) == 1 && g.snap(16) == 1); g.unchanged();
            } }, filter);
    for (unsigned i = 0; i < 3; ++i) {
        run("init-result-" + std::to_string(i), [=] { init_result(i); }, filter);
        run("reuse-" + std::to_string(i), [=] { reuse(i); }, filter);
        run("open-version-result-" + std::to_string(i), [=] { versions_and_open_zero(i); }, filter);
        run("exec-thunk-" + std::to_string(i), [=] { thunk_calls(i); }, filter);
    }
    run("library-list-full", full_list, filter);
    run("expunge-reload", expunge_reload, filter);
    run("regression-slot-reserved-before-init", reserved_slot_before_init, filter);
    run("regression-init-keeps-name-copy", init_keeps_name_copy, filter);
    run("regression-init-self-open", init_self_open, filter);
    run("regression-close-other-library", close_other_library, filter);
    run("regression-close-self-reentrant", close_self_reentrant, filter);
    run("regression-unload-library-seglist", unload_library_seglist, filter);
    run("regression-run-library-seglist", run_library_seglist, filter);
    run("regression-init-seglist-guards", init_seglist_guards, filter);
    run("regression-close-per-opener-base", close_per_opener_base, filter);
    run("regression-close-jump-outside-hunks", [] { close_foreign_base(true); }, filter);
    run("regression-close-without-jump", [] { close_foreign_base(false); }, filter);
    run("regression-library-hunks-1001-refused", [] { library_hunk_limit(1001); }, filter);
    run("regression-library-hunks-1000-expunged", [] { library_hunk_limit(1000); }, filter);
    run("regression-program-hunks-1001", program_hunks_unlimited, filter);
    const char *name_cases[] = {"65536-nonzero", "256-nonzero", "255-plus-nul", "memory-end", "null"};
    for (unsigned i = 0; i < 5; ++i)
        run(std::string("regression-name-") + name_cases[i], [=] { bounded_library_name(i); }, filter);
    run("version-expunge-open", version_expunge_open, filter);
    run("version-expunge-closed", version_expunge_closed, filter);
    run("version-expunge-seglist", version_expunge_seglist, filter);
    run("version-expunge-stop", version_expunge_stop, filter);
    run("version-expunge-first-load", version_expunge_first_load, filter);
    run("loader-flushes", loader_flushes, filter);
    for (unsigned phase = 0; phase < 3; ++phase)
        for (UWORD lvo : {552, 408})
            if (phase < 2 || lvo == 552)
                run("stop-phase-" + std::to_string(phase) + "-lvo-" + std::to_string(lvo),
                    [=] { thunk_stop(phase, lvo); }, filter);
    for (unsigned kind : {2u, 3u})
        for (unsigned size : {0u, 1u}) {
            std::string suffix = "-" + std::to_string(kind) + "-" + std::to_string(size);
            run("init-odd-offset" + suffix, [=] { init_odd_offset(kind, size, 65); }, filter);
            run("init-odd-offset-boundary" + suffix,
                [=] { init_odd_offset(kind, size, POS - (size == 0 ? 4 : 2) - 1); }, filter);
            run("refuse-odd-offset" + suffix,
                [=] { init_odd_offset(kind, size, POS - 1, true); }, filter);
        }
    std::printf("M68k disk libraries: %u cases, %u checks, %u failed\n", cases, checks, failures);
    return failures || cases == 0 ? 1 : 0;
}

/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Call ReadArgs(), FreeArgs() and GetProgramName() for 68k programs
 * through the real dispatcher with literal LVOs. Behind the thunks runs
 * the native AROS parser from rom/dos, built for the 64-bit host by
 * dos_readargs_host.c, so its IPTR array holds what the thunk has to turn
 * into 32-bit guest values: strings, /N numbers, /M arrays and switches.
 * The context and its heap are the production ones, and every ReadArgs()
 * call is checked for writes outside the guest array, the result block
 * and the position field of the program's own RDArgs.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_readargs.h"

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *what, unsigned long a = 0, unsigned long b = 0)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::printf("FAIL: %s (0x%08lx, 0x%08lx)\n", what, a, b);
    }
}

/* Count parser/context blocks separately from the heap's MEMF_PRIVATE
   ownership records. Guest templates, arrays and RDArgs can outlive a parser
   cleanup; their records are checked at context destruction instead. */
static long live_blocks;
static std::set<APTR> heap_records;
static Process process;

extern "C" APTR AllocVec(ULONG size, ULONG flags)
{
    void *block = std::malloc(size ? size : 1);

    if (block)
    {
        std::memset(block, (flags & MEMF_CLEAR) ? 0 : 0xA5, size);
        if (flags & MEMF_PRIVATE)
            heap_records.insert(block);
        else
            live_blocks++;
    }
    return block;
}

extern "C" void FreeVec(APTR block)
{
    if (block)
    {
        if (!heap_records.erase(block))
            live_blocks--;
        std::free(block);
    }
}

extern "C" Task *FindTask(CONST_STRPTR) { return reinterpret_cast<Task *>(&process); }

/* dos.library: the arguments wait in Input(), where RunCommand() puts
   them with vbuf_inject(). What the parser prints goes to `printed`. */
static std::string input;
static size_t input_pos;
static std::vector<std::string> printed;

extern "C" BPTR Input(void) { return &input; }
extern "C" BPTR Output(void) { return &printed; }
extern "C" LONG IsInteractive(BPTR) { return FALSE; }

extern "C" STRPTR FGets(BPTR, STRPTR buffer, ULONG size)
{
    ULONG n = 0;

    if (input_pos >= input.size())
        return nullptr;
    while (n + 1 < size && input_pos < input.size())
        if ((buffer[n++] = input[input_pos++]) == '\n')
            break;
    buffer[n] = 0;
    return buffer;
}

extern "C" LONG FGetC(BPTR) { return input_pos < input.size() ? (UBYTE)input[input_pos++] : -1; }
extern "C" LONG UnGetC(BPTR, LONG c) { if (input_pos) input_pos--; return c; }
extern "C" LONG FPuts(BPTR, CONST_STRPTR s) { printed.push_back(s); return 0; }
extern "C" LONG Flush(BPTR) { return TRUE; }
extern "C" SIPTR IoErr(void) { return process.pr_Result2; }

extern "C" SIPTR SetIoErr(SIPTR code)
{
    SIPTR old = process.pr_Result2;

    process.pr_Result2 = code;
    return old;
}

/* As in rom/dos: an RDArgs is a cleared AllocVec() block */
extern "C" APTR AllocDosObject(ULONG type, const TagItem *)
{
    if (type != DOS_RDARGS)
        std::abort();
    return AllocVec(sizeof(RDArgs), MEMF_CLEAR);
}

extern "C" void FreeDosObject(ULONG type, APTR object)
{
    if (type != DOS_RDARGS)
        std::abort();
    FreeVec(object);
}

/* kaffeine_memory.c opens libraries for the HUNK loader, unused here */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }

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

enum { LVO_GETPROGRAMNAME = 576, LVO_READARGS = 798, LVO_FREEARGS = 858 };

/* struct RDArgs of the 68k */
enum
{
    RDA_CS_BUFFER = 0, RDA_CS_LENGTH = 4, RDA_CS_CURCHR = 8, RDA_DALIST = 12,
    RDA_BUFFER = 16, RDA_BUFSIZ = 20, RDA_EXTHELP = 24, RDA_FLAGS = 28, RDA_SIZE = 32
};

static unsigned generated_name_calls;
static IPTR poisoned_program_name(KaffeineContext *, void *)
{
    ++generated_name_calls;
    return 0xBAD0BAD0;
}
static const M68KThunkEntry generated_names[] = { { 576, poisoned_program_name } };

static const M68KThunkEntry dos_thunks[] = { KAFFEINE_DOS_READARGS_THUNKS, { 0, nullptr } };

struct Fixture
{
    static constexpr ULONG dos_base = 0x1000;
    static constexpr ULONG return_pc = 0x76543210;
    KaffeineContext *ctx;
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture() : ctx(Kaffeine_CreateContext(nullptr))
    {
        if (!ctx)
            std::abort();
        ctx->num_libs = 1;
        auto &lib = ctx->libs[0];
        lib.m68k_addr = dos_base;
        lib.jt_start = 0x800;
        std::strcpy(lib.name, "dos.library");
        lib.thunks = dos_thunks;
        lib.num_thunks = sizeof(dos_thunks) / sizeof(dos_thunks[0]) - 1;
        lib.gen_thunks = generated_names;
        lib.num_gen_thunks = 1;
    }

    ~Fixture()
    {
        Kaffeine_ReadArgsCleanup(ctx);
        Kaffeine_DestroyContext(ctx);
        if (!heap_records.empty() || live_blocks)
            std::abort();
    }

    /* Call an LVO with D1-D3 set; the other registers hold markers. */
    ULONG call(ULONG lvo, ULONG d1, ULONG d2 = 0, ULONG d3 = 0)
    {
        const ULONG sp = ctx->stack_top - 0x100;

        for (int i = 0; i < 8; i++)
        {
            state.d[i] = 0xD0D0D000 + i;
            state.a[i] = 0xA0A0A000 + i;
        }
        state.d[1] = d1;
        state.d[2] = d2;
        state.d[3] = d3;
        state.a[7] = sp;
        m68k_write32(ctx, sp, return_pc);

        KaffeineDispatchResult result = {};
        bool dispatched = Kaffeine_DispatchLibraryCall(ctx, &cpu, dos_base - lvo, &result);
        check(dispatched, "dos.library LVO is dispatched", lvo);
        check(state.a[7] == sp + 4 && result.return_pc == return_pc && !result.supervisor,
              "dispatch consumes exactly one guest return address", lvo);
        bool others = state.d[1] == d1 && state.d[2] == d2 && state.d[3] == d3;
        for (int i = 4; i < 8; i++)
            others = others && state.d[i] == 0xD0D0D000 + (ULONG)i;
        for (int i = 0; i < 7; i++)
            others = others && state.a[i] == 0xA0A0A000 + (ULONG)i;
        check(others, "only D0 changes", lvo);
        if (lvo == 576)
            check(generated_name_calls == 0, "GetProgramName wins over poisoned generated fallback", lvo);
        return state.d[0];
    }
};

static Fixture fx;

static ULONG peek(ULONG addr) { return m68k_read32(fx.ctx, addr); }
static void poke(ULONG addr, ULONG value) { m68k_write32(fx.ctx, addr, value); }

static ULONG guest_string(const std::string &s)
{
    ULONG addr = Kaffeine_HeapAlloc(fx.ctx, s.size() + 1, 0);

    std::memcpy(fx.ctx->mem + addr, s.c_str(), s.size() + 1);
    return addr;
}

static std::string read_string(ULONG addr)
{
    if (!addr || addr >= fx.ctx->mem_size)
        return "<outside guest memory>";
    const char *s = reinterpret_cast<const char *>(fx.ctx->mem + addr);
    size_t n = strnlen(s, fx.ctx->mem_size - addr);
    return n < fx.ctx->mem_size - addr ? std::string(s, n) : "<unterminated>";
}

static ULONG heap_free_bytes()
{
    ULONG total = 0;

    for (ULONG block = fx.ctx->heap_free; block; block = peek(block + 4))
        total += peek(block);
    return total;
}

/* Fill the free guest heap with a pattern, so that a missing terminator
   or an uncleared field does not read as zero by chance. */
static void poison_free_heap()
{
    for (ULONG block = fx.ctx->heap_free; block; block = peek(block + 4))
        std::memset(fx.ctx->mem + block + KAFFEINE_HEAP_HDR, 0xEE, peek(block) - KAFFEINE_HEAP_HDR);
}

struct Parse
{
    ULONG tmpl = 0, array = 0, rdargs = 0, handle = 0, block = 0;
    std::vector<ULONG> values;
    SIPTR error = 0;
    long live_before = 0;
    ULONG heap_before = 0;
    unsigned stray = 0;
};

/* The guest block that holds the results of a handle */
static ULONG result_block(ULONG handle)
{
    for (int i = 0; handle && i < KAFFEINE_MAX_RDARGS; i++)
        if (fx.ctx->rdargs_map[i].native && fx.ctx->rdargs_map[i].handle == handle)
            return fx.ctx->rdargs_map[i].block;
    return 0;
}

/* Guest words that changed outside the array, the result block and the
   position in the program's RDArgs. The heap may change the size of the
   free block it split. */
static unsigned stray_writes(const std::vector<UBYTE> &before, const Parse &p)
{
    const ULONG block = p.block ? p.block - KAFFEINE_HEAP_HDR : 0;
    const ULONG block_end = p.block ? block + peek(block) : 0;
    const ULONG sp = fx.ctx->stack_top - 0x100;
    unsigned stray = 0;

    for (ULONG page = 0; page < fx.ctx->mem_size; page += 4096)
    {
        if (!std::memcmp(&before[page], fx.ctx->mem + page, 4096))
            continue;
        for (ULONG addr = page; addr < page + 4096; addr += 4)
        {
            if (!std::memcmp(&before[addr], fx.ctx->mem + addr, 4))
                continue;
            if (addr >= p.array && addr < p.array + 4 * p.values.size())
                continue;
            if (addr >= block && addr < block_end)
                continue;
            if (addr == sp || (p.rdargs && addr == p.rdargs + RDA_CS_CURCHR))
                continue;
            bool split = false;
            for (ULONG free = fx.ctx->heap_free; free && !split; free = peek(free + 4))
                split = free == addr && free + peek(free) == block;
            stray += !split;
        }
    }
    return stray;
}

/* ReadArgs() from a 68k program: the template and the array are in guest
   memory, the command line in Input(), and RDArgs is the program's own
   or none. */
static Parse read_args(const char *tmpl, const std::string &line,
                       const std::vector<ULONG> &initial, ULONG rdargs = 0)
{
    Parse p;

    p.tmpl = guest_string(tmpl);
    p.array = Kaffeine_HeapAlloc(fx.ctx, 4 * initial.size(), 0);
    p.rdargs = rdargs;
    for (size_t i = 0; i < initial.size(); i++)
        poke(p.array + 4 * i, initial[i]);
    input = line;
    input_pos = 0;
    printed.clear();
    process.pr_Result2 = 0;
    poison_free_heap();
    p.live_before = live_blocks;
    p.heap_before = heap_free_bytes();

    std::vector<UBYTE> before(fx.ctx->mem, fx.ctx->mem + fx.ctx->mem_size);
    p.handle = fx.call(LVO_READARGS, p.tmpl, p.array, rdargs);
    p.error = process.pr_Result2;
    p.block = result_block(p.handle);
    for (size_t i = 0; i < initial.size(); i++)
        p.values.push_back(peek(p.array + 4 * i));
    p.stray = stray_writes(before, p);
    check(p.stray == 0, "ReadArgs() writes only its array, its result block and CS_CurChr",
          p.stray);
    check(printed.empty() || line.find('?') != std::string::npos,
          "ReadArgs() prints only for a '?'", printed.size());
    return p;
}

static void release(Parse &p)
{
    Kaffeine_HeapFree(fx.ctx, p.array, 0);
    Kaffeine_HeapFree(fx.ctx, p.tmpl, 0);
}

/* FreeArgs() returns the native memory and the guest block. */
static void free_args(Parse &p)
{
    fx.call(LVO_FREEARGS, p.handle);
    check(live_blocks == p.live_before, "FreeArgs() releases the native RDArgs",
          live_blocks, p.live_before);
    check(heap_free_bytes() == p.heap_before, "FreeArgs() returns the guest block",
          heap_free_bytes(), p.heap_before);
    release(p);
}

/* Does the result lie in the block that FreeArgs() releases? */
static bool in_block(const Parse &p, ULONG addr, ULONG bytes)
{
    const ULONG block = p.block - KAFFEINE_HEAP_HDR;

    return p.block && addr >= p.block && addr + bytes <= block + peek(block);
}

static bool guest_string_is(const Parse &p, ULONG addr, const std::string &want)
{
    return in_block(p, addr, want.size() + 1) && (addr & 3) == 0 && read_string(addr) == want;
}

static void test_table()
{
    check(dos_thunks[0].lvo == LVO_GETPROGRAMNAME && dos_thunks[0].thunk == Kaffeine_GetProgramName &&
          dos_thunks[1].lvo == LVO_READARGS && dos_thunks[1].thunk == Kaffeine_ReadArgs &&
          dos_thunks[2].lvo == LVO_FREEARGS && dos_thunks[2].thunk == Kaffeine_FreeArgs &&
          dos_thunks[3].thunk == nullptr,
          "the DOS table registers -576, -798 and -858");
}

/* The template and the command line of the dos_readargs target test */
static void test_cli_tool()
{
    Parse p = read_args("FILES/M,SIZE/K/N,NAME/K,QUIET/S", "alpha beta SIZE=42 NAME=hello QUIET\n",
                        { 0, 0, 0, 0 });

    check(p.handle != 0 && p.block == p.handle,
          "ReadArgs() accepts the dos_readargs command line, and the handle is its block",
          p.error);
    bool cleared = in_block(p, p.handle, RDA_SIZE);
    for (ULONG i = 0; i < RDA_SIZE; i += 4)
        cleared = cleared && peek(p.handle + i) == 0;
    check(cleared, "the handle is a cleared 68k RDArgs", p.handle);

    ULONG files = p.values[0];
    check(in_block(p, files, 12) && (files & 3) == 0, "FILES/M is a guest array", files);
    check(guest_string_is(p, peek(files), "alpha"), "FILES/M first name", peek(files));
    check(guest_string_is(p, peek(files + 4), "beta"), "FILES/M second name", peek(files + 4));
    check(peek(files + 8) == 0, "FILES/M ends with NULL", peek(files + 8));

    ULONG size = p.values[1];
    check(in_block(p, size, 4) && (size & 3) == 0 && peek(size) == 42,
          "SIZE/K/N points to the LONG 42", size, peek(size));
    check(guest_string_is(p, p.values[2], "hello"), "NAME/K is the guest string \"hello\"", p.values[2]);
    check(p.values[3] == 0xFFFFFFFF, "QUIET/S is DOSTRUE", p.values[3]);
    free_args(p);
}

/* An absent argument keeps the program's default; only /M is cleared, as
   by the native ReadArgs(). */
static void test_defaults()
{
    Parse p = read_args("FILE,SIZE/K/N,NAME/K,QUIET/S,FILES/M,REST/F", "\n",
                        { 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666 });

    check(p.handle != 0, "ReadArgs() accepts an empty command line", p.error);
    check(p.values[0] == 0x11111111 && p.values[1] == 0x22222222 && p.values[2] == 0x33333333 &&
          p.values[3] == 0x44444444 && p.values[5] == 0x66666666,
          "absent arguments keep the program's values");
    check(p.values[4] == 0, "an absent /M argument becomes NULL", p.values[4]);
    free_args(p);
}

static void test_toggles()
{
    Parse p = read_args("VERBOSE/T,DEBUG/T,SAFE/T", "VERBOSE DEBUG\n", { 0, 0xFFFFFFFF, 0xFFFFFFFF });

    check(p.handle != 0, "ReadArgs() accepts two toggles", p.error);
    check(p.values[0] == 0xFFFFFFFF, "VERBOSE/T turns 0 into DOSTRUE", p.values[0]);
    check(p.values[1] == 0, "DEBUG/T turns DOSTRUE into 0", p.values[1]);
    check(p.values[2] == 0xFFFFFFFF, "an absent /T keeps its value", p.values[2]);
    free_args(p);
}

static void test_numbers()
{
    const LONG want[] = { 1, -2, 2147483647, -2147483647 };
    Parse p = read_args("VALUES/M/N,OFFSET/K/N,LIMIT/N", "1 -2 2147483647 -2147483647 OFFSET=-7\n",
                        { 0, 0, 0x77777777 });

    check(p.handle != 0, "ReadArgs() accepts the numbers", p.error);
    ULONG values = p.values[0];
    bool ok = in_block(p, values, 20) && (values & 3) == 0;
    for (ULONG i = 0; ok && i < 4; i++)
    {
        ULONG number = peek(values + 4 * i);
        ok = in_block(p, number, 4) && (number & 3) == 0 && (LONG)peek(number) == want[i];
    }
    check(ok && peek(values + 16) == 0, "VALUES/M/N is a guest array of LONGs", values);
    check(in_block(p, p.values[1], 4) && (LONG)peek(p.values[1]) == -7, "OFFSET/K/N is -7", p.values[1]);
    check(p.values[2] == 0x77777777, "an absent /N keeps its value", p.values[2]);
    free_args(p);
}

static void test_strings()
{
    Parse p = read_args("FROM/A,TO/K,REST/F", "\"two words\" TO=dest the rest of it\n", { 0, 0, 0 });

    check(p.handle != 0, "ReadArgs() accepts a quoted name and /F", p.error);
    check(guest_string_is(p, p.values[0], "two words"), "FROM/A drops the quotes", p.values[0]);
    check(guest_string_is(p, p.values[1], "dest"), "TO/K", p.values[1]);
    check(guest_string_is(p, p.values[2], "the rest of it"), "REST/F takes the rest of the line",
          p.values[2]);
    free_args(p);
}

/* Names of 1 to 7 characters exercise the rounding of every string. */
static void test_many_names()
{
    std::vector<std::string> names;
    std::string line;

    for (int i = 0; i < 40; i++)
    {
        names.push_back(std::string(1 + i % 7, 'a' + i % 26));
        line += names.back() + " ";
    }
    line += "\n";
    check(line.size() < 256, "the command line fits the native line buffer", line.size());

    Parse p = read_args("FILES/M/A", line, { 0 });
    check(p.handle != 0, "ReadArgs() accepts 40 names", p.error);
    ULONG files = p.values[0];
    bool ok = in_block(p, files, 4 * 41);
    for (ULONG i = 0; ok && i < names.size(); i++)
        ok = guest_string_is(p, peek(files + 4 * i), names[i]);
    check(ok && peek(files + 4 * 40) == 0, "FILES/M holds the 40 names", files);
    free_args(p);
}

/* The options are read like the native parser reads them, letters in
   either case, and one item more than there are commas. */
static void test_template_items()
{
    Parse p = read_args("name/k,size/n,q/s", "NAME=x SIZE=5 Q\n", { 0, 0, 0 });

    check(p.handle != 0, "ReadArgs() accepts lowercase options", p.error);
    check(guest_string_is(p, p.values[0], "x") && in_block(p, p.values[1], 4) &&
          peek(p.values[1]) == 5 && p.values[2] == 0xFFFFFFFF,
          "lowercase /k, /n and /s work", p.values[0], p.values[1]);
    free_args(p);

    p = read_args("FIRST/,SECOND", "one two\n", { 0, 0, 0x33333333 });
    check(guest_string_is(p, p.values[0], "one") && guest_string_is(p, p.values[1], "two"),
          "a '/' before a comma still ends the item", p.values[0], p.values[1]);
    check(p.values[2] == 0x33333333, "nothing is written after the last item", p.values[2]);
    free_args(p);
}

/* A failed ReadArgs() leaves the array alone and keeps nothing. */
static void test_failures()
{
    static const struct { const char *tmpl, *line; SIPTR error; } cases[] =
    {
        { "NAME/A", "\n", ERROR_REQUIRED_ARG_MISSING },
        { "SIZE/N", "lots\n", ERROR_BAD_NUMBER },
        { "NAME", "one two\n", ERROR_TOO_MANY_ARGS },
        { "NAME/K", "NAME\n", ERROR_KEY_NEEDS_ARG },
    };

    for (const auto &c : cases)
    {
        Parse p = read_args(c.tmpl, c.line, { 0x12345678 });
        check(p.handle == 0 && p.error == c.error, c.tmpl, p.handle, p.error);
        check(p.values[0] == 0x12345678, "a failed ReadArgs() leaves the array alone", p.values[0]);
        check(live_blocks == p.live_before && heap_free_bytes() == p.heap_before,
              "a failed ReadArgs() keeps no memory", live_blocks, p.live_before);
        release(p);
    }
}

/* Calls that the native ReadArgs() must not see */
static void test_bad_calls()
{
    ULONG tmpl = guest_string("A,B"), many = guest_string(std::string(256, ','));
    ULONG array = Kaffeine_HeapAlloc(fx.ctx, 8, 0);
    long live = live_blocks;

    input = "x y\n";
    input_pos = 0;
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, tmpl, 0) == 0 && IoErr() == ERROR_BAD_TEMPLATE,
          "a NULL array is refused", IoErr());
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, tmpl, fx.ctx->mem_size - 4) == 0 && IoErr() == ERROR_BAD_TEMPLATE,
          "an array past the end of guest memory is refused", IoErr());
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, many, array) == 0 && IoErr() == ERROR_BAD_TEMPLATE,
          "a template of 257 items is refused", IoErr());
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, 0, array) == 0 && IoErr() == ERROR_BAD_TEMPLATE,
          "a NULL template is refused", IoErr());
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, tmpl, array, fx.ctx->mem_size - RDA_SIZE + 4) == 0 &&
          IoErr() == ERROR_BAD_TEMPLATE,
          "an RDArgs past the end of guest memory is refused", IoErr());

    /* A template without its terminator in guest memory */
    const ULONG end = fx.ctx->mem_size - 16;
    std::vector<UBYTE> saved(fx.ctx->mem + end, fx.ctx->mem + fx.ctx->mem_size);
    std::memset(fx.ctx->mem + end, 'A', 16);
    process.pr_Result2 = 0;
    check(fx.call(LVO_READARGS, end, array) == 0 && IoErr() == ERROR_BAD_TEMPLATE,
          "a template that runs off the end of guest memory is refused", IoErr());
    std::memcpy(fx.ctx->mem + end, saved.data(), saved.size());

    check(input_pos == 0 && live_blocks == live, "the refused calls read no arguments",
          input_pos, live_blocks - live);
    Kaffeine_HeapFree(fx.ctx, array, 0);
    Kaffeine_HeapFree(fx.ctx, many, 0);
    Kaffeine_HeapFree(fx.ctx, tmpl, 0);
}

/* Up to KAFFEINE_MAX_RDARGS results can be held at once. */
static void test_slots()
{
    std::vector<Parse> held;

    for (int i = 0; i < KAFFEINE_MAX_RDARGS; i++)
        held.push_back(read_args("NAME", "n" + std::to_string(i) + "\n", { 0 }));
    bool ok = true;
    for (int i = 0; i < KAFFEINE_MAX_RDARGS; i++)
        ok = ok && guest_string_is(held[i], held[i].values[0], "n" + std::to_string(i));
    check(ok, "eight results are held at once, each in its own block");

    Parse extra = read_args("NAME", "x\n", { 0 });
    check(extra.handle == 0 && extra.error == ERROR_NO_FREE_STORE, "a ninth one fails",
          extra.handle, extra.error);
    check(extra.values[0] == 0 && live_blocks == extra.live_before &&
          heap_free_bytes() == extra.heap_before, "and keeps nothing", live_blocks, extra.live_before);
    release(extra);

    for (int i = KAFFEINE_MAX_RDARGS - 1; i >= 0; i--)
        free_args(held[i]);
}

static void test_free_twice()
{
    Parse p = read_args("NAME", "x\n", { 0 });
    const ULONG handle = p.handle;

    free_args(p);
    long live = live_blocks;
    ULONG heap = heap_free_bytes();
    fx.call(LVO_FREEARGS, handle);
    fx.call(LVO_FREEARGS, 0);
    check(live_blocks == live && heap_free_bytes() == heap,
          "FreeArgs() of a released handle or NULL does nothing");
}

/* What a program leaves is freed when its context ends. */
static void test_cleanup()
{
    long live = live_blocks;
    Parse a = read_args("FILES/M", "a b c\n", { 0 });
    Parse b = read_args("NAME/K", "NAME=x\n", { 0 });

    check(guest_string_is(a, peek(a.values[0] + 8), "c") && guest_string_is(b, b.values[0], "x"),
          "a second ReadArgs() keeps the first results", a.values[0]);
    check(live_blocks > live, "both hold native memory", live_blocks, live);
    Kaffeine_ReadArgsCleanup(fx.ctx);
    check(live_blocks == live, "Kaffeine_ReadArgsCleanup() frees it", live_blocks, live);
    fx.call(LVO_FREEARGS, a.handle);
    check(live_blocks == live, "FreeArgs() after the cleanup does nothing", live_blocks, live);
    Kaffeine_HeapFree(fx.ctx, b.handle, 0);
    Kaffeine_HeapFree(fx.ctx, a.handle, 0);
    release(b);
    release(a);
}

/* AllocDosObject(DOS_RDARGS) gives the program a cleared 68k RDArgs, and
   FreeDosObject() returns it to the guest heap. */
static ULONG alloc_rdargs()
{
    poison_free_heap();
    ULONG rda = Kaffeine_AllocRDArgs(fx.ctx);
    bool cleared = rda != 0 && (rda & 3) == 0;
    for (ULONG i = 0; cleared && i < RDA_SIZE; i += 4)
        cleared = peek(rda + i) == 0;
    check(cleared, "Kaffeine_AllocRDArgs() gives a cleared 68k RDArgs", rda);
    return rda;
}

static void test_alloc_rdargs()
{
    ULONG heap = heap_free_bytes();
    long live = live_blocks;
    ULONG rda = alloc_rdargs();

    check(heap_free_bytes() < heap && live_blocks == live,
          "the 68k RDArgs comes from the guest heap", heap_free_bytes(), heap);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
    Kaffeine_FreeRDArgs(fx.ctx, 0);
    check(heap_free_bytes() == heap, "Kaffeine_FreeRDArgs() returns it", heap_free_bytes(), heap);
}

/* With RDA_Source the arguments come from the program's string, not from
   Input(), and the position in the string goes back to the program. */
static void test_caller_source()
{
    static const char line[] = "gamma delta SIZE=7 NAME=x\n";
    ULONG heap = heap_free_bytes();
    ULONG rda = alloc_rdargs(), text = guest_string(line);

    poke(rda + RDA_CS_BUFFER, text);
    poke(rda + RDA_CS_LENGTH, sizeof(line) - 1);
    poke(rda + RDA_FLAGS, RDAF_NOPROMPT);
    Parse p = read_args("FILES/M,SIZE/K/N,NAME/K", "the input stays unread\n", { 0, 0, 0 }, rda);

    check(p.handle == rda && p.block != 0, "ReadArgs() returns the program's RDArgs", p.handle);
    check(input_pos == 0, "the parser does not read Input()", input_pos);
    ULONG files = p.values[0];
    check(in_block(p, files, 12) && guest_string_is(p, peek(files), "gamma") &&
          guest_string_is(p, peek(files + 4), "delta") && peek(files + 8) == 0,
          "FILES/M comes from RDA_Source", files);
    check(in_block(p, p.values[1], 4) && peek(p.values[1]) == 7 &&
          guest_string_is(p, p.values[2], "x"), "SIZE/K/N and NAME/K come from RDA_Source");

    /* The native parser on the same source gives the position */
    std::string copy = line;
    auto *ref = static_cast<RDArgs *>(AllocDosObject(DOS_RDARGS, nullptr));
    SIPTR ref_array[3] = {};
    ref->RDA_Source.CS_Buffer = reinterpret_cast<UBYTE *>(&copy[0]);
    ref->RDA_Source.CS_Length = (LONG)copy.size();
    ref->RDA_Flags = RDAF_NOPROMPT;
    bool ref_ok = ReadArgs("FILES/M,SIZE/K/N,NAME/K", ref_array, ref) != nullptr;
    const LONG want = ref->RDA_Source.CS_CurChr;
    FreeArgs(ref);
    FreeDosObject(DOS_RDARGS, ref);
    check(ref_ok && want > 0 && peek(rda + RDA_CS_CURCHR) == (ULONG)want,
          "CS_CurChr goes back to the program", peek(rda + RDA_CS_CURCHR), want);
    check(peek(rda + RDA_CS_BUFFER) == text && peek(rda + RDA_CS_LENGTH) == sizeof(line) - 1 &&
          peek(rda + RDA_DALIST) == 0 && peek(rda + RDA_FLAGS) == RDAF_NOPROMPT,
          "the rest of the program's RDArgs is unchanged");
    free_args(p);

    /* FreeArgs() twice on the program's RDArgs is allowed */
    long live = live_blocks;
    fx.call(LVO_FREEARGS, rda);
    check(live_blocks == live, "a second FreeArgs() of the program's RDArgs does nothing");
    Kaffeine_HeapFree(fx.ctx, text, 0);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
    check(heap_free_bytes() == heap, "FreeDosObject() returns the 68k RDArgs", heap_free_bytes(), heap);
}

/* Without RDA_Source the program's RDArgs reads Input(), and a second '?'
   shows its RDA_ExtHelp, a guest string. */
static void test_caller_help()
{
    ULONG rda = alloc_rdargs(), help = guest_string("Give the program a name");

    poke(rda + RDA_EXTHELP, help);
    Parse p = read_args("NAME/A", "?\n?\nNAME=x\n", { 0 }, rda);
    check(p.handle == rda && guest_string_is(p, p.values[0], "x"),
          "ReadArgs() reads the answer after the prompts", p.handle, p.error);
    check(printed.size() == 4 && printed[0] == "NAME/A" && printed[2] == "Give the program a name",
          "the template, then RDA_ExtHelp, is shown", printed.size());
    free_args(p);
    Kaffeine_HeapFree(fx.ctx, help, 0);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
}

/* Neither the private flag of ReadArgs() nor RDAF_NOALLOC reaches the
   native RDArgs: the native FreeArgs() would free the RDArgs itself, or
   the program's buffer as its own. */
static void test_caller_flags()
{
    static const char line[] = "one two\n";
    ULONG rda = alloc_rdargs(), text = guest_string(line);
    ULONG buffer = Kaffeine_HeapAlloc(fx.ctx, 64, 0);

    poke(rda + RDA_CS_BUFFER, text);
    poke(rda + RDA_CS_LENGTH, sizeof(line) - 1);
    poke(rda + RDA_BUFFER, buffer);
    poke(rda + RDA_BUFSIZ, 64);
    poke(rda + RDA_FLAGS, 0x80000000 | RDAF_NOALLOC | RDAF_NOPROMPT);
    Parse p = read_args("A,B", "", { 0, 0 }, rda);
    check(p.handle == rda && guest_string_is(p, p.values[0], "one") &&
          guest_string_is(p, p.values[1], "two"),
          "ReadArgs() works with the private flag and RDAF_NOALLOC set", p.handle, p.error);
    free_args(p);
    Kaffeine_HeapFree(fx.ctx, buffer, 0);
    Kaffeine_HeapFree(fx.ctx, text, 0);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
}

/* A source that does not lie in guest memory is refused before the native
   function sees it. */
static void test_caller_invalid()
{
    ULONG rda = alloc_rdargs(), text = guest_string("x\n");
    const struct { ULONG buffer; LONG length, current; const char *what; } cases[] =
    {
        { fx.ctx->mem_size + 16, 2, 0, "a CS_Buffer outside guest memory is refused" },
        { fx.ctx->mem_size - 2, 8, 0, "a CS_Length past the end of guest memory is refused" },
        { text, -1, 0, "a negative CS_Length is refused" },
        { text, 2, 3, "a CS_CurChr past CS_Length is refused" },
        { text, 2, -1, "a negative CS_CurChr is refused" },
    };

    for (const auto &c : cases)
    {
        poke(rda + RDA_CS_BUFFER, c.buffer);
        poke(rda + RDA_CS_LENGTH, (ULONG)c.length);
        poke(rda + RDA_CS_CURCHR, (ULONG)c.current);
        Parse p = read_args("NAME", "y\n", { 0x12345678 }, rda);
        check(p.handle == 0 && p.error == ERROR_BAD_TEMPLATE && p.values[0] == 0x12345678 &&
              input_pos == 0 && live_blocks == p.live_before, c.what, p.handle, p.error);
        release(p);
    }
    Kaffeine_HeapFree(fx.ctx, text, 0);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
}

/* A failed ReadArgs() with the program's RDArgs frees the native one. */
static void test_caller_failure()
{
    static const char line[] = "\n";
    ULONG rda = alloc_rdargs(), text = guest_string(line);

    poke(rda + RDA_CS_BUFFER, text);
    poke(rda + RDA_CS_LENGTH, sizeof(line) - 1);
    poke(rda + RDA_FLAGS, RDAF_NOPROMPT);
    Parse p = read_args("NAME/A", "", { 0x12345678 }, rda);
    check(p.handle == 0 && p.error == ERROR_REQUIRED_ARG_MISSING && p.values[0] == 0x12345678,
          "a missing argument in RDA_Source fails", p.handle, p.error);
    check(live_blocks == p.live_before && heap_free_bytes() == p.heap_before,
          "and keeps no memory", live_blocks, p.live_before);
    release(p);
    Kaffeine_HeapFree(fx.ctx, text, 0);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
}

/* FreeDosObject() without FreeArgs() first releases the results too, and
   on the handle of ReadArgs() without an RDArgs it frees the block once. */
static void test_free_held()
{
    static const char line[] = "x\n";
    ULONG heap = heap_free_bytes();
    long live = live_blocks;
    ULONG rda = alloc_rdargs(), text = guest_string(line);

    poke(rda + RDA_CS_BUFFER, text);
    poke(rda + RDA_CS_LENGTH, sizeof(line) - 1);
    Parse p = read_args("NAME", "", { 0 }, rda);
    check(p.handle == rda, "ReadArgs() holds results for the program's RDArgs", p.handle);
    Kaffeine_FreeRDArgs(fx.ctx, rda);
    release(p);
    Kaffeine_HeapFree(fx.ctx, text, 0);
    check(live_blocks == live && heap_free_bytes() == heap,
          "FreeDosObject() releases results that FreeArgs() did not", live_blocks, live);

    Parse q = read_args("NAME", "y\n", { 0 });
    Kaffeine_FreeRDArgs(fx.ctx, q.handle);
    release(q);
    check(live_blocks == live && heap_free_bytes() == heap,
          "FreeDosObject() of a ReadArgs() handle frees its block once", live_blocks, live);
}

/* GetProgramName() into a guest buffer of len bytes, which starts with a
   pattern; returns D0, and the buffer and IoErr() after the call. */
static ULONG program_name(LONG len, std::string &got, SIPTR &error, ULONG buf = 0)
{
    const bool own = !buf;

    if (own)
    {
        buf = Kaffeine_HeapAlloc(fx.ctx, 64, 0);
        std::memset(fx.ctx->mem + buf, 'Z', 64);
    }
    process.pr_Result2 = 12345;
    ULONG result = fx.call(LVO_GETPROGRAMNAME, buf, (ULONG)len);
    error = process.pr_Result2;
    got = std::string(reinterpret_cast<const char *>(fx.ctx->mem + buf),
                      strnlen(reinterpret_cast<const char *>(fx.ctx->mem + buf), 64));
    if (own)
        Kaffeine_HeapFree(fx.ctx, buf, 0);
    return result;
}

/* The name of the Shell's command, as the autodoc of GetProgramName()
   describes: a name that does not fit with its terminator is cut and the
   call fails with ERROR_LINE_TOO_LONG. */
static void test_program_name()
{
    static const char name[] = "SYS:EMU68/c_stdio";
    const LONG n = sizeof(name) - 1;
    std::string got;
    SIPTR error;

    std::strcpy(fx.ctx->program_name, name);
    check(program_name(64, got, error) == 0xFFFFFFFF && got == name && error == 12345,
          "GetProgramName() copies the name and returns DOSTRUE", error);
    check(program_name(n + 1, got, error) == 0xFFFFFFFF && got == name,
          "a buffer of exactly the name and its terminator is enough", error);
    check(program_name(n, got, error) == 0 && got == std::string(name, n - 1) &&
          error == ERROR_LINE_TOO_LONG,
          "a buffer one byte short cuts the name and fails", error);
    check(program_name(5, got, error) == 0 && got == "SYS:" && error == ERROR_LINE_TOO_LONG,
          "a buffer of five bytes gets four characters", error);
    check(program_name(1, got, error) == 0 && got.empty() && error == ERROR_LINE_TOO_LONG,
          "a buffer of one byte gets the terminator only", error);

    ULONG buf = Kaffeine_HeapAlloc(fx.ctx, 64, 0);
    std::memset(fx.ctx->mem + buf, 'Z', 64);
    check(program_name(0, got, error, buf) == 0 && fx.ctx->mem[buf] == 'Z' &&
          error == ERROR_LINE_TOO_LONG, "a buffer of no bytes is not written", error);
    check(program_name(-1, got, error, buf) == 0 && fx.ctx->mem[buf] == 'Z',
          "a negative length is refused", error);
    Kaffeine_HeapFree(fx.ctx, buf, 0);

    const ULONG end = fx.ctx->mem_size - 8;
    const UBYTE saved = fx.ctx->mem[end];
    fx.ctx->mem[end] = 'Z';
    check(program_name(64, got, error, end) == 0 && fx.ctx->mem[end] == 'Z',
          "a buffer past the end of guest memory is refused", error);
    fx.ctx->mem[end] = saved;

    fx.ctx->program_name[0] = 0;
    check(program_name(64, got, error) == 0 && got.empty() && error == ERROR_OBJECT_WRONG_TYPE,
          "without a name the buffer is empty and the call fails", error);
}

static void test_guest_program_name()
{
    ULONG cli = Kaffeine_HeapAlloc(fx.ctx, 64, 0);
    ULONG buffer = Kaffeine_HeapAlloc(fx.ctx, 64, 0);
    ULONG name = fx.ctx->mem_size - 4;
    std::string got;
    SIPTR error;
    fx.ctx->mem[name] = 3;
    std::memcpy(fx.ctx->mem + name + 1, "end", 3);
    poke(cli + 16, name >> 2);
    fx.ctx->m68k_cli = cli;
    check(program_name(16, got, error, buffer) == 0xFFFFFFFF && got == "end" && error == 12345,
          "GetProgramName honors counted guest CLI BSTR at the arena end without C NUL", error);
    fx.ctx->mem[name] = 4;
    fx.ctx->mem[buffer] = 'Z';
    check(program_name(16, got, error, buffer) == 0 && fx.ctx->mem[buffer] == 'Z' &&
          error == ERROR_BAD_NUMBER, "GetProgramName refuses a BSTR one byte past guest memory", error);
    fx.ctx->mem[name] = 3;
    for (ULONG bad : { 0u, 0x40000000u, 0xFFFFFFFFu, fx.ctx->mem_size >> 2 })
    {
        poke(cli + 16, bad);
        fx.ctx->mem[buffer] = 'Z';
        check(program_name(16, got, error, buffer) == 0 && fx.ctx->mem[buffer] == 'Z' &&
              error == ERROR_BAD_NUMBER, "GetProgramName refuses invalid and wrapping CLI BPTRs", error);
    }
    poke(cli + 16, name >> 2);
    for (ULONG bad : { fx.ctx->mem_size - 63, fx.ctx->mem_size, 0xFFFFFFFEu })
    {
        fx.ctx->m68k_cli = bad;
        fx.ctx->mem[buffer] = 'Z';
        check(program_name(16, got, error, buffer) == 0 && fx.ctx->mem[buffer] == 'Z' &&
              error == ERROR_BAD_NUMBER, "GetProgramName checks the whole guest CLI before reading its field", error);
    }
    fx.ctx->m68k_cli = fx.ctx->mem_size - 64;
    poke(fx.ctx->m68k_cli + 16, name >> 2);
    check(program_name(16, got, error, buffer) == 0xFFFFFFFF && got == "end",
          "GetProgramName accepts complete CLI ending exactly at guest boundary", error);
    fx.ctx->m68k_cli = cli;
    fx.ctx->mem[name] = 0;
    check(program_name(16, got, error, buffer) == 0 && got.empty() && error == ERROR_OBJECT_WRONG_TYPE,
          "GetProgramName preserves the missing-name result for empty CLI BSTR", error);
    fx.ctx->m68k_cli = 0;
    Kaffeine_HeapFree(fx.ctx, buffer, 64);
    Kaffeine_HeapFree(fx.ctx, cli, 64);
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--program-name-only") == 0)
    {
        test_program_name();
        test_guest_program_name();
        std::printf("Program name thunks: %u/%u checks passed\n", checks - failures, checks);
        return failures ? 1 : 0;
    }
    if (argc != 1)
        return 2;
    test_table();
    test_cli_tool();
    test_defaults();
    test_toggles();
    test_numbers();
    test_strings();
    test_many_names();
    test_template_items();
    test_failures();
    test_bad_calls();
    test_slots();
    test_free_twice();
    test_cleanup();
    test_alloc_rdargs();
    test_caller_source();
    test_caller_help();
    test_caller_flags();
    test_caller_invalid();
    test_caller_failure();
    test_free_held();
    test_program_name();
    test_guest_program_name();
    std::printf("ReadArgs thunks: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

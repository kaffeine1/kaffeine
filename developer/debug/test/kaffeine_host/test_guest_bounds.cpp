/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Small arenas make partial accesses visible to ASan. The CPU loop and
    dispatcher are linked as C objects without unwind tables.
*/
#include "kaffeine_intern.h"
#include "kaffeine_cpu.h"
#include <aros/debug.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static std::string fault_log;
static void capture_bug(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fault_log += line;
}

#include "Moira/Moira.cpp"
#include "Moira/MoiraDebugger.cpp"
#define bug capture_bug
#include "kaffeine_moira.cpp"
#undef bug

static int failures;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; return; \
} } while (0)

struct Guest
{
    std::vector<UBYTE> bytes;
    KaffeineContext ctx = {};

    explicit Guest(ULONG size = 1024) : bytes(size, 0xA5)
    {
        ctx.mem = bytes.data();
        ctx.mem_size = size;
        ctx.entry_point = 0x200;
        ctx.stack_top = size & ~3u;
    }
};

static void accessors()
{
    for (ULONG size : { 0u, 1u, 2u, 3u, 4u, 17u })
    {
        Guest g(size);
        const ULONG addresses[] = { 0, 1, 2, 3, size ? size - 1 : 0,
            size >= 2 ? size - 2 : 0, size >= 4 ? size - 4 : 0,
            size, size + 1, 0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu };
        for (ULONG addr : addresses)
        {
            for (ULONG width : { 2u, 4u })
            {
                std::fill(g.bytes.begin(), g.bytes.end(), 0xA5);
                bool valid = (unsigned long long)addr + width <= size;
                ULONG read = width == 2 ? m68k_read16(&g.ctx, addr) : m68k_read32(&g.ctx, addr);
                CHECK(read == (valid ? (width == 2 ? 0xA5A5u : 0xA5A5A5A5u) : 0));
                auto expected = g.bytes;
                if (valid)
                    for (ULONG i = 0; i < width; i++) expected[addr + i] = 0x5A;
                if (width == 2) m68k_write16(&g.ctx, addr, 0x5A5A);
                else m68k_write32(&g.ctx, addr, 0x5A5A5A5A);
                CHECK(g.bytes == expected);
            }
        }
    }
    std::puts("Accessors: tiny arenas, last valid byte, partial and wrapping ranges: PASS");
}

static void callbacks()
{
    Guest g(17);
    AROSMoira cpu(&g.ctx);
    for (ULONG size : { 0u, 1u, 2u, 3u, 4u, 17u })
    {
        g.ctx.mem_size = size;
        const ULONG addresses[] = { 0, 1, 2, size ? size - 1 : 0,
            size >= 2 ? size - 2 : 0, size, size + 1,
            0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu };
        for (ULONG addr : addresses)
        {
            std::fill(g.bytes.begin(), g.bytes.end(), 0xA5);
            bool byte_valid = addr < size;
            bool word_valid = (unsigned long long)addr + 2 <= size;
            CHECK(cpu.read8(addr) == (byte_valid ? 0xA5 : 0));
            CHECK(cpu.read16(addr) == (word_valid ? 0xA5A5 : 0));
            CHECK(cpu.read16OnReset(addr) == (word_valid ? 0xA5A5 : 0));
            CHECK(cpu.read16Dasm(addr) == (word_valid ? 0xA5A5 : 0));
            auto expected = g.bytes;
            if (word_valid) expected[addr] = expected[addr + 1] = 0x5A;
            cpu.write16(addr, 0x5A5A);
            CHECK(g.bytes == expected);
            if (byte_valid) expected[addr] = 0x3C;
            cpu.write8(addr, 0x3C);
            CHECK(g.bytes == expected);
        }
    }
    /* Special addresses retain priority even with an arena smaller than them. */
    CHECK(cpu.read16(CUSTOM_BASE + 0x16) == POTGOR_DEFAULT);
    CHECK(cpu.read8(CUSTOM_BASE + 0x16) == 0xFF);
    cpu.write16(CUSTOM_BASE + 0x2C, 0x1208);
    CHECK(g.ctx.beam_v == 0x12 && g.ctx.beam_h == 8);
    cpu.write8(CUSTOM_BASE + 0x2C, 0);
    CHECK(g.ctx.beam_v == 0x12 && g.ctx.beam_h == 8);
    CHECK(cpu.read16OnReset(CUSTOM_BASE + 0x16) == 0);
    CHECK(cpu.read16Dasm(CUSTOM_BASE + 0x16) == 0);
    for (ULONG addr : { SENTINEL_ADDR, SENTINEL_ADDR + 1 })
    {
        g.ctx.running = TRUE;
        CHECK(cpu.read16(addr) == M68K_OP_STOP && !g.ctx.running);
        CHECK(cpu.read16OnReset(addr) == 0 && cpu.read16Dasm(addr) == 0);
    }
    std::puts("Moira callbacks: bounds, reset/disassembly, custom chips and sentinel: PASS");
}

/* The 68EC040 decodes 32 address bits: the custom chips answer at $DFF000
   only, and an arena past 16 MB keeps its RAM at $1DFF000. The sentinel
   lies below the heap: the old one at $DEAD00 is memory. */
static void custom_window()
{
    Guest g(0x02000000);  /* the size of Kaffeine_CreateContext() */
    AROSMoira cpu(&g.ctx);
    const ULONG alias = 0x01000000 + CUSTOM_BASE;

    g.ctx.running = TRUE;
    CHECK(cpu.read16(0x00DEAD00) == 0xA5A5 && g.ctx.running);
    CHECK(SENTINEL_ADDR + 2 <= KAFFEINE_HEAP_BASE);
    CHECK(cpu.read16(alias + 0x16) == 0xA5A5 && cpu.read8(alias + 0x16) == 0xA5);
    cpu.write16(alias + 0x2C, 0x1208);
    cpu.write8(alias + 0x2E, 0x3C);
    CHECK(g.bytes[alias + 0x2C] == 0x12 && g.bytes[alias + 0x2D] == 0x08 &&
          g.bytes[alias + 0x2E] == 0x3C);
    CHECK(g.ctx.beam_v == 0 && g.ctx.beam_h == 0);
    /* The registers keep $DFF000 even where the arena has bytes. */
    CHECK(cpu.read16(CUSTOM_BASE + 0x16) == POTGOR_DEFAULT);
    cpu.write16(CUSTOM_BASE + 0x2C, 0x1208);
    CHECK(g.ctx.beam_v == 0x12 && g.ctx.beam_h == 8 && g.bytes[CUSTOM_BASE + 0x2C] == 0xA5);
    CHECK(cpu.read16(CUSTOM_BASE - 2) == 0xA5A5 && cpu.read16(CUSTOM_BASE + CUSTOM_SIZE) == 0xA5A5);
    /* Above the arena the top byte still counts: no registers there. */
    for (ULONG addr : { 0x02DFF016u, 0x80DFF016u, 0xFFDFF016u })
        CHECK(cpu.read16(addr) == 0 && cpu.read8(addr) == 0);
    std::puts("Custom chips: one window at $DFF000, RAM at $1DFF000 and $DEAD00: PASS");
}

static void execute_bounds()
{
    for (ULONG addr : { 1023u, 1024u, 1025u, 0xFFFFFFFCu, 0xFFFFFFFDu,
                        0xFFFFFFFEu, 0xFFFFFFFFu })
    {
        Guest g;
        g.ctx.stack_top = 0x380;
        ULONG pc = g.ctx.entry_point;
        /* MOVEA.L #addr,A0; MOVE.L (A0),D0; MOVE.L D0,$300;
           MOVE.W #$5678,(A0); MOVE.B #$12,(A0); MOVEQ #42,D0; RTS.
           Use a word read at odd addresses. Moira splits long accesses into
           two word callbacks and wraps the second at the 32-bit boundary. */
        const UWORD code[] = { 0x207C, (UWORD)(addr >> 16), (UWORD)addr,
            (UWORD)((addr & 1) ? 0x3010 : 0x2010), 0x21C0, 0x0300,
            0x30BC, 0x5678, 0x10BC, 0x0012, 0x702A, 0x4E75 };
        for (UWORD word : code) { m68k_write16(&g.ctx, pc, word); pc += 2; }
        CHECK(Kaffeine_Execute(&g.ctx) == 42);
        CHECK(m68k_read32(&g.ctx, 0x300) == 0);
        CHECK(!g.ctx.running && g.ctx.exit_code == 42);
    }
    std::puts("Execute: invalid MOVE.L reads zero and reaches RTS after refused stores: PASS");
}

static void execute_fpu()
{
    Guest g;
    const ULONG pc = g.ctx.entry_point;
    m68k_write16(&g.ctx, pc, 0xF200);      /* FMOVE.L FPCR,D0 */
    m68k_write16(&g.ctx, pc + 2, 0xB000);
    m68k_write16(&g.ctx, pc + 4, 0x702A);  /* must not run */
    m68k_write16(&g.ctx, pc + 6, 0x4E75);
    fault_log.clear();
    bool escaped = false;
    LONG result = 0;
    try { result = Kaffeine_Execute(&g.ctx); }
    catch (...) { escaped = true; }
    CHECK(!escaped);
    CHECK(result == 20 && g.ctx.exit_code == 20 && !g.ctx.running);
    CHECK(fault_log.find("FPU") != std::string::npos);
    CHECK(fault_log.find("Line-F vector 11") != std::string::npos);
    CHECK(fault_log.find("0x00000200") != std::string::npos);
    std::puts("Execute: unsupported FPU returns RETURN_FAIL through Line-F vector 11: PASS");
}

static void fpu_no_throw()
{
    Guest g;
    AROSMoira cpu(&g.ctx);

    /* Call Moira directly, without moira_execute()'s catch. In this pinned
       core a throwing FPU handler rethrows out of execute(), so catching it
       in the adapter cannot satisfy this test. Cover the whole CP-ID-1 area. */
    for (unsigned opcode = 0xF200; opcode <= 0xF3FF; opcode++)
    {
        m68k_write32(&g.ctx, 0, g.ctx.stack_top);
        m68k_write32(&g.ctx, 4, g.ctx.entry_point);
        m68k_write32(&g.ctx, 11 * 4, 0x280);
        m68k_write16(&g.ctx, 0x200, (UWORD)opcode);
        m68k_write16(&g.ctx, 0x202, 0xB000);
        m68k_write16(&g.ctx, 0x280, M68K_OP_NOP);
        cpu.reset();
        g.ctx.running = TRUE;
        fault_log.clear();
        bool threw = false;
        try { cpu.execute(); }
        catch (...) { threw = true; }
        CHECK(!threw && !g.ctx.running);
        CHECK(fault_log.find("FPU") != std::string::npos);
        CHECK(fault_log.find("Line-F vector 11") != std::string::npos);
        CHECK(fault_log.find("0x00000200") != std::string::npos);
    }
    std::puts("512 CP-ID-1 opcodes reach Line-F without the adapter catch or any throw: PASS");
}

static void execute_other_linef()
{
    const struct { UWORD opcode; Instr decoded; } cases[] = {
        { 0xF410, Instr::CINV }, { 0xF430, Instr::CPUSH },
        { 0xF620, Instr::MOVE16 }, { 0xFFFF, Instr::LINE_F }
    };
    for (auto test : cases)
    {
        Guest g;
        AROSMoira cpu(&g.ctx);
        CHECK(cpu.getInstrInfo(test.opcode).I == test.decoded);
        m68k_write16(&g.ctx, 0x200, test.opcode);
        m68k_write16(&g.ctx, 0x202, 0x702A);
        m68k_write16(&g.ctx, 0x204, 0x4E75);
        fault_log.clear();
        CHECK(Kaffeine_Execute(&g.ctx) == 20);
        CHECK(g.ctx.exit_code == 20 && !g.ctx.running);
        CHECK(fault_log.find("unsupported Line-F instruction (vector 11)") != std::string::npos);
        CHECK(fault_log.find("0x00000200") != std::string::npos);
    }
    std::puts("CINV, CPUSH, MOVE16 and unknown Line-F end with RETURN_FAIL: PASS");
}

/* The 68040 PFLUSH (1111 0101 000x xxxx) and PTEST (1111 0101 01x0 1xxx)
   encodings, which Moira decodes only for M68040 and M68LC040. */
static bool is_mmu040(unsigned opcode)
{
    return (opcode & 0xFFE0) == 0xF500 || (opcode & 0xFFF8) == 0xF548 ||
           (opcode & 0xFFF8) == 0xF568;
}

static void model_compatibility()
{
    Guest g;
    AROSMoira cpu(&g.ctx), previous(&g.ctx);
    previous.setModel(Model::M68040);
    CHECK(!cpu.hasFPU() && previous.hasFPU());
    CHECK(!cpu.hasMMU() && previous.hasMMU());
    CHECK(cpu.hasCPI() == previous.hasCPI());
    CHECK(cpu.addrMask() == previous.addrMask());

    /* Both models select the C68020 core. Check the entire instruction
       description table, including CINV/CPUSH and MOVE16 encodings. */
    unsigned mmu = 0;
    for (unsigned opcode = 0; opcode <= 0xFFFF; opcode++)
    {
        auto actual = cpu.getInstrInfo((UWORD)opcode);
        auto old = previous.getInstrInfo((UWORD)opcode);
        if ((opcode & 0xFE00) == 0xF200)
            CHECK(actual.I == Instr::LINE_F);
        else if (is_mmu040(opcode))
        {
            CHECK(actual.I == Instr::LINE_F);
            CHECK(old.I == Instr::PFLUSH || old.I == Instr::PTEST);
            mmu++;
        }
        else
            CHECK(actual.I == old.I && actual.M == old.M && actual.S == old.S);
    }
    CHECK(mmu == 48);
    std::puts("M68EC040 keeps the M68040 decode table without FPU, PFLUSH and PTEST: PASS");
}

/* M68LC040 decoded PFLUSH and PTEST to Moira handlers that throw, and a C++
   exception inside kaffeine.library aborts AROS. Call execute() directly,
   without moira_execute()'s catch, for every encoding. */
static void mmu_no_throw()
{
    Guest g;
    AROSMoira cpu(&g.ctx), old(&g.ctx);
    old.setModel(Model::M68LC040);
    unsigned checked = 0;
    for (unsigned opcode = 0xF500; opcode <= 0xF5FF; opcode++)
    {
        if (!is_mmu040(opcode))
            continue;
        CHECK(old.getInstrInfo((UWORD)opcode).I != Instr::LINE_F);
        CHECK(cpu.getInstrInfo((UWORD)opcode).I == Instr::LINE_F);
        m68k_write32(&g.ctx, 0, g.ctx.stack_top);
        m68k_write32(&g.ctx, 4, g.ctx.entry_point);
        m68k_write32(&g.ctx, 11 * 4, 0x280);
        m68k_write16(&g.ctx, 0x200, (UWORD)opcode);
        m68k_write16(&g.ctx, 0x202, 0x702A);
        m68k_write16(&g.ctx, 0x280, M68K_OP_NOP);
        cpu.reset();
        g.ctx.running = TRUE;
        fault_log.clear();
        bool threw = false;
        try { cpu.execute(); }
        catch (...) { threw = true; }
        CHECK(!threw && !g.ctx.running);
        CHECK(fault_log.find("unsupported Line-F instruction (vector 11)") != std::string::npos);
        CHECK(fault_log.find("0x00000200") != std::string::npos);
        checked++;
    }
    CHECK(checked == 48);

    Guest run;
    m68k_write16(&run.ctx, 0x200, 0xF518);  /* PFLUSHA */
    m68k_write16(&run.ctx, 0x202, 0x702A);  /* must not run */
    m68k_write16(&run.ctx, 0x204, 0x4E75);
    fault_log.clear();
    CHECK(Kaffeine_Execute(&run.ctx) == 20);
    CHECK(run.ctx.exit_code == 20 && !run.ctx.running);
    CHECK(fault_log.find("unsupported Line-F instruction (vector 11)") != std::string::npos);
    std::puts("48 PFLUSH and PTEST opcodes reach Line-F without any throw; PFLUSHA ends with RETURN_FAIL: PASS");
}

/* Address-error emulation is disabled in the pinned Moira, and M68LC040 uses
   its C68020 core. Inject callback failures to cover the otherwise dormant
   exception branches without changing the submodule or production model. */
class FaultMoira : public AROSMoira
{
public:
    enum Kind { ADDRESS, BUS, DOUBLE, DOUBLE_POINTER, HALT, STANDARD, UNKNOWN };
    Kind kind;
    bool inject = false;
    mutable unsigned throws = 0;
    DoubleFault double_fault;

    FaultMoira(KaffeineContext *ctx, Kind fault) : AROSMoira(ctx), kind(fault) {}

    void pendingTrace() { flags |= State::TRACE_EXC; }

    u16 read16(u32 addr) const override
    {
        if (inject)
        {
            throws++;
            switch (kind)
            {
                case ADDRESS: throw AddressError({});
                case BUS: throw BusError({});
                case DOUBLE: throw DoubleFault();
                case DOUBLE_POINTER: throw &double_fault;
                case HALT:
                    if (throws == 1) throw AddressError({});
                    throw DoubleFault();
                case STANDARD: throw std::runtime_error("injected callback failure");
                case UNKNOWN: throw 7;
            }
        }
        return AROSMoira::read16(addr);
    }
};

static void injected_faults()
{
    const char *kinds[] = { "address error", "bus error", "double fault",
        "double fault", "CPU halt", "backend exception", "unknown backend exception" };
    for (unsigned kind = FaultMoira::ADDRESS; kind <= FaultMoira::UNKNOWN; kind++)
    {
        Guest g;
        FaultMoira cpu(&g.ctx, (FaultMoira::Kind)kind);
        /* Moira's address-error stack frame is implemented for C68010.
           Use it only to exercise its internal double-fault halt path. */
        if (kind == FaultMoira::HALT) cpu.setModel(Model::M68010);
        m68k_write32(&g.ctx, 0, g.ctx.stack_top);
        m68k_write32(&g.ctx, 4, g.ctx.entry_point);
        for (ULONG pc = 0x200; pc < 0x208; pc += 2) m68k_write16(&g.ctx, pc, 0x4E71);
        cpu.reset();
        /* Pending trace processing is outside Moira's instruction catch.
           This exposes typed address/bus errors directly to the adapter. */
        if (kind == FaultMoira::ADDRESS || kind == FaultMoira::BUS) cpu.pendingTrace();
        cpu.inject = true;
        fault_log.clear();
        bool escaped = false;
        try { Kaffeine_RunCPU(&g.ctx, &cpu.cpu_handle); }
        catch (...) { escaped = true; }
        CHECK(!escaped && !g.ctx.running);
        CHECK(fault_log.find(kinds[kind]) != std::string::npos);
        if (fault_log.find("0x00000200") == std::string::npos)
            std::printf("Injected fault %u: %s", kind, fault_log.c_str());
        CHECK(fault_log.find("0x00000200") != std::string::npos);
    }
    std::puts("Injected address/bus/double/pointer/standard/unknown faults and halt stay in C++: PASS");
}

int main(int argc, char **argv)
{
    /* Separate modes also allow each old failure to be reproduced under ASan. */
    std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "all" || mode == "accessors") accessors();
    if (mode == "all" || mode == "callbacks") callbacks();
    if (mode == "all" || mode == "custom") custom_window();
    if (mode == "all" || mode == "execute") execute_bounds();
    if (mode == "all" || mode == "fpu") execute_fpu();
    if (mode == "all" || mode == "no-throw") fpu_no_throw();
    if (mode == "all" || mode == "model") model_compatibility();
    if (mode == "all" || mode == "mmu") mmu_no_throw();
    if (mode == "all" || mode == "linef") execute_other_linef();
    if (mode == "all" || mode == "injected") injected_faults();
    std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}

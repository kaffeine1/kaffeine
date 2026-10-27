/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * The production CLI helper and dispatcher are compiled as C. Native Cli()
 * and guest allocation are controlled collaborators; native CLI memory must
 * never be dereferenced or returned to the guest.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <proto/dos.h>
#include "kaffeine_intern.h"
#include "kaffeine_cli.h"
#include "kaffeine_dispatch.h"

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *message)
{
    ++checks;
    if (!passed)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

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

struct Calls
{
    bool native_cli_present = true;
    bool allocation_fails = false;
    unsigned native_cli_calls = 0;
    unsigned allocation_calls = 0;
    unsigned generated_calls = 0;
    ULONG allocation_size = 0;
    ULONG allocation_address = 0x400;
    ULONG allocation_flags = 0;
    KaffeineContext *allocation_context = nullptr;
};

static Calls calls;
static SIPTR cli_error;
extern "C" SIPTR SetIoErr(SIPTR error) { SIPTR old = cli_error; cli_error = error; return old; }
static constexpr ULONG guest_cli = 0x400;

/* Deliberately not a dereferenceable host address. Its low 32 bits also differ
   from the guest address, so native pointer truncation cannot pass the test. */
static CommandLineInterface *const native_cli =
    reinterpret_cast<CommandLineInterface *>(static_cast<IPTR>(0x12345678ABCDEF00ULL));

extern "C" CommandLineInterface *Cli(void)
{
    ++calls.native_cli_calls;
    return calls.native_cli_present ? native_cli : nullptr;
}

extern "C" ULONG Kaffeine_HeapAlloc(KaffeineContext *ctx, ULONG size, ULONG flags)
{
    ++calls.allocation_calls;
    calls.allocation_context = ctx;
    calls.allocation_size = size;
    calls.allocation_flags = flags;
    return calls.allocation_fails ? 0 : calls.allocation_address;
}

static IPTR generated_cli(KaffeineContext *, void *)
{
    ++calls.generated_calls;
    return reinterpret_cast<IPTR>(native_cli);
}

/* This is the same registration included by the production DOS table. */
static const M68KThunkEntry manual_thunks[] = { KAFFEINE_DOS_CLI_THUNKS };
static const M68KThunkEntry generated_thunks[] = { { 492, generated_cli } };

struct Fixture
{
    KaffeineContext ctx = {};
    UBYTE memory[4096];
    TestCPU state;
    KaffeineCPU cpu = { &cpu_ops, &state };

    Fixture()
    {
        calls = {};
        std::memset(memory, 0xA5, sizeof(memory));
        ctx.mem = memory;
        ctx.mem_size = sizeof(memory);
        ctx.seg_start = 0xC00;
        std::strcpy(ctx.program_name, "SYS:Tools/guest-command");
        ctx.num_libs = 1;
        auto &lib = ctx.libs[0];
        lib.m68k_addr = 0x1000;
        lib.jt_start = 0x800;
        std::strcpy(lib.name, "dos.library");
        lib.thunks = manual_thunks;
        lib.num_thunks = sizeof(manual_thunks) / sizeof(manual_thunks[0]);
        lib.gen_thunks = generated_thunks;
        lib.num_gen_thunks = sizeof(generated_thunks) / sizeof(generated_thunks[0]);

        /* Cli() has no arguments. In particular, D1 cannot be mistaken for
           a CreateNewProc() tag list at the neighboring vector. */
        for (unsigned i = 0; i < 8; i++)
        {
            state.d[i] = 0xFE000001u + i;
            state.a[i] = 0xFD000001u + i;
        }
    }

    ULONG dispatch()
    {
        KaffeineDispatchResult result = {};
        state.a[7] = 0x80;
        m68k_write32(&ctx, 0x80, 0x76543210);
        check(Kaffeine_DispatchLibraryCall(&ctx, &cpu, 0x1000 - 492, &result),
              "literal DOS LVO -492 is dispatched");
        check(state.a[7] == 0x84 && result.return_pc == 0x76543210 && !result.supervisor,
              "Cli dispatch consumes one guest return address");
        return state.d[0];
    }
};

static void test_cli_dispatch_and_layout()
{
    Fixture f;
    ULONG cli = f.dispatch();
    check(cli == guest_cli && f.ctx.m68k_cli == cli,
          "Cli returns the cached direct guest address, not a BPTR");
    check(cli != static_cast<ULONG>(reinterpret_cast<IPTR>(native_cli)) &&
          calls.generated_calls == 0,
          "manual Cli prevents the generated native pointer return");
    check(calls.native_cli_calls == 1 && calls.allocation_calls == 1 &&
          calls.allocation_context == &f.ctx && calls.allocation_flags == 0,
          "first Cli allocates one context-owned guest object");
    check(cli % 4 == 0 && cli + calls.allocation_size <= f.ctx.mem_size,
          "CLI and its complete allocation are within guest memory");

    ULONG name = m68k_read32(&f.ctx, cli + 16) << 2;
    const size_t len = std::strlen(f.ctx.program_name);
    check(name == cli + 64 && name % 4 == 0 &&
          calls.allocation_size == 64 + 3 * 260,
          "cli_CommandName is a BPTR into the same allocation");
    check(f.memory[name] == len &&
          std::memcmp(f.memory + name + 1, f.ctx.program_name, len) == 0 &&
          f.memory[name + len + 1] == 0,
          "guest command name is a terminated BCPL string");
    check(f.memory[cli + 16] == 0 && f.memory[cli + 17] == 0 &&
          f.memory[cli + 18] == 1 && f.memory[cli + 19] == 0x10,
          "command-name BPTR is stored big endian");
    check(m68k_read32(&f.ctx, cli + 52) == 4096 &&
          f.memory[cli + 54] == 0x10 && f.memory[cli + 55] == 0,
          "cli_DefaultStack retains the guest startup policy in big endian");
    check(m68k_read32(&f.ctx, cli + 60) == 0x300,
          "cli_Module is derived from the guest segment address");

    bool unsupported_zero = true;
    for (ULONG offset = 0; offset < 64; offset++)
        if (!(offset >= 4 && offset < 8) && !(offset >= 24 && offset < 28) &&
            !(offset >= 16 && offset < 20) &&
            !(offset >= 52 && offset < 56) && !(offset >= 60 && offset < 64))
            unsupported_zero &= f.memory[cli + offset] == 0;
    check(unsupported_zero,
          "unsupported fields are zero even when the heap returns dirty memory");
    check(f.memory[cli - 1] == 0xA5 &&
          f.memory[cli + calls.allocation_size] == 0xA5,
          "CLI construction leaves both allocation canaries untouched");
    check(f.ctx.num_shadows == 0,
          "partial CLI is not registered as a native shadow");

    bool untouched = true;
    for (unsigned i = 1; i < 8; i++)
        untouched &= f.state.d[i] == 0xFE000001u + i;
    for (unsigned i = 0; i < 7; i++)
        untouched &= f.state.a[i] == 0xFD000001u + i;
    check(untouched, "Cli ignores every argument register");
}

static void test_cli_identity_and_guest_writes()
{
    Fixture f;
    ULONG setup_cli = Kaffeine_GetCLI(&f.ctx);
    /* The setup callers store this BPTR in every current-process shadow. */
    m68k_write32(&f.ctx, 0x200 + 172, setup_cli >> 2);
    m68k_write32(&f.ctx, setup_cli, 0x12345678);
    m68k_write32(&f.ctx, setup_cli + 52, 0x10203040);
    UBYTE snapshot[4096];
    std::memcpy(snapshot, f.memory, sizeof(snapshot));

    ULONG result = f.dispatch();
    check(result == (m68k_read32(&f.ctx, 0x200 + 172) << 2) &&
          result == Kaffeine_GetCLI(&f.ctx),
          "setup helper, Process.pr_CLI and repeated Cli share one guest identity");
    check(calls.allocation_calls == 1 && calls.native_cli_calls == 1,
          "cached Cli does not allocate or consult the native process again");
    check(std::memcmp(snapshot + setup_cli, f.memory + setup_cli,
                      calls.allocation_size) == 0,
          "repeated Cli preserves guest changes to the partial CLI");

    calls.native_cli_present = false;
    check(f.dispatch() == setup_cli && calls.native_cli_calls == 1,
          "guest CLI survives execution in a child with no native CLI");
}

static void test_absent_cli_and_retry()
{
    Fixture f;
    calls.native_cli_present = false;
    check(f.dispatch() == 0 && f.ctx.m68k_cli == 0 && calls.allocation_calls == 0 &&
          cli_error == ERROR_OBJECT_WRONG_TYPE,
          "absent native CLI returns null without allocating a guest CLI");
    calls.native_cli_present = true;
    check(f.dispatch() == guest_cli && calls.allocation_calls == 1,
          "an earlier absent CLI does not poison a later valid setup");
}

static void test_allocation_failure()
{
    Fixture f;
    calls.allocation_fails = true;
    UBYTE snapshot[4096];
    std::memcpy(snapshot, f.memory, sizeof(snapshot));
    check(Kaffeine_GetCLI(&f.ctx) == 0 && f.ctx.m68k_cli == 0 && cli_error == ERROR_NO_FREE_STORE,
          "allocation failure returns null without publishing a partial CLI");
    check(calls.allocation_calls == 1 &&
          std::memcmp(snapshot, f.memory, sizeof(snapshot)) == 0,
          "one failed allocation leaves guest memory unchanged");
    calls.allocation_fails = false;
    check(f.dispatch() == guest_cli && calls.allocation_calls == 2,
          "CLI creation can retry after allocation failure");
}

static void test_bad_cli_allocation()
{
    for (ULONG addr : { 4096u, 0xFFFFFFFEu, 4096u - (64 + 3 * 260) + 1 })
    {
        Fixture f;
        calls.allocation_address = addr;
        m68k_write32(&f.ctx, 0x80, 0x76543210);
        UBYTE before[4096];
        std::memcpy(before, f.memory, sizeof(before));
        check(f.dispatch() == 0 && f.ctx.m68k_cli == 0 && cli_error == ERROR_BAD_NUMBER &&
              std::memcmp(before, f.memory, sizeof(before)) == 0,
              "CLI rejects invalid whole allocation ranges before any initialization writes");
    }
}

static void test_empty_and_bounded_names()
{
    Fixture empty;
    empty.ctx.program_name[0] = 0;
    empty.ctx.seg_start = 0;
    ULONG cli = empty.dispatch();
    check(calls.allocation_size == 64 + 3 * 260 &&
          m68k_read32(&empty.ctx, cli + 16) == (cli + 64) >> 2 && empty.memory[cli + 64] == 0,
          "empty command name has an owned zero-length BSTR");
    check(m68k_read32(&empty.ctx, cli + 60) == 1,
          "a missing guest segment retains the existing nonzero startup marker");

    Fixture maximum;
    std::memset(maximum.ctx.program_name, 'X', sizeof(maximum.ctx.program_name));
    cli = maximum.dispatch();
    ULONG name = m68k_read32(&maximum.ctx, cli + 16) << 2;
    check(calls.allocation_size == 64 + 3 * 260 && maximum.memory[name] == 255 &&
          maximum.memory[name + 256] == 0 && maximum.memory[name + 257] == 0,
          "an unterminated source is bounded to the one-byte BSTR length");
    bool copied = true;
    for (unsigned i = 1; i <= 255; i++)
        copied &= maximum.memory[name + i] == 'X';
    check(copied, "all 255 bytes of the maximum guest command name survive");
}

int main()
{
    test_cli_dispatch_and_layout();
    test_cli_identity_and_guest_writes();
    test_absent_cli_and_retry();
    test_allocation_failure();
    test_empty_and_bounded_names();
    test_bad_cli_allocation();
    std::printf("CLI thunk regressions: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
